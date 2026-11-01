/*
 * Minimal SHA-1 (FIPS 180-4) used by the tests as an independent reference
 * for the SHA-1 values lib-chd-kit writes into CHD headers.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "sha1.h"

#include <stdio.h>
#include <string.h>

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(test_sha1 *s, const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e;
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for (int i = 0; i < 80; i++)
	{
		uint32_t f, k;
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
		uint32_t t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void test_sha1_init(test_sha1 *s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
	s->length = 0;
	s->used = 0;
}

void test_sha1_update(test_sha1 *s, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	s->length += len;
	while (len > 0)
	{
		size_t n = 64 - s->used;
		if (n > len)
			n = len;
		memcpy(s->buf + s->used, p, n);
		s->used += n;
		p += n;
		len -= n;
		if (s->used == 64)
		{
			sha1_block(s, s->buf);
			s->used = 0;
		}
	}
}

void test_sha1_final_hex(test_sha1 *s, char out[41])
{
	uint64_t bits = s->length * 8;
	uint8_t pad = 0x80;
	test_sha1_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56)
		test_sha1_update(s, &pad, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	test_sha1_update(s, len, 8);
	for (int i = 0; i < 5; i++)
		snprintf(out + i * 8, 9, "%08x", s->h[i]);
}
