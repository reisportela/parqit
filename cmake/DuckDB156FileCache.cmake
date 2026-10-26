# PARQIT-CACHE-IDENTITY: keep byte-cache entries tied to their source generation.

parqit_patch_duckdb_file_cache(
    "src/include/duckdb/storage/external_file_cache.hpp"
    5d0eb65d26d5305a9ef06d494e304977dd7b97e7e66141ef552a51371fb7ed2e
    37e0b054a911083bd3c11e3d5c0227c90a814bccd240836c11f374e4a36199d5
    [=[		StorageLock lock;]=]
    [=[		StorageLock lock;
		//! PARQIT-CACHE-IDENTITY: guarded by lock, distinct from the global generation.
		idx_t content_generation = 0;]=])

parqit_patch_duckdb_file_cache(
    "src/include/duckdb/storage/caching_file_system.hpp"
    7e4f863194ef2d02002096c08232cb2b7509701eb5f9d0f92b92bb7e8d0be4b5
    3c058dc25ef58106f17b88d355d70db603f61af0094280077feb0aabe0d51e20
    [=[	string version_tag;]=]
    [=[	string version_tag;
	idx_t content_generation = 0;]=])

parqit_patch_duckdb_file_cache(
    "src/storage/caching_file_system.cpp"
    2c150fdd44203527bd0f1de71d4e8cb23c995d282cb12ff40d4728a131670430
    a99cf9b7ff5e2e31236e0b1bb874b9503fdd27fea6c5b164b25692344738d797
    [=[	if (cached_file && cached_file->generation == external_file_cache.GetGeneration()) {
		return cached_file;
	}
	const bool needs_reopen = file_handle != nullptr;
	if (needs_reopen) {
		file_handle.reset();
	}
	cached_file = external_file_cache.GetOrCreateCachedFile(path.path);
	if (needs_reopen) {
		GetFileHandle();
	}
	return cached_file;]=]
    [=[	// PARQIT-CACHE-IDENTITY: retain this handle's source across cache toggles.
	// A retired entry is read uncached; only new handles join the new cache.
	return cached_file;]=]
    [=[	auto guard = cached_file->lock.GetSharedLock();
	if (cached_file->Ranges(guard).empty()) {]=]
    [=[	auto guard = cached_file->lock.GetSharedLock();
	content_generation = cached_file->content_generation;
	if (cached_file->Ranges(guard).empty()) {]=]
    [=[if (!cached_file->IsValid(guard, Validate(), version_tag, last_modified)) {
			cached_file->Ranges(guard).clear(); // Invalidate entire cache
		}]=]
    [=[// Opening a lazy miss must not combine known different sources, even with NO_VALIDATION.
		if (!cached_file->IsValid(guard, true, version_tag, last_modified)) {
			cached_file->Ranges(guard).clear(); // Invalidate entire cache
			++cached_file->content_generation;
		}
		content_generation = cached_file->content_generation;]=]
    [=[if (!external_file_cache.IsEnabled() || no_validation_metadata) {]=]
    [=[if (!external_file_cache.IsEnabled() || cached_file->generation != external_file_cache.GetGeneration() ||
	    no_validation_metadata) {]=]
    [=[	idx_t new_nr_bytes = nr_bytes;]=]
    [=[	// A cache-only handle binds its actual source on the first miss. Re-select overlaps afterwards.
	if (!file_handle) {
		GetFileHandle();
		overlapping_ranges.clear();
		start_location_of_next_range = optional_idx();
		result = TryReadFromCache(buffer, nr_bytes, location, overlapping_ranges, start_location_of_next_range);
		if (result.IsValid()) {
			return result;
		}
	}

	idx_t new_nr_bytes = nr_bytes;]=]
    [=[if (!external_file_cache.IsEnabled() || !CanSeek() || no_validation_metadata) {]=]
    [=[if (!external_file_cache.IsEnabled() || cached_file->generation != external_file_cache.GetGeneration() ||
	    !CanSeek() || no_validation_metadata) {]=]
    [=[	// Finally, if we weren't able to find the file range in the cache, we have to create a new file range
	result = external_file_cache.GetBufferManager().Allocate(MemoryTag::EXTERNAL_FILE_CACHE, nr_bytes);]=]
    [=[	GetFileHandle();
	// Finally, if we weren't able to find the file range in the cache, we have to create a new file range
	result = external_file_cache.GetBufferManager().Allocate(MemoryTag::EXTERNAL_FILE_CACHE, nr_bytes);]=]
    [=[idx_t CachingFileHandle::GetFileSize() {
	if (!Validate()) {]=]
    [=[idx_t CachingFileHandle::GetFileSize() {
	if (!file_handle && !Validate()) {]=]
    [=[timestamp_t CachingFileHandle::GetLastModifiedTime() {
	if (!Validate()) {]=]
    [=[timestamp_t CachingFileHandle::GetLastModifiedTime() {
	if (!file_handle && !Validate()) {]=]
    [=[string CachingFileHandle::GetVersionTag() {
	if (!Validate()) {]=]
    [=[string CachingFileHandle::GetVersionTag() {
	if (!file_handle && !Validate()) {]=]
    [=[bool CachingFileHandle::CanSeek() {
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::CanSeek() {
	if (!file_handle && !Validate()) {]=]
    [=[bool CachingFileHandle::OnDiskFile() {
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::OnDiskFile() {
	if (!file_handle && !Validate()) {]=]
    [=[	// Get read lock for cached ranges
	auto guard = cached_file->lock.GetSharedLock();]=]
    [=[	// Get read lock for cached ranges
	auto guard = cached_file->lock.GetSharedLock();
	if (content_generation != cached_file->content_generation ||
	    cached_file->generation != external_file_cache.GetGeneration()) {
		return result;
	}]=]
    [=[	// Grab the lock again (write lock this time) to insert the newly created buffer into the ranges
	auto guard = cached_file->lock.GetExclusiveLock();]=]
    [=[	// Grab the lock again (write lock this time) to insert the newly created buffer into the ranges
	auto guard = cached_file->lock.GetExclusiveLock();
	if (content_generation != cached_file->content_generation || !external_file_cache.IsEnabled() ||
	    cached_file->generation != external_file_cache.GetGeneration()) {
		return std::move(pin);
	}]=]
    [=[		nr_bytes = NumericCast<idx_t>(GetFileHandle().Read(context, buffer, nr_bytes));
		position += NumericCast<idx_t>(nr_bytes);]=]
    [=[		auto &handle = GetFileHandle();
		if (handle.CanSeek()) {
			handle.Seek(position);
		}
		nr_bytes = NumericCast<idx_t>(handle.Read(context, buffer, nr_bytes));
		position += NumericCast<idx_t>(nr_bytes);]=])
