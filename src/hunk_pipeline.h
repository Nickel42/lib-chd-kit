// license:BSD-3-Clause
// lib-chd-kit: decodes a CHD's hunks ahead of the reader, on several threads.
//
// Internal C++ interface used by chdman_core.cpp.

#ifndef LIBCHDKIT_HUNK_PIPELINE_H
#define LIBCHDKIT_HUNK_PIPELINE_H

#include "chd.h"
#include "chdcodec.h"
#include "corefile.h"
#include "hashing.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace chdman {

// ======================> hunk_pipeline
//
// Attached to an open chd_file as its hunk_source. One I/O thread reads the
// stored bytes of hunks [first, end) in file order, through a file handle of
// its own, so the disk sees sequential reads. Worker threads decode them in
// batches, and chd_file::read_hunk() takes each hunk from the pipeline when
// the caller reaches it. The caller reads exactly the bytes it read before;
// they are just decoded ahead of it.
//
// Hunks the pipeline doesn't decode itself (parent references, v3/v4 or
// uncompressed CHDs, A/V codecs) and hunks behind the read position fall back
// to chd_file's own decoding on the calling thread.
//
// With hash set, every hunk in the range is also added to a SHA-1 of the
// CHD's logical bytes, in order, whether or not the caller reads it, so an
// extraction can verify the CHD in the same pass. threads = 0 decodes on the
// calling thread as hunks are needed (still hashing everything).

class hunk_pipeline final : public chd_file::hunk_source
{
public:
	// returns nullptr when neither more threads nor hashing are wanted
	// (num_processors is the session limit; 0 = all processors)
	static std::unique_ptr<hunk_pipeline> attach(chd_file &chd, std::string const &path, uint32_t first_hunk, uint32_t end_hunk, int num_processors, bool hash);

	hunk_pipeline(chd_file &chd, std::string const &path, uint32_t first_hunk, uint32_t end_hunk, int threads, bool hash);
	~hunk_pipeline();
	hunk_pipeline(hunk_pipeline const &) = delete;
	hunk_pipeline &operator=(hunk_pipeline const &) = delete;

	bool supply_hunk(uint32_t hunknum, void *buffer, std::error_condition &err) override;

	// hashes the hunks not reached yet and returns the SHA-1 of the logical
	// bytes of the range. check is called between batches (it may throw to
	// cancel). Throws std::error_condition if a hunk can't be read.
	util::sha1_t finish_hash(std::function<void ()> const &check);

	int threads() const noexcept { return int(m_workers.size()); }

private:
	struct entry
	{
		bool located = false;            // decoded here, not by chd_file
		chd_file::hunk_location loc{};
		uint32_t stored_offset = 0;      // offset of its bytes in batch::stored
		std::error_condition err;
	};

	struct batch
	{
		uint32_t index = UINT32_MAX;     // which batch the slot holds
		bool ready = false;              // decoded and waiting for the consumer
		uint32_t first = 0, count = 0;   // hunk range
		std::vector<entry> entries;
		std::vector<uint8_t> stored;     // the bytes stored for its hunks
		std::vector<uint8_t> data;       // decoded hunks
	};

	using decoder_set = std::array<chd_decompressor::ptr, 4>;

	batch &slot(uint32_t b) noexcept { return m_slots[b % m_slots.size()]; }
	void read_batch(uint32_t b);
	void decode_batch(batch &bt, decoder_set &decoders);
	batch &wait_ready(uint32_t b);
	void release(uint32_t b);
	void advance_to(uint32_t b);
	void reader_main();
	void worker_main(size_t id);
	void stop() noexcept;

	chd_file &m_chd;
	util::core_file::ptr m_file;
	uint32_t const m_first, m_end;
	uint32_t const m_hunkbytes;
	uint32_t m_batch_hunks = 1;
	uint32_t m_batch_count = 0;
	std::vector<batch> m_slots;
	std::vector<decoder_set> m_decoders; // one set per worker, or one for the caller

	// consumer state (calling thread only)
	uint32_t m_next = 0;      // next batch the caller takes
	bool m_holding = false;   // batch m_next - 1 is still held for reads
	bool m_bypass = false;    // let chd_file decode (fallback hunks while hashing)
	bool m_hash;
	util::sha1_creator m_sha1;
	std::error_condition m_hash_err;
	std::vector<uint8_t> m_scratch;

	// shared state
	std::mutex m_lock;
	std::condition_variable m_space_cv, m_work_cv, m_ready_cv;
	uint32_t m_released = 0;  // batches handed back; their slots may be reused
	std::deque<uint32_t> m_queue;
	bool m_stop = false;
	std::thread m_reader;
	std::vector<std::thread> m_workers;
};

} // namespace chdman

#endif // LIBCHDKIT_HUNK_PIPELINE_H
