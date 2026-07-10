#include "doctest.h"

#include <cstdio>
#include <fstream>

#include "engine/hexcodec.hpp"
#include "engine/request.hpp"
#include "test_tmp.hpp"

using namespace parqit;

static std::string tmpfile_with(const std::string &content) {
    std::string path = parqit_test::tmp_path("parqit_test_req.json");
    std::ofstream f(path, std::ios::binary);
    f << content;
    return path;
}

TEST_CASE("request parsing: hex fields decode; malformed input is loud") {
    json req;
    std::string err;
    std::string path = tmpfile_with(
        R"({"cmd":"use_prepare","files":[")" + hex_encode("/data/q p.parquet") +
        R"("],"respfile":")" + hex_encode("/tmp/resp") + R"(","tmpdir":")" +
        hex_encode("/tmp") + R"("})");
    REQUIRE(load_request(path, &req, &err));

    std::vector<std::string> files;
    REQUIRE(req_text_list(req, "files", &files, &err));
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "/data/q p.parquet");

    std::string respfile;
    REQUIRE(req_text(req, "respfile", &respfile, &err));
    CHECK(respfile == "/tmp/resp");

    /* optional fields are quietly empty */
    std::vector<std::string> vl;
    REQUIRE(req_text_list(req, "varlist", &vl, &err, false));
    CHECK(vl.empty());

    /* required-and-missing or non-hex are errors */
    std::string out;
    CHECK_FALSE(req_text(req, "nope", &out, &err));
    std::string bad = tmpfile_with(R"({"files":["zz"]})");
    REQUIRE(load_request(bad, &req, &err));
    CHECK_FALSE(req_text_list(req, "files", &files, &err));

    std::string notjson = tmpfile_with("{nope");
    CHECK_FALSE(load_request(notjson, &req, &err));
    CHECK_FALSE(load_request("/definitely/not/here.json", &req, &err));
    std::remove(path.c_str());
}

TEST_CASE("response writer emits pipe records with hex text fields") {
    std::string path = parqit_test::tmp_path("parqit_test_resp.txt");
    {
        ResponseWriter w;
        std::string err;
        REQUIRE(w.open(path, &err));
        w.rec("var", {"1"}, {"wage", "wa|ge", "double"});
        w.rec("warn", {}, {"some | pipey, 'quoted' warning"});
        REQUIRE(w.close(&err));
    }
    std::ifstream f(path);
    std::string l1, l2;
    std::getline(f, l1);
    std::getline(f, l2);
    CHECK(l1 == "var|1|" + hex_encode("wage") + "|" + hex_encode("wa|ge") + "|" +
                    hex_encode("double"));
    CHECK(l2 == "warn|" + hex_encode("some | pipey, 'quoted' warning"));
    std::remove(path.c_str());
}
