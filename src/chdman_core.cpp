// license:BSD-3-Clause
// copyright-holders:Aaron Giles, lib-chd-kit contributors
//
// lib-chd-kit core: the logic of MAME 0.289 src/tools/chdman.cpp, refactored so
// that it
//   - takes typed option structs instead of a string map,
//   - reports progress through a callback with cancellation,
//   - writes nothing to stdout/stderr (informational lines go to a log
//     callback),
//   - signals failure by throwing chdman::error, which the C ABI layer
//     converts into a status code and message.
// Function and variable names follow upstream to make diffing easy.

#include "chdman_core.h"

#include "avhuff.h"
#include "aviio.h"
#include "bitmap.h"
#include "cdrom.h"
#include "chd.h"
#include "chdcodec.h"
#include "corefile.h"
#include "corestr.h"
#include "coretmpl.h"
#include "hashing.h"
#include "multibyte.h"
#include "osdcore.h"
#include "osdfile.h"
#include "path.h"
#include "strformat.h"
#include "vbiparse.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <regex>
#include <system_error>
#include <tuple>
#include <utility>

extern int osd_num_processors; // osdsync.cpp

namespace chdman {

namespace {

//**************************************************************************
//  CONSTANTS
//**************************************************************************

constexpr uint32_t HUNK_SIZE_MIN = 16;
constexpr uint32_t HUNK_SIZE_MAX = 1024 * 1024;
constexpr uint32_t IDE_SECTOR_SIZE = 512;
constexpr uint32_t TEMP_BUFFER_SIZE = 32 * 1024 * 1024;

constexpr int MODE_NORMAL = 0;
constexpr int MODE_CUEBIN = 1;
constexpr int MODE_GDI = 2;

using codec_array = std::array<chd_codec_type, 4>;

const codec_array s_no_compression = { CHD_CODEC_NONE, CHD_CODEC_NONE, CHD_CODEC_NONE, CHD_CODEC_NONE };
const codec_array s_default_raw_compression = { CHD_CODEC_LZMA, CHD_CODEC_ZLIB, CHD_CODEC_HUFFMAN, CHD_CODEC_FLAC };
const codec_array s_default_hd_compression = { CHD_CODEC_LZMA, CHD_CODEC_ZLIB, CHD_CODEC_HUFFMAN, CHD_CODEC_FLAC };
const codec_array s_default_cd_compression = { CHD_CODEC_CD_LZMA, CHD_CODEC_CD_ZLIB, CHD_CODEC_CD_FLAC };
const codec_array s_default_ld_compression = { CHD_CODEC_AVHUFF };

// serialises changes to the process-wide osd_num_processors used when a
// compressor allocates its work queues
std::mutex s_numprocessors_lock;


//**************************************************************************
//  ERROR HELPERS
//**************************************************************************

template <typename Format, typename... Params>
[[noreturn]] void fail(chdman_status status, Format &&fmt, Params &&...args)
{
	throw error(status, util::string_format(std::forward<Format>(fmt), std::forward<Params>(args)...));
}

chdman_status status_from_condition(std::error_condition const &err)
{
	if (err.category() == chd_category())
	{
		switch (chd_file::error(err.value()))
		{
		case chd_file::error::METADATA_NOT_FOUND:
			return CHDMAN_ERR_NOT_FOUND;
		case chd_file::error::UNSUPPORTED_VERSION:
		case chd_file::error::UNSUPPORTED_FORMAT:
		case chd_file::error::UNKNOWN_COMPRESSION:
			return CHDMAN_ERR_UNSUPPORTED;
		default:
			return CHDMAN_ERR_CHD;
		}
	}
	if (err == std::errc::not_enough_memory)
		return CHDMAN_ERR_OUT_OF_MEMORY;
	if (err == std::errc::no_such_file_or_directory || err == std::errc::permission_denied)
		return CHDMAN_ERR_FILE_OPEN;
	return CHDMAN_ERR_IO;
}

template <typename Format, typename... Params>
[[noreturn]] void fail(std::error_condition const &err, Format &&fmt, Params &&...args)
{
	throw error(status_from_condition(err), util::string_format(std::forward<Format>(fmt), std::forward<Params>(args)...));
}


//**************************************************************************
//  STRING HELPERS
//**************************************************************************

std::string big_int_string(uint64_t intvalue)
{
	if (intvalue == 0)
		return "0";

	std::string str;
	bool first = true;
	while (intvalue != 0)
	{
		int chunk = intvalue % 1000;
		intvalue /= 1000;

		std::string insert = util::string_format((intvalue != 0) ? "%03d" : "%d", chunk);

		if (!first)
			str.insert(0, ",");
		first = false;
		str.insert(0, insert);
	}
	return str;
}

std::string msf_string_from_frames(uint32_t frames)
{
	return util::string_format("%02d:%02d:%02d", frames / (75 * 60), (frames / 75) % 60, frames % 75);
}

std::string sha1_string(util::sha1_t const &sha1)
{
	return (sha1 == util::sha1_t::null) ? std::string() : sha1.as_string();
}

std::string compression_string(chd_codec_type const (&compression)[4])
{
	if (compression[0] == CHD_CODEC_NONE)
		return "none";

	std::string str;
	for (int index = 0; index < 4; index++)
	{
		chd_codec_type type = compression[index];
		if (type == CHD_CODEC_NONE)
			break;
		if (index != 0)
			str.append(", ");
		str.push_back((type >> 24) & 0xff);
		str.push_back((type >> 16) & 0xff);
		str.push_back((type >> 8) & 0xff);
		str.push_back(type & 0xff);
		str.append(" (").append(chd_codec_list::codec_name(type)).append(")");
	}
	return str;
}

chd_metadata_tag parse_tag(std::string tag)
{
	// chdman pads the tag with spaces and uses the first four characters
	tag.append("    ");
	return CHD_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]);
}

std::string tag_string(chd_metadata_tag tag)
{
	return util::string_format("%c%c%c%c", (tag >> 24) & 0xff, (tag >> 16) & 0xff, (tag >> 8) & 0xff, tag & 0xff);
}


//**************************************************************************
//  INPUT/OUTPUT HELPERS
//**************************************************************************

void guess_chs(
		const std::string *filename,
		uint64_t filesize,
		int sectorsize,
		uint32_t &cylinders,
		uint32_t &heads,
		uint32_t &sectors,
		uint32_t &bps)
{
	// if this is a direct physical drive read, handle it specially
	if (filename && osd_get_physical_drive_geometry(filename->c_str(), &cylinders, &heads, &sectors, &bps))
		return;

	// if we have no length to work with, we can't guess
	if (filesize == 0)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Can't guess CHS values because there is no input file");

	// now find a valid value
	for (uint32_t totalsectors = filesize / sectorsize; ; totalsectors++)
	{
		for (uint32_t cursectors = 63; cursectors > 1; cursectors--)
		{
			if (totalsectors % cursectors == 0)
			{
				uint32_t totalheads = totalsectors / cursectors;
				for (uint32_t curheads = 16; curheads > 1; curheads--)
				{
					if (totalheads % curheads == 0)
					{
						cylinders = totalheads / curheads;
						heads = curheads;
						sectors = cursectors;
						return;
					}
				}
			}
		}
	}
}

void open_parent_chd(std::optional<std::string> const &path, chd_file &parent)
{
	if (path)
	{
		std::error_condition err = parent.open(*path);
		if (err)
			fail(err, "Error opening parent CHD file (%s): %s", *path, err.message());
	}
}

void open_input_chd(std::string const &input, std::optional<std::string> const &input_parent, chd_file &input_chd, chd_file &input_parent_chd, bool writeable = false)
{
	open_parent_chd(input_parent, input_parent_chd);
	std::error_condition err = input_chd.open(input, writeable, input_parent_chd.opened() ? &input_parent_chd : nullptr);
	if (err)
		fail(err, "Error opening CHD file (%s): %s", input, err.message());
}

std::pair<uint64_t, uint64_t> parse_input_start_end(
		range_options const &range,
		uint64_t logical_size,
		uint32_t hunkbytes,
		uint32_t framebytes)
{
	// process input start
	uint64_t input_start = 0;
	if (range.start_byte)
	{
		if (range.start_hunk)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Start offset cannot be specified in both bytes and hunks");
		if (range.start_frame)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Start offset cannot be specified in both bytes and frames");
		input_start = *range.start_byte;
	}
	else if (range.start_hunk)
	{
		if (range.start_frame)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Start offset cannot be specified in both hunks and frames");
		input_start = *range.start_hunk * hunkbytes;
	}
	else if (range.start_frame)
	{
		input_start = *range.start_frame * framebytes;
	}

	// process input length
	std::optional<uint64_t> input_length;
	if (range.length_bytes)
	{
		if (range.length_hunks)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Length cannot be specified in both bytes and hunks");
		if (range.length_frames)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Length cannot be specified in both bytes and frames");
		input_length = *range.length_bytes;
	}
	else if (range.length_hunks)
	{
		if (range.length_frames)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Length cannot be specified in both hunks and frames");
		input_length = *range.length_hunks * hunkbytes;
	}
	else if (range.length_frames)
	{
		input_length = *range.length_frames * framebytes;
	}

	// check that offsets are within input
	if (input_start >= logical_size)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Input start offset is beyond end of input");
	if (input_length && ((input_start + *input_length) > logical_size))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Input length is larger than available input from start offset");
	return std::make_pair(input_start, input_length ? (input_start + *input_length) : logical_size);
}

std::tuple<uint64_t, uint64_t, uint64_t> parse_input_start_end(
		range_options const &range,
		util::random_read &input_file,
		uint32_t hunkbytes,
		uint32_t framebytes)
{
	uint64_t input_size = 0;
	const std::error_condition err = input_file.length(input_size);
	if (err)
		fail(err, "Error getting size of input file: %s", err.message());
	const auto [input_start, input_end] = parse_input_start_end(range, input_size, hunkbytes, framebytes);
	return std::make_tuple(input_size, input_start, input_end);
}

bool range_has_bytes_or_hunks(range_options const &range)
{
	return range.start_byte || range.start_hunk || range.length_bytes || range.length_hunks;
}

void check_existing_output_file(bool force, std::string_view filename)
{
	if (!force)
	{
		util::core_file::ptr file;
		std::error_condition const filerr = util::core_file::open(filename, OPEN_FLAG_READ, file);
		if (!filerr)
		{
			file.reset();
			fail(CHDMAN_ERR_FILE_EXISTS, "Output file already exists (%s); set force to overwrite", filename);
		}
	}
}

void parse_output_chd_parameters(output_options const &out, chd_file &output_parent_chd)
{
	if (out.output.empty())
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output file name is required");
	open_parent_chd(out.output_parent, output_parent_chd);
	check_existing_output_file(out.force, out.output);
}

uint32_t parse_hunk_size(
		output_options const &out,
		const chd_file &output_parent,
		uint32_t required_granularity,
		uint32_t default_size)
{
	uint32_t hunk_size = default_size;
	if (out.hunk_size)
	{
		hunk_size = *out.hunk_size;
		if (output_parent.opened() && (output_parent.hunk_bytes() != hunk_size))
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Specified hunk size %u bytes does not match output parent CHD hunk size %u bytes", hunk_size, output_parent.hunk_bytes());
		if (hunk_size < HUNK_SIZE_MIN)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Invalid hunk size (minimum %u)", HUNK_SIZE_MIN);
		if (hunk_size > HUNK_SIZE_MAX)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Invalid hunk size (maximum %u)", HUNK_SIZE_MAX);
	}
	else if (output_parent.opened())
	{
		hunk_size = output_parent.hunk_bytes();
	}

	if (required_granularity == 0 || hunk_size % required_granularity)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Hunk size %u bytes is not a whole multiple of %u", hunk_size, required_granularity);
	return hunk_size;
}

void parse_compression(std::optional<std::string> const &spec, const codec_array &defaults, chd_codec_type (&compression)[4])
{
	std::copy(std::begin(defaults), std::end(defaults), compression);
	if (!spec)
		return;

	// special case: 'none'
	if (*spec == "none")
	{
		compression[0] = compression[1] = compression[2] = compression[3] = CHD_CODEC_NONE;
		return;
	}

	// iterate through compressors
	std::string const &str = *spec;
	int index = 0;
	for (std::string::size_type start = 0, end = str.find_first_of(','); index < 4; start = end + 1, end = str.find_first_of(',', end + 1))
	{
		std::string name(str, start, (end == std::string::npos) ? std::string::npos : end - start);
		if (name.length() != 4)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Invalid compressor '%s' specified", name);
		chd_codec_type type = CHD_MAKE_TAG(name[0], name[1], name[2], name[3]);
		if (!chd_codec_list::codec_exists(type))
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Invalid compressor '%s' specified", name);
		compression[index++] = type;
		if (end == std::string::npos)
			break;
	}

	for (; index < 4; ++index)
		compression[index] = CHD_CODEC_NONE;
}

util::core_file::ptr open_input_file(std::string const &path)
{
	util::core_file::ptr file;
	const std::error_condition err = util::core_file::open(path, OPEN_FLAG_READ, file);
	if (err)
		throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Unable to open input file (%s): %s", path, err.message()));
	return file;
}

void create_output_chd(
		chd_file_compressor &compressor,
		std::string_view path,
		uint64_t logical_size,
		uint32_t hunk_size,
		uint32_t unit_size,
		const chd_codec_type (&compression)[4],
		chd_file &parent)
{
	std::error_condition err;
	if (parent.opened())
		err = compressor.create(path, logical_size, hunk_size, compression, parent);
	else
		err = compressor.create(path, logical_size, hunk_size, unit_size, compression);
	if (err)
		throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Error creating CHD file (%s): %s", path, err.message()));
}


//**************************************************************************
//  COMPRESSORS
//**************************************************************************

// ======================> compressor_base
//
// Adds two things to chd_file_compressor:
//  - read errors raised on the I/O thread keep a descriptive message
//  - the destructor waits for asynchronous work before derived members die,
//    so abandoning compression (error/cancel) is safe

class compressor_base : public chd_file_compressor
{
public:
	std::string read_error_message() const
	{
		std::lock_guard<std::mutex> guard(m_error_lock);
		return m_error_message;
	}

protected:
	[[noreturn]] void read_failed(std::string const &message, std::error_condition err = std::errc::io_error)
	{
		{
			std::lock_guard<std::mutex> guard(m_error_lock);
			if (m_error_message.empty())
				m_error_message = message;
		}
		throw err; // caught by chd_file_compressor::async_read
	}

private:
	mutable std::mutex m_error_lock;
	std::string m_error_message;
};

template <typename T, typename... Params>
std::unique_ptr<T> make_compressor(session &s, Params &&...args)
{
	// work queues are sized from osd_num_processors when the compressor is
	// constructed, so scope the per-session limit to construction
	std::lock_guard<std::mutex> guard(s_numprocessors_lock);
	int const saved = osd_num_processors;
	osd_num_processors = (s.num_processors > 0) ? s.num_processors : 0;
	std::unique_ptr<T> result;
	try
	{
		result = std::make_unique<T>(std::forward<Params>(args)...);
	}
	catch (...)
	{
		osd_num_processors = saved;
		throw;
	}
	osd_num_processors = saved;
	return result;
}


// ======================> chd_zero_compressor

class chd_zero_compressor : public compressor_base
{
public:
	chd_zero_compressor(std::uint64_t offset = 0, std::uint64_t maxoffset = 0)
		: m_offset(offset)
		, m_maxoffset(maxoffset)
	{
	}

	~chd_zero_compressor() { compress_wait_idle(); }

	virtual std::uint32_t read_data(void *dest, std::uint64_t offset, std::uint32_t length) override
	{
		offset += m_offset;
		if (offset >= m_maxoffset)
			return 0;
		if (offset + length > m_maxoffset)
			length = m_maxoffset - offset;
		std::memset(dest, 0, length);
		return length;
	}

private:
	std::uint64_t   m_offset;
	std::uint64_t   m_maxoffset;
};


// ======================> chd_rawfile_compressor

class chd_rawfile_compressor : public compressor_base
{
public:
	chd_rawfile_compressor(util::random_read &file, std::uint64_t offset = 0, std::uint64_t maxoffset = std::numeric_limits<std::uint64_t>::max())
		: m_file(file)
		, m_offset(offset)
	{
		std::uint64_t filelen;
		if (!file.length(filelen))
			m_maxoffset = (std::min)(maxoffset, filelen);
		else
			m_maxoffset = maxoffset;
	}

	~chd_rawfile_compressor() { compress_wait_idle(); }

	virtual std::uint32_t read_data(void *dest, std::uint64_t offset, std::uint32_t length) override
	{
		offset += m_offset;
		if (offset >= m_maxoffset)
			return 0;
		if (offset + length > m_maxoffset)
			length = m_maxoffset - offset;
		std::error_condition err = m_file.seek(offset, SEEK_SET);
		std::size_t actual = 0;
		if (!err)
			std::tie(err, actual) = read(m_file, dest, length);
		// upstream ignores these errors (FIXME there); report them instead
		if (err || actual != length)
			read_failed(util::string_format("Error reading input file at offset %u", offset), err ? err : std::errc::io_error);
		return actual;
	}

private:
	util::random_read & m_file;
	std::uint64_t       m_offset;
	std::uint64_t       m_maxoffset;
};


// ======================> chd_chdfile_compressor

class chd_chdfile_compressor : public compressor_base
{
public:
	chd_chdfile_compressor(chd_file &file, uint64_t offset = 0, uint64_t maxoffset = ~0)
		: m_toc(nullptr)
		, m_file(file)
		, m_offset(offset)
		, m_maxoffset(std::min(maxoffset, file.logical_bytes()))
	{
	}

	~chd_chdfile_compressor() { compress_wait_idle(); }

	virtual uint32_t read_data(void *dest, uint64_t offset, uint32_t length) override
	{
		offset += m_offset;
		if (offset >= m_maxoffset)
			return 0;
		if (offset + length > m_maxoffset)
			length = m_maxoffset - offset;
		std::error_condition err = m_file.read_bytes(offset, dest, length);
		if (err)
			read_failed(util::string_format("Error reading input CHD: %s", err.message()), err);

		// if we have TOC - detect audio sectors and swap data
		if (m_toc)
		{
			assert(offset % cdrom_file::FRAME_SIZE == 0);
			assert(length % cdrom_file::FRAME_SIZE == 0);

			int startlba = offset / cdrom_file::FRAME_SIZE;
			int lenlba = length / cdrom_file::FRAME_SIZE;
			uint8_t *_dest = reinterpret_cast<uint8_t *>(dest);

			for (int chdlba = 0; chdlba < lenlba; chdlba++)
			{
				// find current frame's track number
				int tracknum = m_toc->numtrks;
				for (int track = 0; track < m_toc->numtrks; track++)
					if ((chdlba + startlba) < m_toc->tracks[track + 1].chdframeofs)
					{
						tracknum = track;
						break;
					}
				// is it audio ?
				if (m_toc->tracks[tracknum].trktype != cdrom_file::CD_TRACK_AUDIO)
					continue;
				// byteswap if yes
				int dataoffset = chdlba * cdrom_file::FRAME_SIZE;
				for (uint32_t swapindex = dataoffset; swapindex < (dataoffset + cdrom_file::MAX_SECTOR_DATA); swapindex += 2)
				{
					uint8_t temp = _dest[swapindex];
					_dest[swapindex] = _dest[swapindex + 1];
					_dest[swapindex + 1] = temp;
				}
			}
		}

		return length;
	}

	const cdrom_file::toc *m_toc;

private:
	chd_file &      m_file;
	uint64_t        m_offset;
	uint64_t        m_maxoffset;
};


// ======================> chd_cd_compressor

class chd_cd_compressor : public compressor_base
{
public:
	chd_cd_compressor(cdrom_file::toc &toc, cdrom_file::track_input_info &info)
		: m_file()
		, m_toc(toc)
		, m_info(info)
	{
	}

	~chd_cd_compressor() { compress_wait_idle(); }

	virtual uint32_t read_data(void *_dest, uint64_t offset, uint32_t length) override
	{
		// verify assumptions made below
		assert(offset % cdrom_file::FRAME_SIZE == 0);
		assert(length % cdrom_file::FRAME_SIZE == 0);

		// initialize destination to 0 so that unused areas are filled
		uint8_t *dest = reinterpret_cast<uint8_t *>(_dest);
		memset(dest, 0, length);

		// find out which track we're starting in
		uint64_t startoffs = 0;
		uint32_t length_remaining = length;
		for (int tracknum = 0; tracknum < m_toc.numtrks; tracknum++)
		{
			const cdrom_file::track_info &trackinfo = m_toc.tracks[tracknum];
			uint64_t endoffs = startoffs + (uint64_t)(trackinfo.frames + trackinfo.extraframes) * cdrom_file::FRAME_SIZE;

			if (offset >= startoffs && offset < endoffs)
			{
				// if we don't already have this file open, open it now
				if (!m_file || m_lastfile.compare(m_info.track[tracknum].fname) != 0)
				{
					m_file.reset();
					m_lastfile = m_info.track[tracknum].fname;
					std::error_condition const filerr = util::core_file::open(m_lastfile, OPEN_FLAG_READ, m_file);
					if (filerr)
						read_failed(util::string_format("Error opening input file (%s): %s", m_lastfile, filerr.message()), filerr);
				}

				// iterate over frames
				uint64_t bytesperframe = trackinfo.datasize + trackinfo.subsize;
				uint64_t src_track_start = m_info.track[tracknum].offset;
				uint64_t src_track_end = src_track_start + bytesperframe * (uint64_t)trackinfo.frames;
				uint64_t split_track_start = src_track_end - ((uint64_t)trackinfo.splitframes * bytesperframe);
				uint64_t pad_track_start = split_track_start - ((uint64_t)trackinfo.padframes * bytesperframe);

				// dont split when split-bin read not required
				if ((uint64_t)trackinfo.splitframes == 0L)
					split_track_start = UINT64_MAX;

				while (length_remaining != 0 && offset < endoffs)
				{
					// determine start of current frame
					uint64_t src_frame_start = src_track_start + ((offset - startoffs) / cdrom_file::FRAME_SIZE) * bytesperframe;

					// auto-advance next track for split-bin read
					if (src_frame_start >= split_track_start && src_frame_start < src_track_end && m_lastfile.compare(m_info.track[tracknum + 1].fname) != 0)
					{
						m_file.reset();
						m_lastfile = m_info.track[tracknum + 1].fname;
						std::error_condition const filerr = util::core_file::open(m_lastfile, OPEN_FLAG_READ, m_file);
						if (filerr)
							read_failed(util::string_format("Error opening input file (%s): %s", m_lastfile, filerr.message()), filerr);
					}

					if (src_frame_start < src_track_end)
					{
						// read it in, or pad if we're into the padframes
						if (src_frame_start >= pad_track_start && src_frame_start < split_track_start)
						{
							memset(dest, 0, bytesperframe);
						}
						else
						{
							std::error_condition err = m_file->seek(
									(src_frame_start >= split_track_start)
										? src_frame_start - split_track_start
										: src_frame_start,
									SEEK_SET);
							std::size_t count = 0;
							if (!err)
								std::tie(err, count) = read(*m_file, dest, bytesperframe);
							if (err || (count != bytesperframe))
								read_failed(util::string_format("Error reading input file (%s)", m_lastfile), err ? err : std::errc::io_error);
						}

						// swap if appropriate
						if (m_info.track[tracknum].swap)
							for (uint32_t swapindex = 0; swapindex < 2352; swapindex += 2)
							{
								uint8_t temp = dest[swapindex];
								dest[swapindex] = dest[swapindex + 1];
								dest[swapindex + 1] = temp;
							}
					}

					// advance
					offset += cdrom_file::FRAME_SIZE;
					dest += cdrom_file::FRAME_SIZE;
					length_remaining -= cdrom_file::FRAME_SIZE;
					if (length_remaining == 0)
						break;
				}
			}

			// next track starts after the previous one
			startoffs = endoffs;
		}
		return length - length_remaining;
	}

private:
	std::string                   m_lastfile;
	util::core_file::ptr          m_file;
	cdrom_file::toc &             m_toc;
	cdrom_file::track_input_info &m_info;
};


// ======================> avi_info

struct avi_info
{
	uint32_t fps_times_1million;
	uint32_t width;
	uint32_t height;
	bool interlaced;
	uint32_t channels;
	uint32_t rate;
	uint32_t max_samples_per_frame;
	uint32_t bytes_per_frame;
};


// ======================> chd_avi_compressor

class chd_avi_compressor : public compressor_base
{
public:
	chd_avi_compressor(avi_file &file, avi_info &info, uint32_t first_frame, uint32_t num_frames)
		: m_file(file)
		, m_info(info)
		, m_bitmap(info.width, info.height * (info.interlaced ? 2 : 1))
		, m_start_frame(first_frame)
		, m_frame_count(num_frames)
		, m_ldframedata(num_frames * VBI_PACKED_BYTES)
		, m_rawdata(info.bytes_per_frame)
	{
	}

	~chd_avi_compressor() { compress_wait_idle(); }

	const std::vector<uint8_t> &ldframedata() const { return m_ldframedata; }

	virtual uint32_t read_data(void *_dest, uint64_t offset, uint32_t length) override
	{
		uint8_t *dest = reinterpret_cast<uint8_t *>(_dest);
		uint8_t interlace_factor = m_info.interlaced ? 2 : 1;
		uint32_t length_remaining = length;

		// iterate over frames
		int32_t start_frame = offset / m_info.bytes_per_frame;
		int32_t end_frame = (offset + length - 1) / m_info.bytes_per_frame;
		for (int32_t framenum = start_frame; framenum <= end_frame; framenum++)
		{
			if (framenum < m_frame_count)
			{
				// determine effective frame number and first/last samples
				int32_t effframe = m_start_frame + framenum;
				uint32_t first_sample = (uint64_t(m_info.rate) * uint64_t(effframe) * uint64_t(1000000) + m_info.fps_times_1million - 1) / uint64_t(m_info.fps_times_1million);
				uint32_t samples = (uint64_t(m_info.rate) * uint64_t(effframe + 1) * uint64_t(1000000) + m_info.fps_times_1million - 1) / uint64_t(m_info.fps_times_1million) - first_sample;

				// loop over channels and read the samples
				int channels = unsigned(std::min<std::size_t>(m_info.channels, std::size(m_audio)));
				EQUIVALENT_ARRAY(m_audio, int16_t *) samplesptr;
				for (int chnum = 0; chnum < channels; chnum++)
				{
					// read the sound samples
					m_audio[chnum].resize(samples);
					samplesptr[chnum] = &m_audio[chnum][0];
					avi_file::error avierr = m_file.read_sound_samples(chnum, first_sample, samples, &m_audio[chnum][0]);
					if (avierr != avi_file::error::NONE)
						read_failed(util::string_format("Error reading audio samples %d-%d from channel %d: %s", first_sample, samples, chnum, avi_file::error_string(avierr)));
				}

				// read the video data
				avi_file::error avierr = m_file.read_video_frame(effframe / interlace_factor, m_bitmap);
				if (avierr != avi_file::error::NONE)
					read_failed(util::string_format("Error reading AVI frame %d: %s", effframe / interlace_factor, avi_file::error_string(avierr)));
				bitmap_yuy16 subbitmap(&m_bitmap.pix(effframe % interlace_factor), m_bitmap.width(), m_bitmap.height() / interlace_factor, m_bitmap.rowpixels() * interlace_factor);

				// update metadata for this frame
				if (m_info.height == 524/2 || m_info.height == 624/2)
				{
					vbi_metadata vbi;
					vbi_parse_all(&subbitmap.pix(0), subbitmap.rowpixels(), subbitmap.width(), 8, &vbi);
					vbi_metadata_pack(&m_ldframedata[framenum * VBI_PACKED_BYTES], framenum, &vbi);
				}

				// assemble the data into final form
				avhuff_error averr = avhuff_encoder::assemble_data(m_rawdata, subbitmap, channels, samples, samplesptr);
				if (averr != AVHERR_NONE)
					read_failed(util::string_format("Error assembling data for frame %d", framenum));
				if (m_rawdata.size() < m_info.bytes_per_frame)
				{
					int old_size = m_rawdata.size();
					m_rawdata.resize(m_info.bytes_per_frame);
					memset(&m_rawdata[old_size], 0, m_info.bytes_per_frame - old_size);
				}

				// copy to the destination
				uint64_t start_offset = uint64_t(framenum) * uint64_t(m_info.bytes_per_frame);
				uint64_t end_offset = start_offset + m_info.bytes_per_frame;
				uint32_t bytes_to_copy = (std::min<uint64_t>)(length_remaining, end_offset - offset);
				memcpy(dest, &m_rawdata[offset - start_offset], bytes_to_copy);

				// advance
				offset += bytes_to_copy;
				dest += bytes_to_copy;
				length_remaining -= bytes_to_copy;
			}
		}

		return length;
	}

private:
	avi_file &                  m_file;
	avi_info &                  m_info;
	bitmap_yuy16                m_bitmap;
	uint32_t                    m_start_frame;
	uint32_t                    m_frame_count;
	std::vector<int16_t>        m_audio[8];
	std::vector<uint8_t>        m_ldframedata;
	std::vector<uint8_t>        m_rawdata;
};


//**************************************************************************
//  COMPRESSION LOOP AND OUTPUT CLEANUP
//**************************************************************************

void compress_common(session &s, compressor_base &chd)
{
	chd.compress_begin();

	double complete = 0.0, ratio = 0.0;
	std::error_condition err;
	while ((err = chd.compress_continue(complete, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
	{
		if (err == chd_file::error::WALKING_PARENT)
			s.progress(CHDMAN_PHASE_WALKING_PARENT, complete);
		else
			s.progress(CHDMAN_PHASE_COMPRESSING, complete, ratio);
	}

	if (err)
	{
		std::string detail = chd.read_error_message();
		if (detail.empty())
			fail(err, "Error during compression: %s", err.message());
		fail(err, "Error during compression: %s", detail);
	}

	s.progress(CHDMAN_PHASE_COMPRESSING, 1.0, ratio, true);
	s.info(util::string_format("Compression complete ... final ratio = %.1f%%", 100.0 * ratio));
}

// Removes the output file unless dismiss() was called; replaces chdman's
// catch (...) { osd_file::remove(...); throw; } blocks.
class output_guard
{
public:
	explicit output_guard(std::string path = std::string()) { if (!path.empty()) m_paths.push_back(std::move(path)); }
	~output_guard()
	{
		if (m_armed)
			for (auto const &path : m_paths)
				osd_file::remove(path);
	}
	void add(std::string path) { m_paths.push_back(std::move(path)); }
	void dismiss() { m_armed = false; }

private:
	std::vector<std::string> m_paths;
	bool m_armed = true;
};


//**************************************************************************
//  CD TOC OUTPUT
//**************************************************************************

// TOC text: written to the file line by line as chdman does, or, with no
// file, collected (with the same platform line endings) for an extract_sink
class toc_writer
{
public:
	explicit toc_writer(util::core_file *file) : m_file(file) { }

	template <typename Format, typename... Params> void printf(Format &&fmt, Params &&...args)
	{
		std::string const s = util::string_format(std::forward<Format>(fmt), std::forward<Params>(args)...);
		if (m_file)
		{
			m_file->puts(s);
			return;
		}
		for (char const ch : s)
		{
			if (ch != '\n')
				m_text += ch;
			else if (CRLF == 1)
				m_text += '\r';
			else if (CRLF == 2)
				m_text += '\n';
			else
				m_text += "\r\n";
		}
	}

	std::string const &text() const noexcept { return m_text; }

private:
	util::core_file *m_file;
	std::string m_text;
};

void output_track_metadata(int mode, toc_writer &file, int tracknum, const cdrom_file::track_info &info, const std::string &filename, uint32_t frameoffs, uint64_t outputoffs)
{
	if (mode == MODE_GDI)
	{
		const int tracktype = info.trktype == cdrom_file::CD_TRACK_AUDIO ? 0 : 4;
		const bool needquote = filename.find(' ') != std::string::npos;
		const char *const quotestr = needquote ? "\"" : "";
		file.printf("%d %d %d %d %s%s%s %d\n", tracknum+1, frameoffs, tracktype, info.datasize, quotestr, filename, quotestr, outputoffs);
	}
	else if (mode == MODE_CUEBIN)
	{
		// specify a new file when writing to the beginning of a file
		if (outputoffs == 0)
			file.printf("FILE \"%s\" BINARY\n", filename);

		// determine submode
		std::string tempstr;
		switch (info.trktype)
		{
			case cdrom_file::CD_TRACK_MODE1:
			case cdrom_file::CD_TRACK_MODE1_RAW:
				tempstr = util::string_format("MODE1/%04d", info.datasize);
				break;

			case cdrom_file::CD_TRACK_MODE2:
			case cdrom_file::CD_TRACK_MODE2_FORM1:
			case cdrom_file::CD_TRACK_MODE2_FORM2:
			case cdrom_file::CD_TRACK_MODE2_FORM_MIX:
			case cdrom_file::CD_TRACK_MODE2_RAW:
				tempstr = util::string_format("MODE2/%04d", info.datasize);
				break;

			case cdrom_file::CD_TRACK_AUDIO:
				tempstr.assign("AUDIO");
				break;
		}

		// output TRACK entry
		file.printf("  TRACK %02d %s\n", tracknum + 1, tempstr);

		// output PREGAP tag if pregap sectors are not in the file
		if ((info.pregap > 0) && (info.pgdatasize == 0))
		{
			file.printf("    PREGAP %s\n", msf_string_from_frames(info.pregap));
			file.printf("    INDEX 01 %s\n", msf_string_from_frames(frameoffs));
		}
		else if ((info.pregap > 0) && (info.pgdatasize > 0))
		{
			file.printf("    INDEX 00 %s\n", msf_string_from_frames(frameoffs));
			file.printf("    INDEX 01 %s\n", msf_string_from_frames(frameoffs+info.pregap));
		}

		// if no pregap at all, output index 01 only
		if (info.pregap == 0)
		{
			file.printf("    INDEX 01 %s\n", msf_string_from_frames(frameoffs));
		}

		// output POSTGAP
		if (info.postgap > 0)
			file.printf("    POSTGAP %s\n", msf_string_from_frames(info.postgap));
	}
	// non-CUE mode
	else if (mode == MODE_NORMAL)
	{
		file.printf("// Track %d\n", tracknum + 1);

		// write out the track type
		std::string modesubmode;
		if (info.subtype != cdrom_file::CD_SUB_NONE)
			modesubmode = util::string_format("%s %s", cdrom_file::get_type_string(info.trktype), cdrom_file::get_subtype_string(info.subtype));
		else
			modesubmode = util::string_format("%s", cdrom_file::get_type_string(info.trktype));
		file.printf("TRACK %s\n", modesubmode);

		// write out the attributes
		file.printf("NO COPY\n");
		if (info.trktype == cdrom_file::CD_TRACK_AUDIO)
		{
			file.printf("NO PRE_EMPHASIS\n");
			file.printf("TWO_CHANNEL_AUDIO\n");
		}

		// output pregap
		if (info.pregap > 0)
			file.printf("ZERO %s %s\n", modesubmode, msf_string_from_frames(info.pregap));

		if (outputoffs == 0)
			file.printf("DATAFILE \"%s\" %s // length in bytes: %d\n", filename, msf_string_from_frames(info.frames), info.frames * (info.datasize + info.subsize));
		else
			file.printf("DATAFILE \"%s\" #%d %s // length in bytes: %d\n", filename, uint32_t(outputoffs), msf_string_from_frames(info.frames), info.frames * (info.datasize + info.subsize));

		// tracks with pregaps get a START marker too
		if (info.pregap > 0)
			file.printf("START %s\n", msf_string_from_frames(info.pregap));

		file.printf("\n\n");
	}
}


//**************************************************************************
//  MISC
//**************************************************************************

const codec_array &get_compression_defaults(chd_file &input_chd)
{
	std::error_condition err = input_chd.check_is_hd();
	if (err == chd_file::error::METADATA_NOT_FOUND)
		err = input_chd.check_is_dvd();
	if (!err)
		return s_default_hd_compression;
	if (err != chd_file::error::METADATA_NOT_FOUND)
		fail(err, "Error reading CHD metadata: %s", err.message());

	err = input_chd.check_is_av();
	if (!err)
		return s_default_ld_compression;
	if (err != chd_file::error::METADATA_NOT_FOUND)
		fail(err, "Error reading CHD metadata: %s", err.message());

	err = input_chd.check_is_cd();
	if (err == chd_file::error::METADATA_NOT_FOUND)
		err = input_chd.check_is_gd();
	if (!err)
		return s_default_cd_compression;
	if (err != chd_file::error::METADATA_NOT_FOUND)
		fail(err, "Error reading CHD metadata: %s", err.message());

	return s_default_raw_compression;
}

std::string media_type(chd_file &chd)
{
	if (!chd.check_is_hd())
		return "hd";
	if (!chd.check_is_dvd())
		return "dvd";
	if (!chd.check_is_av())
		return "av";
	if (!chd.check_is_gd())
		return "gd";
	if (!chd.check_is_cd())
		return "cd";
	return "raw";
}

void print_output_header(session &s, output_options const &out, chd_file const &output_parent)
{
	s.info(util::string_format("Output CHD:   %s", out.output));
	if (output_parent.opened())
		s.info(util::string_format("Parent CHD:   %s", *out.output_parent));
}

const std::vector<hd_template> s_hd_templates =
{
	{ "Conner",     "CFA170A",               332, 16, 63, 512 }, //   163 MB, IDE (ATA)
	{ "Rodime",     "R0201",                 321,  2, 16, 512 }, //     5 MB, ST-506/MFM
	{ "Rodime",     "R0202",                 321,  4, 16, 512 }, //    10 MB, ST-506/MFM
	{ "Rodime",     "R0203",                 321,  6, 16, 512 }, //    15 MB, ST-506/MFM
	{ "Rodime",     "R0204",                 321,  8, 16, 512 }, //    20 MB, ST-506/MFM
	{ "Seagate",    "ST-213",                615,  2, 17, 512 }, //    10 MB, ST-506/MFM
	{ "Seagate",    "ST-225",                615,  4, 17, 512 }, //    20 MB, ST-506/MFM
	{ "Seagate",    "ST-251",                820,  6, 17, 512 }, //    40 MB, ST-506/MFM
	{ "Seagate",    "ST-3600N",             1877,  7, 76, 512 }, //   525 MB, SCSI
	{ "Maxtor",     "LXT-213S",             1314,  7, 53, 512 }, //   200 MB, SCSI
	{ "Maxtor",     "LXT-340S",             1574,  7, 70, 512 }, //   340 MB, SCSI
	{ "Maxtor",     "MXT-540SL",            2466,  7, 87, 512 }, //   540 MB, SCSI
	{ "Micropolis", "1528",                 2094, 15, 83, 512 }, //  1342 MB, SCSI-2
	{ "Quantum",    "Fireball CR 4.3 AT",  14848,  9, 63, 512 }, //  4110 MB (4.3 GB), Ultra ATA/66 (ATA-5)
	{ "Quantum",    "Fireball CR 6.4 AT",  13328, 15, 63, 512 }, //  6149 MB (6.4 GB), Ultra ATA/66 (ATA-5)
	{ "Quantum",    "Fireball CR 8.4 AT",  16383, 16, 63, 512 }, //  8063 MB (8.4 GB), Ultra ATA/66 (ATA-5)
	{ "Quantum",    "Fireball CR 13.0 AT", 25228, 16, 63, 512 }, // 12416 MB (13.0 GB), Ultra ATA/66 (ATA-5)
};

} // anonymous namespace


//**************************************************************************
//  SESSION
//**************************************************************************

void session::progress(chdman_phase phase, double fraction, double ratio, bool force)
{
	check_cancel();
	if (!progress_cb)
		return;

	// throttle to 10 updates per second (chdman uses 2) unless forced
	int64_t const now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (!force && m_last_progress_ns >= 0 && (now - m_last_progress_ns) < 100'000'000)
		return;
	m_last_progress_ns = now;

	if (progress_cb(phase, fraction, ratio))
		cancel_requested = true;
	check_cancel();
}

void session::check_cancel()
{
	if (cancel_requested.load(std::memory_order_relaxed))
		throw error(CHDMAN_ERR_CANCELLED, "Operation cancelled");
}

void session::info(std::string const &message) const
{
	if (log_cb)
		log_cb(CHDMAN_LOG_INFO, message);
}

void session::warning(std::string const &message) const
{
	if (log_cb)
		log_cb(CHDMAN_LOG_WARNING, message);
}

void session::osd_message(chdman_log_level level, std::string message)
{
	// MAME terminates lines with '\n'; strip it, and keep only the last line
	// of a multi-line error for the error message
	while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back())))
		message.pop_back();
	if (message.empty())
		return;
	if (log_cb)
		log_cb(level, message);
	if (level == CHDMAN_LOG_ERROR)
	{
		std::string::size_type const nl = message.find_last_of('\n');
		std::string line = (nl == std::string::npos) ? message : message.substr(nl + 1);
		if (line.compare(0, 7, "ERROR: ") == 0)
			line.erase(0, 7);
		m_last_osd_error = std::move(line);
	}
}


//**************************************************************************
//  OSD OUTPUT ROUTING
//**************************************************************************

namespace {

thread_local session *t_current_session = nullptr;

// The osd_output stack in osdcore.cpp is a global array without locking, so
// exactly one delegate is pushed, once, for the whole process. It forwards
// each message to whichever session is active on the printing thread.
class osd_router : public osd_output
{
public:
	osd_router() { osd_output::push(this); }
	~osd_router() { osd_output::pop(this); }

	virtual void output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args) override
	{
		session *const s = t_current_session;
		if (!s)
			return;
		try
		{
			switch (channel)
			{
			case OSD_OUTPUT_CHANNEL_ERROR:   s->osd_message(CHDMAN_LOG_ERROR, util::string_format(args)); break;
			case OSD_OUTPUT_CHANNEL_WARNING: s->osd_message(CHDMAN_LOG_WARNING, util::string_format(args)); break;
			case OSD_OUTPUT_CHANNEL_INFO:    s->osd_message(CHDMAN_LOG_INFO, util::string_format(args)); break;
			default:                         break; // verbose/debug/log are dropped, as in chdman
			}
		}
		catch (...)
		{
			// diagnostics must never turn into failures
		}
	}
};

} // anonymous namespace

session_scope::session_scope(session &s) : m_previous(t_current_session)
{
	static osd_router router;
	t_current_session = &s;
}

session_scope::~session_scope()
{
	t_current_session = m_previous;
}


//**************************************************************************
//  INFO
//**************************************************************************

info_result info(session &s, info_options const &opts)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.input, opts.input_parent, input_chd, input_parent_chd);

	info_result result;
	result.version = input_chd.version();
	if (input_chd.version() < 3)
		fail(CHDMAN_ERR_UNSUPPORTED, "Unsupported version (%d); use an older chdman to upgrade to version 3 or later", input_chd.version());

	chd_codec_type compression[4] = { input_chd.compression(0), input_chd.compression(1), input_chd.compression(2), input_chd.compression(3) };
	uint64_t filesize = 0;
	input_chd.file().length(filesize);
	result.logical_bytes = input_chd.logical_bytes();
	result.hunk_bytes = input_chd.hunk_bytes();
	result.hunk_count = input_chd.hunk_count();
	result.unit_bytes = input_chd.unit_bytes();
	result.unit_count = input_chd.unit_count();
	result.file_bytes = filesize;
	std::copy(std::begin(compression), std::end(compression), result.codecs);
	result.compression = compression_string(compression);
	result.media_type = media_type(input_chd);

	// SHA-1s; chdman only shows them when the overall SHA-1 is set
	util::sha1_t overall = input_chd.sha1();
	if (overall != util::sha1_t::null)
	{
		result.sha1 = overall.as_string();
		if (input_chd.version() >= 4)
			result.raw_sha1 = sha1_string(input_chd.raw_sha1());
	}
	result.parent_sha1 = sha1_string(input_chd.parent_sha1());

	// metadata, with per-tag indexes
	for (int index = 0; ; index++)
	{
		metadata_item item;
		chd_metadata_tag metatag;
		uint8_t metaflags;
		std::error_condition err = input_chd.read_metadata(CHDMETATAG_WILDCARD, index, item.data, metatag, metaflags);
		if (err)
			break;

		item.tag = metatag;
		item.flags = metaflags;
		item.index = 0;
		for (auto it = result.metadata.rbegin(); it != result.metadata.rend(); ++it)
			if (it->tag == metatag)
			{
				item.index = it->index + 1;
				break;
			}
		result.metadata.push_back(std::move(item));
	}

	// compression stats (chdman info --verbose)
	if (opts.hunk_stats)
	{
		uint32_t compression_types[10] = { 0 };
		for (uint32_t hunknum = 0; hunknum < input_chd.hunk_count(); hunknum++)
		{
			s.progress(CHDMAN_PHASE_ANALYZING, double(hunknum) / double(input_chd.hunk_count()));

			chd_codec_type codec;
			uint32_t compbytes;
			std::error_condition err = input_chd.hunk_info(hunknum, codec, compbytes);
			if (err)
				fail(err, "Error getting info on hunk %d: %s", hunknum, err.message());

			if (codec > CHD_CODEC_MINI)
				for (int comptype = 0; comptype < 4; comptype++)
					if (codec == input_chd.compression(comptype))
					{
						codec = CHD_CODEC_MINI + 1 + comptype;
						break;
					}
			if (codec >= std::size(compression_types))
				codec = std::size(compression_types) - 1;

			compression_types[codec]++;
		}

		for (int comptype = 0; comptype < std::size(compression_types); comptype++)
			if (compression_types[comptype] != 0)
			{
				const char *name = "Unknown";
				switch (comptype)
				{
					case CHD_CODEC_NONE:        name = "Uncompressed";                  break;
					case CHD_CODEC_SELF:        name = "Copy from self";                break;
					case CHD_CODEC_PARENT:      name = "Copy from parent";              break;
					case CHD_CODEC_MINI:        name = "Legacy 8-byte mini";            break;
					default:
						int index = comptype - 1 - CHD_CODEC_MINI;
						if (index < 4)
							name = chd_codec_list::codec_name(input_chd.compression(index));
						break;
				}
				result.hunk_stats.push_back(hunk_stat{ name, compression_types[comptype] });
			}
		s.progress(CHDMAN_PHASE_ANALYZING, 1.0, 0.0, true);
	}

	return result;
}


//**************************************************************************
//  VERIFY
//**************************************************************************

verify_result verify(session &s, verify_options const &opts)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_parent_chd(opts.input_parent, input_parent_chd);

	{
		const uint32_t openflags = opts.fix ? (OPEN_FLAG_READ | OPEN_FLAG_WRITE) : OPEN_FLAG_READ;
		util::core_file::ptr file;
		std::error_condition err = util::core_file::open(opts.input, openflags, file);
		if (!err)
			err = input_chd.open(std::move(file), false, input_parent_chd.opened() ? &input_parent_chd : nullptr);
		if (err)
			fail(err, "Error opening CHD file (%s): %s", opts.input, err.message());
	}

	verify_result result;

	// only makes sense for compressed CHDs with valid SHA-1's
	if (!input_chd.compressed())
	{
		result.message = "No verification to be done; CHD is uncompressed";
		return result;
	}
	util::sha1_t raw_sha1 = (input_chd.version() <= 3) ? input_chd.sha1() : input_chd.raw_sha1();
	if (raw_sha1 == util::sha1_t::null)
	{
		result.message = "No verification to be done; CHD has no checksum";
		return result;
	}
	result.header_raw_sha1 = raw_sha1.as_string();

	// read all the data and build up an SHA-1
	std::vector<uint8_t> buffer((TEMP_BUFFER_SIZE / input_chd.hunk_bytes()) * input_chd.hunk_bytes());
	util::sha1_creator rawsha1;
	for (uint64_t offset = 0; offset < input_chd.logical_bytes(); )
	{
		s.progress(CHDMAN_PHASE_VERIFYING, double(offset) / double(input_chd.logical_bytes()));

		uint32_t bytes_to_read = (std::min<uint64_t>)(buffer.size(), input_chd.logical_bytes() - offset);
		std::error_condition err = input_chd.read_bytes(offset, &buffer[0], bytes_to_read);
		if (err)
			fail(err, "Error reading CHD file (%s): %s", opts.input, err.message());

		rawsha1.append(&buffer[0], bytes_to_read);
		offset += bytes_to_read;
	}
	s.progress(CHDMAN_PHASE_VERIFYING, 1.0, 0.0, true);
	util::sha1_t computed_sha1 = rawsha1.finish();
	result.computed_raw_sha1 = computed_sha1.as_string();

	if (raw_sha1 != computed_sha1)
	{
		result.outcome = CHDMAN_VERIFY_RAW_MISMATCH;

		// fixing the raw SHA-1 also fixes the overall one
		if (opts.fix)
		{
			std::error_condition err = input_chd.set_raw_sha1(computed_sha1);
			if (err)
				fail(err, "Error updating SHA1: %s", err.message());
			result.outcome = CHDMAN_VERIFY_FIXED;
		}
	}
	else
	{
		result.outcome = CHDMAN_VERIFY_OK;

		// now include the metadata for >= v4
		if (input_chd.version() >= 4)
		{
			util::sha1_t computed_overall_sha1 = input_chd.compute_overall_sha1(computed_sha1);
			result.header_sha1 = sha1_string(input_chd.sha1());
			result.computed_sha1 = computed_overall_sha1.as_string();
			if (input_chd.sha1() != computed_overall_sha1)
			{
				result.outcome = CHDMAN_VERIFY_OVERALL_MISMATCH;
				if (opts.fix)
				{
					std::error_condition err = input_chd.set_raw_sha1(computed_sha1);
					if (err)
						fail(err, "Error updating SHA1: %s", err.message());
					result.outcome = CHDMAN_VERIFY_FIXED;
				}
			}
		}
	}
	return result;
}


//**************************************************************************
//  CREATE
//**************************************************************************

void create_raw(session &s, create_raw_options const &opts)
{
	util::core_file::ptr input_file = open_input_file(opts.input);

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	// process unit size
	uint32_t unit_size = output_parent.opened() ? output_parent.unit_bytes() : 0;
	if (opts.unit_size)
	{
		unit_size = *opts.unit_size;
		if (output_parent.opened() && (output_parent.unit_bytes() != unit_size))
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Specified unit size %u bytes does not match output parent CHD unit size %u bytes", unit_size, output_parent.unit_bytes());
	}
	else if (!output_parent.opened())
	{
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Unit size must be specified if no output parent CHD is supplied");
	}
	if (unit_size == 0)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Unit size must be non-zero");

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, unit_size, std::max((4096 / unit_size) * unit_size, unit_size));

	const auto [input_size, input_start, input_end] = parse_input_start_end(opts.range, *input_file, hunk_size, hunk_size);
	if ((input_end - input_start) % unit_size)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Data size %s is not divisible by unit size %d", big_int_string(input_end - input_start), unit_size);

	chd_codec_type compression[4];
	parse_compression(opts.out.compression, s_default_raw_compression, compression);

	print_output_header(s, opts.out, output_parent);
	s.info(util::string_format("Input file:   %s", opts.input));
	if (input_start != 0 || input_end != input_size)
	{
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
		s.info(util::string_format("Input length: %s", big_int_string(input_end - input_start)));
	}
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	s.info(util::string_format("Hunk size:    %s", big_int_string(hunk_size)));
	s.info(util::string_format("Logical size: %s", big_int_string(input_end - input_start)));

	output_guard guard(opts.out.output);
	{
		auto chd = make_compressor<chd_rawfile_compressor>(s, *input_file, input_start, input_end);
		create_output_chd(*chd, opts.out.output, input_end - input_start, hunk_size, unit_size, compression, output_parent);

		// if we have a parent, copy forward all the metadata
		if (output_parent.opened())
			chd->clone_all_metadata(output_parent);

		compress_common(s, *chd);
	}
	guard.dismiss();
}

void create_hd(session &s, create_hd_options const &opts)
{
	util::core_file::ptr input_file;
	if (opts.input)
		input_file = open_input_file(*opts.input);

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	// process template
	uint32_t cylinders = 0;
	uint32_t heads = 0;
	uint32_t sectors = 0;
	uint32_t sector_size = output_parent.opened() ? output_parent.unit_bytes() : IDE_SECTOR_SIZE;
	uint32_t template_id = 0;
	if (opts.template_id)
	{
		if (output_parent.opened())
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Template cannot be used when a parent CHD is supplied");
		template_id = *opts.template_id;
		if (template_id >= s_hd_templates.size())
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Template '%u' is invalid", template_id);

		cylinders = s_hd_templates[template_id].cylinders;
		heads = s_hd_templates[template_id].heads;
		sectors = s_hd_templates[template_id].sectors;
		sector_size = s_hd_templates[template_id].sector_size;
	}

	// process CHS
	if (opts.cylinders || opts.heads || opts.sectors)
	{
		if (opts.template_id)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "CHS geometry cannot be specified separately when a template is specified");
		if (output_parent.opened())
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "CHS geometry cannot be specified when a parent CHD is supplied");
		if (!opts.cylinders || !opts.heads || !opts.sectors)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Invalid CHS geometry; cylinders, heads and sectors must all be specified");
		cylinders = *opts.cylinders;
		heads = *opts.heads;
		sectors = *opts.sectors;
	}

	// process sector size
	if (opts.sector_size)
	{
		if (opts.template_id)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Sector size cannot be specified separately when a template is specified");
		sector_size = *opts.sector_size;
	}
	if (sector_size == 0)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Sector size must be non-zero");
	if (output_parent.opened() && (output_parent.unit_bytes() != sector_size))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Sector size %u bytes does not match output parent CHD sector size %u bytes", sector_size, output_parent.unit_bytes());

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, sector_size, std::max((4096 / sector_size) * sector_size, sector_size));

	// process input start/end
	uint64_t filesize = 0;
	uint64_t input_size = 0;
	uint64_t input_start = 0;
	uint64_t input_end = 0;
	if (input_file)
	{
		std::tie(input_size, input_start, input_end) = parse_input_start_end(opts.range, *input_file, hunk_size, hunk_size);
		filesize = input_end - input_start;
		if (opts.size)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Size cannot be specified when an input file is supplied");
	}
	else
	{
		if (opts.size)
			filesize = *opts.size;
		if (range_has_bytes_or_hunks(opts.range))
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Input start/length cannot be specified when no input file is supplied");
	}

	chd_codec_type compression[4];
	parse_compression(opts.out.compression, input_file ? s_default_hd_compression : s_no_compression, compression);
	if (!input_file && (compression[0] != CHD_CODEC_NONE))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Blank hard disk images must be uncompressed");

	// process ident
	std::vector<uint8_t> identdata;
	if (output_parent.opened())
		output_parent.read_metadata(HARD_DISK_IDENT_METADATA_TAG, 0, identdata);
	if (opts.ident)
	{
		std::error_condition const filerr = util::core_file::load(*opts.ident, identdata);
		if (filerr)
			throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Error reading ident file (%s): %s", *opts.ident, filerr.message()));

		// must be at least 14 bytes; extract CHS data from there
		if (identdata.size() < 14)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Ident file '%s' is invalid (too short)", *opts.ident);
		cylinders = get_u16le(&identdata[2]);
		heads = get_u16le(&identdata[6]);
		sectors = get_u16le(&identdata[12]);

		// ignore CHS for > 8GB drives
		if (cylinders * heads * sectors >= 16'514'064)
			cylinders = 0;
	}

	// extract geometry from the parent if we have one
	if (output_parent.opened() && cylinders == 0)
	{
		std::string metadata;
		if (output_parent.read_metadata(HARD_DISK_METADATA_TAG, 0, metadata))
			fail(CHDMAN_ERR_NOT_FOUND, "Unable to find hard disk metadata in parent CHD");
		if (sscanf(metadata.c_str(), HARD_DISK_METADATA_FORMAT, &cylinders, &heads, &sectors, &sector_size) != 4)
			fail(CHDMAN_ERR_CHD, "Error parsing hard disk metadata in parent CHD");
	}

	// validate the size
	if (filesize % sector_size)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Data size %s is not divisible by sector size %u", big_int_string(filesize), sector_size);

	// if no CHS values, try to guess them
	if (cylinders == 0)
	{
		if (!input_file && filesize == 0)
			fail(CHDMAN_ERR_INVALID_ARGUMENT, "Length or CHS geometry must be specified when creating a blank hard disk image");
		guess_chs(opts.input ? &*opts.input : nullptr, filesize, sector_size, cylinders, heads, sectors, sector_size);
	}
	uint32_t totalsectors = cylinders * heads * sectors;

	print_output_header(s, opts.out, output_parent);
	if (input_file)
	{
		s.info(util::string_format("Input file:   %s", *opts.input));
		if (input_start != 0 || input_end != input_size)
		{
			s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
			s.info(util::string_format("Input length: %s", big_int_string(filesize)));
		}
	}
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	if (opts.template_id)
		s.info(util::string_format("Template:     %s %s", s_hd_templates[template_id].manufacturer, s_hd_templates[template_id].model));
	s.info(util::string_format("Cylinders:    %u", cylinders));
	s.info(util::string_format("Heads:        %u", heads));
	s.info(util::string_format("Sectors:      %u", sectors));
	s.info(util::string_format("Bytes/sector: %u", sector_size));
	s.info(util::string_format("Sectors/hunk: %u", hunk_size / sector_size));
	s.info(util::string_format("Logical size: %s", big_int_string(uint64_t(totalsectors) * uint64_t(sector_size))));

	output_guard guard(opts.out.output);
	{
		std::unique_ptr<compressor_base> chd;
		if (input_file)
			chd = make_compressor<chd_rawfile_compressor>(s, *input_file, input_start, input_end);
		else
			chd = make_compressor<chd_zero_compressor>(s, input_start, input_end);
		create_output_chd(*chd, opts.out.output, uint64_t(totalsectors) * sector_size, hunk_size, sector_size, compression, output_parent);

		// add the standard hard disk metadata
		std::string metadata = util::string_format(HARD_DISK_METADATA_FORMAT, cylinders, heads, sectors, sector_size);
		std::error_condition err = chd->write_metadata(HARD_DISK_METADATA_TAG, 0, metadata);
		if (err)
			fail(err, "Error adding hard disk metadata: %s", err.message());

		// write the ident if present
		if (!identdata.empty())
		{
			err = chd->write_metadata(HARD_DISK_IDENT_METADATA_TAG, 0, identdata);
			if (err)
				fail(err, "Error adding hard disk metadata: %s", err.message());
		}

		if (input_file)
			compress_common(s, *chd);
	}
	guard.dismiss();
}

void create_cd(session &s, create_cd_options const &opts)
{
	cdrom_file::track_input_info track_info;
	cdrom_file::toc toc = { 0 };
	{
		std::error_condition err = cdrom_file::parse_toc(opts.input, toc, track_info);
		if (err)
			fail(err, "Error parsing input file (%s: %s)", opts.input, err.message());
	}

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, cdrom_file::FRAME_SIZE, cdrom_file::FRAMES_PER_HUNK * cdrom_file::FRAME_SIZE);
	if (output_parent.opened() && (output_parent.unit_bytes() != cdrom_file::FRAME_SIZE))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output parent CHD sector size %u bytes does not match CD-ROM frame size %u bytes", output_parent.unit_bytes(), cdrom_file::FRAME_SIZE);

	chd_codec_type compression[4];
	parse_compression(opts.out.compression, s_default_cd_compression, compression);

	// pad each track to a 4-frame boundary. cdrom.cpp will deal with this on the read side
	uint32_t origtotalsectors = 0;
	uint32_t totalsectors = 0;
	for (int tracknum = 0; tracknum < toc.numtrks; tracknum++)
	{
		cdrom_file::track_info &trackinfo = toc.tracks[tracknum];
		int padded = (trackinfo.frames + cdrom_file::TRACK_PADDING - 1) / cdrom_file::TRACK_PADDING;
		trackinfo.extraframes = padded * cdrom_file::TRACK_PADDING - trackinfo.frames;
		origtotalsectors += trackinfo.frames;
		totalsectors += trackinfo.frames + trackinfo.extraframes;
	}

	print_output_header(s, opts.out, output_parent);
	s.info(util::string_format("Input file:   %s", opts.input));
	s.info(util::string_format("Input tracks: %d", toc.numtrks));
	s.info(util::string_format("Input length: %s", msf_string_from_frames(origtotalsectors)));
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	s.info(util::string_format("Logical size: %s", big_int_string(uint64_t(totalsectors) * cdrom_file::FRAME_SIZE)));

	output_guard guard(opts.out.output);
	{
		auto chd = make_compressor<chd_cd_compressor>(s, toc, track_info);
		create_output_chd(*chd, opts.out.output, uint64_t(totalsectors) * cdrom_file::FRAME_SIZE, hunk_size, cdrom_file::FRAME_SIZE, compression, output_parent);

		// add the standard CD metadata; we do this even if we have a parent because it might be different
		const std::error_condition err = cdrom_file::write_metadata(chd.get(), toc);
		if (err)
			fail(err, "Error adding CD metadata: %s", err.message());

		compress_common(s, *chd);
	}
	guard.dismiss();
}

void create_dvd(session &s, create_dvd_options const &opts)
{
	util::core_file::ptr input_file = open_input_file(opts.input);

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, 2048, 2 * 2048);
	if (output_parent.opened() && (output_parent.unit_bytes() != 2048))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output parent CHD sector size %u bytes does not match DVD-ROM sector size 2048 bytes", output_parent.unit_bytes());

	const auto [input_size, input_start, input_end] = parse_input_start_end(opts.range, *input_file, hunk_size, hunk_size);
	if ((input_end - input_start) % 2048)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Data size %s is not divisible by sector size 2048", big_int_string(input_end - input_start));

	chd_codec_type compression[4];
	parse_compression(opts.out.compression, s_default_hd_compression, compression); // No reason to be different than HD for compression

	print_output_header(s, opts.out, output_parent);
	s.info(util::string_format("Input file:   %s", opts.input));
	if (input_start != 0 || input_end != input_size)
	{
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
		s.info(util::string_format("Input length: %s", big_int_string(input_end - input_start)));
	}
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	s.info(util::string_format("Logical size: %s", big_int_string(input_end - input_start)));

	output_guard guard(opts.out.output);
	{
		auto chd = make_compressor<chd_rawfile_compressor>(s, *input_file, input_start, input_end);
		create_output_chd(*chd, opts.out.output, input_end - input_start, hunk_size, 2048, compression, output_parent);

		const std::error_condition err = chd->write_metadata(DVD_METADATA_TAG, 0, "");
		if (err)
			fail(err, "Error adding DVD metadata: %s", err.message());

		compress_common(s, *chd);
	}
	guard.dismiss();
}

void create_ld(session &s, create_ld_options const &opts)
{
	avi_file::ptr input_file;
	{
		avi_file::error avierr = avi_file::open(opts.input, input_file);
		if (avierr != avi_file::error::NONE)
			fail(CHDMAN_ERR_FILE_OPEN, "Error opening AVI file (%s): %s", opts.input, avi_file::error_string(avierr));
	}
	const avi_file::movie_info &aviinfo = input_file->get_movie_info();

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	if (range_has_bytes_or_hunks(opts.range))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Laserdisc input range must be given in frames");
	auto [input_start, input_end] = parse_input_start_end(opts.range, aviinfo.video_numsamples, 0, 1);

	// determine parameters of the incoming video stream
	avi_info info;
	if (aviinfo.video_sampletime == 0)
		fail(CHDMAN_ERR_UNSUPPORTED, "AVI file has no video timing information");
	info.fps_times_1million = uint64_t(aviinfo.video_timescale) * 1000000 / aviinfo.video_sampletime;
	info.width = aviinfo.video_width;
	info.height = aviinfo.video_height;
	info.interlaced = ((info.fps_times_1million / 1000000) <= 30) && (info.height % 2 == 0) && (info.height > 288);
	info.channels = aviinfo.audio_channels;
	info.rate = aviinfo.audio_samplerate;

	// adjust for interlacing
	if (info.interlaced)
	{
		info.fps_times_1million *= 2;
		info.height /= 2;
		input_start *= 2;
		input_end *= 2;
	}

	// determine the number of bytes per frame
	info.max_samples_per_frame = (uint64_t(info.rate) * 1000000 + info.fps_times_1million - 1) / info.fps_times_1million;
	info.bytes_per_frame = avhuff_encoder::raw_data_size(info.width, info.height, info.channels, info.max_samples_per_frame);

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, info.bytes_per_frame, info.bytes_per_frame);

	chd_codec_type compression[4];
	parse_compression(opts.out.compression, s_default_ld_compression, compression);
	// disable support for uncompressed ones until the extraction code can handle it
	if (compression[0] == CHD_CODEC_NONE)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Uncompressed is not supported");

	print_output_header(s, opts.out, output_parent);
	s.info(util::string_format("Input file:   %s", opts.input));
	if (input_start != 0 && input_end != aviinfo.video_numsamples)
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
	s.info(util::string_format("Input length: %s (%02d:%02d:%02d)", big_int_string(input_end - input_start),
			uint32_t((uint64_t(input_end - input_start) * 1000000 / info.fps_times_1million / 60 / 60)),
			uint32_t(((uint64_t(input_end - input_start) * 1000000 / info.fps_times_1million / 60) % 60)),
			uint32_t(((uint64_t(input_end - input_start) * 1000000 / info.fps_times_1million) % 60))));
	s.info(util::string_format("Frame rate:   %d.%06d", info.fps_times_1million / 1000000, info.fps_times_1million % 1000000));
	s.info(util::string_format("Frame size:   %d x %d %s", info.width, info.height * (info.interlaced ? 2 : 1), info.interlaced ? "interlaced" : "non-interlaced"));
	s.info(util::string_format("Audio:        %d channels at %d Hz", info.channels, info.rate));
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	s.info(util::string_format("Hunk size:    %s", big_int_string(hunk_size)));
	s.info(util::string_format("Logical size: %s", big_int_string(uint64_t(input_end - input_start) * hunk_size)));

	output_guard guard(opts.out.output);
	{
		auto chd = make_compressor<chd_avi_compressor>(s, *input_file, info, input_start, input_end);
		create_output_chd(*chd, opts.out.output, uint64_t(input_end - input_start) * hunk_size, hunk_size, info.bytes_per_frame, compression, output_parent);

		// write the core A/V metadata
		std::string metadata = util::string_format(AV_METADATA_FORMAT, info.fps_times_1million / 1000000, info.fps_times_1million % 1000000, info.width, info.height, info.interlaced, info.channels, info.rate);
		std::error_condition err = chd->write_metadata(AV_METADATA_TAG, 0, metadata);
		if (err)
			fail(err, "Error adding AV metadata: %s", err.message());

		compress_common(s, *chd);

		// write the final LD metadata
		if (info.height == 524/2 || info.height == 624/2)
		{
			err = chd->write_metadata(AV_LD_METADATA_TAG, 0, chd->ldframedata(), 0);
			if (err)
				fail(err, "Error adding AVLD metadata: %s", err.message());
		}
	}
	guard.dismiss();
}


//**************************************************************************
//  COPY
//**************************************************************************

void copy(session &s, copy_options const &opts)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.input, opts.input_parent, input_chd, input_parent_chd);

	const auto [input_start, input_end] = parse_input_start_end(opts.range, input_chd.logical_bytes(), input_chd.hunk_bytes(), input_chd.hunk_bytes());

	chd_file output_parent;
	parse_output_chd_parameters(opts.out, output_parent);

	const uint32_t hunk_size = parse_hunk_size(opts.out, output_parent, input_chd.unit_bytes(), input_chd.hunk_bytes());
	if ((hunk_size % input_chd.hunk_bytes()) && (input_chd.hunk_bytes() % hunk_size))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Hunk size is not a whole multiple or factor of input hunk size");

	// we default to our current preferences using metadata to pick the type
	chd_codec_type compression[4];
	parse_compression(opts.out.compression, get_compression_defaults(input_chd), compression);

	print_output_header(s, opts.out, output_parent);
	s.info(util::string_format("Input CHD:    %s", opts.input));
	if (input_start != 0 || input_end != input_chd.logical_bytes())
	{
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
		s.info(util::string_format("Input length: %s", big_int_string(input_end - input_start)));
	}
	s.info(util::string_format("Compression:  %s", compression_string(compression)));
	s.info(util::string_format("Hunk size:    %s", big_int_string(hunk_size)));
	s.info(util::string_format("Logical size: %s", big_int_string(input_end - input_start)));

	output_guard guard(opts.out.output);
	{
		std::unique_ptr<cdrom_file> cdrom; // want this to be unwound after chd
		auto chd = make_compressor<chd_chdfile_compressor>(s, input_chd, input_start, input_end);
		create_output_chd(*chd, opts.out.output, input_end - input_start, hunk_size, input_chd.unit_bytes(), compression, output_parent);

		// clone all the metadata, upgrading where appropriate
		std::error_condition err;
		std::vector<uint8_t> metadata;
		chd_metadata_tag metatag;
		uint8_t metaflags;
		uint32_t index = 0;
		bool redo_cd = false;
		bool cdda_swap = false;
		for (err = input_chd.read_metadata(CHDMETATAG_WILDCARD, index++, metadata, metatag, metaflags); !err; err = input_chd.read_metadata(CHDMETATAG_WILDCARD, index++, metadata, metatag, metaflags))
		{
			// if this is an old CD-CHD tag, note that we want to re-do it
			if (metatag == CDROM_OLD_METADATA_TAG || metatag == CDROM_TRACK_METADATA_TAG)
			{
				redo_cd = true;
				continue;
			}
			// if this is old GD tag we want re-do it and swap CDDA
			if (metatag == GDROM_OLD_METADATA_TAG)
			{
				cdda_swap = redo_cd = true;
				continue;
			}

			// otherwise, clone it
			err = chd->write_metadata(metatag, CHDMETAINDEX_APPEND, metadata, metaflags);
			if (err)
				fail(err, "Error writing cloned metadata: %s", err.message());
		}

		// if we need to re-do the CD metadata, do it now
		if (redo_cd)
		{
			cdrom = std::make_unique<cdrom_file>(&input_chd);
			const cdrom_file::toc &toc = cdrom->get_toc();
			err = cdrom_file::write_metadata(chd.get(), toc);
			if (err)
				fail(err, "Error writing upgraded CD metadata: %s", err.message());
			if (cdda_swap)
				chd->m_toc = &toc;
		}

		compress_common(s, *chd);
	}
	guard.dismiss();
}


//**************************************************************************
//  EXTRACT
//**************************************************************************

void extract_raw(session &s, extract_raw_options const &opts, extract_sink *sink)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.input, opts.input_parent, input_chd, input_parent_chd);

	const auto [input_start, input_end] = parse_input_start_end(opts.range, input_chd.logical_bytes(), input_chd.hunk_bytes(), input_chd.hunk_bytes());

	if (opts.output.empty())
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output file name is required");

	if (sink)
	{
		std::vector<uint8_t> buffer((TEMP_BUFFER_SIZE / input_chd.hunk_bytes()) * input_chd.hunk_bytes());
		sink->begin_file(opts.output);
		for (uint64_t offset = input_start; offset < input_end; )
		{
			s.progress(CHDMAN_PHASE_EXTRACTING, double(offset - input_start) / double(input_end - input_start));

			uint32_t bytes_to_read = (std::min<uint64_t>)(buffer.size(), input_end - offset);
			std::error_condition err = input_chd.read_bytes(offset, &buffer[0], bytes_to_read);
			if (err)
				fail(err, "Error reading CHD file (%s): %s", opts.input, err.message());

			sink->write(&buffer[0], bytes_to_read);
			offset += bytes_to_read;
		}
		sink->end_file();
		s.progress(CHDMAN_PHASE_EXTRACTING, 1.0, 0.0, true);
		return;
	}

	check_existing_output_file(opts.force, opts.output);

	s.info(util::string_format("Output File:  %s", opts.output));
	s.info(util::string_format("Input CHD:    %s", opts.input));
	if (input_start != 0 || input_end != input_chd.logical_bytes())
	{
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
		s.info(util::string_format("Input length: %s", big_int_string(input_end - input_start)));
	}

	// guard is declared first so the file is closed before it is removed
	output_guard guard;
	util::core_file::ptr output_file;
	std::error_condition const filerr = util::core_file::open(opts.output, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, output_file);
	if (filerr)
		throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Unable to open file (%s): %s", opts.output, filerr.message()));
	guard.add(opts.output);

	std::vector<uint8_t> buffer((TEMP_BUFFER_SIZE / input_chd.hunk_bytes()) * input_chd.hunk_bytes());
	for (uint64_t offset = input_start; offset < input_end; )
	{
		s.progress(CHDMAN_PHASE_EXTRACTING, double(offset - input_start) / double(input_end - input_start));

		uint32_t bytes_to_read = (std::min<uint64_t>)(buffer.size(), input_end - offset);
		std::error_condition err = input_chd.read_bytes(offset, &buffer[0], bytes_to_read);
		if (err)
			fail(err, "Error reading CHD file (%s): %s", opts.input, err.message());

		auto const [writerr, count] = write(*output_file, &buffer[0], bytes_to_read);
		if (writerr)
			fail(writerr, "Error writing to file; check disk space (%s)", opts.output);

		offset += bytes_to_read;
	}

	std::error_condition const flusherr = output_file->flush();
	output_file.reset();
	if (flusherr)
		fail(flusherr, "Error writing to file; check disk space (%s)", opts.output);
	s.progress(CHDMAN_PHASE_EXTRACTING, 1.0, 0.0, true);
	s.info("Extraction complete");
	guard.dismiss();
}

void extract_cd(session &s, extract_cd_options const &opts, extract_sink *sink)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.input, opts.input_parent, input_chd, input_parent_chd);

	// further process input file
	std::unique_ptr<cdrom_file> cdrom;
	try
	{
		cdrom = std::make_unique<cdrom_file>(&input_chd);
	}
	catch (std::nullptr_t)
	{
		fail(CHDMAN_ERR_UNSUPPORTED, "Input CHD (%s) is not a valid CD-ROM image", opts.input);
	}
	const cdrom_file::toc &toc = cdrom->get_toc();

	if (opts.output.empty())
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output file name is required");
	if (!sink)
		check_existing_output_file(opts.force, opts.output);

	// determine output type based on the specified file extension
	int mode = MODE_NORMAL;
	if (core_filename_ends_with(opts.output, ".cue"))
		mode = MODE_CUEBIN;
	else if (core_filename_ends_with(opts.output, ".gdi"))
		mode = MODE_GDI;

	// determine the output bin filename based on provided input parameters
	std::string default_name(opts.output);

	// split path and extension
	std::string::size_type chop = default_name.find_last_of('.');
	if (chop != std::string::npos)
		default_name.erase(chop, default_name.size());

	// GDIs will always output as split bin
	bool is_splitbin = mode == MODE_GDI || opts.split_bin;
	if (!is_splitbin && cdrom->is_gdrom() && mode == MODE_CUEBIN)
	{
		// GD-ROM cue/bin is in Redump format which should always be split by tracks
		s.warning("Warning: split bin is required for this specific combination of input disc type and output format, enabling automatically");
		is_splitbin = true;
	}

	if (is_splitbin)
	{
		if (mode == MODE_GDI)
		{
			default_name += "%02t";
		}
		else
		{
			const std::string format = toc.numtrks >= 10 ? "%02t" : "%t";
			default_name += " (Track " + format + ")";
		}
	}

	std::string output_bin_file_ext = ".bin";
	std::string output_bin_file_str;
	if (!opts.output_bin)
	{
		output_bin_file_str = default_name;
	}
	else
	{
		output_bin_file_str = *opts.output_bin;

		chop = output_bin_file_str.find_last_of('.');
		if (chop != std::string::npos)
		{
			output_bin_file_ext = output_bin_file_str.substr(chop, output_bin_file_str.size() - chop);
			output_bin_file_str.erase(chop, output_bin_file_str.size());
		}
	}

	if (output_bin_file_str.find('"') != std::string::npos || output_bin_file_ext.find('"') != std::string::npos)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output bin filename (%s%s) must not contain quotation marks", output_bin_file_str, output_bin_file_ext);

	s.info(util::string_format("Input CHD:    %s", opts.input));
	s.info(util::string_format("Output TOC:   %s", opts.output));

	std::vector<std::string> track_filenames;
	util::core_file::ptr output_bin_file;
	util::core_file::ptr output_toc_file;
	std::string trackbin_name;

	output_guard guard;
	std::error_condition filerr;
	if (!sink)
	{
		filerr = util::core_file::open(opts.output, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_NO_BOM, output_toc_file);
		if (filerr)
			throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Unable to open file (%s): %s", opts.output, filerr.message()));
		guard.add(opts.output);
	}

	// a sink gets the TOC before the data, so it is collected in memory
	toc_writer toc_out(sink ? nullptr : output_toc_file.get());

	try
	{
		uint64_t total_bytes = 0;
		for (int tracknum = 0; tracknum < toc.numtrks; tracknum++)
		{
			// determine total frames
			total_bytes += toc.tracks[tracknum].frames * (toc.tracks[tracknum].datasize + toc.tracks[tracknum].subsize);

			// generate output filename for each track
			std::string new_trackbin_name = output_bin_file_str;

			if (mode == MODE_GDI && toc.tracks[tracknum].trktype == cdrom_file::CD_TRACK_AUDIO)
				new_trackbin_name += ".raw";
			else
				new_trackbin_name += output_bin_file_ext;

			// variable replacement in output filename
			const std::regex variables_regex("(%*)(%([+-]?\\d+)?([a-zA-Z]))");
			std::string::const_iterator new_trackbin_name_itr = new_trackbin_name.begin();
			std::string::const_iterator new_trackbin_name_end = new_trackbin_name.end();
			std::string filename_formatted = new_trackbin_name;
			std::smatch variable_matches;
			bool found_track_variable = false;

			while (std::regex_search(new_trackbin_name_itr, new_trackbin_name_end, variable_matches, variables_regex))
			{
				// full_match will always have one leading %, so if leading_escape has an even number of %s then
				// we can know that we're working on an unescaped %
				const std::string leading_escape = variable_matches[1].str();
				const std::string full_match = variable_matches[2].str();
				const std::string format_part = variable_matches[3].str();
				const std::string format_type = variable_matches[4].str();

				if ((leading_escape.size() % 2) == 0)
				{
					std::string replacement;

					if (format_type == "t")
					{
						// track number
						if (is_splitbin)
						{
							replacement = util::string_format("%" + format_part + "d", tracknum+1);
							found_track_variable = true;
						}
					}
					else
					{
						s.warning(util::string_format("Warning: encountered unknown format value '%s', ignoring", format_type));
					}

					if (!replacement.empty())
					{
						// replace all instances of encountered full format token
						size_t index = std::string::npos;
						while ((index = filename_formatted.find(full_match)) != std::string::npos)
							filename_formatted.replace(index, full_match.size(), replacement);
					}
				}

				new_trackbin_name_itr = variable_matches.suffix().first; // move past match for next loop
			}

			if (is_splitbin && !found_track_variable)
				fail(CHDMAN_ERR_INVALID_ARGUMENT, "A track number variable (%%t) must be specified in the output bin filename when split bin is enabled");

			// verify output BIN file doesn't exist
			if (!sink)
				check_existing_output_file(opts.force, filename_formatted);

			if (track_filenames.empty() || filename_formatted != track_filenames.back())
				s.info(util::string_format("Output Data:  %s", filename_formatted));

			track_filenames.push_back(filename_formatted);
		}

		// GDI must start with the # of tracks
		if (mode == MODE_GDI)
		{
			toc_out.printf("%d\n", toc.numtrks);
		}
		else if (mode == MODE_NORMAL)
		{
			bool mode1 = false;
			bool mode2 = false;
			bool cdda = false;

			for (int tracknum = 0; tracknum < toc.numtrks; tracknum++)
			{
				switch (toc.tracks[tracknum].trktype)
				{
					case cdrom_file::CD_TRACK_MODE1:
					case cdrom_file::CD_TRACK_MODE1_RAW:
						mode1 = true;
						break;

					case cdrom_file::CD_TRACK_MODE2:
					case cdrom_file::CD_TRACK_MODE2_FORM1:
					case cdrom_file::CD_TRACK_MODE2_FORM2:
					case cdrom_file::CD_TRACK_MODE2_FORM_MIX:
					case cdrom_file::CD_TRACK_MODE2_RAW:
						mode2 = true;
						break;

					case cdrom_file::CD_TRACK_AUDIO:
						cdda = true;
						break;
				}
			}

			if (mode2)
				toc_out.printf("CD_ROM_XA\n\n\n");
			else if (cdda && !mode1)
				toc_out.printf("CD_DA\n\n\n");
			else
				toc_out.printf("CD_ROM\n\n\n");
		}

		if (cdrom->is_gdrom() && mode == MODE_CUEBIN)
		{
			// modify TOC to match Redump cue/bin format as best as possible
			cdrom_file::toc *trackinfo = const_cast<cdrom_file::toc *>(&toc);

			// TOSEC GDI-based CHDs have the padframes field set to non-0 where the pregaps for the next track would be
			const bool has_physical_pregap = trackinfo->tracks[0].padframes == 0;

			for (int tracknum = 1; tracknum < toc.numtrks; tracknum++)
			{
				// pgdatasize should never be set in GD-ROMs currently, so if it is set then assume the TOC has proper pregap values
				if (trackinfo->tracks[tracknum].pgdatasize != 0)
					break;

				// don't adjust the first track of the single-density and high-density areas
				if (toc.tracks[tracknum].physframeofs == 45000)
					continue;

				if (!has_physical_pregap)
				{
					// NOTE: This will generate a cue with PREGAP commands instead of INDEX 00 because the pregap data isn't baked into the bins
					trackinfo->tracks[tracknum].pregap += trackinfo->tracks[tracknum-1].padframes;

					// "type 1" (only one data track in high-density area) and "type 2" (1 data and then the rest of the tracks being audio tracks in high-density area) don't require any adjustments
					if (tracknum + 1 >= toc.numtrks && toc.tracks[tracknum].trktype != cdrom_file::CD_TRACK_AUDIO)
					{
						if (toc.tracks[tracknum-1].trktype != cdrom_file::CD_TRACK_AUDIO)
						{
							// "type 3" where the high-density area is just two data tracks
							// there shouldn't be any pregap in the padframes from the previous track in this case, and the full 3s pregap is baked into the previous track
							// Only known to be used by Shenmue II JP's discs 2, 3, 4 and Virtua Fighter History & VF4
							trackinfo->tracks[tracknum-1].padframes += 225;

							trackinfo->tracks[tracknum].pregap += 225;
							trackinfo->tracks[tracknum].splitframes = 225;
							trackinfo->tracks[tracknum].pgdatasize = trackinfo->tracks[tracknum].datasize;
							trackinfo->tracks[tracknum].pgtype = trackinfo->tracks[tracknum].trktype;
						}
						else
						{
							// "type 3 split" where the first track and last of the high-density area are data tracks and in between is audio tracks
							// TODO: These 75 frames are actually included at the end of the previous track so should be written
							// It's currently not possible to format it as expected without hacky code because the 150 pregap for the last track
							// is sandwiched between these 75 frames and the actual track data.
							// The 75 frames seems to normally be 0s so this should be ok for now until a use case is found.
							trackinfo->tracks[tracknum-1].frames -= 75;
							trackinfo->tracks[tracknum].pregap += 75;
						}
					}
				}
				else
				{
					int curextra = 150; // 00:02:00
					if (tracknum + 1 >= toc.numtrks && toc.tracks[tracknum].trktype != cdrom_file::CD_TRACK_AUDIO)
						curextra += 75; // 00:01:00, special case when last track is data

					trackinfo->tracks[tracknum-1].padframes = curextra;

					trackinfo->tracks[tracknum].pregap += curextra;
					trackinfo->tracks[tracknum].splitframes = curextra;
					trackinfo->tracks[tracknum].pgdatasize = trackinfo->tracks[tracknum].datasize;
					trackinfo->tracks[tracknum].pgtype = trackinfo->tracks[tracknum].trktype;
				}
			}
		}

		// iterate over tracks and copy all data. With write_data false nothing
		// is read or written and only the TOC text is produced: a sink receives
		// the TOC first, so it gets a pass of its own before the data.
		auto process_tracks = [&] (toc_writer &toc_text, bool write_data)
		{
			uint64_t totaloutputoffs = 0;
			uint64_t outputoffs = 0;
			uint32_t discoffs = 0;
			std::vector<uint8_t> buffer;
			bool sink_file_open = false;
			trackbin_name.clear();

			for (int tracknum = 0; tracknum < toc.numtrks; tracknum++)
			{
				if (track_filenames[tracknum] != trackbin_name)
				{
					totaloutputoffs += outputoffs;
					outputoffs = 0;

					if (mode != MODE_GDI)
						discoffs = 0;

					if (write_data && !sink && output_bin_file)
					{
						std::error_condition const flusherr = output_bin_file->flush();
						if (flusherr)
							fail(flusherr, "Error writing to file (%s)", trackbin_name);
					}

					trackbin_name = track_filenames[tracknum];

					if (write_data && sink)
					{
						if (sink_file_open)
							sink->end_file();
						sink->begin_file(trackbin_name);
						sink_file_open = true;
					}
					else if (write_data)
					{
						output_bin_file.reset();

						filerr = util::core_file::open(trackbin_name, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, output_bin_file);
						if (filerr)
							throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Unable to open file (%s): %s", trackbin_name, filerr.message()));

						guard.add(trackbin_name);
					}
				}

				if (cdrom->is_gdrom() && mode == MODE_CUEBIN)
				{
					if (tracknum == 0)
						toc_text.printf("REM SINGLE-DENSITY AREA\n");
					else if (toc.tracks[tracknum].physframeofs == 45000)
						toc_text.printf("REM HIGH-DENSITY AREA\n");
				}

				// output the metadata about the track to the TOC file
				const cdrom_file::track_info &trackinfo = toc.tracks[tracknum];
				output_track_metadata(mode, toc_text, tracknum, trackinfo, std::string(core_filename_extract_base(trackbin_name)), discoffs, outputoffs);

				// If this is bin/cue output and the CHD contains subdata, warn the user and don't include
				// the subdata size in the buffer calculation.
				uint32_t output_frame_size = trackinfo.datasize + ((trackinfo.subtype != cdrom_file::CD_SUB_NONE) ? trackinfo.subsize : 0);
				if (trackinfo.subtype != cdrom_file::CD_SUB_NONE && ((mode == MODE_CUEBIN) || (mode == MODE_GDI)))
				{
					if (write_data)
					{
						s.warning(util::string_format("Warning: Track %d has subcode data.  bin/cue and gdi formats cannot contain subcode data and it will be omitted.", tracknum+1));
						s.warning("       : This may affect usage of the output image.  Use bin/toc output to keep all data.");
					}
					output_frame_size = trackinfo.datasize;
				}

				// resize the buffer for the track
				if (write_data)
					buffer.resize((TEMP_BUFFER_SIZE / output_frame_size) * output_frame_size);

				// now read and output the actual data
				uint32_t bufferoffs = 0;
				uint32_t actualframes = trackinfo.frames - trackinfo.padframes + trackinfo.splitframes;
				for (uint32_t frame = 0; frame < actualframes; frame++)
				{
					if (write_data)
						s.progress(CHDMAN_PHASE_EXTRACTING, double(totaloutputoffs + outputoffs) / double(total_bytes));

					int trk, frameofs;
					if (tracknum > 0 && frame < trackinfo.splitframes)
					{
						// pull data from previous track, the reverse of how splitframes is used when making the GD-ROM CHDs
						trk = tracknum - 1;
						frameofs = toc.tracks[trk].frames - trackinfo.splitframes + frame;
					}
					else
					{
						trk = tracknum;
						frameofs = frame - trackinfo.splitframes;
					}

					// read the data
					if (write_data)
					{
						if (!cdrom->read_data(cdrom->get_track_start_phys(trk) + frameofs, &buffer[bufferoffs], toc.tracks[trk].trktype, true))
							fail(CHDMAN_ERR_CHD, "Error reading frame %d of track %d from CHD file (%s)", frameofs, trk + 1, opts.input);

						// for CDRWin and GDI audio tracks must be reversed
						// in the case of GDI and CHD version < 5 we assuming source CHD image is GDROM so audio tracks is already reversed
						if (((mode == MODE_GDI && input_chd.version() > 4) || (mode == MODE_CUEBIN)) && (toc.tracks[trk].trktype == cdrom_file::CD_TRACK_AUDIO))
							for (int swapindex = 0; swapindex < toc.tracks[trk].datasize; swapindex += 2)
							{
								uint8_t swaptemp = buffer[bufferoffs + swapindex];
								buffer[bufferoffs + swapindex] = buffer[bufferoffs + swapindex + 1];
								buffer[bufferoffs + swapindex + 1] = swaptemp;
							}
					}
					bufferoffs += toc.tracks[trk].datasize;
					discoffs++;

					// read the subcode data
					if (toc.tracks[trk].subtype != cdrom_file::CD_SUB_NONE && (mode == MODE_NORMAL))
					{
						if (write_data && !cdrom->read_subcode(cdrom->get_track_start_phys(trk) + frameofs, &buffer[bufferoffs], true))
							fail(CHDMAN_ERR_CHD, "Error reading subcode for frame %d of track %d from CHD file (%s)", frameofs, trk + 1, opts.input);
						bufferoffs += toc.tracks[trk].subsize;
					}

					// write it out if we need to
					if ((write_data && bufferoffs == buffer.size()) || frame == actualframes - 1)
					{
						if (write_data && sink)
						{
							sink->write(&buffer[0], bufferoffs);
						}
						else if (write_data)
						{
							output_bin_file->seek(outputoffs, SEEK_SET);
							auto const [writerr, byteswritten] = write(*output_bin_file, &buffer[0], bufferoffs);
							if (writerr)
								fail(writerr, "Error writing frame %d to file (%s): %s", frame, trackbin_name, writerr.message());
						}
						outputoffs += bufferoffs;
						bufferoffs = 0;
					}
				}

				discoffs += trackinfo.padframes;
			}

			if (sink_file_open)
				sink->end_file();
		};

		if (sink)
		{
			process_tracks(toc_out, false);
			sink->begin_file(opts.output);
			sink->write(toc_out.text().data(), toc_out.text().size());
			sink->end_file();

			toc_writer unused(nullptr);
			process_tracks(unused, true);
		}
		else
		{
			process_tracks(toc_out, true);

			// finish up
			std::error_condition flusherr;
			if (output_bin_file)
				flusherr = output_bin_file->flush();
			if (!flusherr)
				flusherr = output_toc_file->flush();
			output_bin_file.reset();
			output_toc_file.reset();
			if (flusherr)
				fail(flusherr, "Error writing output files: %s", flusherr.message());
		}
		s.progress(CHDMAN_PHASE_EXTRACTING, 1.0, 0.0, true);
		s.info("Extraction complete");
	}
	catch (...)
	{
		// close the files before the guard deletes them
		output_bin_file.reset();
		output_toc_file.reset();
		throw;
	}
	guard.dismiss();
}

void extract_ld(session &s, extract_ld_options const &opts)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.input, opts.input_parent, input_chd, input_parent_chd);

	// read core metadata
	std::string metadata;
	std::error_condition err = input_chd.read_metadata(AV_METADATA_TAG, 0, metadata);
	if (err)
		fail(CHDMAN_ERR_NOT_FOUND, "Unable to find A/V metadata in the input CHD");

	// parse the metadata
	uint32_t fps_times_1million;
	uint32_t max_samples_per_frame;
	uint32_t frame_bytes;
	int width;
	int height;
	int interlaced;
	int channels;
	int rate;
	{
		int fps;
		int fpsfrac;
		if (sscanf(metadata.c_str(), AV_METADATA_FORMAT, &fps, &fpsfrac, &width, &height, &interlaced, &channels, &rate) != 7)
			fail(CHDMAN_ERR_CHD, "Improperly formatted A/V metadata found");
		fps_times_1million = fps * 1000000 + fpsfrac;
	}
	if (fps_times_1million == 0)
		fail(CHDMAN_ERR_CHD, "Improperly formatted A/V metadata found");
	uint8_t interlace_factor = interlaced ? 2 : 1;

	// determine key parameters and validate
	max_samples_per_frame = (uint64_t(rate) * 1000000 + fps_times_1million - 1) / fps_times_1million;
	frame_bytes = avhuff_encoder::raw_data_size(width, height, channels, max_samples_per_frame);
	if (frame_bytes != input_chd.hunk_bytes())
		fail(CHDMAN_ERR_CHD, "Frame size does not match hunk size for this CHD");

	if (range_has_bytes_or_hunks(opts.range))
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Laserdisc input range must be given in frames");
	auto [input_start, input_end] = parse_input_start_end(opts.range, input_chd.hunk_count() / interlace_factor, 0, 1);
	input_start *= interlace_factor;
	input_end *= interlace_factor;

	// build up the movie info
	avi_file::movie_info info;
	info.video_format = avi_file::FORMAT_YUY2;
	info.video_timescale = fps_times_1million / interlace_factor;
	info.video_sampletime = 1000000;
	info.video_width = width;
	info.video_height = height * interlace_factor;
	info.video_depth = 16;
	info.audio_format = 0;
	info.audio_timescale = rate;
	info.audio_sampletime = 1;
	info.audio_channels = channels;
	info.audio_samplebits = 16;
	info.audio_samplerate = rate;

	if (opts.output.empty())
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output file name is required");
	check_existing_output_file(opts.force, opts.output);

	s.info(util::string_format("Output File:  %s", opts.output));
	s.info(util::string_format("Input CHD:    %s", opts.input));
	if (input_start != 0 || input_end != input_chd.hunk_count())
	{
		s.info(util::string_format("Input start:  %s", big_int_string(input_start)));
		s.info(util::string_format("Input length: %s", big_int_string(input_end - input_start)));
	}

	avi_file::ptr output_file;
	{
		avi_file::error avierr = avi_file::create(opts.output, info, output_file);
		if (avierr != avi_file::error::NONE)
			fail(CHDMAN_ERR_FILE_OPEN, "Unable to open file (%s)", opts.output);
	}
	output_guard guard(opts.output);

	try
	{
		// create the codec configuration
		avhuff_decoder::config avconfig;
		bitmap_yuy16 avvideo;
		std::vector<int16_t> audio_data[16];
		uint32_t actsamples;
		avconfig.video = &avvideo;
		avconfig.maxsamples = max_samples_per_frame;
		avconfig.actsamples = &actsamples;
		for (int chnum = 0; chnum < std::size(audio_data); chnum++)
		{
			audio_data[chnum].resize(std::max(1U, max_samples_per_frame));
			avconfig.audio[chnum] = &audio_data[chnum][0];
		}

		// iterate over frames
		bitmap_yuy16 fullbitmap(width, height * interlace_factor);
		for (uint64_t framenum = input_start; framenum < input_end; framenum++)
		{
			s.progress(CHDMAN_PHASE_EXTRACTING, double(framenum - input_start) / double(input_end - input_start), 0.0, framenum == input_start);

			// set up the fake bitmap for this frame
			avvideo.wrap(&fullbitmap.pix(framenum % interlace_factor), fullbitmap.width(), fullbitmap.height() / interlace_factor, fullbitmap.rowpixels() * interlace_factor);
			input_chd.codec_configure(CHD_CODEC_AVHUFF, AVHUFF_CODEC_DECOMPRESS_CONFIG, &avconfig);

			// read the hunk into the buffers
			std::error_condition err = input_chd.codec_process_hunk(framenum);
			if (err)
				fail(err, "Error reading hunk %d from CHD file (%s): %s", framenum, opts.input, err.message());

			// write audio
			for (int chnum = 0; chnum < channels; chnum++)
			{
				avi_file::error avierr = output_file->append_sound_samples(chnum, avconfig.audio[chnum], actsamples, 0);
				if (avierr != avi_file::error::NONE)
					fail(CHDMAN_ERR_IO, "Error writing samples for hunk %d to file (%s): %s", framenum, opts.output, avi_file::error_string(avierr));
			}

			// write video
			if ((framenum + 1) % interlace_factor == 0)
			{
				avi_file::error avierr = output_file->append_video_frame(fullbitmap);
				if (avierr != avi_file::error::NONE)
					fail(CHDMAN_ERR_IO, "Error writing video for hunk %d to file (%s): %s", framenum, opts.output, avi_file::error_string(avierr));
			}
		}

		// close and return
		output_file.reset();
		s.progress(CHDMAN_PHASE_EXTRACTING, 1.0, 0.0, true);
		s.info("Extraction complete");
	}
	catch (...)
	{
		output_file.reset();
		throw;
	}
	guard.dismiss();
}


//**************************************************************************
//  METADATA
//**************************************************************************

void add_metadata(session &s, add_metadata_options const &opts)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(opts.ref.input, opts.ref.input_parent, input_chd, input_parent_chd, true);

	chd_metadata_tag const tag = parse_tag(opts.ref.tag);
	uint32_t const index = opts.ref.index;

	// exactly one source of data
	int const sources = (opts.value_text ? 1 : 0) + (opts.value_file ? 1 : 0) + (opts.value_data ? 1 : 0);
	if (sources == 0)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Missing metadata value; supply text, a file or data");
	if (sources > 1)
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "More than one metadata value supplied; only one permitted");

	std::vector<uint8_t> file;
	if (opts.value_file)
	{
		std::error_condition const filerr = util::core_file::load(*opts.value_file, file);
		if (filerr)
			throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Error reading metadata file (%s): %s", *opts.value_file, filerr.message()));
	}

	uint8_t flags = CHD_MDFLAGS_CHECKSUM;
	if (opts.no_checksum)
		flags &= ~CHD_MDFLAGS_CHECKSUM;

	s.info(util::string_format("Input file:   %s", opts.ref.input));
	s.info(util::string_format("Tag:          %s", tag_string(tag)));
	s.info(util::string_format("Index:        %d", index));
	if (opts.value_text)
		s.info(util::string_format("Text:         %s", *opts.value_text));
	else if (opts.value_file)
		s.info(util::string_format("Data:         %s (%d bytes)", *opts.value_file, file.size()));
	else
		s.info(util::string_format("Data:         %d bytes", opts.value_data->size()));

	std::error_condition err;
	if (opts.value_text)
		err = input_chd.write_metadata(tag, index, *opts.value_text, flags);
	else if (opts.value_file)
		err = input_chd.write_metadata(tag, index, file.data(), file.size(), flags);
	else
		err = input_chd.write_metadata(tag, index, opts.value_data->data(), opts.value_data->size(), flags);
	if (err)
		fail(err, "Error adding metadata: %s", err.message());
	s.info("Metadata added");
}

void del_metadata(session &s, metadata_ref const &ref)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(ref.input, ref.input_parent, input_chd, input_parent_chd, true);

	chd_metadata_tag const tag = parse_tag(ref.tag);

	s.info(util::string_format("Input file:   %s", ref.input));
	s.info(util::string_format("Tag:          %s", tag_string(tag)));
	s.info(util::string_format("Index:        %d", ref.index));

	std::error_condition err = input_chd.delete_metadata(tag, ref.index);
	if (err)
		fail(err, "Error removing metadata: %s", err.message());
	s.info("Metadata removed");
}

std::vector<uint8_t> get_metadata(session &s, metadata_ref const &ref)
{
	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(ref.input, ref.input_parent, input_chd, input_parent_chd);

	std::vector<uint8_t> buffer;
	std::error_condition err = input_chd.read_metadata(parse_tag(ref.tag), ref.index, buffer);
	if (err)
		fail(err, "Error reading metadata: %s", err.message());
	return buffer;
}

void dump_metadata(session &s, metadata_ref const &ref, std::string const &output, bool force)
{
	if (output.empty())
		fail(CHDMAN_ERR_INVALID_ARGUMENT, "Output file name is required");

	chd_file input_parent_chd;
	chd_file input_chd;
	open_input_chd(ref.input, ref.input_parent, input_chd, input_parent_chd);

	check_existing_output_file(force, output);

	std::vector<uint8_t> buffer;
	std::error_condition err = input_chd.read_metadata(parse_tag(ref.tag), ref.index, buffer);
	if (err)
		fail(err, "Error reading metadata: %s", err.message());

	output_guard guard;
	util::core_file::ptr output_file;
	std::error_condition filerr = util::core_file::open(output, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, output_file);
	if (filerr)
		throw error(CHDMAN_ERR_FILE_OPEN, util::string_format("Unable to open file (%s): %s", output, filerr.message()));
	guard.add(output);

	size_t count;
	std::tie(filerr, count) = write(*output_file, buffer.data(), buffer.size());
	if (!filerr)
		filerr = output_file->flush();
	output_file.reset();
	if (filerr)
		fail(filerr, "Error writing file (%s)", output);

	s.info(util::string_format("File (%s) written, %s bytes", output, big_int_string(buffer.size())));
	guard.dismiss();
}


//**************************************************************************
//  TEMPLATES / ERRORS
//**************************************************************************

std::vector<hd_template> const &templates()
{
	return s_hd_templates;
}

chdman_status translate_current_exception(std::string &message) noexcept
{
	try
	{
		throw;
	}
	catch (error const &err)
	{
		message = err.what();
		return err.status();
	}
	catch (std::error_condition const &err)
	{
		message = "CHD error occurred: " + err.message();
		return status_from_condition(err);
	}
	catch (std::nullptr_t)
	{
		// cdrom_file throws nullptr when CD metadata cannot be parsed
		message = "Invalid or unsupported CD-ROM metadata";
		return CHDMAN_ERR_CHD;
	}
	catch (std::bad_alloc const &)
	{
		message = "Out of memory";
		return CHDMAN_ERR_OUT_OF_MEMORY;
	}
	catch (std::exception const &ex)
	{
		message = std::string("Unhandled exception: ") + ex.what();
		return CHDMAN_ERR_INTERNAL;
	}
	catch (...)
	{
		message = "Unhandled exception";
		return CHDMAN_ERR_INTERNAL;
	}
}

} // namespace chdman
