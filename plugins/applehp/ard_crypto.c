/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2026 Gabriel Speckhahn
 *
 * The protocol handling in this plugin is a port of remotex
 * (https://github.com/andrewtheguy/remotex), Copyright (c) 2026 Andrew Chen,
 * released under the MIT License.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */

#include "ard_crypto.h"
#include "ard_wire.h"

#include <string.h>
#include <openssl/bn.h>

#define TRAILER 20
#define BODY_LEN 2
/* The largest ciphertext the u16 length can describe, in whole blocks. */
#define MAX_CIPHERTEXT ((G_MAXUINT16 / ARD_BLOCK) * ARD_BLOCK)
#define MAX_BODY (MAX_CIPHERTEXT - BODY_LEN - TRAILER)

static gsize filler_len(gsize body_len)
{
	return (ARD_BLOCK - (BODY_LEN + body_len + TRAILER) % ARD_BLOCK) % ARD_BLOCK;
}

/* An empty body still carries its length, the filler that rounds it to a
 * block, and the trailer: 2 + 10 + 20. */
#define MIN_CIPHERTEXT 32

gboolean ard_dh_exchange(guint16 generator, const guint8 *prime, const guint8 *peer_public, gsize len,
			 guint8 *secret, guint8 *public_key, GError **error)
{
	BN_CTX *ctx = BN_CTX_new();
	BIGNUM *p = BN_bin2bn(prime, len, NULL);
	BIGNUM *peer = BN_bin2bn(peer_public, len, NULL);
	BIGNUM *g = BN_new();
	BIGNUM *x = BN_new();
	BIGNUM *pub = BN_new();
	BIGNUM *shared = BN_new();
	BIGNUM *p_minus_1 = BN_new();
	gboolean ok = FALSE;

	if (!ctx || !p || !peer || !g || !x || !pub || !shared || !p_minus_1) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "out of memory in the DH exchange");
		goto out;
	}
	if (BN_is_zero(p)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the Mac offered a zero DH prime");
		goto out;
	}
	/* 0, 1 and p-1 collapse the shared secret to a value anyone watching the
	 * exchange can work out, which leaves the account password without cover. */
	BN_copy(p_minus_1, p);
	BN_sub_word(p_minus_1, 1);
	if (BN_cmp(peer, BN_value_one()) <= 0 || BN_cmp(peer, p_minus_1) >= 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the Mac offered a degenerate DH public key");
		goto out;
	}
	BN_set_word(g, generator);
	/* Twice the key length, as Apple's viewer draws it. */
	if (!BN_rand(x, len * 2 * 8, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)
	    || !BN_mod_exp(pub, g, x, p, ctx)
	    || !BN_mod_exp(shared, peer, x, p, ctx)
	    || BN_bn2binpad(pub, public_key, len) < 0
	    || BN_bn2binpad(shared, secret, len) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the DH exchange failed");
		goto out;
	}
	ok = TRUE;
out:
	BN_clear_free(x);
	BN_clear_free(shared);
	BN_free(p);
	BN_free(peer);
	BN_free(g);
	BN_free(pub);
	BN_free(p_minus_1);
	BN_CTX_free(ctx);
	return ok;
}

void ard_md5(const guint8 *data, gsize len, guint8 out[16])
{
	unsigned int n = 16;

	EVP_Digest(data, len, out, &n, EVP_md5(), NULL);
}

gboolean ard_aes128_ecb(const guint8 key[16], gboolean encrypt, guint8 *data, gsize len)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int n = 0;
	gboolean ok;

	ok = ctx && EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0)
	     && EVP_CIPHER_CTX_set_padding(ctx, 0)
	     && EVP_CipherUpdate(ctx, data, &n, data, len);
	EVP_CIPHER_CTX_free(ctx);
	return ok && (gsize)n == len;
}

void ard_unwrap_rekey(const guint8 wrap_key[16], const guint8 body[ARD_REKEY_LEN], guint8 key[16], guint8 iv[16])
{
	memcpy(key, body + 4, 16);
	memcpy(iv, body + 4 + 16, 16);
	ard_aes128_ecb(wrap_key, FALSE, key, 16);
	ard_aes128_ecb(wrap_key, FALSE, iv, 16);
}

gboolean ard_cbc_init(ArdCbc *cbc, const guint8 key[16], const guint8 iv[16], gboolean encrypt)
{
	/* CBC is run by hand over an ECB context, so the chain is in plain view and
	 * no library buffering can hold a block back between records. */
	cbc->ecb = EVP_CIPHER_CTX_new();
	memcpy(cbc->chain, iv, ARD_BLOCK);
	cbc->seq = 0;
	return cbc->ecb
	       && EVP_CipherInit_ex(cbc->ecb, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0)
	       && EVP_CIPHER_CTX_set_padding(cbc->ecb, 0);
}

void ard_cbc_clear(ArdCbc *cbc)
{
	if (cbc->ecb)
		EVP_CIPHER_CTX_free(cbc->ecb);
	cbc->ecb = NULL;
}

static void cbc_encrypt(ArdCbc *cbc, guint8 *data, gsize len)
{
	int n;

	for (gsize at = 0; at < len; at += ARD_BLOCK) {
		for (int i = 0; i < ARD_BLOCK; i++)
			data[at + i] ^= cbc->chain[i];
		EVP_CipherUpdate(cbc->ecb, data + at, &n, data + at, ARD_BLOCK);
		memcpy(cbc->chain, data + at, ARD_BLOCK);
	}
}

static void cbc_decrypt(ArdCbc *cbc, guint8 *data, gsize len)
{
	guint8 ciphertext[ARD_BLOCK];
	int n;

	for (gsize at = 0; at < len; at += ARD_BLOCK) {
		memcpy(ciphertext, data + at, ARD_BLOCK);
		EVP_CipherUpdate(cbc->ecb, data + at, &n, data + at, ARD_BLOCK);
		for (int i = 0; i < ARD_BLOCK; i++)
			data[at + i] ^= cbc->chain[i];
		memcpy(cbc->chain, ciphertext, ARD_BLOCK);
	}
}

/* SHA1(u32 seq || covered), consuming one sequence number. Plain SHA-1, not
 * an HMAC: the key authenticates nothing here. */
static void trailer(guint32 seq, const guint8 *covered, gsize len, guint8 out[TRAILER])
{
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	guint8 seq_be[4];
	unsigned int n = TRAILER;

	ard_put32(seq_be, seq);
	EVP_DigestInit_ex(md, EVP_sha1(), NULL);
	EVP_DigestUpdate(md, seq_be, 4);
	EVP_DigestUpdate(md, covered, len);
	EVP_DigestFinal_ex(md, out, &n);
	EVP_MD_CTX_free(md);
}

static void push_record(ArdCbc *cbc, const guint8 *body, gsize body_len, GByteArray *out)
{
	gsize total = BODY_LEN + body_len + filler_len(body_len) + TRAILER;
	gsize plain;
	guint8 tag[TRAILER];

	/* The outer length is neither encrypted nor covered by the trailer. */
	ard_append16(out, total);
	plain = out->len;
	ard_append16(out, body_len);
	g_byte_array_append(out, body, body_len);
	/* Zero filler, which the Mac accepts: the chain already makes two identical
	 * messages encrypt differently. */
	g_byte_array_set_size(out, plain + total - TRAILER);
	memset(out->data + plain + BODY_LEN + body_len, 0, filler_len(body_len));
	trailer(cbc->seq++, out->data + plain, total - TRAILER, tag);
	g_byte_array_append(out, tag, TRAILER);
	cbc_encrypt(cbc, out->data + plain, total);
}

void ard_record_frame(ArdCbc *cbc, const guint8 *msg, gsize len, GByteArray *out)
{
	do {
		gsize take = MIN(len, MAX_BODY);

		push_record(cbc, msg, take, out);
		msg += take;
		len -= take;
	} while (len > 0);
}

gboolean ard_record_len_ok(gsize len)
{
	return len >= MIN_CIPHERTEXT && len % ARD_BLOCK == 0;
}

gboolean ard_record_open(ArdCbc *cbc, guint8 *record, gsize len, gsize *body_at, gsize *body_len, GError **error)
{
	guint8 expected[TRAILER];
	guint32 seq = cbc->seq;
	gsize body;

	cbc_decrypt(cbc, record, len);
	trailer(cbc->seq++, record, len - TRAILER, expected);
	if (memcmp(record + len - TRAILER, expected, TRAILER) != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "record %u of %" G_GSIZE_FORMAT " bytes failed its integrity check", seq, len);
		return FALSE;
	}
	body = ard_get16(record);
	if (BODY_LEN + body + TRAILER > len) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "a record claims a %" G_GSIZE_FORMAT "-byte body in %" G_GSIZE_FORMAT " bytes", body, len);
		return FALSE;
	}
	*body_at = BODY_LEN;
	*body_len = body;
	return TRUE;
}
