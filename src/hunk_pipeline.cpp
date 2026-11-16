// license:BSD-3-Clause
// lib-chd-kit: decodes a CHD's hunks ahead of the reader, on several threads.
// See hunk_pipeline.h.

#include "hunk_pipeline.h"

#include "ioprocs.h"
#include "osdfile.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace chdman {

namespace {

// decoded bytes per batch: a batch is the unit of work handed to a thread
constexpr uint32_t BATCH_BYTES = 512 * 1024;
// the cap MAME's work queues use for compression (WORK_MAX_THREADS)
constexpr int MAX_THREADS = 16;

// codecs that decode a hunk on their own, with no configuration or metadata
// (A/V Huffman needs both, so A/V hunks stay with chd_file)
bool decodes_alone(chd_codec_type codec) noexcept
{
	switch (codec)
	{
	case CHD_CODEC_ZLIB:
	case CHD_CODEC_ZSTD:
	case CHD_CODEC_LZMA:
	case CHD_CODEC_HUFFMAN:
	case CHD_CODEC_FLAC:
	case CHD_CODEC_CD_ZLIB:
	case CHD_CODEC_CD_ZSTD:
	case CHD_CODEC_CD_LZMA:
	case CHD_CODEC_CD_FLAC:
		return true;
	default:
		return false;
	}
}

} // anonymous namespace


std::unique_ptr<hunk_pipeline> hunk_pipeline::attach(chd_file &chd, std::string const &path, uint32_t first_hunk, uint32_t end_hunk, int num_processors, bool hash)
{
	int threads = (num_processors > 0) ? num_processors : int(std::thread::hardware_concurrency());
	threads = std::clamp(threads, 1, MAX_THREADS);
	bool const decodable = (chd.version() == 5) && chd.compressed();
	if (threads < 2 || !decodable || end_hunk <= first_hunk)
	{
		if (!hash)
			return nullptr;
		threads = 0;
	}

	auto pipeline = std::make_unique<hunk_pipeline>(chd, path, first_hunk, end_hunk, threads, hash);
	if (!hash && pipeline->threads() == 0)
		return nullptr; // couldn't open the file a second time
	return pipeline;
}


hunk_pipeline::hunk_pipeline(chd_file &chd, std::string const &path, uint32_t first_hunk, uint32_t end_hunk, int threads, bool hash)
	: m_chd(chd)
	, m_first(first_hunk)
	, m_end(std::max(first_hunk, end_hunk))
	, m_hunkbytes(chd.hunk_bytes())
	, m_hash(hash)
{
	m_batch_hunks = std::max<uint32_t>(1, BATCH_BYTES / m_hunkbytes);
	m_batch_count = uint32_t((uint64_t(m_end - m_first) + m_batch_hunks - 1) / m_batch_hunks);

	// the stored bytes are read through a handle of our own, leaving chd_file's
	// to the calling thread; without one every hunk falls back to chd_file
	if ((chd.version() == 5) && chd.compressed() && util::core_file::open(path, OPEN_FLAG_READ, m_file))
		m_file.reset();
	if (!m_file)
		threads = 0;
	threads = std::min<int>(threads, int(m_batch_count));

	m_decoders.resize(std::max(threads, 1));
	for (decoder_set &decoders : m_decoders)
		for (int i = 0; i < 4; i++)
			if (decodes_alone(chd.compression(i)))
				decoders[i] = chd_codec_list::new_decompressor(chd.compression(i), chd);

	// each thread can have a batch in hand while the caller holds one and the
	// I/O thread reads ahead
	m_slots.resize((threads > 0) ? std::min<size_t>(2 * threads + 2, m_batch_count) : 1);
	for (batch &bt : m_slots)
	{
		bt.entries.resize(m_batch_hunks);
		bt.stored.reserve(size_t(m_batch_hunks) * m_hunkbytes);
		bt.data.resize(size_t(m_batch_hunks) * m_hunkbytes);
	}
	if (m_hash)
		m_scratch.resize(m_hunkbytes);

	try
	{
		if (threads > 0)
		{
			m_reader = std::thread([this] { reader_main(); });
			for (int i = 0; i < threads; i++)
				m_workers.emplace_back([this, i] { worker_main(size_t(i)); });
		}
	}
	catch (...)
	{
		stop();
		throw;
	}
	m_chd.set_hunk_source(this);
}

hunk_pipeline::~hunk_pipeline()
{
	m_chd.set_hunk_source(nullptr);
	stop();
}

void hunk_pipeline::stop() noexcept
{
	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_stop = true;
	}
	m_space_cv.notify_all();
	m_work_cv.notify_all();
	m_ready_cv.notify_all();
	if (m_reader.joinable())
		m_reader.join();
	for (std::thread &worker : m_workers)
		if (worker.joinable())
			worker.join();
}


// ======================> I/O and decoding

// finds the stored bytes of batch b's hunks and reads them, one read per run
// of hunks stored back to back (normally the whole batch)
void hunk_pipeline::read_batch(uint32_t b)
{
	batch &bt = slot(b);
	bt.first = m_first + b * m_batch_hunks;
	bt.count = std::min(m_batch_hunks, m_end - bt.first);

	uint64_t total = 0;
	for (uint32_t i = 0; i < bt.count; i++)
	{
		entry &e = bt.entries[i];
		e = entry();
		if (m_file && m_chd.locate_hunk(bt.first + i, e.loc) && (e.loc.codec < 0 || m_decoders[0][e.loc.codec]))
		{
			e.located = true;
			e.stored_offset = uint32_t(total);
			total += e.loc.length;
		}
	}
	try
	{
		bt.stored.resize(size_t(total));
	}
	catch (...)
	{
		// leave the batch to chd_file
		for (uint32_t i = 0; i < bt.count; i++)
			bt.entries[i].located = false;
		return;
	}

	for (uint32_t i = 0; i < bt.count; )
	{
		entry const &head = bt.entries[i];
		if (!head.located)
		{
			i++;
			continue;
		}
		uint32_t j = i + 1;
		uint64_t length = head.loc.length;
		while (j < bt.count && bt.entries[j].located && bt.entries[j].loc.offset == head.loc.offset + length)
			length += bt.entries[j++].loc.length;

		// a failed or short read covers the whole run, but serially only the
		// hunks actually affected would fail: leave the run to chd_file, which
		// reads each hunk on its own with upstream's error handling
		auto const [readerr, actual] = util::read_at(*m_file, head.loc.offset, bt.stored.data() + head.stored_offset, size_t(length));
		if (readerr || (actual != length))
			for (uint32_t k = i; k < j; k++)
				bt.entries[k].located = false;
		i = j;
	}
}

// decodes a batch the way chd_file::read_hunk() does, CRC check included
void hunk_pipeline::decode_batch(batch &bt, decoder_set &decoders)
{
	for (uint32_t i = 0; i < bt.count; i++)
	{
		entry &e = bt.entries[i];
		if (!e.located || e.err)
			continue;
		uint8_t const *const src = bt.stored.data() + e.stored_offset;
		uint8_t *const dest = bt.data.data() + size_t(i) * m_hunkbytes;
		try
		{
			util::crc16_t calculated;
			if (e.loc.codec < 0)
			{
				std::memcpy(dest, src, m_hunkbytes);
				calculated = util::crc16_creator::simple(dest, m_hunkbytes);
			}
			else
			{
				chd_decompressor &decompressor = *decoders[e.loc.codec];
				decompressor.decompress(src, e.loc.length, dest, m_hunkbytes);
				calculated = !decompressor.lossy()
						? util::crc16_creator::simple(dest, m_hunkbytes)
						: util::crc16_creator::simple(src, e.loc.length);
			}
			if (calculated != util::crc16_t(e.loc.crc16))
				e.err = chd_file::error::DECOMPRESSION_ERROR;
		}
		catch (std::error_condition const &err)
		{
			e.err = err;
		}
		catch (...)
		{
			// read_hunk() lets anything else propagate: decode it there again,
			// on the calling thread, so the caller sees what it saw before
			e.located = false;
		}
	}
}

void hunk_pipeline::reader_main()
{
	for (uint32_t b = 0; b < m_batch_count; b++)
	{
		{
			std::unique_lock<std::mutex> lock(m_lock);
			m_space_cv.wait(lock, [this, b] { return m_stop || b < m_released + m_slots.size(); });
			if (m_stop)
				return;
			slot(b).index = b;
			slot(b).ready = false;
		}
		read_batch(b);
		{
			std::lock_guard<std::mutex> guard(m_lock);
			m_queue.push_back(b);
		}
		m_work_cv.notify_one();
	}
}

void hunk_pipeline::worker_main(size_t id)
{
	for (;;)
	{
		uint32_t b;
		{
			std::unique_lock<std::mutex> lock(m_lock);
			m_work_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
			if (m_stop)
				return;
			b = m_queue.front();
			m_queue.pop_front();
		}
		decode_batch(slot(b), m_decoders[id]);
		{
			std::lock_guard<std::mutex> guard(m_lock);
			slot(b).ready = true;
		}
		m_ready_cv.notify_all();
	}
}


// ======================> consumer side (the calling thread)

hunk_pipeline::batch &hunk_pipeline::wait_ready(uint32_t b)
{
	batch &bt = slot(b);
	if (m_workers.empty())
	{
		// no threads: decode it here and now
		bt.index = b;
		read_batch(b);
		decode_batch(bt, m_decoders[0]);
		bt.ready = true;
		return bt;
	}
	std::unique_lock<std::mutex> lock(m_lock);
	m_ready_cv.wait(lock, [&bt, b] { return bt.index == b && bt.ready; });
	return bt;
}

// hands batch b back (hashing it first when asked); b is the oldest unreleased batch
void hunk_pipeline::release(uint32_t b)
{
	batch &bt = slot(b);
	if (m_hash && !m_hash_err)
	{
		uint64_t const logical = m_chd.logical_bytes();
		for (uint32_t i = 0; i < bt.count; i++)
		{
			uint32_t const hunknum = bt.first + i;
			uint64_t const start = uint64_t(hunknum) * m_hunkbytes;
			if (start >= logical)
				break;
			entry const &e = bt.entries[i];
			uint8_t const *data = bt.data.data() + size_t(i) * m_hunkbytes;
			std::error_condition err = e.err;
			if (!err && !e.located)
			{
				m_bypass = true;
				err = m_chd.read_hunk(hunknum, m_scratch.data());
				m_bypass = false;
				data = m_scratch.data();
			}
			if (err)
			{
				m_hash_err = err;
				break;
			}
			m_sha1.append(data, uint32_t(std::min<uint64_t>(m_hunkbytes, logical - start)));
		}
	}

	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_released = b + 1;
		bt.ready = false;
	}
	m_space_cv.notify_one();
}

void hunk_pipeline::advance_to(uint32_t b)
{
	if (m_holding)
	{
		release(m_next - 1);
		m_holding = false;
	}
	for (; m_next < b; m_next++)
	{
		wait_ready(m_next);
		release(m_next);
	}
	wait_ready(b);
	m_next = b + 1;
	m_holding = true;
}

bool hunk_pipeline::supply_hunk(uint32_t hunknum, void *buffer, std::error_condition &err)
{
	if (m_bypass || hunknum < m_first || hunknum >= m_end)
		return false;

	uint32_t const b = (hunknum - m_first) / m_batch_hunks;
	if (!m_holding || b != m_next - 1)
	{
		// hunks behind the read position are chd_file's to decode
		if (b < m_next)
			return false;
		advance_to(b);
	}

	batch const &bt = slot(b);
	uint32_t const i = hunknum - bt.first;
	entry const &e = bt.entries[i];
	if (e.err)
	{
		err = e.err;
		return true;
	}
	if (!e.located)
		return false;
	std::memcpy(buffer, bt.data.data() + size_t(i) * m_hunkbytes, m_hunkbytes);
	err.clear();
	return true;
}

util::sha1_t hunk_pipeline::finish_hash(std::function<void ()> const &check)
{
	if (m_holding)
	{
		release(m_next - 1);
		m_holding = false;
	}
	for (; m_next < m_batch_count; m_next++)
	{
		if (check)
			check();
		wait_ready(m_next);
		release(m_next);
	}
	if (m_hash_err)
		throw m_hash_err;
	return m_sha1.finish();
}

} // namespace chdman
