#pragma once

#include <stddef.h>

/* Downloads the firmware for this chip from an OCI reference (plain HTTP
 * registry) into the inactive OTA slot and makes it the next boot image.
 * `digest` gets the digest of the resolved reference (index or manifest).
 * Returns 0 on success; on failure `err` says why and the slot is untouched. */
int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len);

/* Downloads one layer of `image` into a malloc'd buffer (caller frees):
 * through an index the manifest with platform.architecture == `arch`, then
 * the first layer with mediaType `layer_type`; sha256 checked. */
int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len,
                       char *err, size_t err_len);

/* Public key (PEM, ECDSA P-256) that firmware and apps must be signed with,
 * in cosign's format. NULL or empty: signatures are not checked. */
void fc_oci_set_signing_key(const char *pub_pem);
