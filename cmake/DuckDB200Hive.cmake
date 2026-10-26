parqit_duckdb200_source(
    "src/common/hive_partitioning.cpp"
    d65d1fbf4b80cb0ffcf5cadbb50eef26ad90eda91725aabe9d6615a2422e3298
    9f85ec8558f1336709df0ee36182d20c892c10fe84435069d9fb98487e7548e2
    [=[	// On SQLNULL, DuckDB writes "__HIVE_DEFAULT_PARTITION__", instead of string version "NULL".
	if (str_val == DEFAULT_PARTITION_NAME) {]=]
    [=[	// parqit: retain the published 1.5.6 interpretation of legacy Hive markers.
	// Check before unescaping so an encoded literal remains literal text.
	if (IsNull(str_val)) {]=])
