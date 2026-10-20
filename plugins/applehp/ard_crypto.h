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

/* Apple's type-30 authentication and the 003.889 record layer.
 *
 * Type 30 is a Diffie-Hellman exchange whose shared secret, hashed with MD5,
 * encrypts the account's credentials in AES-128-ECB. The same digest is the
 * first key the record layer's rekey is wrapped under.
 *
 * Every message after the rekey, both ways, is a record:
 *
 *   u16 ciphertext_len
 *   AES-128-CBC( u16 body_len || body || filler || SHA1(u32 seq || plaintext) )
 *
 * One CBC chain per direction for the whole session: a record's last
 * ciphertext block is the next record's IV. The sequence number is not on the
 * wire, so both ends agree only while they have counted the same records. */

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <openssl/evp.h>

#define ARD_BLOCK 16
#define ARD_REKEY_LEN (4 + ARD_BLOCK + ARD_BLOCK)
#define ARD_CREDENTIALS_LEN 128
#define ARD_CREDENTIAL_FIELD 64
#define ARD_DH_MIN_KEY 64
#define ARD_DH_MAX_KEY 1024

/* One direction of the record layer. */
typedef struct {
	EVP_CIPHER_CTX *ecb;
	guint8 chain[ARD_BLOCK];
	guint32 seq;
} ArdCbc;

/* The Diffie-Hellman half of type 30: the shared secret and this side's public
 * key, both left-padded to `len`, the server's key length. The padding matters:
 * the secret is hashed as bytes, and a numerically small one has to keep its
 * leading zeros for both ends to derive the same key. */
gboolean ard_dh_exchange(guint16 generator, const guint8 *prime, const guint8 *peer_public, gsize len,
			 guint8 *secret, guint8 *public_key, GError **error);

void ard_md5(const guint8 *data, gsize len, guint8 out[16]);

/* AES-128-ECB over whole blocks, in place. */
gboolean ard_aes128_ecb(const guint8 key[16], gboolean encrypt, guint8 *data, gsize len);

/* The record layer's key and IV out of a rekey body, each half one AES block
 * decrypted on its own under `wrap_key`. */
void ard_unwrap_rekey(const guint8 wrap_key[16], const guint8 body[ARD_REKEY_LEN], guint8 key[16], guint8 iv[16]);

gboolean ard_cbc_init(ArdCbc *cbc, const guint8 key[16], const guint8 iv[16], gboolean encrypt);
void ard_cbc_clear(ArdCbc *cbc);

/* Append `msg` to `out` as one or more records. A message past one record's
 * body goes out as full records and then the rest, the concatenation the Mac
 * reads back. */
void ard_record_frame(ArdCbc *cbc, const guint8 *msg, gsize len, GByteArray *out);

/* Decrypt and verify one record's ciphertext in place. On success the body is
 * `record[*body_at .. *body_at + *body_len]`. A record that fails its check ends
 * the session: nothing of it may be handed upward. */
gboolean ard_record_open(ArdCbc *cbc, guint8 *record, gsize len, gsize *body_at, gsize *body_len, GError **error);

/* Whether `len` is a ciphertext length a record may have. */
gboolean ard_record_len_ok(gsize len);
