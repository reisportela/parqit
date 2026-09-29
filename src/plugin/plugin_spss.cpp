/* parqit — `spss_convert`: one SPSS system file (.sav/.zsav) → one Parquet
 * file (SPSS-READ-1).
 *
 * Serves `parqit save <file.parquet> using <file.sav>` and every command that
 * reads a .sav (the ado converts it into a package-owned Parquet bridge that
 * the ordinary Parquet path then reads). The plan (engine/spss_plan.hpp)
 * profiles the whole file first — a truncated or corrupt file fails here,
 * before anything is written — and the rows stream through the
 * parqit_read_sav table function into the verified Parquet writer
 * (copy_out_parquet: staged file, engine write count re-checked by a scan of
 * the staged file, atomic publish). One more check runs before publishing:
 * the rows written must equal the cases the profile counted, and the source
 * file must be the one profiled. */
#include "plugin/plugin_spss.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/hexcodec.hpp"
#include "engine/legacy_encoding.hpp"
#include "engine/request.hpp"
#include "engine/session.hpp"
#include "engine/spss_plan.hpp"
#include "engine/spss_table.hpp"
#include "plugin/plugin_io.hpp"

namespace parqit_plugin {

namespace {

constexpr ST_retcode kRcUsage = 198;
constexpr ST_retcode kRcFileNotFound = 601;
constexpr ST_retcode kRcNotSpss = 610; /* like Stata's "file not ... format" */
constexpr ST_retcode kRcEngine = 920;

void cry(const std::string &s) {
    std::string line = parqit::with_encoding_hint(s); /* CSV-ENC-1 */
    line.push_back('\n');
    SF_error(const_cast<char *>(line.c_str()));
}

void save_local(const std::string &name, const std::string &value) {
    SF_macro_save(const_cast<char *>(name.c_str()), const_cast<char *>(value.c_str()));
}

/* the engine wraps a table function's error in its exception class name;
 * the parqit message after it is the one the user can act on */
std::string engine_text(std::string err) {
    for (const char *p : {"Invalid Input Error: ", "Invalid Error: ", "IO Error: "}) {
        const size_t at = err.find(p);
        if (at != std::string::npos) return err.substr(at + std::string(p).size());
    }
    return err;
}

bool same_file(const std::string &a, const std::string &b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path pa = fs::u8path(a), pb = fs::u8path(b);
    if (!fs::exists(pb, ec)) return fs::weakly_canonical(pa, ec) == fs::weakly_canonical(pb, ec);
    return fs::equivalent(pa, pb, ec);
}

} // namespace

ST_retcode cmd_spss_convert(const std::vector<std::string> &args) {
    std::string reqpath, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        cry("parqit: malformed request path");
        return kRcUsage;
    }
    parqit::json req;
    if (!parqit::load_request(reqpath, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string src, dest, tmpdir, compression, encoding, who;
    if (!parqit::req_text(req, "src", &src, &err) || !parqit::req_text(req, "dest", &dest, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err) ||
        !parqit::req_text(req, "compression", &compression, &err, false) ||
        !parqit::req_text(req, "encoding", &encoding, &err, false) ||
        !parqit::req_text(req, "who", &who, &err, false)) {
        cry("parqit: " + err);
        return kRcUsage;
    }
    if (who.empty()) who = "parqit";
    /* ENC-3: a name parqit does not decode is a usage error, refused before
     * the file is opened */
    if (!encoding.empty()) {
        parqit::LegacyEncoding enc;
        if (!parqit::legacy_encoding_parse(encoding, &enc) || enc.is_utf16()) {
            cry(who + ": encoding(" + encoding + ") is not an encoding parqit decodes; it decodes " +
                std::string(parqit::legacy_encoding_families()));
            return kRcUsage;
        }
    }
    const bool replace = req.value("replace", false);
    const long long level = req.value("compression_level", -1LL);

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::u8path(src), ec) || fs::is_directory(fs::u8path(src), ec)) {
        cry(who + ": file " + src + " not found");
        return kRcFileNotFound;
    }
    if (same_file(src, dest)) {
        cry(who + ": the destination " + dest + " is the SPSS file itself; write the Parquet "
                  "file under another name");
        return kRcUsage;
    }

    parqit::Session &s = parqit::Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());
    if (!s.ensure_open()) {
        cry(who + ": could not start the engine: " + s.last_error());
        return kRcEngine;
    }

    parqit::spss::ReadOptions opt;
    opt.encoding = encoding;
    opt.default_encoding = parqit::legacy_encoding_name(encoding_session_default()); /* ENC-3 */
    std::shared_ptr<const parqit::spss::Plan> plan;
    try {
        plan = std::make_shared<const parqit::spss::Plan>(parqit::spss::make_plan(src, opt));
    } catch (const std::exception &e) {
        cry(who + ": " + e.what());
        return kRcNotSpss;
    }

    const std::string token = parqit::spss::register_plan(plan);
    long long written = -1;
    const std::function<bool(std::string *)> pre_publish = [&](std::string *perr) {
        if (written != plan->ncases) {
            *perr = "the Parquet file holds " + std::to_string(written) + " rows but the SPSS "
                    "file has " + std::to_string(plan->ncases) + " cases";
            return false;
        }
        uint64_t size = 0;
        long long mtime = 0;
        if (!parqit::spss::file_identity(src, &size, &mtime) || size != plan->file_size ||
            mtime != plan->file_mtime) {
            *perr = "the SPSS file " + src + " changed while it was being converted; convert it again";
            return false;
        }
        return true;
    };
    const std::string sql =
        "SELECT * FROM parqit_read_sav(" + parqit::quote_literal(token) + ")";
    ST_retcode rc = copy_out_parquet(s, sql, dest, replace, compression, level, {}, 0,
                                     plan->kv_metadata_sql, &written, &err, nullptr, &pre_publish);
    parqit::spss::release_plan(token);
    if (rc != 0) {
        cry(who + ": " + engine_text(err));
        return rc;
    }

    std::error_code aec;
    const fs::path abs = fs::absolute(fs::u8path(dest), aec);
    save_local("_parqit_spss_dest", parqit::hex_encode(aec ? dest : abs.u8string()));
    save_local("_parqit_spss_n", std::to_string(plan->ncases));
    save_local("_parqit_spss_k", std::to_string(plan->cols.size()));
    std::string xm;
    for (const auto &n : plan->xmissing_stata) xm += (xm.empty() ? "" : " ") + n;
    save_local("_parqit_spss_xmvars", xm);
    save_local("_parqit_spss_encoding", plan->dict.encoding_used);
    save_local("_parqit_spss_declared", parqit::hex_encode(plan->dict.declared_encoding));
    const parqit::spss::Compression cz = plan->dict.compression;
    save_local("_parqit_spss_compression",
               cz == parqit::spss::Compression::Zlib       ? "zlib"
               : cz == parqit::spss::Compression::Bytecode ? "bytecode"
                                                          : "none");
    save_local("_parqit_spss_tcells", std::to_string(plan->transcoded_cells));
    save_local("_parqit_spss_tmeta", std::to_string(plan->transcoded_meta));
    save_local("_parqit_spss_nnotes", std::to_string(plan->notes.size()));
    for (size_t i = 0; i < plan->notes.size(); i++)
        save_local("_parqit_spss_note" + std::to_string(i + 1), parqit::hex_encode(plan->notes[i]));
    return 0;
}

} // namespace parqit_plugin
