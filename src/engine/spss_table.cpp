#include "engine/spss_table.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace parqit {
namespace spss {

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
    std::vector<int> companion_of; /* output column of each variable's companion, -1 none */
};

struct InitData {
    /* its own reference: the reader points into plan->dict, which must outlive
     * it whatever order the engine destroys bind and init data in */
    std::shared_ptr<const Plan> plan;
    std::unique_ptr<CaseReader> reader;
    std::string raw, utf8;
    bool done = false;
};

void destroy_bind(void *p) { delete static_cast<BindData *>(p); }
void destroy_init(void *p) { delete static_cast<InitData *>(p); }

duckdb_type out_type(OutKind k) {
    switch (k) {
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

void sav_bind(duckdb_bind_info info) {
    try {
        duckdb_value v = duckdb_bind_get_parameter(info, 0);
        char *s = duckdb_get_varchar(v);
        const std::string token = s ? s : "";
        if (s) duckdb_free(s);
        duckdb_destroy_value(&v);
        std::shared_ptr<const Plan> plan = lookup(token);
        if (!plan) {
            duckdb_bind_set_error(info, "parqit_read_sav: no prepared SPSS conversion under this "
                                        "token (it is internal to parqit)");
            return;
        }
        auto *bd = new BindData;
        bd->plan = plan;
        bd->companion_of.assign(plan->cols.size(), -1);
        for (const auto &c : plan->cols) add_column(info, c.name, out_type(c.kind));
        int next = static_cast<int>(plan->cols.size());
        for (size_t i = 0; i < plan->cols.size(); i++) {
            if (!plan->cols[i].companion) continue;
            add_column(info, plan->cols[i].companion_name, DUCKDB_TYPE_TINYINT);
            bd->companion_of[i] = next++;
        }
        duckdb_bind_set_cardinality(info, static_cast<idx_t>(plan->ncases), true);
        duckdb_bind_set_bind_data(info, bd, destroy_bind);
    } catch (const std::exception &e) {
        duckdb_bind_set_error(info, e.what());
    }
}

void sav_init(duckdb_init_info info) {
    try {
        const auto *bd = static_cast<const BindData *>(duckdb_init_get_bind_data(info));
        const Plan &plan = *bd->plan;
        uint64_t size = 0;
        long long mtime = 0;
        if (!file_identity(plan.path, &size, &mtime) || size != plan.file_size ||
            mtime != plan.file_mtime) {
            duckdb_init_set_error(info, ("the SPSS file " + plan.path +
                                         " changed while it was being converted; convert it again")
                                            .c_str());
            return;
        }
        auto *id = new InitData;
        id->plan = bd->plan;
        id->reader = std::make_unique<CaseReader>(id->plan->path, id->plan->dict);
        duckdb_init_set_init_data(info, id, destroy_init);
        duckdb_init_set_max_threads(info, 1); /* one sequential decoder */
    } catch (const std::exception &e) {
        duckdb_init_set_error(info, e.what());
    }
}

void sav_scan(duckdb_function_info info, duckdb_data_chunk out) {
    const auto *bd = static_cast<const BindData *>(duckdb_function_get_bind_data(info));
    auto *id = static_cast<InitData *>(duckdb_function_get_init_data(info));
    try {
        const Plan &plan = *bd->plan;
        const Dictionary &d = plan.dict;
        const size_t ncols = duckdb_data_chunk_get_column_count(out);
        std::vector<duckdb_vector> vec(ncols);
        std::vector<uint64_t *> valid(ncols);
        std::vector<void *> data(ncols);
        for (size_t c = 0; c < ncols; c++) {
            vec[c] = duckdb_data_chunk_get_vector(out, c);
            duckdb_vector_ensure_validity_writable(vec[c]);
            valid[c] = duckdb_vector_get_validity(vec[c]);
            data[c] = duckdb_vector_get_data(vec[c]);
        }
        const idx_t cap = duckdb_vector_size();
        idx_t row = 0;
        while (row < cap && !id->done) {
            if (!id->reader->next()) {
                id->done = true;
                break;
            }
            for (size_t i = 0; i < plan.cols.size(); i++) {
                const ColumnSpec &c = plan.cols[i];
                const Variable &v = d.vars[static_cast<size_t>(c.var)];
                const int comp = bd->companion_of[i];
                int8_t code = 0;
                bool null = false;
                if (c.kind == OutKind::Varchar) {
                    id->reader->raw_string(v, &id->raw);
                    decode_text(d, id->raw.data(), id->raw.size(), &id->utf8);
                    duckdb_vector_assign_string_element_len(vec[i], row, id->utf8.data(),
                                                            id->utf8.size());
                    duckdb_validity_set_row_valid(valid[i], row);
                    continue;
                }
                const double x = id->reader->number(v);
                if (id->reader->is_sysmis(v)) {
                    null = true;
                } else if (!v.missing.empty() && v.missing.matches(x)) {
                    null = true;
                    code = static_cast<int8_t>(c.code_of(x));
                } else {
                    bool ok = true;
                    switch (c.kind) {
                    case OutKind::Date: {
                        int32_t days;
                        ok = seconds_to_unix_days(x, &days);
                        static_cast<duckdb_date *>(data[i])[row].days = days;
                        break;
                    }
                    case OutKind::Timestamp: {
                        int64_t us;
                        ok = seconds_to_unix_us(x, &us);
                        static_cast<duckdb_timestamp *>(data[i])[row].micros = us;
                        break;
                    }
                    case OutKind::Time: {
                        int64_t us;
                        ok = seconds_to_time_us(x, &us);
                        static_cast<duckdb_time *>(data[i])[row].micros = us;
                        break;
                    }
                    default:
                        static_cast<double *>(data[i])[row] = x;
                    }
                    if (!ok) {
                        /* the profile proved every value fits; a value that does
                         * not means the file is not the one profiled */
                        duckdb_function_set_error(
                            info, ("the SPSS file " + plan.path + " changed while it was being "
                                   "converted (variable " + v.name + ", case " +
                                   std::to_string(id->reader->cases_read()) + "); convert it again")
                                      .c_str());
                        duckdb_data_chunk_set_size(out, 0);
                        return;
                    }
                }
                if (null) duckdb_validity_set_row_invalid(valid[i], row);
                else duckdb_validity_set_row_valid(valid[i], row);
                if (comp >= 0) {
                    static_cast<int8_t *>(data[static_cast<size_t>(comp)])[row] = code;
                    duckdb_validity_set_row_valid(valid[static_cast<size_t>(comp)], row);
                }
            }
            row++;
        }
        if (id->done && id->reader->cases_read() != plan.ncases) {
            duckdb_function_set_error(
                info, ("the SPSS file " + plan.path + " changed while it was being converted (" +
                       std::to_string(id->reader->cases_read()) + " cases now, " +
                       std::to_string(plan.ncases) + " when profiled); convert it again")
                          .c_str());
            duckdb_data_chunk_set_size(out, 0);
            return;
        }
        duckdb_data_chunk_set_size(out, row);
    } catch (const std::exception &e) {
        duckdb_function_set_error(info, e.what());
        duckdb_data_chunk_set_size(out, 0);
    }
}

} // namespace

bool register_table_function(duckdb_connection con, std::string *err) {
    duckdb_table_function f = duckdb_create_table_function();
    if (!f) {
        if (err) *err = "could not create internal function parqit_read_sav";
        return false;
    }
    duckdb_table_function_set_name(f, "parqit_read_sav");
    duckdb_logical_type vt = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(f, vt);
    duckdb_destroy_logical_type(&vt);
    duckdb_table_function_set_bind(f, sav_bind);
    duckdb_table_function_set_init(f, sav_init);
    duckdb_table_function_set_function(f, sav_scan);
    const duckdb_state st = duckdb_register_table_function(con, f);
    duckdb_destroy_table_function(&f);
    if (st != DuckDBSuccess) {
        if (err) *err = "could not register internal function parqit_read_sav";
        return false;
    }
    return true;
}

std::string register_plan(std::shared_ptr<const Plan> plan) {
    const std::string token = "sav" + std::to_string(++g_seq);
    std::lock_guard<std::mutex> lock(g_mu);
    g_plans[token] = std::move(plan);
    return token;
}

void release_plan(const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_plans.erase(token);
}

} // namespace spss
} // namespace parqit
