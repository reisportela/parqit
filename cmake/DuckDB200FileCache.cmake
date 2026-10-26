# PARQIT-CACHE-IDENTITY: keep byte-cache entries tied to their source generation.

parqit_duckdb200_source(
    "src/include/duckdb/storage/external_file_cache/external_file_cache.hpp"
    a6dd03f8660eec00d689b453d4300c24ab092fdc997c98f9ae644ee27749de21
    0541d057b83769064fd8863897f1031e4b80d8d4d0f14b8212ad7a75098222ba
    [=[idx_t max_block_size);]=]
    [=[idx_t max_block_size, idx_t expected_generation);]=])

parqit_duckdb200_source(
    "src/storage/external_file_cache/external_file_cache.cpp"
    6bc01b43ce10cedd902bec9582970887eba8787aacc27b6ade823442535d4c27
    74e32c3d66f74bd10eb06d101797d9209985c14f5ae5b9c29eb773d1b7d0c2b4
    [=[idx_t max_block_size) {
	D_ASSERT(nr_bytes > 0);]=]
    [=[idx_t max_block_size, idx_t expected_generation) {
	D_ASSERT(nr_bytes > 0);]=]
    [=[	const annotated_lock_guard<annotated_mutex> map_guard(cached_file.map_lock);
	auto &blocks = cached_file.blocks;]=]
    [=[	const annotated_lock_guard<annotated_mutex> map_guard(cached_file.map_lock);
	// PARQIT-CACHE-IDENTITY: checking and acquiring must be indivisible with DropBlocks.
	if (cached_file.content_generation != expected_generation) {
		return {};
	}
	auto &blocks = cached_file.blocks;]=])

parqit_duckdb200_source(
    "src/include/duckdb/storage/external_file_cache/caching_file_system.hpp"
    641714fbdfc8b9625e1aeac1ba6699a826c560617eee5f34cd41f920b515dd21
    361cfcbbadbac3516f3f47e4e24965642d6a3b05b016343c3a1f8f473ee1e45b
    [=[void ReconcileCacheAfterRead(CachedFile &cached_file);]=]
    [=[bool ReconcileCacheAfterRead(CachedFile &cached_file, idx_t read_generation);]=]
    [=[	CacheValidationInfo validation_info;]=]
    [=[	CacheValidationInfo validation_info;
	idx_t content_generation DUCKDB_GUARDED_BY(file_handle_mutex) = 0;]=])

parqit_duckdb200_source(
    "src/storage/external_file_cache/caching_file_system.cpp"
    2dfe7906cfa653fcda7261a2c2742216d12797767b4d41e5451f176611d59f9b
    9904fad0095ff1eee12bf360a65e4b9dfa64a790450115a55010a427fc261bc6
    [=[	bool needs_reopen = false;
	shared_ptr<CachedFile> current_cached_file;
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (cached_file && cached_file->generation == external_file_cache.GetGeneration()) {
			return cached_file;
		}
		needs_reopen = file_handle != nullptr;
		if (needs_reopen) {
			file_handle.reset();
		}
		cached_file = external_file_cache.GetOrCreateCachedFile(path.path);
		current_cached_file = cached_file;
	}

	if (needs_reopen) {
		GetFileHandle();
	}
	return current_cached_file;]=]
    [=[	// PARQIT-CACHE-IDENTITY: a read and its async tasks retain one source.
	// Retired entries are read uncached; new handles join the current cache.
	return cached_file;]=]
    [=[bool CachingFileHandle::CanUseCache() {
	auto current_cached_file = EnsureCachedFileCurrent();]=]
    [=[bool CachingFileHandle::CanUseCache() {
	auto current_cached_file = EnsureCachedFileCurrent();
	if (current_cached_file->generation != external_file_cache.GetGeneration()) {
		return false;
	}]=]
    [=[void CachingFileHandle::ReconcileCacheAfterRead(CachedFile &cached_file) {
	CacheValidationInfo current;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		current = validation_info;
	}

	const annotated_lock_guard<annotated_mutex> guard(cached_file.meta_lock);
	auto &cached = cached_file.validation_info;
	if (ExternalFileCache::IsValid(Validate(), cached, current)) {
		TightenCacheDeadline(cached.cache_valid_until, current.cache_valid_until);
		return;
	}

	cached = current;
	// Existing readers retain their pinned blocks while future reads fetch replacements.
	external_file_cache.DropBlocks(cached_file);
}]=]
    [=[bool CachingFileHandle::ReconcileCacheAfterRead(CachedFile &cached_file, idx_t read_generation) {
	const annotated_lock_guard<annotated_mutex> handle_guard(file_handle_mutex);
	const annotated_lock_guard<annotated_mutex> guard(cached_file.meta_lock);
	// Every consumer checks: another lazy reader may have fetched a different source into an old block.
	if (cached_file.generation != external_file_cache.GetGeneration() ||
	    external_file_cache.GetContentGeneration(cached_file) != read_generation ||
	    (file_handle && content_generation != read_generation)) {
		return false;
	}
	if (!file_handle) {
		return true; // A complete NO_VALIDATION hit needs no file open.
	}
	auto &cached = cached_file.validation_info;
	if (ExternalFileCache::IsValid(Validate(), cached, validation_info)) {
		TightenCacheDeadline(cached.cache_valid_until, validation_info.cache_valid_until);
		return true;
	}

	cached = validation_info;
	external_file_cache.DropBlocks(cached_file);
	content_generation = external_file_cache.GetContentGeneration(cached_file);
	return true;
}]=]
    [=[		const bool first_access = (cached_file->validation_info.file_size == 0);
		const bool refresh_expired_cache = !Validate() && cached_file->validation_info.IsExpired();
		if (first_access || Validate() || refresh_expired_cache) {]=]
    [=[		const bool refresh_expired_cache = !Validate() && cached_file->validation_info.IsExpired();
		// A lazy miss has opened a real source: do not mix it with known different cached bytes.
		{]=]
    [=[ExternalFileCache::IsValid(Validate(), cached_file->validation_info, validation_info);]=]
    [=[ExternalFileCache::IsValid(true, cached_file->validation_info, validation_info);]=]
    [=[			cached_file->on_disk_file = file_handle->OnDiskFile();
		}
	}
	return file_handle;]=]
    [=[			cached_file->on_disk_file = file_handle->OnDiskFile();
		}
		content_generation = external_file_cache.GetContentGeneration(*cached_file);
	}
	return file_handle;]=]
    [=[	if (!external_file_cache.IsEnabled() || !external_file_cache.ShouldCacheFile(path.path) || !CanUseCache()) {
		auto buf = AllocateUncachedReadBuffer(external_file_cache.GetBufferManager(), nr_bytes);
		ReadUncached(buf.GetDataMutable(), nr_bytes, location);
		vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
		mem_handles.push_back({std::move(buf), 0, nr_bytes});
		return FileBufferHandleGroup(std::move(mem_handles));
	}]=]
    [=[	auto read_uncached = [&]() {
		auto buf = AllocateUncachedReadBuffer(external_file_cache.GetBufferManager(), nr_bytes);
		ReadUncached(buf.GetDataMutable(), nr_bytes, location);
		vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
		mem_handles.push_back({std::move(buf), 0, nr_bytes});
		return FileBufferHandleGroup(std::move(mem_handles));
	};
	if (!external_file_cache.IsEnabled() || !external_file_cache.ShouldCacheFile(path.path) || !CanUseCache()) {
		return read_uncached();
	}]=]
    [=[	auto blocks = external_file_cache.AcquireBlocks(*current_cached_file, fetch_location, fetch_end - fetch_location,
	                                                max_block_size);]=]
    [=[	idx_t read_generation;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		read_generation = file_handle ? content_generation :
		    external_file_cache.GetContentGeneration(*current_cached_file);
	}
	auto blocks = external_file_cache.AcquireBlocks(*current_cached_file, fetch_location, fetch_end - fetch_location,
	                                                max_block_size, read_generation);
	if (blocks.empty()) {
		return read_uncached();
	}]=]
    [=[	ReconcileCacheAfterRead(*current_cached_file);]=]
    [=[	if (!ReconcileCacheAfterRead(*current_cached_file, read_generation)) {
		return read_uncached();
	}]=]
    [=[idx_t CachingFileHandle::GetFileSize() {
	if (!Validate()) {]=]
    [=[idx_t CachingFileHandle::GetFileSize() {
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle) {
			return validation_info.file_size;
		}
	}
	if (!Validate()) {]=]
    [=[timestamp_t CachingFileHandle::GetLastModifiedTime() {
	if (!Validate()) {]=]
    [=[timestamp_t CachingFileHandle::GetLastModifiedTime() {
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle) {
			return validation_info.last_modified;
		}
	}
	if (!Validate()) {]=]
    [=[string CachingFileHandle::GetVersionTag() {
	if (!Validate()) {]=]
    [=[string CachingFileHandle::GetVersionTag() {
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle) {
			return validation_info.version_tag;
		}
	}
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::CanSeek() {
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::CanSeek() {
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle) {
			return file_handle->CanSeek();
		}
	}
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::OnDiskFile() {
	if (!Validate()) {]=]
    [=[bool CachingFileHandle::OnDiskFile() {
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle) {
			return file_handle->OnDiskFile();
		}
	}
	if (!Validate()) {]=])
