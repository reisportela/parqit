#include "engine/rdata_table.hpp"
#include "engine/rdata_convert.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace parqit {
namespace rdata {

namespace {

std::mutex g_mu;
std::map<std::string, std::shared_ptr<const Plan>> g_plans;
std::atomic<unsigned long long> g_seq{0};

std::shared_ptr<const Plan> lookup(const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_plans.find(token);
    return it == g_plans.end() ? nullptr : it->second;
}

struct BindData {
    std::shared_ptr<const Plan> plan;
    std::vector<int> companion_of; /* output column of each column's companion, -1 none */
};

struct InitData {
    /* its own reference: the cursors read plan->parsed.source, which must
     * outlive them whatever order the engine destroys bind and init data in */
    std::shared_ptr<const Plan> plan;
    std::vector<std::unique_ptr<Cursor>> cursors;
    uint64_t done = 0;
    std::string text;
};

void destroy_bind(void *p) { delete static_cast<BindData *>(p); }
void destroy_init(void *p) { delete static_cast<InitData *>(p); }

duckdb_type out_type(OutKind k) {
    switch (k) {
    case OutKind::Boolean: return DUCKDB_TYPE_BOOLEAN;
    case OutKind::Integer: return DUCKDB_TYPE_INTEGER;
    case OutKind::BigInt: return DUCKDB_TYPE_BIGINT;
    case OutKind::Date: return DUCKDB_TYPE_DATE;
    case OutKind::Timestamp: return DUCKDB_TYPE_TIMESTAMP;
    case OutKind::Time: return DUCKDB_TYPE_TIME;
    case OutKind::Varchar: return DUCKDB_TYPE_VARCHAR;
    default: return DUCKDB_TYPE_DOUBLE;
    }
}

void add_column(duckdb_bind_info info, const std::string &name, duckdb_type t) {
    duckdb_logical_type lt = duckdb_create_logical_type(t);
    duckdb_bind_add_result_column(info, name.c_str(), lt);
    duckdb_destroy_logical_type(&lt);
}

void rdata_bind(duckdb_bind_info info) {
    try {
        duckdb_value v = duckdb_bind_get_parameter(info, 0);
        char *s = duckdb_get_varchar(v);
        const std::string token = s ? s : "";
        if (s) duckdb_free(s);
        duckdb_destroy_value(&v);
        std::shared_ptr<const Plan> plan = lookup(token);
        if (!plan) {
            duckdb_bind_set_error(info, "parqit_read_rdata: no prepared R conversion under this "
                                        "token (it is internal to parqit)");
            return;
        }
        auto *bd = new BindData;
        bd->plan = plan;
        bd->companion_of.assign(plan->cols.size(), -1);
        for (const auto &c : plan->cols) add_column(info, c.engine_name, out_type(c.kind));
        int next = static_cast<int>(plan->cols.size());
        for (size_t i = 0; i < plan->cols.size(); i++) {
            if (!plan->cols[i].companion) continue;
            add_column(info, plan->cols[i].companion_engine_name, DUCKDB_TYPE_TINYINT);
            bd->companion_of[i] = next++;
        }
        duckdb_bind_set_cardinality(info, static_cast<idx_t>(plan->nrow), true);
        duckdb_bind_set_bind_data(info, bd, destroy_bind);
    } catch (const std::exception &e) {
        duckdb_bind_set_error(info, e.what());
    }
}

void rdata_init(duckdb_init_info info) {
    try {
        const auto *bd = static_cast<const BindData *>(duckdb_init_get_bind_data(info));
        const Plan &plan = *bd->plan;
        uint64_t size = 0;
        long long mtime = 0;
        if (!file_identity(plan.path, &size, &mtime) || size != plan.file_size ||
            mtime != plan.file_mtime) {
            duckdb_init_set_error(info, ("the R data file " + plan.path +
                                         " changed while it was being converted; convert it again")
                                            .c_str());
            return;
        }
        auto *id = new InitData;
        id->plan = bd->plan;
        /* each column reads its own stretch of the file: bound the buffers
         * together, not per column (a buffer grows when one string needs it) */
        const size_t n = std::max<size_t>(1, plan.cols.size());
        const size_t per = std::min<size_t>(size_t(1) << 20,
                                            std::max<size_t>(size_t(4) << 10, (size_t(256) << 20) / n));
        for (const auto &c : plan.cols)
            id->cursors.push_back(std::make_unique<Cursor>(plan.parsed.source, c.vec, plan.parsed.codec, per));
        duckdb_init_set_init_data(info, id, destroy_init);
        duckdb_init_set_max_threads(info, 1); /* the cursors share one file */
    } catch (const std::exception &e) {
        duckdb_init_set_error(info, e.what());
    }
}

/* the table function's destination of convert_rows: one DuckDB vector (and
 * its companion's) */
struct VectorOut {
    duckdb_vector vec;
    uint64_t *validity;
    void *data;
    int8_t *codes;
    uint64_t *cvalid;
    void text(uint64_t r, const char *p, size_t n) { duckdb_vector_assign_string_element_len(vec, r, p, n); }
    void boolean(uint64_t r, bool b) { static_cast<bool *>(data)[r] = b; }
    void i64(uint64_t r, int64_t v) { static_cast<int64_t *>(data)[r] = v; }
    void i32(uint64_t r, int32_t v) { static_cast<int32_t *>(data)[r] = v; }
    void f64(uint64_t r, double v) { static_cast<double *>(data)[r] = v; }
    void days(uint64_t r, int32_t d) { static_cast<duckdb_date *>(data)[r].days = d; }
    void timestamp(uint64_t r, int64_t us) { static_cast<duckdb_timestamp *>(data)[r].micros = us; }
    void time(uint64_t r, int64_t us) { static_cast<duckdb_time *>(data)[r].micros = us; }
    void valid(uint64_t r, bool v) {
        if (v) duckdb_validity_set_row_valid(validity, r);
        else duckdb_validity_set_row_invalid(validity, r);
    }
    void code(uint64_t r, int8_t k) {
        if (!codes) return;
        codes[r] = k;
        duckdb_validity_set_row_valid(cvalid, r);
    }
};

void rdata_scan(duckdb_function_info info, duckdb_data_chunk out) {
    const auto *bd = static_cast<const BindData *>(duckdb_function_get_bind_data(info));
    auto *id = static_cast<InitData *>(duckdb_function_get_init_data(info));
    try {
        const Plan &plan = *bd->plan;
        const uint64_t total = static_cast<uint64_t>(plan.nrow);
        const uint64_t rows = std::min<uint64_t>(duckdb_vector_size(), total - id->done);
        if (rows == 0) {
            duckdb_data_chunk_set_size(out, 0);
            return;
        }
        for (size_t i = 0; i < plan.cols.size(); i++) {
            const ColumnSpec &c = plan.cols[i];
            Cursor &cur = *id->cursors[i];
            duckdb_vector vec = duckdb_data_chunk_get_vector(out, i);
            duckdb_vector_ensure_validity_writable(vec);
            uint64_t *valid = duckdb_vector_get_validity(vec);
            void *data = duckdb_vector_get_data(vec);
            int8_t *codes = nullptr;
            uint64_t *cvalid = nullptr;
            if (bd->companion_of[i] >= 0) {
                duckdb_vector cv = duckdb_data_chunk_get_vector(out, static_cast<idx_t>(bd->companion_of[i]));
                duckdb_vector_ensure_validity_writable(cv);
                cvalid = duckdb_vector_get_validity(cv);
                codes = static_cast<int8_t *>(duckdb_vector_get_data(cv));
            }
            VectorOut o{vec, valid, data, codes, cvalid};
            const std::string err = convert_rows(plan, c, cur, id->done, rows, o, &id->text);
            if (!err.empty()) {
                duckdb_function_set_error(info, err.c_str());
                duckdb_data_chunk_set_size(out, 0);
                return;
            }
        }
        id->done += rows;
        duckdb_data_chunk_set_size(out, static_cast<idx_t>(rows));
    } catch (const std::exception &e) {
        duckdb_function_set_error(info, e.what());
        duckdb_data_chunk_set_size(out, 0);
    }
}

} // namespace

bool register_table_function(duckdb_connection con, std::string *err) {
    duckdb_table_function f = duckdb_create_table_function();
    if (!f) {
        if (err) *err = "could not create internal function parqit_read_rdata";
        return false;
    }
    duckdb_table_function_set_name(f, "parqit_read_rdata");
    duckdb_logical_type vt = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(f, vt);
    duckdb_destroy_logical_type(&vt);
    duckdb_table_function_set_bind(f, rdata_bind);
    duckdb_table_function_set_init(f, rdata_init);
    duckdb_table_function_set_function(f, rdata_scan);
    const duckdb_state st = duckdb_register_table_function(con, f);
    duckdb_destroy_table_function(&f);
    if (st != DuckDBSuccess) {
        if (err) *err = "could not register internal function parqit_read_rdata";
        return false;
    }
    return true;
}

std::string register_plan(std::shared_ptr<const Plan> plan) {
    const std::string token = "rdata" + std::to_string(++g_seq);
    std::lock_guard<std::mutex> lock(g_mu);
    g_plans[token] = std::move(plan);
    return token;
}

void release_plan(const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_plans.erase(token);
}

size_t registered_plans() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_plans.size();
}

} // namespace rdata
} // namespace parqit
