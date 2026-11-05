#pragma once

#include <stddef.h>

/* Downloads the firmware for this chip from an OCI reference (plain HTTP
 * registry) into the inactive OTA slot and makes it the next boot image.
 * `digest` gets the digest of the resolved reference (index or manifest).
 * Returns 0 on success; on failure `err` says why and the slot is untouched. */
int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len);
