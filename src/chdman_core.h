// license:BSD-3-Clause
// lib-chd-kit core: chdman's do_* commands refactored into a library.
//
// Internal C++ interface. Errors are reported by throwing chdman::error; the
// C ABI layer (chdman_capi.cpp) catches everything at the boundary.

#ifndef LIBCHDKIT_CORE_H
#define LIBCHDKIT_CORE_H

#include "chdman.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace chdman {

// ======================> error

class error : public std::runtime_error
{
public:
	error(chdman_status status, std::string const &message) : std::runtime_error(message), m_status(status) { }
	chdman_status status() const noexcept { return m_status; }

private:
	chdman_status m_status;
};


// ======================> session
//
// Per-operation environment: progress/log callbacks, cancellation and
// thread-count limit. Replaces chdman's globals (lastprogress, std::cout,
// osd_num_processors).

class session
{
public:
	std::function<bool (chdman_phase, double, double)> progress_cb; // return true to cancel
	std::function<void (chdman_log_level, std::string const &)> log_cb;
	std::atomic<bool> cancel_requested{ false };
	int num_processors = 0;

	// resets progress throttling and MAME diagnostics; called as each operation
	// starts. Leaves cancel_requested alone so a cancel that arrives just
	// before the operation still cancels it.
	void begin_operation() noexcept { m_last_progress_ns = -1; m_last_osd_error.clear(); }
	// clears cancellation; called as each operation finishes, however it ends
	void end_operation() noexcept { cancel_requested = false; }

	// reports progress (throttled unless force) and throws if cancelled
	void progress(chdman_phase phase, double fraction, double ratio = 0.0, bool force = false);
	void check_cancel();
	void info(std::string const &message) const;
	void warning(std::string const &message) const;

	// MAME's osd_printf_error/warning/info output from this thread while a
	// session_scope for this session is active
	void osd_message(chdman_log_level level, std::string message);
	// last error line MAME printed during the current operation ("" if none)
	std::string const &last_osd_error() const noexcept { return m_last_osd_error; }

private:
	int64_t m_last_progress_ns = -1;
	std::string m_last_osd_error;
};


// ======================> session_scope
//
// Routes MAME's osd_printf_* output on the calling thread to a session for
// the lifetime of the scope. The process-wide osd_output that does this is
// registered once, on first use; output with no active scope is dropped.

class session_scope
{
public:
	explicit session_scope(session &s);
	~session_scope();
	session_scope(session_scope const &) = delete;
	session_scope &operator=(session_scope const &) = delete;

private:
	session *m_previous;
};


// ======================> option structs

struct output_options
{
	std::string output;
	std::optional<std::string> output_parent;
	bool force = false;
	std::optional<uint32_t> hunk_size;
	std::optional<std::string> compression; // nullopt = default for media type
};

struct range_options
{
	std::optional<uint64_t> start_byte;
	std::optional<uint64_t> start_hunk;
	std::optional<uint64_t> start_frame;
	std::optional<uint64_t> length_bytes;
	std::optional<uint64_t> length_hunks;
	std::optional<uint64_t> length_frames;
};

struct info_options
{
	std::string input;
	std::optional<std::string> input_parent;
	bool hunk_stats = false;
};

struct verify_options
{
	std::string input;
	std::optional<std::string> input_parent;
	bool fix = false;
};

struct create_raw_options
{
	output_options out;
	std::string input;
	range_options range;
	std::optional<uint32_t> unit_size;
};

struct create_hd_options
{
	output_options out;
	std::optional<std::string> input;
	range_options range;
	std::optional<uint64_t> size;
	std::optional<uint32_t> template_id;
	std::optional<std::string> ident;
	std::optional<uint32_t> cylinders, heads, sectors; // all or none
	std::optional<uint32_t> sector_size;
};

struct create_cd_options
{
	output_options out;
	std::string input;
};

struct create_dvd_options
{
	output_options out;
	std::string input;
	range_options range;
};

struct create_ld_options
{
	output_options out;
	std::string input;
	range_options range; // frames only
};

struct copy_options
{
	output_options out;
	std::string input;
	std::optional<std::string> input_parent;
	range_options range;
};

struct extract_raw_options
{
	std::string input;
	std::optional<std::string> input_parent;
	std::string output;
	bool force = false;
	range_options range;
};

struct extract_cd_options
{
	std::string input;
	std::optional<std::string> input_parent;
	std::string output;
	std::optional<std::string> output_bin;
	bool split_bin = false;
	bool force = false;
};

struct extract_ld_options
{
	std::string input;
	std::optional<std::string> input_parent;
	std::string output;
	bool force = false;
	range_options range; // frames only
};

struct metadata_ref
{
	std::string input;
	std::optional<std::string> input_parent;
	std::string tag;
	uint32_t index = 0;
};

struct add_metadata_options
{
	metadata_ref ref;
	std::optional<std::string> value_text;
	std::optional<std::string> value_file;
	std::optional<std::vector<uint8_t>> value_data;
	bool no_checksum = false;
};


// ======================> results

struct metadata_item
{
	uint32_t tag;
	uint32_t index;
	uint8_t flags;
	std::vector<uint8_t> data;
};

struct hunk_stat
{
	std::string name;
	uint32_t hunks;
};

struct info_result
{
	uint32_t version = 0;
	uint64_t logical_bytes = 0;
	uint32_t hunk_bytes = 0;
	uint32_t hunk_count = 0;
	uint32_t unit_bytes = 0;
	uint64_t unit_count = 0;
	uint64_t file_bytes = 0;
	uint32_t codecs[4] = { 0, 0, 0, 0 };
	std::string compression;
	std::string sha1, raw_sha1, parent_sha1; // "" when null
	std::string media_type;
	std::vector<metadata_item> metadata;
	std::vector<hunk_stat> hunk_stats;
};

struct verify_result
{
	chdman_verify_outcome outcome = CHDMAN_VERIFY_NOT_APPLICABLE;
	std::string header_raw_sha1, computed_raw_sha1;
	std::string header_sha1, computed_sha1;
	std::string message; // why verification was not applicable
};

struct hd_template
{
	const char *manufacturer;
	const char *model;
	uint32_t cylinders;
	uint32_t heads;
	uint32_t sectors;
	uint32_t sector_size;
};


// ======================> operations

info_result info(session &s, info_options const &opts);
verify_result verify(session &s, verify_options const &opts);
void create_raw(session &s, create_raw_options const &opts);
void create_hd(session &s, create_hd_options const &opts);
void create_cd(session &s, create_cd_options const &opts);
void create_dvd(session &s, create_dvd_options const &opts);
void create_ld(session &s, create_ld_options const &opts);
void copy(session &s, copy_options const &opts);
void extract_raw(session &s, extract_raw_options const &opts);
void extract_cd(session &s, extract_cd_options const &opts);
void extract_ld(session &s, extract_ld_options const &opts);
void add_metadata(session &s, add_metadata_options const &opts);
void del_metadata(session &s, metadata_ref const &ref);
std::vector<uint8_t> get_metadata(session &s, metadata_ref const &ref);
void dump_metadata(session &s, metadata_ref const &ref, std::string const &output, bool force);

std::vector<hd_template> const &templates();

// translate any in-flight exception into status + message (call from catch)
chdman_status translate_current_exception(std::string &message) noexcept;

} // namespace chdman

#endif // LIBCHDKIT_CORE_H
