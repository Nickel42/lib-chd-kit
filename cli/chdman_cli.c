/*
 * chdman-cli: a chdman-compatible command line front end built only on the
 * lib-chd-kit C API (chdman.h). Option names, short names and output follow
 * MAME 0.289 chdman.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chdman.h"

#include <ctype.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- options */

enum {
	OPT_INPUT, OPT_INPUT_PARENT, OPT_OUTPUT, OPT_OUTPUT_BIN, OPT_SPLITBIN,
	OPT_FORCE, OPT_OUTPUT_PARENT, OPT_START_BYTE, OPT_START_HUNK,
	OPT_START_FRAME, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, OPT_LENGTH_FRAMES,
	OPT_HUNK_SIZE, OPT_UNIT_SIZE, OPT_COMPRESSION, OPT_IDENT, OPT_CHS,
	OPT_SECTOR_SIZE, OPT_TAG, OPT_INDEX, OPT_VALUE_TEXT, OPT_VALUE_FILE,
	OPT_NUMPROCESSORS, OPT_NO_CHECKSUM, OPT_FIX, OPT_VERBOSE, OPT_SIZE,
	OPT_TEMPLATE, OPT_COUNT
};

typedef struct option_desc {
	const char *name;
	const char *shortname;
	int parameter;
	const char *description;
} option_desc;

static const option_desc s_options[OPT_COUNT] = {
	{ "input",           "i",    1, " <filename>: input file name" },
	{ "inputparent",     "ip",   1, " <filename>: parent file name for input CHD" },
	{ "output",          "o",    1, " <filename>: output file name" },
	{ "outputbin",       "ob",   1, " <filename>: output file name for binary data" },
	{ "splitbin",        "sb",   0, ": output one binary file per track" },
	{ "force",           "f",    0, ": force overwriting an existing file" },
	{ "outputparent",    "op",   1, " <filename>: parent file name for output CHD" },
	{ "inputstartbyte",  "isb",  1, " <offset>: starting byte offset within the input" },
	{ "inputstarthunk",  "ish",  1, " <offset>: starting hunk offset within the input" },
	{ "inputstartframe", "isf",  1, " <offset>: starting frame within the input" },
	{ "inputbytes",      "ib",   1, " <length>: effective length of input in bytes" },
	{ "inputhunks",      "ih",   1, " <length>: effective length of input in hunks" },
	{ "inputframes",     "if",   1, " <length>: effective length of input in frames" },
	{ "hunksize",        "hs",   1, " <bytes>: size of each hunk, in bytes" },
	{ "unitsize",        "us",   1, " <bytes>: size of each unit, in bytes" },
	{ "compression",     "c",    1, " <none|type1[,type2[,...]]>: which compression codecs to use (up to 4)" },
	{ "ident",           "id",   1, " <filename>: name of ident file to provide CHS information" },
	{ "chs",             "chs",  1, " <cylinders,heads,sectors>: specifies CHS geometry directly" },
	{ "sectorsize",      "ss",   1, " <bytes>: size of each hard disk sector" },
	{ "tag",             "t",    1, " <tag>: 4-character tag for metadata" },
	{ "index",           "ix",   1, " <index>: indexed instance of this metadata tag" },
	{ "valuetext",       "vt",   1, " <text>: text for the metadata" },
	{ "valuefile",       "vf",   1, " <file>: file containing data to add" },
	{ "numprocessors",   "np",   1, " <processors>: limit the number of processors to use during compression" },
	{ "nochecksum",      "nocs", 0, ": do not include this metadata information in the overall SHA-1" },
	{ "fix",             "f",    0, ": fix the SHA-1 if it is incorrect" },
	{ "verbose",         "v",    0, ": output additional information" },
	{ "size",            "s",    1, ": <bytes>: size of the output file" },
	{ "template",        "tp",   1, ": <id>: use hard disk template (see listtemplates)" },
};

#define REQ(x) (-(x) - 1)  /* encode "required" as a negative index */
#define END 0x7fff

typedef struct params {
	const char *value[OPT_COUNT];
	int present[OPT_COUNT];
} params;

typedef int (*command_fn)(chdman_context *ctx, params *p);

typedef struct command_desc {
	const char *name;
	command_fn handler;
	const char *description;
	int options[17];
} command_desc;

/* --------------------------------------------------------- utilities */

static chdman_context *g_ctx;
static int g_progress_shown;

static void on_sigint(int sig)
{
	(void)sig;
	if (g_ctx)
		chdman_context_cancel(g_ctx);
}

static uint64_t parse_number(const char *string)
{
	uint64_t result = 0;
	if (!string)
		return 0;
	while (isdigit((unsigned char)*string))
		result = result * 10 + (uint64_t)(*string++ - '0');
	if (*string == 'k' || *string == 'K')
		result *= 1024;
	if (*string == 'm' || *string == 'M')
		result *= 1024 * 1024;
	if (*string == 'g' || *string == 'G')
		result *= 1024 * 1024 * 1024;
	return result;
}

static const char *big_int_string(uint64_t value, char *buf, size_t bufsize)
{
	char tmp[64];
	int len = snprintf(tmp, sizeof(tmp), "%" PRIu64, value);
	size_t out = 0;
	for (int i = 0; i < len && out + 2 < bufsize; i++)
	{
		if (i > 0 && (len - i) % 3 == 0)
			buf[out++] = ',';
		buf[out++] = tmp[i];
	}
	buf[out] = 0;
	return buf;
}

static int CHDMAN_CALL progress_cb(void *user, chdman_phase phase, double fraction, double ratio)
{
	(void)user;
	switch (phase)
	{
	case CHDMAN_PHASE_WALKING_PARENT:
		fprintf(stderr, "Examining parent, %.1f%% complete...  \r", 100.0 * fraction);
		break;
	case CHDMAN_PHASE_COMPRESSING:
		if (fraction < 1.0)
			fprintf(stderr, "Compressing, %.1f%% complete... (ratio=%.1f%%)  \r", 100.0 * fraction, 100.0 * ratio);
		break;
	case CHDMAN_PHASE_VERIFYING:
		fprintf(stderr, "Verifying, %.1f%% complete... \r", 100.0 * fraction);
		break;
	case CHDMAN_PHASE_EXTRACTING:
		fprintf(stderr, "Extracting, %.1f%% complete... \r", 100.0 * fraction);
		break;
	case CHDMAN_PHASE_ANALYZING:
		break;
	}
	fflush(stderr);
	g_progress_shown = 1;
	return 0;
}

static void CHDMAN_CALL log_cb(void *user, chdman_log_level level, const char *message)
{
	(void)user;
	if (g_progress_shown)
	{
		fprintf(stderr, "%70s\r", "");
		g_progress_shown = 0;
	}
	if (level == CHDMAN_LOG_INFO)
		printf("%s\n", message);
	else
		fprintf(stderr, "%s\n", message);
	fflush(stdout);
}

static int report(chdman_context *ctx, chdman_status status)
{
	if (status == CHDMAN_OK)
		return 0;
	if (g_progress_shown)
	{
		fprintf(stderr, "\n");
		g_progress_shown = 0;
	}
	fprintf(stderr, "Error: %s\n", chdman_last_error(ctx));
	return 1;
}

static const char *P(params *p, int opt) { return p->present[opt] ? p->value[opt] : NULL; }
static int HAS(params *p, int opt) { return p->present[opt]; }

static void fill_output(params *p, chdman_output_options *out)
{
	out->output = P(p, OPT_OUTPUT);
	out->output_parent = P(p, OPT_OUTPUT_PARENT);
	out->force = HAS(p, OPT_FORCE);
	if (HAS(p, OPT_HUNK_SIZE))
		out->hunk_size = (uint32_t)parse_number(P(p, OPT_HUNK_SIZE));
	out->compression = P(p, OPT_COMPRESSION);
}

static void fill_range(params *p, chdman_range_options *r)
{
	if (HAS(p, OPT_START_BYTE)) r->start_byte = parse_number(P(p, OPT_START_BYTE));
	if (HAS(p, OPT_START_HUNK)) r->start_hunk = parse_number(P(p, OPT_START_HUNK));
	if (HAS(p, OPT_LENGTH_BYTES)) r->length_bytes = parse_number(P(p, OPT_LENGTH_BYTES));
	if (HAS(p, OPT_LENGTH_HUNKS)) r->length_hunks = parse_number(P(p, OPT_LENGTH_HUNKS));
}

static int check_hunk_size_given(params *p)
{
	/* chdman rejects hunk sizes of 0 via its min-size check; the API treats 0 as default */
	if (HAS(p, OPT_HUNK_SIZE) && parse_number(P(p, OPT_HUNK_SIZE)) == 0)
	{
		fprintf(stderr, "Error: Invalid hunk size (minimum 16)\n");
		return 0;
	}
	return 1;
}

static void apply_numprocessors(chdman_context *ctx, params *p)
{
	if (HAS(p, OPT_NUMPROCESSORS))
		chdman_context_set_num_processors(ctx, atoi(P(p, OPT_NUMPROCESSORS)));
}

/* ---------------------------------------------------------- commands */

static int do_info(chdman_context *ctx, params *p)
{
	chdman_info_options opts;
	chdman_info *info = NULL;
	char buf[64];
	int verbose = HAS(p, OPT_VERBOSE);

	chdman_info_options_init(&opts);
	opts.input = P(p, OPT_INPUT);
	opts.input_parent = P(p, OPT_INPUT_PARENT);
	opts.hunk_stats = verbose;
	if (chdman_info_open(ctx, &opts, &info) != CHDMAN_OK)
		return report(ctx, CHDMAN_ERR_CHD);

	printf("Input file:   %s\n", opts.input);
	printf("File Version: %u\n", chdman_info_version(info));
	printf("Logical size: %s bytes\n", big_int_string(chdman_info_logical_bytes(info), buf, sizeof(buf)));
	printf("Hunk Size:    %s bytes\n", big_int_string(chdman_info_hunk_bytes(info), buf, sizeof(buf)));
	printf("Total Hunks:  %s\n", big_int_string(chdman_info_hunk_count(info), buf, sizeof(buf)));
	printf("Unit Size:    %s bytes\n", big_int_string(chdman_info_unit_bytes(info), buf, sizeof(buf)));
	printf("Total Units:  %s\n", big_int_string(chdman_info_unit_count(info), buf, sizeof(buf)));
	printf("Compression:  %s\n", chdman_info_compression_string(info));
	printf("CHD size:     %s bytes\n", big_int_string(chdman_info_file_bytes(info), buf, sizeof(buf)));
	if (chdman_info_compressed(info))
		printf("Ratio:        %.1f%%\n", 100.0 * (double)chdman_info_file_bytes(info) / (double)chdman_info_logical_bytes(info));
	if (*chdman_info_sha1(info))
	{
		printf("SHA1:         %s\n", chdman_info_sha1(info));
		if (chdman_info_version(info) >= 4)
			printf("Data SHA1:    %s\n", chdman_info_raw_sha1(info));
	}
	if (*chdman_info_parent_sha1(info))
		printf("Parent SHA1:  %s\n", chdman_info_parent_sha1(info));

	for (size_t i = 0; i < chdman_info_metadata_count(info); i++)
	{
		chdman_metadata_entry e;
		chdman_info_metadata_get(info, i, &e);
		if (e.tag_string[0])
			printf("Metadata:     Tag='%s'  Index=%u  Length=%zu bytes\n", e.tag_string, e.index, e.length);
		else
			printf("Metadata:     Tag=%08x  Index=%u  Length=%zu bytes\n", e.tag, e.index, e.length);
		printf("              ");
		size_t count = e.length;
		if (!verbose && count > 60)
			count = 60;
		for (size_t c = 0; c < count; c++)
			putchar(isprint(e.data[c]) ? e.data[c] : '.');
		printf("\n");
	}

	if (verbose)
	{
		uint32_t total = chdman_info_hunk_count(info);
		printf("\n");
		printf("     Hunks  Percent  Name\n");
		printf("----------  -------  ------------------------------------\n");
		for (size_t i = 0; i < chdman_info_hunk_stat_count(info); i++)
		{
			chdman_hunk_stat st;
			chdman_info_hunk_stat_get(info, i, &st);
			printf("%10s   %5.1f%%  %-40s\n", big_int_string(st.hunks, buf, sizeof(buf)), 100.0 * (double)st.hunks / (double)total, st.name);
		}
	}
	chdman_info_free(info);
	return 0;
}

static int do_verify(chdman_context *ctx, params *p)
{
	chdman_verify_options opts;
	chdman_verify_result r;
	chdman_verify_options_init(&opts);
	chdman_verify_result_init(&r);
	opts.input = P(p, OPT_INPUT);
	opts.input_parent = P(p, OPT_INPUT_PARENT);
	opts.fix = HAS(p, OPT_FIX);
	chdman_status st = chdman_verify(ctx, &opts, &r);
	if (st != CHDMAN_OK)
		return report(ctx, st);
	if (g_progress_shown)
	{
		fprintf(stderr, "%70s\r", "");
		g_progress_shown = 0;
	}
	switch (r.outcome)
	{
	case CHDMAN_VERIFY_OK:
		printf("Raw SHA1 verification successful!\n");
		if (r.header_sha1[0])
			printf("Overall SHA1 verification successful!\n");
		break;
	case CHDMAN_VERIFY_RAW_MISMATCH:
		fprintf(stderr, "Error: Raw SHA1 in header = %s\n", r.header_raw_sha1);
		fprintf(stderr, "              actual SHA1 = %s\n", r.computed_raw_sha1);
		return 1;
	case CHDMAN_VERIFY_OVERALL_MISMATCH:
		printf("Raw SHA1 verification successful!\n");
		fprintf(stderr, "Error: Overall SHA1 in header = %s\n", r.header_sha1);
		fprintf(stderr, "                  actual SHA1 = %s\n", r.computed_sha1);
		return 1;
	case CHDMAN_VERIFY_FIXED:
		printf("SHA1 updated to correct value in input CHD\n");
		break;
	case CHDMAN_VERIFY_NOT_APPLICABLE:
		break;
	}
	return 0;
}

static int do_create_raw(chdman_context *ctx, params *p)
{
	chdman_create_raw_options o;
	chdman_create_raw_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	fill_range(p, &o.range);
	o.input = P(p, OPT_INPUT);
	if (HAS(p, OPT_UNIT_SIZE))
		o.unit_size = (uint32_t)parse_number(P(p, OPT_UNIT_SIZE));
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_create_raw(ctx, &o));
}

static int do_create_hd(chdman_context *ctx, params *p)
{
	chdman_create_hd_options o;
	chdman_create_hd_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	fill_range(p, &o.range);
	o.input = P(p, OPT_INPUT);
	if (HAS(p, OPT_SIZE))
	{
		char *end = NULL;
		o.size = strtoull(P(p, OPT_SIZE), &end, 10);
		if (end == P(p, OPT_SIZE))
		{
			fprintf(stderr, "Error: Invalid size specified\n");
			return 1;
		}
	}
	if (HAS(p, OPT_TEMPLATE))
		o.template_id = (int)parse_number(P(p, OPT_TEMPLATE));
	o.ident = P(p, OPT_IDENT);
	if (HAS(p, OPT_CHS))
	{
		unsigned c, h, s;
		if (sscanf(P(p, OPT_CHS), "%u,%u,%u", &c, &h, &s) != 3)
		{
			fprintf(stderr, "Error: Invalid CHS string; must be of the form <cylinders>,<heads>,<sectors>\n");
			return 1;
		}
		o.cylinders = c;
		o.heads = h;
		o.sectors = s;
	}
	if (HAS(p, OPT_SECTOR_SIZE))
		o.sector_size = (uint32_t)parse_number(P(p, OPT_SECTOR_SIZE));
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_create_hd(ctx, &o));
}

static int do_create_cd(chdman_context *ctx, params *p)
{
	chdman_create_cd_options o;
	chdman_create_cd_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	o.input = P(p, OPT_INPUT);
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_create_cd(ctx, &o));
}

static int do_create_dvd(chdman_context *ctx, params *p)
{
	chdman_create_dvd_options o;
	chdman_create_dvd_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	fill_range(p, &o.range);
	o.input = P(p, OPT_INPUT);
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_create_dvd(ctx, &o));
}

static int do_create_ld(chdman_context *ctx, params *p)
{
	chdman_create_ld_options o;
	chdman_create_ld_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	o.input = P(p, OPT_INPUT);
	if (HAS(p, OPT_START_FRAME)) o.start_frame = parse_number(P(p, OPT_START_FRAME));
	if (HAS(p, OPT_LENGTH_FRAMES)) o.length_frames = parse_number(P(p, OPT_LENGTH_FRAMES));
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_create_ld(ctx, &o));
}

static int do_copy(chdman_context *ctx, params *p)
{
	chdman_copy_options o;
	chdman_copy_options_init(&o);
	if (!check_hunk_size_given(p))
		return 1;
	fill_output(p, &o.out);
	fill_range(p, &o.range);
	o.input = P(p, OPT_INPUT);
	o.input_parent = P(p, OPT_INPUT_PARENT);
	apply_numprocessors(ctx, p);
	return report(ctx, chdman_copy(ctx, &o));
}

static int do_extract_raw(chdman_context *ctx, params *p)
{
	chdman_extract_raw_options o;
	chdman_extract_raw_options_init(&o);
	o.input = P(p, OPT_INPUT);
	o.input_parent = P(p, OPT_INPUT_PARENT);
	o.output = P(p, OPT_OUTPUT);
	o.force = HAS(p, OPT_FORCE);
	fill_range(p, &o.range);
	return report(ctx, chdman_extract_raw(ctx, &o));
}

static int do_extract_cd(chdman_context *ctx, params *p)
{
	chdman_extract_cd_options o;
	chdman_extract_cd_options_init(&o);
	o.input = P(p, OPT_INPUT);
	o.input_parent = P(p, OPT_INPUT_PARENT);
	o.output = P(p, OPT_OUTPUT);
	o.output_bin = P(p, OPT_OUTPUT_BIN);
	o.split_bin = HAS(p, OPT_SPLITBIN);
	o.force = HAS(p, OPT_FORCE);
	return report(ctx, chdman_extract_cd(ctx, &o));
}

static int do_extract_ld(chdman_context *ctx, params *p)
{
	chdman_extract_ld_options o;
	chdman_extract_ld_options_init(&o);
	o.input = P(p, OPT_INPUT);
	o.input_parent = P(p, OPT_INPUT_PARENT);
	o.output = P(p, OPT_OUTPUT);
	o.force = HAS(p, OPT_FORCE);
	if (HAS(p, OPT_START_FRAME)) o.start_frame = parse_number(P(p, OPT_START_FRAME));
	if (HAS(p, OPT_LENGTH_FRAMES)) o.length_frames = parse_number(P(p, OPT_LENGTH_FRAMES));
	return report(ctx, chdman_extract_ld(ctx, &o));
}

static int do_add_metadata(chdman_context *ctx, params *p)
{
	chdman_add_metadata_options o;
	char *stripped = NULL;
	int result;
	chdman_add_metadata_options_init(&o);
	o.input = P(p, OPT_INPUT);
	o.tag = P(p, OPT_TAG);
	o.index = HAS(p, OPT_INDEX) ? (uint32_t)atoi(P(p, OPT_INDEX)) : 0;
	o.value_file = P(p, OPT_VALUE_FILE);
	o.no_checksum = HAS(p, OPT_NO_CHECKSUM);
	if (HAS(p, OPT_VALUE_TEXT))
	{
		/* strip one pair of surrounding quotes, as chdman intends */
		const char *text = P(p, OPT_VALUE_TEXT);
		size_t len = strlen(text);
		if (len >= 2 && text[0] == '"' && text[len - 1] == '"')
		{
			stripped = (char *)malloc(len - 1);
			if (!stripped)
				return 1;
			memcpy(stripped, text + 1, len - 2);
			stripped[len - 2] = 0;
			text = stripped;
		}
		o.value_text = text;
	}
	result = report(ctx, chdman_add_metadata(ctx, &o));
	free(stripped);
	return result;
}

static int do_del_metadata(chdman_context *ctx, params *p)
{
	uint32_t index = HAS(p, OPT_INDEX) ? (uint32_t)atoi(P(p, OPT_INDEX)) : 0;
	return report(ctx, chdman_del_metadata(ctx, P(p, OPT_INPUT), NULL, P(p, OPT_TAG), index));
}

static int do_dump_metadata(chdman_context *ctx, params *p)
{
	uint32_t index = HAS(p, OPT_INDEX) ? (uint32_t)atoi(P(p, OPT_INDEX)) : 0;
	if (HAS(p, OPT_OUTPUT))
		return report(ctx, chdman_dump_metadata(ctx, P(p, OPT_INPUT), NULL, P(p, OPT_TAG), index, P(p, OPT_OUTPUT), HAS(p, OPT_FORCE)));

	uint8_t *data = NULL;
	size_t length = 0;
	chdman_status st = chdman_get_metadata(ctx, P(p, OPT_INPUT), NULL, P(p, OPT_TAG), index, &data, &length);
	if (st != CHDMAN_OK)
		return report(ctx, st);
	fwrite(data, 1, length, stdout);
	fflush(stdout);
	chdman_free(data);
	return 0;
}

static int do_list_templates(chdman_context *ctx, params *p)
{
	(void)ctx;
	(void)p;
	printf("\n");
	printf("ID  Manufacturer  Model               Cylinders  Heads  Sectors  Sector Size  Total Size\n");
	printf("----------------------------------------------------------------------------------------\n");
	for (size_t id = 0; id < chdman_template_count(); id++)
	{
		chdman_hd_template t;
		chdman_template_get(id, &t);
		uint32_t size = (uint32_t)(((uint64_t)t.cylinders * t.heads * t.sectors * t.sector_size) / 1024 / 1024);
		printf("%2d  %-13s %-19s %9u  %5u  %7u  %11u  %7u MB\n", (int)id, t.manufacturer, t.model, t.cylinders, t.heads, t.sectors, t.sector_size, size);
	}
	return 0;
}

static const command_desc s_commands[] = {
	{ "info", do_info, ": displays information about a CHD",
		{ REQ(OPT_INPUT), OPT_VERBOSE, END } },
	{ "verify", do_verify, ": verifies a CHD's integrity",
		{ REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_FIX, END } },
	{ "createraw", do_create_raw, ": create a raw CHD from the input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, REQ(OPT_INPUT), OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, OPT_HUNK_SIZE, REQ(OPT_UNIT_SIZE), OPT_COMPRESSION, OPT_NUMPROCESSORS, END } },
	{ "createhd", do_create_hd, ": create a hard disk CHD from the input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, OPT_INPUT, OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, OPT_HUNK_SIZE, OPT_COMPRESSION, OPT_TEMPLATE, OPT_IDENT, OPT_CHS, OPT_SIZE, OPT_SECTOR_SIZE, OPT_NUMPROCESSORS, END } },
	{ "createcd", do_create_cd, ": create a CD CHD from the input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, REQ(OPT_INPUT), OPT_HUNK_SIZE, OPT_COMPRESSION, OPT_NUMPROCESSORS, END } },
	{ "createdvd", do_create_dvd, ": create a DVD CHD from the input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, REQ(OPT_INPUT), OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, OPT_HUNK_SIZE, OPT_COMPRESSION, OPT_NUMPROCESSORS, END } },
	{ "createld", do_create_ld, ": create a laserdisc CHD from the input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, REQ(OPT_INPUT), OPT_START_FRAME, OPT_LENGTH_FRAMES, OPT_HUNK_SIZE, OPT_COMPRESSION, OPT_NUMPROCESSORS, END } },
	{ "extractraw", do_extract_raw, ": extract raw file from a CHD input file",
		{ REQ(OPT_OUTPUT), OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, END } },
	{ "extracthd", do_extract_raw, ": extract raw hard disk file from a CHD input file",
		{ REQ(OPT_OUTPUT), OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, END } },
	{ "extractcd", do_extract_cd, ": extract CD file from a CHD input file",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_BIN, OPT_SPLITBIN, OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, END } },
	{ "extractdvd", do_extract_raw, ": extract DVD file from a CHD input file",
		{ REQ(OPT_OUTPUT), OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, END } },
	{ "extractld", do_extract_ld, ": extract laserdisc AVI from a CHD input file",
		{ REQ(OPT_OUTPUT), OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_START_FRAME, OPT_LENGTH_FRAMES, END } },
	{ "copy", do_copy, ": copy data from one CHD to another of the same type",
		{ REQ(OPT_OUTPUT), OPT_OUTPUT_PARENT, OPT_FORCE, REQ(OPT_INPUT), OPT_INPUT_PARENT, OPT_START_BYTE, OPT_START_HUNK, OPT_LENGTH_BYTES, OPT_LENGTH_HUNKS, OPT_HUNK_SIZE, OPT_COMPRESSION, OPT_NUMPROCESSORS, END } },
	{ "addmeta", do_add_metadata, ": add metadata to the CHD",
		{ REQ(OPT_INPUT), REQ(OPT_TAG), OPT_INDEX, OPT_VALUE_TEXT, OPT_VALUE_FILE, OPT_NO_CHECKSUM, END } },
	{ "delmeta", do_del_metadata, ": remove metadata from the CHD",
		{ REQ(OPT_INPUT), REQ(OPT_TAG), OPT_INDEX, END } },
	{ "dumpmeta", do_dump_metadata, ": dump metadata from the CHD to stdout or to a file",
		{ REQ(OPT_INPUT), OPT_OUTPUT, OPT_FORCE, REQ(OPT_TAG), OPT_INDEX, END } },
	{ "listtemplates", do_list_templates, ": list hard disk templates",
		{ END } },
};

#define COMMAND_COUNT (sizeof(s_commands) / sizeof(s_commands[0]))

static int opt_index(int encoded) { return encoded < 0 ? -encoded - 1 : encoded; }

static int print_help(const char *argv0, const char *error)
{
	if (error)
		fprintf(stderr, "Error: %s\n\n", error);
	printf("Usage:\n");
	for (size_t i = 0; i < COMMAND_COUNT; i++)
		printf("   %s %s%s\n", argv0, s_commands[i].name, s_commands[i].description);
	printf("\nFor help with any command, run:\n");
	printf("   %s help <command>\n", argv0);
	return 1;
}

static int print_command_help(const char *argv0, const command_desc *desc, const char *error)
{
	if (error)
		fprintf(stderr, "Error: %s\n\n", error);
	printf("Usage:\n");
	printf("   %s %s [options], where valid options are:\n", argv0, desc->name);
	for (int i = 0; desc->options[i] != END; i++)
	{
		const option_desc *o = &s_options[opt_index(desc->options[i])];
		printf("      --%s, -%s%s%s\n", o->name, o->shortname, o->description, desc->options[i] < 0 ? " (required)" : "");
	}
	return 1;
}

int main(int argc, char *argv[])
{
	const char *argv0 = argc > 0 ? argv[0] : "chdman-cli";
	printf("chdman-cli - MAME Compressed Hunks of Data (CHD) manager, %s\n", chdman_version());

	if (argc < 2)
		return print_help(argv0, NULL);

	int argnum = 1;
	const char *command = argv[argnum++];
	int help = strcmp(command, "help") == 0;
	if (help)
	{
		if (argc <= 2)
			return print_help(argv0, NULL);
		command = argv[argnum++];
	}

	for (size_t c = 0; c < COMMAND_COUNT; c++)
	{
		const command_desc *desc = &s_commands[c];
		if (strcmp(command, desc->name) != 0)
			continue;
		if (help)
			return print_command_help(argv0, desc, NULL);

		params p;
		memset(&p, 0, sizeof(p));
		while (argnum < argc)
		{
			const char *arg = argv[argnum++];
			if (arg[0] != '-')
				return print_command_help(argv0, desc, "Expected option, not parameter");

			int matched = -1;
			for (int i = 0; desc->options[i] != END; i++)
			{
				int idx = opt_index(desc->options[i]);
				const option_desc *o = &s_options[idx];
				if ((arg[1] == '-' && strcmp(o->name, arg + 2) == 0) ||
					(arg[1] != '-' && strcmp(o->shortname, arg + 1) == 0))
				{
					matched = idx;
					break;
				}
			}
			if (matched < 0)
			{
				char message[256];
				snprintf(message, sizeof(message), "Option '%s' not valid for this command", arg);
				return print_command_help(argv0, desc, message);
			}
			if (p.present[matched])
				return print_command_help(argv0, desc, "Multiple parameters of the same type specified");
			p.present[matched] = 1;
			p.value[matched] = "";
			if (s_options[matched].parameter)
			{
				if (argnum >= argc || (argv[argnum][0] == '-' && argv[argnum][1] != 0))
					return print_command_help(argv0, desc, "Option is missing parameter");
				p.value[matched] = argv[argnum++];
			}
		}

		for (int i = 0; desc->options[i] != END; i++)
			if (desc->options[i] < 0 && !p.present[opt_index(desc->options[i])])
				return print_command_help(argv0, desc, "Required parameters missing");

		chdman_context *ctx = NULL;
		if (chdman_context_create(&ctx) != CHDMAN_OK)
		{
			fprintf(stderr, "Error: out of memory\n");
			return 1;
		}
		g_ctx = ctx;
		signal(SIGINT, on_sigint);
		chdman_context_set_progress(ctx, progress_cb, NULL);
		chdman_context_set_log(ctx, log_cb, NULL);

		int result = desc->handler(ctx, &p);

		g_ctx = NULL;
		chdman_context_destroy(ctx);
		return result;
	}

	return print_help(argv0, NULL);
}
