# Compatibility corrections with independent regression coverage.

parqit_duckdb200_source(
    "src/include/duckdb/main/client_config.hpp"
    c55a6aa0afddebfbe2bb6c4fe9f85fcbf61700cf715e0745e90f9ccfc4285e5c
    b24079c531dbd0a2b2801468de6ba310346ecb15786e603d63d423dae3714eb0
    [=[	idx_t max_streaming_buffer_size = idx_t(10) * 1024 * 1024;]=]
    [=[	idx_t max_streaming_buffer_size = 1000000; // parqit: retain the 1.5.6 reset/default budget]=])

parqit_duckdb200_source(
    "src/common/local_file_system.cpp"
    3ffb2e098a27cbfb92bedf6da73ec3c1928a9fa1fbb5d1a51bb8d8a7748ed4f3
    13bc9330360a4b7b55c79ab1d549ae7ab4d0e15d11db9318f4ab543eef01cd8c
    [=[struct PathSplit {]=]
    [=[// Local glob traversal must let the OS resolve symbolic links before "..".
static string JoinLocalGlobPath(FileSystem &fs, const string &left, const string &right) {
	if (left.empty()) return right;
	if (left.back() == '/') return left + right;
#ifdef _WIN32
	if (left.back() == '\\' ||
	    (left.size() == 2 && left[1] == ':' && StringUtil::CharacterIsAlpha(left[0]))) return left + right;
#endif
	return left + fs.PathSeparator(left) + right;
}

struct PathSplit {]=]
    [=[info.path = fs.JoinPath(path, info.path);
		if (IsSymbolicLink]=]
    [=[info.path = JoinLocalGlobPath(fs, path, info.path);
		if (IsSymbolicLink]=]
    [=[info.path = fs.JoinPath(path, info.path);
			result.push_back]=]
    [=[info.path = JoinLocalGlobPath(fs, path, info.path);
			result.push_back]=]
    [=[auto filename = fs.JoinPath(current_path, next_component);]=]
    [=[auto filename = JoinLocalGlobPath(fs, current_path, next_component);]=]
    [=[expand_directories.emplace(fs.JoinPath(current_path, next_component), split_index + 1);]=]
    [=[expand_directories.emplace(JoinLocalGlobPath(fs, current_path, next_component), split_index + 1);]=]
    [=[	for (auto &seg : parsed.GetPathSegments()) {
		splits.emplace_back(fs, seg);
	}
	absolute_path = parsed.IsAbsolute();]=]
    [=[	// Keep unnormalised local components: a/.. is not lexical when a is a symlink.
	idx_t begin = base.size();
	for (idx_t pos = begin; pos <= path.size(); ++pos) {
		bool separator = pos == path.size() || path[pos] == '/';
#ifdef _WIN32
		separator = separator || (pos < path.size() && path[pos] == '\\');
#endif
		if (!separator) continue;
		auto segment = path.substr(begin, pos - begin);
		if (!segment.empty() && segment != ".") splits.emplace_back(fs, std::move(segment));
		begin = pos + 1;
	}
	absolute_path = parsed.IsAbsolute();]=])

parqit_duckdb200_source(
    "src/planner/binder/expression/bind_window_expression.cpp"
    d1afe194510617626d58f7ed456b019419fc200c15258b8074f3eb4a400d7455
    1a65a9fad82f02cd1f4c56da5ec7f02a389802c9dcc013a07fcfa84f75566069
    [=[		if (!error.HasError() && bound_partition->IsVolatile()) {
			throw BinderException(error_context, "PARTITION BY window expressions cannot be volatile");
		}]=]
    [=[		// parqit: retain 1.5.6 volatile partition keys, with the volatile-aware
		// window rewrite guard and single-evaluation regression tests.]=]
    [=[		if (!error.HasError() && bound_order->IsVolatile()) {
			throw BinderException(error_context, "ORDER BY window expressions cannot be volatile");
		}]=]
    [=[		// parqit: retain 1.5.6 volatile order keys, with the volatile-aware
		// window rewrite guard and single-evaluation regression tests.]=])

# Restore native 2.0 bare-integer inference after the earlier compatibility
# candidate. Fresh upstream source is already final; the only other accepted
# input is the exact reviewed legacy rewrite. Stata literals are independently
# canonicalised to binary64 by exprtrans.cpp before they reach SQL.
parqit_duckdb200_source(
    "src/parser/literal.cpp"
    74fe6b7d53f33d9d4549f0c3d627d0ac3e45159f5b3add548a45a0296fe21b49
    60f975b5d19c5efc03d42814ec7f5654e54591d495aef22525012fb97db0365a
    [=[		// parqit: preserve 1.5.6 bare-literal inference. Integers beyond
		// UHUGEINT use the existing checked DOUBLE parser below. Explicit
		// BIGNUM casts keep their type; no BIGNUM value is narrowed here.]=]
    [=[		// integers beyond uhugeint keep their precision as a bignum
		try {
			auto bignum_str = Bignum::VarcharToBignum(str_val);
			return Value::BIGNUM(bignum_str);
		} catch (const ConversionException &) {
			// not a valid bignum either: fall through to the decimal/double path
		}]=])
