// license:BSD-3-Clause
// lib-chd-kit C ABI: converts C option structs to the typed C++ options, runs
// the core operation and converts every exception into a status code plus a
// message retrievable through chdman_last_error().

#include "chdman.h"
#include "chdman_core.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

struct chdman_context
{
	chdman::session session;
	std::string last_error;
};

struct chdman_info
{
	chdman::info_result result;
};

namespace {

std::optional<std::string> opt_str(const char *s)
{
	if (!s || !*s)
		return std::nullopt;
	return std::string(s);
}

std::string req_str(const char *s, const char *what)
{
	if (!s || !*s)
		throw chdman::error(CHDMAN_ERR_INVALID_ARGUMENT, std::string(what) + " is required");
	return std::string(s);
}

std::optional<uint64_t> opt_u64(uint64_t v)
{
	if (v == CHDMAN_UNSET)
		return std::nullopt;
	return v;
}

std::optional<uint32_t> opt_nonzero(uint32_t v)
{
	if (v == 0)
		return std::nullopt;
	return v;
}

// Smallest struct_size accepted for each versioned struct: its ABI 1 layout
// up to the end of the last v1 field. These values must never change; fields
// added later make sizeof() grow but not the minimum.
template <typename T> constexpr size_t v1_size();
#define CHDMAN_V1_SIZE(T, last) template <> constexpr size_t v1_size<T>() { return offsetof(T, last) + sizeof(T::last); }
CHDMAN_V1_SIZE(chdman_info_options, hunk_stats)
CHDMAN_V1_SIZE(chdman_verify_options, fix)
CHDMAN_V1_SIZE(chdman_verify_result, computed_sha1)
CHDMAN_V1_SIZE(chdman_create_raw_options, unit_size)
CHDMAN_V1_SIZE(chdman_create_hd_options, sector_size)
CHDMAN_V1_SIZE(chdman_create_cd_options, input)
CHDMAN_V1_SIZE(chdman_create_dvd_options, range)
CHDMAN_V1_SIZE(chdman_create_ld_options, length_frames)
CHDMAN_V1_SIZE(chdman_copy_options, range)
CHDMAN_V1_SIZE(chdman_extract_raw_options, range)
CHDMAN_V1_SIZE(chdman_extract_cd_options, force)
CHDMAN_V1_SIZE(chdman_extract_ld_options, length_frames)
CHDMAN_V1_SIZE(chdman_add_metadata_options, no_checksum)
#undef CHDMAN_V1_SIZE

// Copies a caller's struct into a local one. The local copy starts from the
// matching *_init() defaults, so fields the caller's (older) header lacks get
// their defaults, and fields from a newer header that this library doesn't
// know are ignored.
template <typename T>
T read_struct(const T *in, void (CHDMAN_CALL *init)(T *))
{
	if (!in)
		throw chdman::error(CHDMAN_ERR_INVALID_ARGUMENT, "Options pointer is NULL");
	if (in->struct_size < v1_size<T>())
		throw chdman::error(CHDMAN_ERR_ABI_MISMATCH, "Options struct_size is smaller than expected; call the matching *_init()");
	T local;
	init(&local);
	std::memcpy(&local, in, std::min(in->struct_size, sizeof(T)));
	return local;
}

// Copies a result struct obtained from read_struct() back to the caller,
// writing no more than the caller's struct_size.
template <typename T>
void write_struct(T *out, T const &local)
{
	std::memcpy(out, &local, std::min(out->struct_size, sizeof(T)));
}

chdman::output_options convert(const chdman_output_options &o)
{
	chdman::output_options r;
	r.output = req_str(o.output, "Output file name");
	r.output_parent = opt_str(o.output_parent);
	r.force = o.force != 0;
	r.hunk_size = opt_nonzero(o.hunk_size);
	r.compression = opt_str(o.compression);
	return r;
}

chdman::range_options convert(const chdman_range_options &o)
{
	chdman::range_options r;
	r.start_byte = opt_u64(o.start_byte);
	r.start_hunk = opt_u64(o.start_hunk);
	r.length_bytes = opt_u64(o.length_bytes);
	r.length_hunks = opt_u64(o.length_hunks);
	return r;
}

void init_output(chdman_output_options &o)
{
	std::memset(&o, 0, sizeof(o));
}

void init_range(chdman_range_options &r)
{
	r.start_byte = r.start_hunk = r.length_bytes = r.length_hunks = CHDMAN_UNSET;
}

template <typename F>
chdman_status run(chdman_context *ctx, F &&f) noexcept
{
	if (!ctx)
		return CHDMAN_ERR_INVALID_ARGUMENT;
	try
	{
		chdman::session_scope const scope(ctx->session);
		ctx->last_error.clear();
		ctx->session.begin_operation();
		ctx->session.check_cancel(); // honour a cancel requested before the start
		f();
		ctx->session.end_operation();
		return CHDMAN_OK;
	}
	catch (...)
	{
		ctx->session.end_operation();
		std::string message;
		chdman_status const status = chdman::translate_current_exception(message);
		try
		{
			// MAME often prints the specific cause (e.g. a missing .bin file)
			// and returns a generic error code; add the cause to the message
			std::string const &detail = ctx->session.last_osd_error();
			if (!detail.empty() && message.find(detail) == std::string::npos)
				message += ": " + detail;
			ctx->last_error = std::move(message);
		}
		catch (...) { }
		return status;
	}
}

void copy_sha1(char (&dest)[41], std::string const &src)
{
	std::memset(dest, 0, sizeof(dest));
	std::strncpy(dest, src.c_str(), sizeof(dest) - 1);
}

} // anonymous namespace


extern "C" {

//**************************************************************************
//  GENERAL
//**************************************************************************

CHDMAN_API const char *CHDMAN_CALL chdman_status_string(chdman_status status)
{
	switch (status)
	{
	case CHDMAN_OK:                   return "success";
	case CHDMAN_ERR_INVALID_ARGUMENT: return "invalid argument";
	case CHDMAN_ERR_FILE_EXISTS:      return "output file already exists";
	case CHDMAN_ERR_FILE_OPEN:        return "unable to open file";
	case CHDMAN_ERR_IO:               return "I/O error";
	case CHDMAN_ERR_CHD:              return "CHD error";
	case CHDMAN_ERR_UNSUPPORTED:      return "unsupported format or version";
	case CHDMAN_ERR_NOT_FOUND:        return "not found";
	case CHDMAN_ERR_CANCELLED:        return "cancelled";
	case CHDMAN_ERR_OUT_OF_MEMORY:    return "out of memory";
	case CHDMAN_ERR_INTERNAL:         return "internal error";
	case CHDMAN_ERR_ABI_MISMATCH:     return "ABI mismatch";
	}
	return "unknown error";
}

CHDMAN_API const char *CHDMAN_CALL chdman_version(void)
{
	return "0.289 (mame0289) lib-chd-kit 1";
}

CHDMAN_API int CHDMAN_CALL chdman_abi_version(void)
{
	return CHDMAN_ABI_VERSION;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_context_create(chdman_context **out_ctx)
{
	if (!out_ctx)
		return CHDMAN_ERR_INVALID_ARGUMENT;
	*out_ctx = new (std::nothrow) chdman_context();
	return *out_ctx ? CHDMAN_OK : CHDMAN_ERR_OUT_OF_MEMORY;
}

CHDMAN_API void CHDMAN_CALL chdman_context_destroy(chdman_context *ctx)
{
	delete ctx;
}

CHDMAN_API void CHDMAN_CALL chdman_context_set_progress(chdman_context *ctx, chdman_progress_fn fn, void *user)
{
	if (!ctx)
		return;
	if (fn)
		ctx->session.progress_cb = [fn, user] (chdman_phase phase, double fraction, double ratio) { return fn(user, phase, fraction, ratio) != 0; };
	else
		ctx->session.progress_cb = nullptr;
}

CHDMAN_API void CHDMAN_CALL chdman_context_set_log(chdman_context *ctx, chdman_log_fn fn, void *user)
{
	if (!ctx)
		return;
	if (fn)
		ctx->session.log_cb = [fn, user] (chdman_log_level level, std::string const &message) { fn(user, level, message.c_str()); };
	else
		ctx->session.log_cb = nullptr;
}

CHDMAN_API void CHDMAN_CALL chdman_context_set_num_processors(chdman_context *ctx, int count)
{
	if (ctx)
		ctx->session.num_processors = (count > 0) ? count : 0;
}

CHDMAN_API void CHDMAN_CALL chdman_context_cancel(chdman_context *ctx)
{
	if (ctx)
		ctx->session.cancel_requested = true;
}

CHDMAN_API const char *CHDMAN_CALL chdman_last_error(const chdman_context *ctx)
{
	return ctx ? ctx->last_error.c_str() : "invalid context";
}

CHDMAN_API void CHDMAN_CALL chdman_free(void *ptr)
{
	std::free(ptr);
}


//**************************************************************************
//  INFO
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_info_options_init(chdman_info_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_info_open(chdman_context *ctx, const chdman_info_options *opts, chdman_info **out_info)
{
	return run(ctx, [&] {
		if (!out_info)
			throw chdman::error(CHDMAN_ERR_INVALID_ARGUMENT, "out_info is NULL");
		*out_info = nullptr;
		chdman_info_options const args = read_struct(opts, chdman_info_options_init);
		chdman::info_options o;
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.hunk_stats = args.hunk_stats != 0;
		auto info = std::make_unique<chdman_info>();
		info->result = chdman::info(ctx->session, o);
		*out_info = info.release();
	});
}

CHDMAN_API void CHDMAN_CALL chdman_info_free(chdman_info *info)
{
	delete info;
}

CHDMAN_API uint32_t CHDMAN_CALL chdman_info_version(const chdman_info *info) { return info ? info->result.version : 0; }
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_logical_bytes(const chdman_info *info) { return info ? info->result.logical_bytes : 0; }
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_hunk_bytes(const chdman_info *info) { return info ? info->result.hunk_bytes : 0; }
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_hunk_count(const chdman_info *info) { return info ? info->result.hunk_count : 0; }
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_unit_bytes(const chdman_info *info) { return info ? info->result.unit_bytes : 0; }
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_unit_count(const chdman_info *info) { return info ? info->result.unit_count : 0; }
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_file_bytes(const chdman_info *info) { return info ? info->result.file_bytes : 0; }
CHDMAN_API int CHDMAN_CALL chdman_info_compressed(const chdman_info *info) { return (info && info->result.codecs[0] != 0) ? 1 : 0; }

CHDMAN_API uint32_t CHDMAN_CALL chdman_info_codec(const chdman_info *info, int slot)
{
	if (!info || slot < 0 || slot > 3)
		return 0;
	return info->result.codecs[slot];
}

CHDMAN_API const char *CHDMAN_CALL chdman_info_compression_string(const chdman_info *info) { return info ? info->result.compression.c_str() : ""; }
CHDMAN_API const char *CHDMAN_CALL chdman_info_sha1(const chdman_info *info) { return info ? info->result.sha1.c_str() : ""; }
CHDMAN_API const char *CHDMAN_CALL chdman_info_raw_sha1(const chdman_info *info) { return info ? info->result.raw_sha1.c_str() : ""; }
CHDMAN_API const char *CHDMAN_CALL chdman_info_parent_sha1(const chdman_info *info) { return info ? info->result.parent_sha1.c_str() : ""; }
CHDMAN_API const char *CHDMAN_CALL chdman_info_media_type(const chdman_info *info) { return info ? info->result.media_type.c_str() : ""; }

CHDMAN_API size_t CHDMAN_CALL chdman_info_metadata_count(const chdman_info *info)
{
	return info ? info->result.metadata.size() : 0;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_info_metadata_get(const chdman_info *info, size_t i, chdman_metadata_entry *out_entry)
{
	if (!info || !out_entry || i >= info->result.metadata.size())
		return CHDMAN_ERR_INVALID_ARGUMENT;
	auto const &item = info->result.metadata[i];
	out_entry->tag = item.tag;
	char const chars[4] = { char((item.tag >> 24) & 0xff), char((item.tag >> 16) & 0xff), char((item.tag >> 8) & 0xff), char(item.tag & 0xff) };
	bool printable = true;
	for (char ch : chars)
		printable = printable && std::isprint(static_cast<unsigned char>(ch));
	std::memset(out_entry->tag_string, 0, sizeof(out_entry->tag_string));
	if (printable)
		std::memcpy(out_entry->tag_string, chars, 4);
	out_entry->index = item.index;
	out_entry->flags = item.flags;
	out_entry->data = item.data.empty() ? nullptr : item.data.data();
	out_entry->length = item.data.size();
	return CHDMAN_OK;
}

CHDMAN_API size_t CHDMAN_CALL chdman_info_hunk_stat_count(const chdman_info *info)
{
	return info ? info->result.hunk_stats.size() : 0;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_info_hunk_stat_get(const chdman_info *info, size_t i, chdman_hunk_stat *out_stat)
{
	if (!info || !out_stat || i >= info->result.hunk_stats.size())
		return CHDMAN_ERR_INVALID_ARGUMENT;
	out_stat->name = info->result.hunk_stats[i].name.c_str();
	out_stat->hunks = info->result.hunk_stats[i].hunks;
	return CHDMAN_OK;
}


//**************************************************************************
//  VERIFY
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_verify_options_init(chdman_verify_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
}

CHDMAN_API void CHDMAN_CALL chdman_verify_result_init(chdman_verify_result *result)
{
	if (!result)
		return;
	std::memset(result, 0, sizeof(*result));
	result->struct_size = sizeof(*result);
	result->outcome = CHDMAN_VERIFY_NOT_APPLICABLE;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_verify(chdman_context *ctx, const chdman_verify_options *opts, chdman_verify_result *result)
{
	return run(ctx, [&] {
		chdman_verify_options const args = read_struct(opts, chdman_verify_options_init);
		chdman_verify_result res = read_struct(result, chdman_verify_result_init);
		chdman::verify_options o;
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.fix = args.fix != 0;
		chdman::verify_result r = chdman::verify(ctx->session, o);
		res.outcome = r.outcome;
		copy_sha1(res.header_raw_sha1, r.header_raw_sha1);
		copy_sha1(res.computed_raw_sha1, r.computed_raw_sha1);
		copy_sha1(res.header_sha1, r.header_sha1);
		copy_sha1(res.computed_sha1, r.computed_sha1);
		write_struct(result, res);
		if (!r.message.empty())
			ctx->session.info(r.message);
	});
}


//**************************************************************************
//  CREATE
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_create_raw_options_init(chdman_create_raw_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
	init_range(opts->range);
}

CHDMAN_API void CHDMAN_CALL chdman_create_hd_options_init(chdman_create_hd_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
	init_range(opts->range);
	opts->size = CHDMAN_UNSET;
	opts->template_id = -1;
}

CHDMAN_API void CHDMAN_CALL chdman_create_cd_options_init(chdman_create_cd_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
}

CHDMAN_API void CHDMAN_CALL chdman_create_dvd_options_init(chdman_create_dvd_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
	init_range(opts->range);
}

CHDMAN_API void CHDMAN_CALL chdman_create_ld_options_init(chdman_create_ld_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
	opts->start_frame = CHDMAN_UNSET;
	opts->length_frames = CHDMAN_UNSET;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_create_raw(chdman_context *ctx, const chdman_create_raw_options *opts)
{
	return run(ctx, [&] {
		chdman_create_raw_options const args = read_struct(opts, chdman_create_raw_options_init);
		chdman::create_raw_options o;
		o.out = convert(args.out);
		o.input = req_str(args.input, "Input file name");
		o.range = convert(args.range);
		o.unit_size = opt_nonzero(args.unit_size);
		chdman::create_raw(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_create_hd(chdman_context *ctx, const chdman_create_hd_options *opts)
{
	return run(ctx, [&] {
		chdman_create_hd_options const args = read_struct(opts, chdman_create_hd_options_init);
		chdman::create_hd_options o;
		o.out = convert(args.out);
		o.input = opt_str(args.input);
		o.range = convert(args.range);
		o.size = opt_u64(args.size);
		if (args.template_id >= 0)
			o.template_id = uint32_t(args.template_id);
		o.ident = opt_str(args.ident);
		o.cylinders = opt_nonzero(args.cylinders);
		o.heads = opt_nonzero(args.heads);
		o.sectors = opt_nonzero(args.sectors);
		o.sector_size = opt_nonzero(args.sector_size);
		chdman::create_hd(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_create_cd(chdman_context *ctx, const chdman_create_cd_options *opts)
{
	return run(ctx, [&] {
		chdman_create_cd_options const args = read_struct(opts, chdman_create_cd_options_init);
		chdman::create_cd_options o;
		o.out = convert(args.out);
		o.input = req_str(args.input, "Input file name");
		chdman::create_cd(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_create_dvd(chdman_context *ctx, const chdman_create_dvd_options *opts)
{
	return run(ctx, [&] {
		chdman_create_dvd_options const args = read_struct(opts, chdman_create_dvd_options_init);
		chdman::create_dvd_options o;
		o.out = convert(args.out);
		o.input = req_str(args.input, "Input file name");
		o.range = convert(args.range);
		chdman::create_dvd(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_create_ld(chdman_context *ctx, const chdman_create_ld_options *opts)
{
	return run(ctx, [&] {
		chdman_create_ld_options const args = read_struct(opts, chdman_create_ld_options_init);
		chdman::create_ld_options o;
		o.out = convert(args.out);
		o.input = req_str(args.input, "Input file name");
		o.range.start_frame = opt_u64(args.start_frame);
		o.range.length_frames = opt_u64(args.length_frames);
		chdman::create_ld(ctx->session, o);
	});
}


//**************************************************************************
//  COPY
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_copy_options_init(chdman_copy_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_output(opts->out);
	init_range(opts->range);
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_copy(chdman_context *ctx, const chdman_copy_options *opts)
{
	return run(ctx, [&] {
		chdman_copy_options const args = read_struct(opts, chdman_copy_options_init);
		chdman::copy_options o;
		o.out = convert(args.out);
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.range = convert(args.range);
		chdman::copy(ctx->session, o);
	});
}


//**************************************************************************
//  EXTRACT
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_extract_raw_options_init(chdman_extract_raw_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	init_range(opts->range);
}

CHDMAN_API void CHDMAN_CALL chdman_extract_cd_options_init(chdman_extract_cd_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
}

CHDMAN_API void CHDMAN_CALL chdman_extract_ld_options_init(chdman_extract_ld_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
	opts->start_frame = CHDMAN_UNSET;
	opts->length_frames = CHDMAN_UNSET;
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_raw(chdman_context *ctx, const chdman_extract_raw_options *opts)
{
	return run(ctx, [&] {
		chdman_extract_raw_options const args = read_struct(opts, chdman_extract_raw_options_init);
		chdman::extract_raw_options o;
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.output = req_str(args.output, "Output file name");
		o.force = args.force != 0;
		o.range = convert(args.range);
		chdman::extract_raw(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_cd(chdman_context *ctx, const chdman_extract_cd_options *opts)
{
	return run(ctx, [&] {
		chdman_extract_cd_options const args = read_struct(opts, chdman_extract_cd_options_init);
		chdman::extract_cd_options o;
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.output = req_str(args.output, "Output file name");
		o.output_bin = opt_str(args.output_bin);
		o.split_bin = args.split_bin != 0;
		o.force = args.force != 0;
		chdman::extract_cd(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_ld(chdman_context *ctx, const chdman_extract_ld_options *opts)
{
	return run(ctx, [&] {
		chdman_extract_ld_options const args = read_struct(opts, chdman_extract_ld_options_init);
		chdman::extract_ld_options o;
		o.input = req_str(args.input, "Input file name");
		o.input_parent = opt_str(args.input_parent);
		o.output = req_str(args.output, "Output file name");
		o.force = args.force != 0;
		o.range.start_frame = opt_u64(args.start_frame);
		o.range.length_frames = opt_u64(args.length_frames);
		chdman::extract_ld(ctx->session, o);
	});
}


//**************************************************************************
//  METADATA
//**************************************************************************

CHDMAN_API void CHDMAN_CALL chdman_add_metadata_options_init(chdman_add_metadata_options *opts)
{
	if (!opts)
		return;
	std::memset(opts, 0, sizeof(*opts));
	opts->struct_size = sizeof(*opts);
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_add_metadata(chdman_context *ctx, const chdman_add_metadata_options *opts)
{
	return run(ctx, [&] {
		chdman_add_metadata_options const args = read_struct(opts, chdman_add_metadata_options_init);
		chdman::add_metadata_options o;
		o.ref.input = req_str(args.input, "Input file name");
		o.ref.input_parent = opt_str(args.input_parent);
		o.ref.tag = req_str(args.tag, "Metadata tag");
		o.ref.index = args.index;
		if (args.value_text)
			o.value_text = std::string(args.value_text);
		o.value_file = opt_str(args.value_file);
		if (args.value_data)
			o.value_data.emplace(args.value_data, args.value_data + args.value_length);
		o.no_checksum = args.no_checksum != 0;
		chdman::add_metadata(ctx->session, o);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_del_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index)
{
	return run(ctx, [&] {
		chdman::metadata_ref ref;
		ref.input = req_str(input, "Input file name");
		ref.input_parent = opt_str(input_parent);
		ref.tag = req_str(tag, "Metadata tag");
		ref.index = index;
		chdman::del_metadata(ctx->session, ref);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_dump_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index, const char *output, int force)
{
	return run(ctx, [&] {
		chdman::metadata_ref ref;
		ref.input = req_str(input, "Input file name");
		ref.input_parent = opt_str(input_parent);
		ref.tag = req_str(tag, "Metadata tag");
		ref.index = index;
		chdman::dump_metadata(ctx->session, ref, req_str(output, "Output file name"), force != 0);
	});
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_get_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index, uint8_t **out_data, size_t *out_length)
{
	return run(ctx, [&] {
		if (!out_data || !out_length)
			throw chdman::error(CHDMAN_ERR_INVALID_ARGUMENT, "Output pointers are NULL");
		*out_data = nullptr;
		*out_length = 0;
		chdman::metadata_ref ref;
		ref.input = req_str(input, "Input file name");
		ref.input_parent = opt_str(input_parent);
		ref.tag = req_str(tag, "Metadata tag");
		ref.index = index;
		std::vector<uint8_t> data = chdman::get_metadata(ctx->session, ref);
		auto *buffer = static_cast<uint8_t *>(std::malloc(data.empty() ? 1 : data.size()));
		if (!buffer)
			throw std::bad_alloc();
		if (!data.empty())
			std::memcpy(buffer, data.data(), data.size());
		*out_data = buffer;
		*out_length = data.size();
	});
}


//**************************************************************************
//  TEMPLATES
//**************************************************************************

CHDMAN_API size_t CHDMAN_CALL chdman_template_count(void)
{
	return chdman::templates().size();
}

CHDMAN_API chdman_status CHDMAN_CALL chdman_template_get(size_t id, chdman_hd_template *out_template)
{
	auto const &list = chdman::templates();
	if (!out_template || id >= list.size())
		return CHDMAN_ERR_INVALID_ARGUMENT;
	out_template->manufacturer = list[id].manufacturer;
	out_template->model = list[id].model;
	out_template->cylinders = list[id].cylinders;
	out_template->heads = list[id].heads;
	out_template->sectors = list[id].sectors;
	out_template->sector_size = list[id].sector_size;
	return CHDMAN_OK;
}

} // extern "C"
