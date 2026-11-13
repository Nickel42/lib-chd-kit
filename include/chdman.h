/*
 * chdman.h - C ABI for lib-chd-kit (MAME 0.289 CHD tooling as a library)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Conventions
 * -----------
 *  - All strings are UTF-8 and NUL-terminated. Paths are UTF-8 on every
 *    platform (including Windows).
 *  - Every operation takes a chdman_context. A context is not thread-safe:
 *    use one context per thread. Separate contexts may run concurrently.
 *    chdman_context_cancel() is the one call that may be made from any thread.
 *  - Functions return chdman_status. On failure, chdman_last_error(ctx)
 *    returns a human-readable message that stays valid until the next call
 *    on the same context.
 *  - No exceptions, no stdout/stderr output, no process exit. Diagnostics go
 *    to the optional log callback.
 *  - Option and result structs carry a struct_size field and must be
 *    initialized with the matching chdman_*_init() function before use. New
 *    fields are only ever appended. The library accepts any struct_size from
 *    the ABI 1 size upward: fields beyond the caller's struct_size take their
 *    *_init() defaults, fields the library doesn't know are ignored, and a
 *    result struct is written only up to its struct_size. A struct_size below
 *    the ABI 1 size fails with CHDMAN_ERR_ABI_MISMATCH.
 *  - chdman_output_options and chdman_range_options are embedded in other
 *    structs without a struct_size of their own, so their layout is frozen.
 *    New output or range settings will be appended to the outer structs.
 *  - Numeric "not specified" values use CHDMAN_UNSET (all bits set).
 */
#ifndef CHDMAN_H
#define CHDMAN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(CHDMAN_BUILDING_SHARED)
#    define CHDMAN_API __declspec(dllexport)
#  elif defined(CHDMAN_STATIC)
#    define CHDMAN_API
#  else
#    define CHDMAN_API __declspec(dllimport)
#  endif
#  define CHDMAN_CALL __cdecl
#else
#  if defined(CHDMAN_BUILDING_SHARED) || defined(CHDMAN_BUILDING_STATIC)
#    define CHDMAN_API __attribute__((visibility("default")))
#  else
#    define CHDMAN_API
#  endif
#  define CHDMAN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CHDMAN_ABI_VERSION 1
#define CHDMAN_UNSET UINT64_MAX

/* ------------------------------------------------------------------ status */

typedef enum chdman_status {
	CHDMAN_OK = 0,
	CHDMAN_ERR_INVALID_ARGUMENT = 1, /* bad option value or combination */
	CHDMAN_ERR_FILE_EXISTS = 2,      /* output exists and force not set */
	CHDMAN_ERR_FILE_OPEN = 3,        /* could not open an input or output */
	CHDMAN_ERR_IO = 4,               /* read/write failure */
	CHDMAN_ERR_CHD = 5,              /* CHD library error (see message) */
	CHDMAN_ERR_UNSUPPORTED = 6,      /* unsupported format/version */
	CHDMAN_ERR_NOT_FOUND = 7,        /* metadata (or similar) not found */
	CHDMAN_ERR_CANCELLED = 8,        /* cancelled via callback or cancel() */
	CHDMAN_ERR_OUT_OF_MEMORY = 9,
	CHDMAN_ERR_INTERNAL = 10,
	CHDMAN_ERR_ABI_MISMATCH = 11     /* struct_size not recognised */
} chdman_status;

/* Static English description of a status code. Never NULL. */
CHDMAN_API const char *CHDMAN_CALL chdman_status_string(chdman_status status);

/* Library version, e.g. "0.289 (mame0289) lib-chd-kit 1". */
CHDMAN_API const char *CHDMAN_CALL chdman_version(void);
CHDMAN_API int CHDMAN_CALL chdman_abi_version(void);

/* ----------------------------------------------------------------- context */

typedef struct chdman_context chdman_context;

typedef enum chdman_phase {
	CHDMAN_PHASE_WALKING_PARENT = 0, /* scanning output parent for dedupe */
	CHDMAN_PHASE_COMPRESSING = 1,
	CHDMAN_PHASE_VERIFYING = 2,
	CHDMAN_PHASE_EXTRACTING = 3,
	CHDMAN_PHASE_ANALYZING = 4       /* info: hunk statistics */
} chdman_phase;

/*
 * Progress callback. fraction is 0.0..1.0. ratio is the running
 * compressed/uncompressed ratio while compressing, otherwise 0.
 * Return non-zero to request cancellation.
 * Called on the thread that invoked the operation.
 */
typedef int (CHDMAN_CALL *chdman_progress_fn)(void *user, chdman_phase phase, double fraction, double ratio);

typedef enum chdman_log_level {
	CHDMAN_LOG_INFO = 0,
	CHDMAN_LOG_WARNING = 1,
	CHDMAN_LOG_ERROR = 2   /* the cause of a failure, reported as it happens */
} chdman_log_level;

/*
 * Log callback for diagnostics: the informational lines and warnings that
 * chdman prints, plus the info, warning and error lines from MAME's parsers
 * at the matching level (e.g. CHDMAN_LOG_ERROR "ERROR: couldn't find bin
 * file [x.bin]"). An error line is also appended to chdman_last_error() if
 * the operation fails. Treat unknown levels as CHDMAN_LOG_WARNING.
 * Called on the thread that invoked the operation.
 */
typedef void (CHDMAN_CALL *chdman_log_fn)(void *user, chdman_log_level level, const char *message);

CHDMAN_API chdman_status CHDMAN_CALL chdman_context_create(chdman_context **out_ctx);
CHDMAN_API void CHDMAN_CALL chdman_context_destroy(chdman_context *ctx);

CHDMAN_API void CHDMAN_CALL chdman_context_set_progress(chdman_context *ctx, chdman_progress_fn fn, void *user);
CHDMAN_API void CHDMAN_CALL chdman_context_set_log(chdman_context *ctx, chdman_log_fn fn, void *user);

/* Limit worker threads used for compression (0 = all processors). */
CHDMAN_API void CHDMAN_CALL chdman_context_set_num_processors(chdman_context *ctx, int count);

/*
 * Request cancellation of the operation running on ctx. Thread-safe.
 * If no operation is running yet, the request is kept and the next
 * operation on ctx fails with CHDMAN_ERR_CANCELLED before doing any work,
 * so a cancel racing with the start of an operation is never lost.
 * The request is cleared when an operation finishes, however it ends.
 */
CHDMAN_API void CHDMAN_CALL chdman_context_cancel(chdman_context *ctx);

/*
 * Message for the last failed call on ctx ("" after success). Never NULL.
 * When MAME reported a more specific cause than its error code (for example
 * a .cue naming a missing .bin), that cause is appended after ": ".
 */
CHDMAN_API const char *CHDMAN_CALL chdman_last_error(const chdman_context *ctx);

/* Free memory returned by the library (e.g. chdman_get_metadata). */
CHDMAN_API void CHDMAN_CALL chdman_free(void *ptr);

/* -------------------------------------------------------------------- info */

typedef struct chdman_info chdman_info;

typedef struct chdman_info_options {
	size_t struct_size;
	const char *input;          /* required */
	const char *input_parent;   /* optional */
	int hunk_stats;             /* non-zero: compute per-codec hunk counts (verbose info) */
} chdman_info_options;

CHDMAN_API void CHDMAN_CALL chdman_info_options_init(chdman_info_options *opts);

CHDMAN_API chdman_status CHDMAN_CALL chdman_info_open(chdman_context *ctx, const chdman_info_options *opts, chdman_info **out_info);
CHDMAN_API void CHDMAN_CALL chdman_info_free(chdman_info *info);

CHDMAN_API uint32_t CHDMAN_CALL chdman_info_version(const chdman_info *info);
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_logical_bytes(const chdman_info *info);
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_hunk_bytes(const chdman_info *info);
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_hunk_count(const chdman_info *info);
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_unit_bytes(const chdman_info *info);
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_unit_count(const chdman_info *info);
CHDMAN_API uint64_t CHDMAN_CALL chdman_info_file_bytes(const chdman_info *info);
CHDMAN_API int CHDMAN_CALL chdman_info_compressed(const chdman_info *info);

/* Codec slot 0..3. Returns the 4CC (e.g. 'lzma' = 0x6c7a6d61) or 0 for none. */
CHDMAN_API uint32_t CHDMAN_CALL chdman_info_codec(const chdman_info *info, int slot);
/* Friendly codec list, e.g. "lzma (LZMA), zlib (Deflate)" or "none". */
CHDMAN_API const char *CHDMAN_CALL chdman_info_compression_string(const chdman_info *info);

/* 40-char lowercase hex SHA-1, or "" when the header has none. */
CHDMAN_API const char *CHDMAN_CALL chdman_info_sha1(const chdman_info *info);
CHDMAN_API const char *CHDMAN_CALL chdman_info_raw_sha1(const chdman_info *info);
CHDMAN_API const char *CHDMAN_CALL chdman_info_parent_sha1(const chdman_info *info);

/* "hd", "cd", "gd", "dvd", "av" or "raw" (detected from metadata). */
CHDMAN_API const char *CHDMAN_CALL chdman_info_media_type(const chdman_info *info);

typedef struct chdman_metadata_entry {
	uint32_t tag;          /* big-endian packed 4CC, e.g. 'GDDD' */
	char tag_string[5];    /* printable tag or "" if non-printable */
	uint32_t index;        /* instance index among entries with the same tag */
	uint8_t flags;         /* CHDMAN_METADATA_FLAG_CHECKSUM */
	const uint8_t *data;   /* owned by the info handle */
	size_t length;
} chdman_metadata_entry;

#define CHDMAN_METADATA_FLAG_CHECKSUM 0x01

CHDMAN_API size_t CHDMAN_CALL chdman_info_metadata_count(const chdman_info *info);
CHDMAN_API chdman_status CHDMAN_CALL chdman_info_metadata_get(const chdman_info *info, size_t i, chdman_metadata_entry *out_entry);

/* Hunk statistics (only when opts.hunk_stats was set; otherwise count is 0). */
typedef struct chdman_hunk_stat {
	const char *name;      /* "Uncompressed", "Copy from self", codec name, ... */
	uint32_t hunks;
} chdman_hunk_stat;

CHDMAN_API size_t CHDMAN_CALL chdman_info_hunk_stat_count(const chdman_info *info);
CHDMAN_API chdman_status CHDMAN_CALL chdman_info_hunk_stat_get(const chdman_info *info, size_t i, chdman_hunk_stat *out_stat);

/* ------------------------------------------------------------------ verify */

typedef struct chdman_verify_options {
	size_t struct_size;
	const char *input;          /* required */
	const char *input_parent;   /* optional */
	int fix;                    /* non-zero: rewrite header SHA-1s if wrong */
} chdman_verify_options;

typedef enum chdman_verify_outcome {
	CHDMAN_VERIFY_OK = 0,             /* raw (and overall, v4+) SHA-1 match */
	CHDMAN_VERIFY_RAW_MISMATCH = 1,   /* data SHA-1 differs from header */
	CHDMAN_VERIFY_OVERALL_MISMATCH = 2, /* data ok, metadata-inclusive SHA-1 differs */
	CHDMAN_VERIFY_FIXED = 3,          /* mismatch found and header corrected */
	CHDMAN_VERIFY_NOT_APPLICABLE = 4  /* uncompressed CHD or no checksum */
} chdman_verify_outcome;

typedef struct chdman_verify_result {
	size_t struct_size;
	chdman_verify_outcome outcome;
	char header_raw_sha1[41];
	char computed_raw_sha1[41];
	char header_sha1[41];       /* overall SHA-1 (v4+), "" otherwise */
	char computed_sha1[41];
} chdman_verify_result;

CHDMAN_API void CHDMAN_CALL chdman_verify_options_init(chdman_verify_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_verify_result_init(chdman_verify_result *result);
/* Returns CHDMAN_OK whenever verification ran; check result->outcome. */
CHDMAN_API chdman_status CHDMAN_CALL chdman_verify(chdman_context *ctx, const chdman_verify_options *opts, chdman_verify_result *result);

/* ------------------------------------------------------------------ create */

/*
 * Shared output settings for create and copy. Frozen: this struct has no
 * struct_size, so it can never grow. New output settings will be appended
 * to the outer chdman_create_*_options / chdman_copy_options structs.
 *   compression: NULL = default for the media type, "none", or up to four
 *                comma-separated 4CCs ("lzma,zlib,huff,flac").
 *   hunk_size:   0 = default for the media type.
 */
typedef struct chdman_output_options {
	const char *output;         /* required */
	const char *output_parent;  /* optional */
	int force;                  /* overwrite an existing output */
	uint32_t hunk_size;
	const char *compression;
} chdman_output_options;

/*
 * Input range selection; at most one start and one length field may be set.
 * Frozen like chdman_output_options: new fields go in the outer structs.
 */
typedef struct chdman_range_options {
	uint64_t start_byte;        /* CHDMAN_UNSET when not used */
	uint64_t start_hunk;
	uint64_t length_bytes;
	uint64_t length_hunks;
} chdman_range_options;

typedef struct chdman_create_raw_options {
	size_t struct_size;
	chdman_output_options out;
	const char *input;          /* required */
	chdman_range_options range;
	uint32_t unit_size;         /* required unless output_parent given */
} chdman_create_raw_options;

typedef struct chdman_create_hd_options {
	size_t struct_size;
	chdman_output_options out;
	const char *input;          /* optional: NULL creates a blank (zeroed) disk */
	chdman_range_options range;
	uint64_t size;              /* blank disk size in bytes; CHDMAN_UNSET when not used */
	int template_id;            /* -1 = none; see chdman_template_get */
	const char *ident;          /* optional IDENTIFY data file (CHS from it) */
	uint32_t cylinders;         /* 0 = guess / from template */
	uint32_t heads;
	uint32_t sectors;
	uint32_t sector_size;       /* 0 = 512 or template/parent */
} chdman_create_hd_options;

typedef struct chdman_create_cd_options {
	size_t struct_size;
	chdman_output_options out;
	const char *input;          /* .cue, .toc, .gdi, .nrg, .iso, ... */
} chdman_create_cd_options;

typedef struct chdman_create_dvd_options {
	size_t struct_size;
	chdman_output_options out;
	const char *input;          /* raw 2048-byte-sector image */
	chdman_range_options range;
} chdman_create_dvd_options;

typedef struct chdman_create_ld_options {
	size_t struct_size;
	chdman_output_options out;
	const char *input;          /* AVI (YUY2 video, 16-bit PCM audio) */
	uint64_t start_frame;       /* CHDMAN_UNSET when not used */
	uint64_t length_frames;     /* CHDMAN_UNSET when not used */
} chdman_create_ld_options;

CHDMAN_API void CHDMAN_CALL chdman_create_raw_options_init(chdman_create_raw_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_create_hd_options_init(chdman_create_hd_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_create_cd_options_init(chdman_create_cd_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_create_dvd_options_init(chdman_create_dvd_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_create_ld_options_init(chdman_create_ld_options *opts);

/* On failure or cancellation the partially written output is deleted. */
CHDMAN_API chdman_status CHDMAN_CALL chdman_create_raw(chdman_context *ctx, const chdman_create_raw_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_create_hd(chdman_context *ctx, const chdman_create_hd_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_create_cd(chdman_context *ctx, const chdman_create_cd_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_create_dvd(chdman_context *ctx, const chdman_create_dvd_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_create_ld(chdman_context *ctx, const chdman_create_ld_options *opts);

/* -------------------------------------------------------------------- copy */

typedef struct chdman_copy_options {
	size_t struct_size;
	chdman_output_options out;  /* compression NULL = default for detected type */
	const char *input;          /* required */
	const char *input_parent;   /* optional */
	chdman_range_options range;
} chdman_copy_options;

CHDMAN_API void CHDMAN_CALL chdman_copy_options_init(chdman_copy_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_copy(chdman_context *ctx, const chdman_copy_options *opts);

/* ----------------------------------------------------------------- extract */

/* Raw extraction; also used for hard disk and DVD CHDs (extracthd/extractdvd). */
typedef struct chdman_extract_raw_options {
	size_t struct_size;
	const char *input;          /* required */
	const char *input_parent;
	const char *output;         /* required */
	int force;
	chdman_range_options range;
} chdman_extract_raw_options;

/*
 * CD extraction. The TOC format follows the output extension:
 * ".cue" -> CUE/BIN, ".gdi" -> GDI, anything else -> cdrdao TOC.
 * output_bin: NULL = derived from output; may contain %t / %02t for the
 * track number (required when split_bin is set).
 */
typedef struct chdman_extract_cd_options {
	size_t struct_size;
	const char *input;
	const char *input_parent;
	const char *output;
	const char *output_bin;
	int split_bin;
	int force;
} chdman_extract_cd_options;

typedef struct chdman_extract_ld_options {
	size_t struct_size;
	const char *input;
	const char *input_parent;
	const char *output;         /* AVI */
	int force;
	uint64_t start_frame;       /* CHDMAN_UNSET when not used */
	uint64_t length_frames;
} chdman_extract_ld_options;

CHDMAN_API void CHDMAN_CALL chdman_extract_raw_options_init(chdman_extract_raw_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_extract_cd_options_init(chdman_extract_cd_options *opts);
CHDMAN_API void CHDMAN_CALL chdman_extract_ld_options_init(chdman_extract_ld_options *opts);

CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_raw(chdman_context *ctx, const chdman_extract_raw_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_cd(chdman_context *ctx, const chdman_extract_cd_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_ld(chdman_context *ctx, const chdman_extract_ld_options *opts);

/* --------------------------------------------------------- extract to sink */

/*
 * Streaming extraction: the files chdman_extract_raw / chdman_extract_cd
 * would write are passed to a callback instead, so nothing touches the disk.
 * The output and output_bin options only pick the format and name the files;
 * force is ignored.
 *
 * Each file arrives as one CHDMAN_SINK_BEGIN_FILE, then zero or more
 * CHDMAN_SINK_DATA blocks holding its bytes in order, then one
 * CHDMAN_SINK_END_FILE. name is the path the file would have been written to
 * and is set for every event; data and length are set for CHDMAN_SINK_DATA
 * only. The data pointer is valid only during the call. Blocks are at most
 * 32 MiB and, for CD data files, hold whole frames.
 *
 * chdman_extract_cd_to_sink delivers the TOC (.cue, .gdi or cdrdao .toc,
 * with the platform's line endings) first, then the data files in track
 * order. The bytes are identical to what chdman_extract_cd writes.
 *
 * Return non-zero from the callback to cancel: the call then fails with
 * CHDMAN_ERR_CANCELLED. Called on the thread that invoked the operation.
 */
typedef enum chdman_sink_event {
	CHDMAN_SINK_BEGIN_FILE = 0,
	CHDMAN_SINK_DATA = 1,
	CHDMAN_SINK_END_FILE = 2
} chdman_sink_event;

typedef int (CHDMAN_CALL *chdman_sink_fn)(void *user, chdman_sink_event event, const char *name, const uint8_t *data, size_t length);

CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_raw_to_sink(chdman_context *ctx, const chdman_extract_raw_options *opts, chdman_sink_fn fn, void *user);
CHDMAN_API chdman_status CHDMAN_CALL chdman_extract_cd_to_sink(chdman_context *ctx, const chdman_extract_cd_options *opts, chdman_sink_fn fn, void *user);

/* ---------------------------------------------------------------- metadata */

/*
 * Tags are given as strings of up to 4 characters (padded with spaces, as
 * chdman does). Exactly one of value_text, value_file or value_data
 * (with value_length) must be supplied.
 */
typedef struct chdman_add_metadata_options {
	size_t struct_size;
	const char *input;          /* opened read/write */
	const char *input_parent;
	const char *tag;
	uint32_t index;
	const char *value_text;     /* stored with a trailing NUL, like chdman */
	const char *value_file;
	const uint8_t *value_data;
	size_t value_length;
	int no_checksum;            /* exclude from the overall SHA-1 */
} chdman_add_metadata_options;

CHDMAN_API void CHDMAN_CALL chdman_add_metadata_options_init(chdman_add_metadata_options *opts);
CHDMAN_API chdman_status CHDMAN_CALL chdman_add_metadata(chdman_context *ctx, const chdman_add_metadata_options *opts);

CHDMAN_API chdman_status CHDMAN_CALL chdman_del_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index);

/* Write one metadata item to a file (dumpmeta --output). */
CHDMAN_API chdman_status CHDMAN_CALL chdman_dump_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index, const char *output, int force);

/* Read one metadata item into memory. Free *out_data with chdman_free. */
CHDMAN_API chdman_status CHDMAN_CALL chdman_get_metadata(chdman_context *ctx, const char *input, const char *input_parent, const char *tag, uint32_t index, uint8_t **out_data, size_t *out_length);

/* --------------------------------------------------------------- templates */

typedef struct chdman_hd_template {
	const char *manufacturer;   /* static strings */
	const char *model;
	uint32_t cylinders;
	uint32_t heads;
	uint32_t sectors;
	uint32_t sector_size;
} chdman_hd_template;

CHDMAN_API size_t CHDMAN_CALL chdman_template_count(void);
CHDMAN_API chdman_status CHDMAN_CALL chdman_template_get(size_t id, chdman_hd_template *out_template);

#ifdef __cplusplus
}
#endif

#endif /* CHDMAN_H */
