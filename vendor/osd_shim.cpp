// license:BSD-3-Clause
// Minimal OSD shim for lib-chd-kit.
//
// MAME's osdsync.cpp (work queues/threads) and the file modules need a few
// helpers that upstream implements in src/osd/modules/lib/osdlib_*.cpp, which
// drag in a lot of unrelated code (dynamic modules, clipboard, process
// control). This file provides only what the CHD code path needs.

#include "osdcore.h"
#include "osdfile.h"

#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#endif

const char *osd_getenv(const char *name)
{
#ifdef _WIN32
	// getenv() on Windows is ANSI-codepage; MAME only queries ASCII names
	// (OSDPROCESSORS, OSDWORKQUEUEMAXTHREADS) so the CRT version is fine, but
	// keep returned pointers stable across calls the same way POSIX does.
	static std::mutex lock;
	static std::unordered_map<std::string, std::string> cache;
	char const *const value = std::getenv(name);
	if (!value)
		return nullptr;
	std::lock_guard<std::mutex> guard(lock);
	auto &slot = cache[name];
	slot = value;
	return slot.c_str();
#else
	return std::getenv(name);
#endif
}


// MAME's file layer also understands "socket.host:port", "domain.path" and
// pseudo-terminal paths. CHD files never use them, so instead of vendoring the
// socket/pty modules the openers report "not supported" and the path checks
// report "not a special path" (so every path is treated as a regular file).
#ifdef _WIN32

#include "winfile.h"

bool win_init_sockets() noexcept { return true; }
void win_cleanup_sockets() noexcept { }
bool win_check_socket_path(std::string const &path) noexcept { return false; }
std::error_condition win_open_socket(std::string const &path, std::uint32_t openflags, osd_file::ptr &file, std::uint64_t &filesize) noexcept { return std::errc::not_supported; }
bool win_check_ptty_path(std::string const &path) noexcept { return false; }
std::error_condition win_open_ptty(std::string const &path, std::uint32_t openflags, osd_file::ptr &file, std::uint64_t &filesize) noexcept { return std::errc::not_supported; }

#else

#include "posixfile.h"

bool posix_check_socket_path(std::string const &path) noexcept { return false; }
std::error_condition posix_open_socket(std::string const &path, std::uint32_t openflags, osd_file::ptr &file, std::uint64_t &filesize) noexcept { return std::errc::not_supported; }
bool posix_check_domain_path(std::string const &path) noexcept { return false; }
std::error_condition posix_open_domain(std::string const &path, std::uint32_t openflags, osd_file::ptr &file, std::uint64_t &filesize) noexcept { return std::errc::not_supported; }
bool posix_check_ptty_path(std::string const &path) noexcept { return false; }
std::error_condition posix_open_ptty(std::uint32_t openflags, osd_file::ptr &file, std::uint64_t &filesize, std::string &name) noexcept { return std::errc::not_supported; }

#endif
