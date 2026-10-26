// Partitioned COPY: every row reaches exactly its partition, and (2.0) the bulk
// of a partition is not encoded behind an earlier delayed sliver (PARTITION-DELAY-1).
#include "doctest.h"
#include "duckdb.h"
#include "engine/session.hpp"
#include "test_tmp.hpp"
#include <filesystem>
#include <string>

namespace {

struct PartitionScope {
    std::string root, threads;
    ~PartitionScope() {
        auto &s = parqit::Session::instance();
        std::string ignored;
        s.set_threads(std::stoll(threads), &ignored);
        s.exec("RESET partitioned_write_flush_threshold", &ignored);
        s.exec("RESET partitioned_write_max_open_files", &ignored);
        s.exec("DROP TABLE IF EXISTS __pcopy_bounds", &ignored);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

std::string scalar(const std::string &sql) {
    std::string value, error;
    REQUIRE_MESSAGE(parqit::Session::instance().query_scalar(sql, &value, &error), error);
    return value;
}

void exec(const std::string &sql) {
    std::string error;
    REQUIRE_MESSAGE(parqit::Session::instance().exec(sql, &error), error);
}

/* 120 clustered partitions of 1500..5999 rows, written in key order with small
 * row groups: a flush generation regularly ends a few rows into a partition. */
std::string clustered_source(const std::string &root) {
    const std::string path = root + "/source.parquet";
    exec("CREATE OR REPLACE TEMP TABLE __pcopy_bounds AS SELECT k, "
         "sum(n) OVER (ORDER BY k) - n AS lo, sum(n) OVER (ORDER BY k) AS hi FROM "
         "(SELECT j AS k, 1500 + (j * 7919) % 4500 AS n FROM range(120) t(j))");
    const std::string rows = scalar("SELECT max(hi) FROM __pcopy_bounds");
    exec("COPY (SELECT r.i, b.k, b.k AS k_copy, 'v' || (r.i % 97)::VARCHAR AS payload "
         "FROM range(" + rows + ") r(i) JOIN __pcopy_bounds b "
         "ON r.i >= b.lo AND r.i < b.hi ORDER BY r.i) TO " + parqit::quote_literal(path) +
         " (FORMAT PARQUET, ROW_GROUP_SIZE 2048)");
    CHECK(scalar("SELECT count(*) FROM read_parquet(" + parqit::quote_literal(path) + ")") == rows);
    return path;
}

std::string tree_scan(const std::string &dir, const std::string &extra = "") {
    return "read_parquet(" + parqit::quote_literal(dir + "/**/*.parquet") + extra + ")";
}

} // namespace

TEST_CASE("partitioned COPY keeps every row in its own partition under many flush generations") {
    auto &s = parqit::Session::instance();
    PartitionScope scope{parqit_test::tmp_path("parqit_pcopy_rows"), scalar("SELECT current_setting('threads')")};
    std::filesystem::create_directories(scope.root);
    const std::string src = clustered_source(scope.root);
    std::string error;
    REQUIRE(s.set_threads(8, &error));
    /* tiny threshold: many generations, delayed slivers; few open files: eviction */
    exec("SET partitioned_write_flush_threshold = 1024");
    exec("SET partitioned_write_max_open_files = 4");

    const std::string mixed = "(SELECT i, CASE WHEN k = 7 THEN NULL WHEN k % 3 = 0 THEN k ELSE (k * 37) % 120 END AS k, "
                              "k_copy, payload FROM read_parquet(" + parqit::quote_literal(src) + "))";
    const std::string numeric_dir = scope.root + "/numeric";
    exec("COPY (SELECT i, k, k AS k_file, payload FROM " + mixed + ") TO " +
         parqit::quote_literal(numeric_dir) + " (FORMAT PARQUET, PARTITION_BY (k))");
    const std::string back = tree_scan(numeric_dir, ", hive_partitioning = true, filename = true");
    CHECK(scalar("SELECT count(*) FROM " + back) == scalar("SELECT count(*) FROM " + mixed));
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k, payload FROM " + mixed +
                 " EXCEPT ALL SELECT i, k_file, payload FROM " + back + ")") == "0");
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k_file, payload FROM " + back +
                 " EXCEPT ALL SELECT i, k, payload FROM " + mixed + ")") == "0");
    /* the directory a row sits in names its own key, NULL included */
    CHECK(scalar("SELECT count(*) FROM " + back + " WHERE k IS DISTINCT FROM k_file") == "0");
    CHECK(scalar("SELECT count(*) FROM (SELECT filename FROM " + back +
                 " GROUP BY filename HAVING count(DISTINCT coalesce(k_file, -1)) <> 1)") == "0");

    const std::string text_dir = scope.root + "/text";
    exec("COPY (SELECT i, CASE WHEN k_copy % 5 = 0 THEN 'a=b ' || k_copy::VARCHAR WHEN k_copy % 5 = 1 "
         "THEN 'ç€ ' || k_copy::VARCHAR ELSE 'k' || (k_copy % 9)::VARCHAR END AS s, "
         "k_copy, payload FROM read_parquet(" + parqit::quote_literal(src) + ")) TO " +
         parqit::quote_literal(text_dir) + " (FORMAT PARQUET, PARTITION_BY (s), WRITE_PARTITION_COLUMNS true)");
    const std::string tback = tree_scan(text_dir, ", hive_partitioning = false");
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k_copy, payload FROM read_parquet(" + parqit::quote_literal(src) +
                 ") EXCEPT ALL SELECT i, k_copy, payload FROM " + tback + ")") == "0");
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k_copy, payload FROM " + tback +
                 " EXCEPT ALL SELECT i, k_copy, payload FROM read_parquet(" + parqit::quote_literal(src) + "))") == "0");
    const std::string tfiles = tree_scan(text_dir, ", hive_partitioning = false, filename = true");
    CHECK(scalar("SELECT count(*) FROM (SELECT filename FROM " + tfiles +
                 " GROUP BY filename HAVING count(DISTINCT s) <> 1)") == "0");
    CHECK(scalar("SELECT count(DISTINCT regexp_replace(filename, '/[^/]*$', '')) FROM " + tfiles) ==
          scalar("SELECT count(DISTINCT s) FROM " + tfiles));
}

#ifdef PARQIT_DUCKDB_200
TEST_CASE("partitioned COPY prepares a partition's bulk beside its delayed sliver") {
    auto &s = parqit::Session::instance();
    PartitionScope scope{parqit_test::tmp_path("parqit_pcopy_delay"), scalar("SELECT current_setting('threads')")};
    std::filesystem::create_directories(scope.root);
    const std::string src = clustered_source(scope.root);
    std::string error;
    /* one thread: generations and their files are deterministic. A partition whose
     * first rows fell into an earlier generation below the threshold must not have
     * its later bulk appended behind that sliver (upstream encodes it serially);
     * the bulk is written first and the sliver joins later, so some partition file
     * is no longer in source order. Sorted runs (ORDER_BY) keep the old routing. */
    REQUIRE(s.set_threads(1, &error));
    exec("SET partitioned_write_flush_threshold = 1024");
    const std::string plain = scope.root + "/plain";
    exec("COPY (SELECT * FROM read_parquet(" + parqit::quote_literal(src) + ")) TO " +
         parqit::quote_literal(plain) + " (FORMAT PARQUET, PARTITION_BY (k))");
    const std::string descents =
        "SELECT count(*) FROM (SELECT i < lag(i) OVER (PARTITION BY filename ORDER BY file_row_number) AS d FROM ";
    CHECK(scalar(descents + tree_scan(plain, ", filename = true, file_row_number = true") + ") WHERE d") != "0");
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k_copy FROM read_parquet(" + parqit::quote_literal(src) +
                 ") EXCEPT ALL SELECT i, k_copy FROM " + tree_scan(plain) + ")") == "0");

    const std::string sorted = scope.root + "/sorted";
    exec("COPY (SELECT * FROM read_parquet(" + parqit::quote_literal(src) + ")) TO " +
         parqit::quote_literal(sorted) + " (FORMAT PARQUET, PARTITION_BY (k), ORDER_BY (i))");
    CHECK(scalar(descents + tree_scan(sorted, ", filename = true, file_row_number = true") + ") WHERE d") == "0");
    CHECK(scalar("SELECT count(*) FROM " + tree_scan(sorted)) == scalar("SELECT count(*) FROM read_parquet(" +
                                                                       parqit::quote_literal(src) + ")"));

    REQUIRE(s.set_threads(8, &error));
    const std::string rotated = scope.root + "/rotated";
    exec("COPY (SELECT * FROM read_parquet(" + parqit::quote_literal(src) + ")) TO " +
         parqit::quote_literal(rotated) + " (FORMAT PARQUET, PARTITION_BY (k), ROW_GROUP_SIZE 1024, ROW_GROUPS_PER_FILE 1)");
    CHECK(scalar("SELECT count(*) FROM (SELECT i, k_copy FROM read_parquet(" + parqit::quote_literal(src) +
                 ") EXCEPT ALL SELECT i, k_copy FROM " + tree_scan(rotated) + ")") == "0");
    CHECK(scalar("SELECT max(num_row_groups) FROM (SELECT file_name, count(DISTINCT row_group_id) AS num_row_groups "
                 "FROM parquet_metadata(" + parqit::quote_literal(rotated + "/**/*.parquet") + ") GROUP BY file_name)") == "1");
}
#endif

TEST_CASE("partitioned COPY: files per partition under file eviction are recorded; rows stay complete") {
    /* A6 (audit of 25 Oct 2026): a partition can be written in several files when
     * open files are evicted, upstream as with the 2.0 corrections. The counts are
     * facts of this engine and these settings, printed with them, not invariants;
     * every row is in its own partition in every case. */
    auto &s = parqit::Session::instance();
    PartitionScope scope{parqit_test::tmp_path("parqit_pcopy_files"), scalar("SELECT current_setting('threads')")};
    std::filesystem::create_directories(scope.root);
    const std::string src = clustered_source(scope.root);
    const std::string engine = scalar("SELECT version()");
    struct Setting {
        int threads, open_files, threshold;
    };
    int n = 0;
    for (const Setting &x : {Setting{8, 1, 1024}, Setting{8, 4, 1024}, Setting{1, 1, 1024}, Setting{8, 100, 1024}}) {
        std::string error;
        REQUIRE(s.set_threads(x.threads, &error));
        exec("SET partitioned_write_max_open_files = " + std::to_string(x.open_files));
        exec("SET partitioned_write_flush_threshold = " + std::to_string(x.threshold));
        const std::string dir = scope.root + "/out" + std::to_string(n++);
        exec("COPY (SELECT i, k, k AS k_file, payload FROM read_parquet(" + parqit::quote_literal(src) + ")) TO " +
             parqit::quote_literal(dir) + " (FORMAT PARQUET, PARTITION_BY (k))");
        const std::string back = tree_scan(dir, ", hive_partitioning = true, filename = true");
        CHECK(scalar("SELECT count(*) FROM (SELECT i, k, payload FROM read_parquet(" + parqit::quote_literal(src) +
                     ") EXCEPT ALL SELECT i, k_file, payload FROM " + back + ")") == "0");
        CHECK(scalar("SELECT count(*) FROM (SELECT i, k_file, payload FROM " + back +
                     " EXCEPT ALL SELECT i, k, payload FROM read_parquet(" + parqit::quote_literal(src) + "))") == "0");
        CHECK(scalar("SELECT count(*) FROM " + back + " WHERE k IS DISTINCT FROM k_file") == "0");
        const std::string per = "(SELECT k, count(DISTINCT filename) AS f FROM " + back + " GROUP BY k)";
        const std::string files = scalar("SELECT sum(f) FROM " + per), split = scalar("SELECT count(*) FROM " + per +
                                                                                    " WHERE f > 1"),
                          most = scalar("SELECT max(f) FROM " + per);
        CHECK(scalar("SELECT count(*) FROM " + per) == "120");
        MESSAGE(engine << ", threads " << x.threads << ", max open files " << x.open_files << ", flush threshold "
                       << x.threshold << ": " << files << " files, " << split
                       << " of 120 partitions in more than one file, at most " << most);
    }
}
