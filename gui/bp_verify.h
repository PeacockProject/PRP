/* bp_verify.h — minimal minisign (Ed25519, non-hashed) signature verification for blueprints.
 * Self-contained (vendored tweetnacl); mirrors feather/src/verify.c so it accepts the SAME
 * genmirror signatures, but does NOT depend on feather — feather stays a package manager. */
#ifndef PRP_BP_VERIFY_H
#define PRP_BP_VERIFY_H

#include <stddef.h>

/* Verify the bytes of `data_path` against minisign `sig_path`, trusting `pubkey_path`
 * (a minisign public-key file). Returns 0 only if the signature is valid AND its key_id
 * matches the pubkey; otherwise -1 with a reason in `err`. */
int bp_verify_file(const char *data_path, const char *sig_path, const char *pubkey_path,
                   char *err, size_t errsz);

#endif /* PRP_BP_VERIFY_H */
