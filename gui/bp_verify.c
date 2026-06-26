/* bp_verify.c — see bp_verify.h. Minisign Ed25519 (non-hashed "Ed") verify via vendored tweetnacl.
 * minisign files:  line 1 = "untrusted comment: ...", line 2 = base64.
 *   pubkey blob (42B): [alg 2][key_id 8][ed25519_pk 32]
 *   sig blob    (74B): [alg 2][key_id 8][ed25519_sig 64]
 * Verify: crypto_sign_open over (sig || data). We verify file authenticity (the trusted-comment
 * global signature is not security-relevant here and is skipped). */
#define _POSIX_C_SOURCE 200809L
#include "bp_verify.h"
#include "tweetnacl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tweetnacl references randombytes() in its keygen paths; verify never calls those. Provide a
 * hard-failing stub so the link resolves without pulling a CSPRNG into the GUI. */
void randombytes(unsigned char *x, unsigned long long n) { (void)x; (void)n; abort(); }

static int b64val(int c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* Decode padded base64 `in` into `out`; returns byte count or -1. */
static long b64decode(const char *in, size_t inlen, unsigned char *out, size_t outcap) {
	if (inlen % 4 != 0) return -1;
	size_t o = 0;
	for (size_t i = 0; i < inlen; i += 4) {
		int a = b64val(in[i]), b = b64val(in[i + 1]);
		int c = in[i + 2] == '=' ? 0 : b64val(in[i + 2]);
		int d = in[i + 3] == '=' ? 0 : b64val(in[i + 3]);
		if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
		if (o >= outcap) return -1;
		out[o++] = (unsigned char)((a << 2) | (b >> 4));
		if (in[i + 2] != '=') {
			if (o >= outcap) return -1;
			out[o++] = (unsigned char)((b << 4) | (c >> 2));
		}
		if (in[i + 3] != '=') {
			if (o >= outcap) return -1;
			out[o++] = (unsigned char)((c << 6) | d);
		}
	}
	return (long)o;
}

/* Read line `n` (1-based) of a file, trimmed of trailing CR/LF, into a malloc'd buffer. */
static char *read_line_n(const char *path, int n) {
	FILE *fp = fopen(path, "r");
	if (!fp) return NULL;
	char *line = NULL; size_t cap = 0; ssize_t len; int cur = 0; char *ret = NULL;
	while ((len = getline(&line, &cap, fp)) >= 0) {
		if (++cur == n) {
			while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
			ret = strdup(line);
			break;
		}
	}
	free(line); fclose(fp);
	return ret;
}

/* Decode the base64 on line 2 of a minisign pubkey/sig file into `raw` (expecting `want` bytes). */
static int load_blob(const char *path, unsigned char *raw, size_t want, char *err, size_t errsz) {
	char *b64 = read_line_n(path, 2);
	if (!b64) { snprintf(err, errsz, "no base64 line in %s", path); return -1; }
	long n = b64decode(b64, strlen(b64), raw, want);
	free(b64);
	if (n != (long)want) { snprintf(err, errsz, "bad blob length in %s", path); return -1; }
	/* alg is a little-endian uint16; non-hashed Ed25519 "Ed" = 0x4564 (matches feather). */
	unsigned alg = (unsigned)raw[0] | ((unsigned)raw[1] << 8);
	if (alg != 0x4564u) { snprintf(err, errsz, "unsupported algorithm in %s", path); return -1; }
	return 0;
}

int bp_verify_file(const char *data_path, const char *sig_path, const char *pubkey_path,
                   char *err, size_t errsz) {
	unsigned char pkblob[42], sigblob[74];
	if (load_blob(pubkey_path, pkblob, sizeof pkblob, err, errsz) != 0) return -1;
	if (load_blob(sig_path, sigblob, sizeof sigblob, err, errsz) != 0) return -1;
	if (memcmp(pkblob + 2, sigblob + 2, 8) != 0) { snprintf(err, errsz, "signature key_id != pubkey key_id"); return -1; }
	const unsigned char *pk = pkblob + 10;       /* 32-byte ed25519 pk */
	const unsigned char *sig = sigblob + 10;     /* 64-byte signature */

	/* read the data file */
	FILE *fp = fopen(data_path, "rb");
	if (!fp) { snprintf(err, errsz, "cannot open %s", data_path); return -1; }
	if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); snprintf(err, errsz, "seek %s", data_path); return -1; }
	long dlen = ftell(fp);
	if (dlen < 0 || dlen > 8 * 1024 * 1024) { fclose(fp); snprintf(err, errsz, "blueprint too large/empty"); return -1; }
	rewind(fp);
	unsigned char *data = malloc((size_t)dlen);
	if (!data || fread(data, 1, (size_t)dlen, fp) != (size_t)dlen) { free(data); fclose(fp); snprintf(err, errsz, "read %s", data_path); return -1; }
	fclose(fp);

	/* signed message = sig(64) || data; crypto_sign_open recovers it iff valid. */
	unsigned long long smlen = 64ULL + (unsigned long long)dlen;
	unsigned char *sm = malloc((size_t)smlen);
	unsigned char *m = malloc((size_t)smlen);
	if (!sm || !m) { free(data); free(sm); free(m); snprintf(err, errsz, "out of memory"); return -1; }
	memcpy(sm, sig, 64);
	memcpy(sm + 64, data, (size_t)dlen);
	unsigned long long mlen = 0;
	int rc = crypto_sign_open(m, &mlen, sm, smlen, pk);
	free(data); free(sm); free(m);
	if (rc != 0) { snprintf(err, errsz, "signature verification failed"); return -1; }
	return 0;
}
