// license:BSD-3-Clause
// lib-chd-kit tests. Uses only the public C API (plus a test-only AVI writer)
// and an independent SHA-1 implementation as the reference.

#include "chdman.h"
#include "sha1.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

extern "C" int chdman_test_write_avi(const char *path, int width, int height, int frames, int fps, int rate, int channels);

namespace {

int g_failures = 0;
int g_checks = 0;
fs::path g_work;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_OK(ctx, expr) do { ++g_checks; chdman_status _st = (expr); if (_st != CHDMAN_OK) { ++g_failures; std::fprintf(stderr, "  %s:%d: %s -> %s: %s\n", __FILE__, __LINE__, #expr, chdman_status_string(_st), chdman_last_error(ctx)); } } while (0)
#define CHECK_EQ_STR(a, b) do { ++g_checks; std::string _a = (a), _b = (b); if (_a != _b) { ++g_failures; std::fprintf(stderr, "  CHECK failed %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)

std::string path(const char *name) { return (g_work / name).string(); }

std::vector<uint8_t> read_file(const std::string &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_file(const std::string &p, const std::vector<uint8_t> &data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}

std::string sha1_hex(const std::vector<uint8_t> &data)
{
	test_sha1 s;
	char out[41];
	test_sha1_init(&s);
	test_sha1_update(&s, data.data(), data.size());
	test_sha1_final_hex(&s, out);
	return out;
}

// Deterministic test pattern: a mix of random, compressible, zero and
// repeated regions so every codec and the self-copy path get exercised.
std::vector<uint8_t> make_raw_image(size_t size)
{
	std::vector<uint8_t> data(size);
	uint32_t seed = 0x1234567u;
	for (size_t i = 0; i < size; i++)
	{
		size_t const region = (i / 65536) % 4;
		seed = seed * 1664525u + 1013904223u;
		switch (region)
		{
		case 0: data[i] = uint8_t(seed >> 24); break;                       // noise
		case 1: data[i] = uint8_t((i / 7) & 0xff); break;                   // ramps
		case 2: data[i] = 0; break;                                         // zeroes
		case 3: data[i] = data[i - 3 * 65536]; break;                       // repeat of region 0
		}
	}
	return data;
}

struct context
{
	chdman_context *ctx = nullptr;
	context() { chdman_context_create(&ctx); }
	~context() { chdman_context_destroy(ctx); }
	operator chdman_context *() const { return ctx; }
};

std::string info_raw_sha1(chdman_context *ctx, const std::string &chd, std::string *overall = nullptr, std::string *media = nullptr)
{
	chdman_info_options o;
	chdman_info_options_init(&o);
	o.input = chd.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &o, &info));
	if (!info)
		return std::string();
	std::string result = chdman_info_raw_sha1(info);
	if (overall)
		*overall = chdman_info_sha1(info);
	if (media)
		*media = chdman_info_media_type(info);
	chdman_info_free(info);
	return result;
}

chdman_verify_outcome verify(chdman_context *ctx, const std::string &chd, bool fix = false)
{
	chdman_verify_options o;
	chdman_verify_result r;
	chdman_verify_options_init(&o);
	chdman_verify_result_init(&r);
	o.input = chd.c_str();
	o.fix = fix;
	CHECK_OK(ctx, chdman_verify(ctx, &o, &r));
	return r.outcome;
}

chdman_status create_raw(chdman_context *ctx, const std::string &in, const std::string &out, const char *compression = nullptr, bool force = false)
{
	chdman_create_raw_options o;
	chdman_create_raw_options_init(&o);
	o.input = in.c_str();
	o.out.output = out.c_str();
	o.out.compression = compression;
	o.out.force = force;
	o.unit_size = 512;
	return chdman_create_raw(ctx, &o);
}

chdman_status extract_raw(chdman_context *ctx, const std::string &in, const std::string &out)
{
	chdman_extract_raw_options o;
	chdman_extract_raw_options_init(&o);
	o.input = in.c_str();
	o.output = out.c_str();
	o.force = 1;
	return chdman_extract_raw(ctx, &o);
}

// ---------------------------------------------------------------- tests

void test_templates()
{
	CHECK(chdman_template_count() == 17);
	chdman_hd_template t;
	CHECK(chdman_template_get(0, &t) == CHDMAN_OK);
	CHECK_EQ_STR(t.manufacturer, "Conner");
	CHECK_EQ_STR(t.model, "CFA170A");
	CHECK(t.cylinders == 332 && t.heads == 16 && t.sectors == 63 && t.sector_size == 512);
	CHECK(chdman_template_get(17, &t) == CHDMAN_ERR_INVALID_ARGUMENT);
	CHECK(chdman_abi_version() == CHDMAN_ABI_VERSION);
}

// Expected header SHA-1s for make_raw_image(RAW_SIZE) stored as a raw CHD
// with 512-byte units and no metadata. Pinned so every platform/build must
// produce byte-identical header checksums.
constexpr size_t RAW_SIZE = 12 * 65536 + 12345 * 512; // unit multiple, not a hunk multiple
const char *const GOLDEN_RAW_SHA1 = "54fd023fbb2e8f1a0eca501c34186d058cf23290";
const char *const GOLDEN_OVERALL_SHA1 = "6207aff990aeb52258dcdf687421ebeffe9cfe69";

void test_raw_roundtrip()
{
	context ctx;
	auto const data = make_raw_image(RAW_SIZE);
	write_file(path("raw.img"), data);
	std::string const expected_sha1 = sha1_hex(data);

	CHECK_OK(ctx, create_raw(ctx, path("raw.img"), path("raw.chd")));

	chdman_info_options io;
	chdman_info_options_init(&io);
	std::string const chd = path("raw.chd");
	io.input = chd.c_str();
	io.hunk_stats = 1;
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	if (info)
	{
		CHECK(chdman_info_version(info) == 5);
		CHECK(chdman_info_logical_bytes(info) == RAW_SIZE);
		CHECK(chdman_info_hunk_bytes(info) == 4096);          // default for unit 512
		CHECK(chdman_info_unit_bytes(info) == 512);
		CHECK(chdman_info_hunk_count(info) == (RAW_SIZE + 4095) / 4096);
		CHECK(chdman_info_codec(info, 0) == 0x6c7a6d61);       // 'lzma'
		CHECK(chdman_info_codec(info, 1) == 0x7a6c6962);       // 'zlib'
		CHECK(chdman_info_codec(info, 2) == 0x68756666);       // 'huff'
		CHECK(chdman_info_codec(info, 3) == 0x666c6163);       // 'flac'
		CHECK_EQ_STR(chdman_info_media_type(info), "raw");
		CHECK_EQ_STR(chdman_info_raw_sha1(info), expected_sha1);
		CHECK(chdman_info_metadata_count(info) == 0);
		CHECK(chdman_info_file_bytes(info) < RAW_SIZE);
		uint32_t total = 0;
		bool saw_self = false;
		for (size_t i = 0; i < chdman_info_hunk_stat_count(info); i++)
		{
			chdman_hunk_stat st;
			CHECK(chdman_info_hunk_stat_get(info, i, &st) == CHDMAN_OK);
			total += st.hunks;
			saw_self = saw_self || std::string(st.name) == "Copy from self";
		}
		CHECK(total == chdman_info_hunk_count(info));
		CHECK(saw_self); // repeated region dedupes
		std::printf("  raw: %llu -> %llu bytes, raw sha1 %s, sha1 %s\n",
				(unsigned long long)RAW_SIZE, (unsigned long long)chdman_info_file_bytes(info),
				chdman_info_raw_sha1(info), chdman_info_sha1(info));
		std::string const golden_raw = GOLDEN_RAW_SHA1, golden_overall = GOLDEN_OVERALL_SHA1;
		if (golden_raw[0] != '@')
		{
			CHECK_EQ_STR(chdman_info_raw_sha1(info), golden_raw);
			CHECK_EQ_STR(chdman_info_sha1(info), golden_overall);
		}
		chdman_info_free(info);
	}

	CHECK(verify(ctx, chd) == CHDMAN_VERIFY_OK);

	CHECK_OK(ctx, extract_raw(ctx, chd, path("raw.out")));
	CHECK(read_file(path("raw.out")) == data);

	// partial extraction by hunks
	chdman_extract_raw_options eo;
	chdman_extract_raw_options_init(&eo);
	eo.input = chd.c_str();
	std::string const part = path("raw.part");
	eo.output = part.c_str();
	eo.force = 1;
	eo.range.start_hunk = 2;
	eo.range.length_bytes = 10000;
	CHECK_OK(ctx, chdman_extract_raw(ctx, &eo));
	CHECK(read_file(part) == std::vector<uint8_t>(data.begin() + 8192, data.begin() + 8192 + 10000));
}

void test_determinism()
{
	context ctx;
	auto const data = make_raw_image(RAW_SIZE);
	write_file(path("det.img"), data);

	// single-threaded vs. all processors must give byte-identical CHDs
	chdman_context_set_num_processors(ctx, 1);
	CHECK_OK(ctx, create_raw(ctx, path("det.img"), path("det1.chd"), nullptr, true));
	chdman_context_set_num_processors(ctx, 0);
	CHECK_OK(ctx, create_raw(ctx, path("det.img"), path("det2.chd"), nullptr, true));
	CHECK_OK(ctx, create_raw(ctx, path("det.img"), path("det3.chd"), nullptr, true));

	std::string o1, o2, o3;
	std::string const r1 = info_raw_sha1(ctx, path("det1.chd"), &o1);
	std::string const r2 = info_raw_sha1(ctx, path("det2.chd"), &o2);
	std::string const r3 = info_raw_sha1(ctx, path("det3.chd"), &o3);
	CHECK(!r1.empty());
	CHECK_EQ_STR(r1, r2);
	CHECK_EQ_STR(r2, r3);
	CHECK_EQ_STR(o1, o2);
	CHECK_EQ_STR(o2, o3);
	CHECK_EQ_STR(r1, sha1_hex(data));
	CHECK(read_file(path("det1.chd")) == read_file(path("det2.chd")));
	CHECK(read_file(path("det2.chd")) == read_file(path("det3.chd")));

	// uncompressed CHDs carry no checksum, so there is nothing to verify
	CHECK_OK(ctx, create_raw(ctx, path("det.img"), path("det_none.chd"), "none", true));
	CHECK(info_raw_sha1(ctx, path("det_none.chd")).empty());
	CHECK(verify(ctx, path("det_none.chd")) == CHDMAN_VERIFY_NOT_APPLICABLE);
	CHECK_OK(ctx, extract_raw(ctx, path("det_none.chd"), path("det_none.out")));
	CHECK(read_file(path("det_none.out")) == data);
}

void test_copy_and_codecs()
{
	context ctx;
	auto const data = make_raw_image(RAW_SIZE);
	write_file(path("copy.img"), data);
	CHECK_OK(ctx, create_raw(ctx, path("copy.img"), path("copy_src.chd"), "zlib", true));

	// copy, recompressing with zstd and a larger hunk size
	chdman_copy_options co;
	chdman_copy_options_init(&co);
	std::string const src = path("copy_src.chd"), dst = path("copy_dst.chd");
	co.input = src.c_str();
	co.out.output = dst.c_str();
	co.out.force = 1;
	co.out.compression = "zstd";
	co.out.hunk_size = 8192;
	CHECK_OK(ctx, chdman_copy(ctx, &co));

	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = dst.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	if (info)
	{
		CHECK(chdman_info_codec(info, 0) == 0x7a737464); // 'zstd'
		CHECK(chdman_info_codec(info, 1) == 0);
		CHECK(chdman_info_hunk_bytes(info) == 8192);
		CHECK_EQ_STR(chdman_info_raw_sha1(info), sha1_hex(data));
		chdman_info_free(info);
	}
	CHECK(verify(ctx, dst) == CHDMAN_VERIFY_OK);
	CHECK_OK(ctx, extract_raw(ctx, dst, path("copy.out")));
	CHECK(read_file(path("copy.out")) == data);
}

void test_metadata_and_fix()
{
	context ctx;
	auto const data = make_raw_image(65536 * 2);
	write_file(path("meta.img"), data);
	// like upstream chdman, metadata can only be edited in uncompressed CHDs
	std::string const compressed = path("meta_c.chd");
	CHECK_OK(ctx, create_raw(ctx, path("meta.img"), compressed, nullptr, true));
	CHECK(chdman_del_metadata(ctx, compressed.c_str(), nullptr, "TEST", 0) == CHDMAN_ERR_CHD);
	CHECK(std::string(chdman_last_error(ctx)).find("not writeable") != std::string::npos);

	std::string const chd = path("meta.chd");
	CHECK_OK(ctx, create_raw(ctx, path("meta.img"), chd, "none", true));

	chdman_add_metadata_options mo;
	chdman_add_metadata_options_init(&mo);
	mo.input = chd.c_str();
	mo.tag = "TEST";
	mo.value_text = "hello world";
	CHECK_OK(ctx, chdman_add_metadata(ctx, &mo));

	// binary metadata, excluded from the checksum
	uint8_t const blob[] = { 1, 2, 3, 0, 255 };
	chdman_add_metadata_options_init(&mo);
	mo.input = chd.c_str();
	mo.tag = "BIN";          // padded to "BIN "
	mo.value_data = blob;
	mo.value_length = sizeof(blob);
	mo.no_checksum = 1;
	CHECK_OK(ctx, chdman_add_metadata(ctx, &mo));

	// neither/both value sources are rejected
	chdman_add_metadata_options_init(&mo);
	mo.input = chd.c_str();
	mo.tag = "TEST";
	CHECK(chdman_add_metadata(ctx, &mo) == CHDMAN_ERR_INVALID_ARGUMENT);

	uint8_t *out = nullptr;
	size_t len = 0;
	CHECK_OK(ctx, chdman_get_metadata(ctx, chd.c_str(), nullptr, "TEST", 0, &out, &len));
	CHECK(len == 12 && out && std::memcmp(out, "hello world", 12) == 0); // stored with NUL
	chdman_free(out);
	CHECK_OK(ctx, chdman_get_metadata(ctx, chd.c_str(), nullptr, "BIN", 0, &out, &len));
	CHECK(len == sizeof(blob) && out && std::memcmp(out, blob, sizeof(blob)) == 0);
	chdman_free(out);

	CHECK_OK(ctx, chdman_dump_metadata(ctx, chd.c_str(), nullptr, "TEST", 0, path("meta.txt").c_str(), 1));
	CHECK(read_file(path("meta.txt")).size() == 12);

	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = chd.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	if (info)
	{
		CHECK(chdman_info_metadata_count(info) == 2);
		chdman_metadata_entry e;
		CHECK(chdman_info_metadata_get(info, 0, &e) == CHDMAN_OK);
		CHECK_EQ_STR(e.tag_string, "TEST");
		CHECK(e.flags == CHDMAN_METADATA_FLAG_CHECKSUM);
		CHECK(chdman_info_metadata_get(info, 1, &e) == CHDMAN_OK);
		CHECK_EQ_STR(e.tag_string, "BIN ");
		CHECK(e.flags == 0);
		chdman_info_free(info);
	}
	CHECK(verify(ctx, chd) == CHDMAN_VERIFY_NOT_APPLICABLE); // uncompressed

	CHECK_OK(ctx, chdman_del_metadata(ctx, chd.c_str(), nullptr, "TEST", 0));
	CHECK(chdman_get_metadata(ctx, chd.c_str(), nullptr, "TEST", 0, &out, &len) == CHDMAN_ERR_NOT_FOUND);
	CHECK(std::strlen(chdman_last_error(ctx)) > 0);
	CHECK_OK(ctx, extract_raw(ctx, chd, path("meta.out")));
	CHECK(read_file(path("meta.out")) == data);

	// corrupt the header raw SHA-1 (v5 header offset 64) of a compressed
	// CHD, then verify/fix
	std::string const fixchd = compressed;
	{
		std::fstream f(fixchd, std::ios::in | std::ios::out | std::ios::binary);
		f.seekp(64);
		char byte = 0;
		f.read(&byte, 1);
		byte ^= 0x5a;
		f.seekp(64);
		f.write(&byte, 1);
	}
	CHECK(verify(ctx, fixchd) == CHDMAN_VERIFY_RAW_MISMATCH);
	CHECK(verify(ctx, fixchd, true) == CHDMAN_VERIFY_FIXED);
	CHECK(verify(ctx, fixchd) == CHDMAN_VERIFY_OK);
	CHECK_EQ_STR(info_raw_sha1(ctx, fixchd), sha1_hex(data));
}

// Writes a synthetic two-track disc: MODE1/2352 data (300 frames) followed by
// AUDIO (151 frames, deliberately not a multiple of the 4-frame CHD padding).
void make_cue_bin(const std::string &cue, const std::string &bin, const char *binname, std::vector<uint8_t> &bindata)
{
	uint32_t seed = 0xc0ffee;
	bindata.assign((300 + 151) * 2352, 0);
	for (size_t frame = 0; frame < 300 + 151; frame++)
	{
		uint8_t *sector = &bindata[frame * 2352];
		if (frame < 300)
		{
			static const uint8_t sync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
			std::memcpy(sector, sync, 12);
			sector[15] = 1; // mode 1
			for (int i = 16; i < 2064; i++)
			{
				seed = seed * 1103515245u + 12345u;
				sector[i] = (i & 64) ? uint8_t(seed >> 24) : uint8_t(i + frame);
			}
		}
		else
		{
			for (int i = 0; i < 2352; i += 2)
			{
				int16_t const sample = int16_t(((frame * 588 + i / 2) * 37) & 0x7fff) - 0x4000;
				sector[i] = uint8_t(sample & 0xff);
				sector[i + 1] = uint8_t(sample >> 8);
			}
		}
	}
	write_file(bin, bindata);
	std::ofstream f(cue, std::ios::trunc);
	f << "FILE \"" << binname << "\" BINARY\n"
	  << "  TRACK 01 MODE1/2352\n"
	  << "    INDEX 01 00:00:00\n"
	  << "  TRACK 02 AUDIO\n"
	  << "    INDEX 01 00:04:00\n";
}

void test_cd_roundtrip()
{
	context ctx;
	std::vector<uint8_t> bindata;
	make_cue_bin(path("disc.cue"), path("disc.bin"), "disc.bin", bindata);

	chdman_create_cd_options co;
	chdman_create_cd_options_init(&co);
	std::string const cue = path("disc.cue"), chd = path("disc.chd");
	co.input = cue.c_str();
	co.out.output = chd.c_str();
	co.out.force = 1;
	CHECK_OK(ctx, chdman_create_cd(ctx, &co));

	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = chd.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	std::string raw1, overall1;
	if (info)
	{
		CHECK_EQ_STR(chdman_info_media_type(info), "cd");
		CHECK(chdman_info_unit_bytes(info) == 2448);
		CHECK(chdman_info_hunk_bytes(info) == 19584);           // 8 frames
		CHECK(chdman_info_codec(info, 0) == 0x63646c7a);        // 'cdlz'
		CHECK(chdman_info_codec(info, 1) == 0x63647a6c);        // 'cdzl'
		CHECK(chdman_info_codec(info, 2) == 0x6364666c);        // 'cdfl'
		CHECK(chdman_info_logical_bytes(info) == uint64_t(300 + 152) * 2448); // audio padded to 152
		CHECK(chdman_info_metadata_count(info) == 2);
		chdman_metadata_entry e;
		if (chdman_info_metadata_get(info, 1, &e) == CHDMAN_OK)
		{
			CHECK_EQ_STR(e.tag_string, "CHT2");
			CHECK(e.index == 1);
			std::string const text(reinterpret_cast<const char *>(e.data), e.length);
			CHECK(text.find("TYPE:AUDIO") != std::string::npos);
			CHECK(text.find("FRAMES:151") != std::string::npos);
		}
		raw1 = chdman_info_raw_sha1(info);
		overall1 = chdman_info_sha1(info);
		std::printf("  cd: raw sha1 %s, sha1 %s\n", raw1.c_str(), overall1.c_str());
		// pinned: CD frames + CHT2 metadata must hash identically everywhere
		CHECK_EQ_STR(raw1, "70beb418eb9c68da5158db9abcf45879f7690ed7");
		CHECK_EQ_STR(overall1, "40662ec18a87f5eda56ea79461777fc25180b929");
		chdman_info_free(info);
	}
	CHECK(verify(ctx, chd) == CHDMAN_VERIFY_OK);

	// extract back to cue/bin and compare the bin byte-for-byte
	chdman_extract_cd_options eo;
	chdman_extract_cd_options_init(&eo);
	std::string const outcue = path("out.cue");
	eo.input = chd.c_str();
	eo.output = outcue.c_str();
	eo.force = 1;
	CHECK_OK(ctx, chdman_extract_cd(ctx, &eo));
	CHECK(read_file(path("out.bin")) == bindata);
	std::vector<uint8_t> const cuetext = read_file(outcue);
	std::string const cuestr(cuetext.begin(), cuetext.end());
	CHECK(cuestr.find("TRACK 01 MODE1/2352") != std::string::npos);
	CHECK(cuestr.find("TRACK 02 AUDIO") != std::string::npos);
	CHECK(cuestr.find("INDEX 01 00:04:00") != std::string::npos);

	// the extracted cue/bin recreates an identical CHD (same header SHA-1s)
	chdman_create_cd_options_init(&co);
	std::string const chd2 = path("disc2.chd");
	co.input = outcue.c_str();
	co.out.output = chd2.c_str();
	co.out.force = 1;
	CHECK_OK(ctx, chdman_create_cd(ctx, &co));
	std::string overall2;
	CHECK_EQ_STR(info_raw_sha1(ctx, chd2, &overall2), raw1);
	CHECK_EQ_STR(overall2, overall1);

	// split-bin extraction requires %t in an explicit output bin name
	chdman_extract_cd_options_init(&eo);
	eo.input = chd.c_str();
	std::string const splitcue = path("split.cue"), splitbin = path("split_%02t.bin");
	eo.output = splitcue.c_str();
	eo.output_bin = splitbin.c_str();
	eo.split_bin = 1;
	eo.force = 1;
	CHECK_OK(ctx, chdman_extract_cd(ctx, &eo));
	auto const t1 = read_file(path("split_01.bin"));
	auto const t2 = read_file(path("split_02.bin"));
	CHECK(t1.size() == 300 * 2352 && t2.size() == 151 * 2352);
	CHECK(std::equal(t1.begin(), t1.end(), bindata.begin()));
	CHECK(std::equal(t2.begin(), t2.end(), bindata.begin() + t1.size()));

	// copy of a CD defaults to CD codecs
	chdman_copy_options cpo;
	chdman_copy_options_init(&cpo);
	std::string const chd3 = path("disc3.chd");
	cpo.input = chd.c_str();
	cpo.out.output = chd3.c_str();
	cpo.out.force = 1;
	CHECK_OK(ctx, chdman_copy(ctx, &cpo));
	std::string overall3, media3;
	CHECK_EQ_STR(info_raw_sha1(ctx, chd3, &overall3, &media3), raw1);
	CHECK_EQ_STR(overall3, overall1);
	CHECK_EQ_STR(media3, "cd");
}

void test_dvd_and_hd()
{
	context ctx;
	auto data = make_raw_image(2048 * 300);
	write_file(path("dvd.iso"), data);

	chdman_create_dvd_options dv;
	chdman_create_dvd_options_init(&dv);
	std::string const iso = path("dvd.iso"), chd = path("dvd.chd");
	dv.input = iso.c_str();
	dv.out.output = chd.c_str();
	dv.out.force = 1;
	CHECK_OK(ctx, chdman_create_dvd(ctx, &dv));
	std::string media;
	CHECK_EQ_STR(info_raw_sha1(ctx, chd, nullptr, &media), sha1_hex(data));
	CHECK_EQ_STR(media, "dvd");
	CHECK_OK(ctx, extract_raw(ctx, chd, path("dvd.out")));
	CHECK(read_file(path("dvd.out")) == data);

	// hard disk from an image with explicit CHS
	auto hd = make_raw_image(4 * 16 * 63 * 512);
	write_file(path("hd.img"), hd);
	chdman_create_hd_options ho;
	chdman_create_hd_options_init(&ho);
	std::string const hdimg = path("hd.img"), hdchd = path("hd.chd");
	ho.input = hdimg.c_str();
	ho.out.output = hdchd.c_str();
	ho.out.force = 1;
	ho.cylinders = 4;
	ho.heads = 16;
	ho.sectors = 63;
	CHECK_OK(ctx, chdman_create_hd(ctx, &ho));
	uint8_t *meta = nullptr;
	size_t len = 0;
	CHECK_OK(ctx, chdman_get_metadata(ctx, hdchd.c_str(), nullptr, "GDDD", 0, &meta, &len));
	if (meta)
	{
		CHECK_EQ_STR(std::string(reinterpret_cast<char *>(meta)), "CYLS:4,HEADS:16,SECS:63,BPS:512");
		chdman_free(meta);
	}
	CHECK_EQ_STR(info_raw_sha1(ctx, hdchd, nullptr, &media), sha1_hex(hd));
	CHECK_EQ_STR(media, "hd");

	// blank hard disk from a template; must be uncompressed
	chdman_create_hd_options_init(&ho);
	std::string const blank = path("blank.chd");
	ho.out.output = blank.c_str();
	ho.out.force = 1;
	ho.template_id = 1; // Rodime R0201: 321/2/16
	CHECK_OK(ctx, chdman_create_hd(ctx, &ho));
	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = blank.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	if (info)
	{
		CHECK(chdman_info_logical_bytes(info) == uint64_t(321) * 2 * 16 * 512);
		CHECK(!chdman_info_compressed(info));
		chdman_info_free(info);
	}
	ho.out.compression = "zlib";
	CHECK(chdman_create_hd(ctx, &ho) == CHDMAN_ERR_INVALID_ARGUMENT);

	// template + CHS conflict
	chdman_create_hd_options_init(&ho);
	ho.out.output = blank.c_str();
	ho.out.force = 1;
	ho.template_id = 1;
	ho.cylinders = 1; ho.heads = 1; ho.sectors = 1;
	CHECK(chdman_create_hd(ctx, &ho) == CHDMAN_ERR_INVALID_ARGUMENT);
}

void test_ld_roundtrip()
{
	context ctx;
	std::string const avi = path("ld.avi"), chd = path("ld.chd"), out = path("ld_out.avi"), chd2 = path("ld2.chd");
	CHECK(chdman_test_write_avi(avi.c_str(), 160, 120, 12, 30, 48000, 2) == 1);

	chdman_create_ld_options lo;
	chdman_create_ld_options_init(&lo);
	lo.input = avi.c_str();
	lo.out.output = chd.c_str();
	lo.out.force = 1;
	CHECK_OK(ctx, chdman_create_ld(ctx, &lo));

	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = chd.c_str();
	chdman_info *info = nullptr;
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	std::string raw1;
	if (info)
	{
		CHECK_EQ_STR(chdman_info_media_type(info), "av");
		CHECK(chdman_info_hunk_count(info) == 12);
		CHECK(chdman_info_codec(info, 0) == 0x61766875); // 'avhu'
		raw1 = chdman_info_raw_sha1(info);
		chdman_info_free(info);
	}
	CHECK(verify(ctx, chd) == CHDMAN_VERIFY_OK);

	chdman_extract_ld_options eo;
	chdman_extract_ld_options_init(&eo);
	eo.input = chd.c_str();
	eo.output = out.c_str();
	eo.force = 1;
	CHECK_OK(ctx, chdman_extract_ld(ctx, &eo));
	CHECK(fs::exists(out) && fs::file_size(out) > 160 * 120 * 2 * 12);

	// avhuff is lossless: re-creating from the extracted AVI gives the same data
	lo.input = out.c_str();
	lo.out.output = chd2.c_str();
	CHECK_OK(ctx, chdman_create_ld(ctx, &lo));
	CHECK_EQ_STR(info_raw_sha1(ctx, chd2), raw1);

	// a sub-range of frames
	chdman_create_ld_options_init(&lo);
	std::string const chd3 = path("ld3.chd");
	lo.input = avi.c_str();
	lo.out.output = chd3.c_str();
	lo.out.force = 1;
	lo.start_frame = 2;
	lo.length_frames = 5;
	CHECK_OK(ctx, chdman_create_ld(ctx, &lo));
	chdman_info_options_init(&io);
	io.input = chd3.c_str();
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	if (info)
	{
		CHECK(chdman_info_hunk_count(info) == 5);
		chdman_info_free(info);
	}
}

void test_errors_and_cancel()
{
	context ctx;
	auto const data = make_raw_image(65536 * 8);
	write_file(path("err.img"), data);
	std::string const chd = path("err.chd");
	CHECK_OK(ctx, create_raw(ctx, path("err.img"), chd, nullptr, true));

	// existing output without force
	CHECK(create_raw(ctx, path("err.img"), chd) == CHDMAN_ERR_FILE_EXISTS);
	CHECK(std::string(chdman_last_error(ctx)).find("already exists") != std::string::npos);
	CHECK(fs::exists(chd)); // untouched

	// bad codec / hunk size / missing unit size / bad input
	CHECK(create_raw(ctx, path("err.img"), path("err2.chd"), "abcd", true) == CHDMAN_ERR_INVALID_ARGUMENT);
	CHECK(std::string(chdman_last_error(ctx)).find("Invalid compressor 'abcd'") != std::string::npos);
	CHECK(!fs::exists(path("err2.chd")));
	chdman_create_raw_options ro;
	chdman_create_raw_options_init(&ro);
	std::string const img = path("err.img"), out2 = path("err2.chd");
	ro.input = img.c_str();
	ro.out.output = out2.c_str();
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_INVALID_ARGUMENT); // unit size required
	ro.unit_size = 512;
	ro.out.hunk_size = 1000;                                           // not a multiple of 512
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_INVALID_ARGUMENT);
	ro.out.hunk_size = 0;
	ro.range.start_byte = 0;
	ro.range.start_hunk = 1;                                           // conflicting start
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_INVALID_ARGUMENT);
	ro.struct_size = 8;                                                // ABI check
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_ABI_MISMATCH);
	CHECK(create_raw(ctx, path("does_not_exist.img"), out2, nullptr, true) == CHDMAN_ERR_FILE_OPEN);
	CHECK(extract_raw(ctx, path("err.img"), path("x.out")) != CHDMAN_OK); // not a CHD
	CHECK(chdman_create_raw(nullptr, &ro) == CHDMAN_ERR_INVALID_ARGUMENT);

	// cancel from the progress callback: output must be removed
	int calls = 0;
	chdman_context_set_progress(ctx, [](void *user, chdman_phase, double, double) -> int {
		return ++*static_cast<int *>(user) >= 1 ? 1 : 0;
	}, &calls);
	CHECK(create_raw(ctx, path("err.img"), path("cancel.chd"), nullptr, true) == CHDMAN_ERR_CANCELLED);
	CHECK(calls >= 1);
	CHECK(!fs::exists(path("cancel.chd")));
	CHECK(extract_raw(ctx, chd, path("cancel.out")) == CHDMAN_ERR_CANCELLED);
	CHECK(!fs::exists(path("cancel.out")));

	// a cancel from the progress callback doesn't leak into the next operation
	chdman_context_set_progress(ctx, nullptr, nullptr);
	CHECK_OK(ctx, create_raw(ctx, path("err.img"), path("after_cb_cancel.chd"), nullptr, true));

	// chdman_context_cancel() before start (here from another thread) is not
	// lost: it cancels the next operation, then is cleared
	std::thread([c = ctx.ctx] { chdman_context_cancel(c); }).join();
	CHECK(create_raw(ctx, path("err.img"), path("early_cancel.chd"), nullptr, true) == CHDMAN_ERR_CANCELLED);
	CHECK(std::string(chdman_last_error(ctx)).find("cancelled") != std::string::npos);
	CHECK(!fs::exists(path("early_cancel.chd")));
	CHECK_OK(ctx, create_raw(ctx, path("err.img"), path("after_cancel.chd"), nullptr, true));
	CHECK(std::strlen(chdman_last_error(ctx)) == 0);

	// it also applies to operations that report no progress
	chdman_context_cancel(ctx);
	chdman_info_options io;
	chdman_info_options_init(&io);
	io.input = chd.c_str();
	chdman_info *info = nullptr;
	CHECK(chdman_info_open(ctx, &io, &info) == CHDMAN_ERR_CANCELLED);
	CHECK(info == nullptr);
	CHECK_OK(ctx, chdman_info_open(ctx, &io, &info));
	chdman_info_free(info);

	// a cancel during an operation is cleared when it finishes
	chdman_context_set_progress(ctx, [](void *user, chdman_phase, double, double) -> int {
		chdman_context_cancel(static_cast<chdman_context *>(user));
		return 0;
	}, ctx.ctx);
	CHECK(create_raw(ctx, path("err.img"), path("mid_cancel.chd"), nullptr, true) == CHDMAN_ERR_CANCELLED);
	CHECK(!fs::exists(path("mid_cancel.chd")));
	chdman_context_set_progress(ctx, nullptr, nullptr);
	CHECK_OK(ctx, create_raw(ctx, path("err.img"), path("after_mid_cancel.chd"), nullptr, true));

	// log callback receives chdman's informational lines
	std::vector<std::string> lines;
	chdman_context_set_log(ctx, [](void *user, chdman_log_level, const char *msg) {
		static_cast<std::vector<std::string> *>(user)->push_back(msg);
	}, &lines);
	CHECK_OK(ctx, create_raw(ctx, path("err.img"), path("log.chd"), nullptr, true));
	bool saw_complete = false;
	for (auto const &l : lines)
		saw_complete = saw_complete || l.find("Compression complete") != std::string::npos;
	CHECK(saw_complete);
}

void test_struct_size()
{
	context ctx;
	auto const data = make_raw_image(65536 * 2);
	write_file(path("ss.img"), data);
	std::string const img = path("ss.img"), out = path("ss.chd");

	// a struct_size that stops at the end of the last v1 field (no trailing
	// padding) is accepted; one that cuts off a v1 field is not
	chdman_create_raw_options ro;
	chdman_create_raw_options_init(&ro);
	ro.input = img.c_str();
	ro.out.output = out.c_str();
	ro.out.force = 1;
	ro.unit_size = 512;
	ro.struct_size = offsetof(chdman_create_raw_options, unit_size) + sizeof(ro.unit_size);
	CHECK_OK(ctx, chdman_create_raw(ctx, &ro));
	ro.struct_size = offsetof(chdman_create_raw_options, unit_size);
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_ABI_MISMATCH);
	CHECK(std::string(chdman_last_error(ctx)).find("struct_size") != std::string::npos);
	ro.struct_size = 0;
	CHECK(chdman_create_raw(ctx, &ro) == CHDMAN_ERR_ABI_MISMATCH);

	// a caller built against a newer header passes a larger struct; the
	// unknown trailing fields are ignored
	struct newer_raw { chdman_create_raw_options v1; uint64_t future[4]; } nr;
	std::memset(&nr, 0xa5, sizeof(nr));
	chdman_create_raw_options_init(&nr.v1);
	nr.v1.struct_size = sizeof(nr);
	nr.v1.input = img.c_str();
	nr.v1.out.output = out.c_str();
	nr.v1.out.force = 1;
	nr.v1.unit_size = 512;
	CHECK_OK(ctx, chdman_create_raw(ctx, &nr.v1));
	std::string const sha = info_raw_sha1(ctx, out);
	CHECK_EQ_STR(sha, sha1_hex(data));

	// a larger result struct is written only up to the library's own size
	struct newer_result { chdman_verify_result v1; uint8_t future[32]; } vr;
	std::memset(&vr, 0xa5, sizeof(vr));
	chdman_verify_result_init(&vr.v1);
	vr.v1.struct_size = sizeof(vr);
	chdman_verify_options vo;
	chdman_verify_options_init(&vo);
	vo.input = out.c_str();
	CHECK_OK(ctx, chdman_verify(ctx, &vo, &vr.v1));
	CHECK(vr.v1.outcome == CHDMAN_VERIFY_OK);
	CHECK(vr.v1.struct_size == sizeof(vr));
	CHECK_EQ_STR(vr.v1.computed_raw_sha1, sha);
	bool untouched = true;
	for (uint8_t b : vr.future)
		untouched = untouched && b == 0xa5;
	CHECK(untouched);

	// a result struct cut short is rejected before anything is written
	chdman_verify_result_init(&vr.v1);
	vr.v1.struct_size = offsetof(chdman_verify_result, computed_sha1);
	CHECK(chdman_verify(ctx, &vo, &vr.v1) == CHDMAN_ERR_ABI_MISMATCH);
	CHECK(vr.v1.outcome == CHDMAN_VERIFY_NOT_APPLICABLE);
}

void test_cd_parse_errors()
{
	context ctx;
	std::vector<std::pair<chdman_log_level, std::string>> lines;
	chdman_context_set_log(ctx, [](void *user, chdman_log_level level, const char *msg) {
		static_cast<std::vector<std::pair<chdman_log_level, std::string>> *>(user)->emplace_back(level, msg);
	}, &lines);

	// a .cue that names a .bin that isn't there: MAME prints the cause and
	// returns a generic error, so the cause must reach last_error and the log
	std::string const cue = path("missing_bin.cue"), chd = path("missing_bin.chd");
	{
		std::ofstream f(cue, std::ios::trunc);
		f << "FILE \"no_such_track.bin\" BINARY\n"
		  << "  TRACK 01 MODE1/2352\n"
		  << "    INDEX 01 00:00:00\n";
	}
	chdman_create_cd_options co;
	chdman_create_cd_options_init(&co);
	co.input = cue.c_str();
	co.out.output = chd.c_str();
	co.out.force = 1;
	CHECK(chdman_create_cd(ctx, &co) != CHDMAN_OK);
	std::string const err = chdman_last_error(ctx);
	CHECK(err.find("Error parsing input file") != std::string::npos);
	CHECK(err.find("couldn't find bin file") != std::string::npos);
	CHECK(err.find("no_such_track.bin") != std::string::npos);
	CHECK(!fs::exists(chd));
	bool logged = false;
	for (auto const &l : lines)
		logged = logged || (l.first == CHDMAN_LOG_ERROR &&l.second.find("couldn't find bin file") != std::string::npos && l.second.back() != '\n');
	CHECK(logged);

	// the detail belongs to that operation only
	auto const data = make_raw_image(65536);
	write_file(path("cdp.img"), data);
	CHECK(create_raw(ctx, path("cdp.img"), path("cdp.chd"), "abcd", true) == CHDMAN_ERR_INVALID_ARGUMENT);
	CHECK(std::string(chdman_last_error(ctx)).find("couldn't find bin file") == std::string::npos);

	// without a log callback the error message still carries the cause
	context quiet;
	CHECK(chdman_create_cd(quiet, &co) != CHDMAN_OK);
	CHECK(std::string(chdman_last_error(quiet)).find("couldn't find bin file") != std::string::npos);
}

} // anonymous namespace

int main()
{
	g_work = fs::path(CHDMAN_TEST_WORKDIR);
	std::error_code ec;
	fs::remove_all(g_work, ec);
	fs::create_directories(g_work);
	std::printf("lib-chd-kit %s, work dir %s\n", chdman_version(), g_work.string().c_str());

	struct { const char *name; void (*fn)(); } const tests[] = {
		{ "templates", test_templates },
		{ "raw_roundtrip", test_raw_roundtrip },
		{ "determinism", test_determinism },
		{ "copy_and_codecs", test_copy_and_codecs },
		{ "metadata_and_fix", test_metadata_and_fix },
		{ "cd_roundtrip", test_cd_roundtrip },
		{ "dvd_and_hd", test_dvd_and_hd },
		{ "ld_roundtrip", test_ld_roundtrip },
		{ "errors_and_cancel", test_errors_and_cancel },
		{ "struct_size", test_struct_size },
		{ "cd_parse_errors", test_cd_parse_errors },
	};
	for (auto const &t : tests)
	{
		int const before = g_failures;
		std::printf("[ RUN  ] %s\n", t.name);
		std::fflush(stdout);
		t.fn();
		std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
