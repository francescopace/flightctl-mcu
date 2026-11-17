/*
 * The device's registry client (core/src/fc_oci.c, with the real fc_http.c)
 * run against a real registry from the host, without Flight Control:
 * tests/registry-bearer.sh drives it.
 *
 *   fc-oci-pull [options] fw <image> <out>
 *       firmware for $FC_FW_PLATFORM (an index's platform.architecture)
 *   fc-oci-pull [options] layer <image> <arch> <mediaType> <out>
 *       one layer, as the app runtime pulls a WASM module
 *   fc-oci-pull [options] expire <image> <arch> <mediaType> <seconds> <out>
 *       resolves the layer (mediaType "": the first one, as for firmware),
 *       waits, then downloads its blob on the same pull: the token got at
 *       the start has run out by then
 *
 * Options, given to the device as its fleet would, as files:
 *   --data DIR        the simulated device's data directory (default build/oci-pull)
 *   --ca HOST=FILE    /etc/containers/certs.d/HOST/ca.crt (repeatable)
 *   --auth FILE       /etc/containers/auth.json
 *   --key FILE        the signing key's public half: signatures checked
 * Prints the digest; exits 1 with the device's error otherwise.
 */
#include "../../core/src/fc_oci.c"

#include <unistd.h>

void fc_port_host_init(const char *data_dir, int debug);

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void put(const char *device_path, const char *file)
{
    size_t len;
    char *b = slurp(file, &len);
    if (fc_port_write_file(device_path, b, len) != 0) {
        fprintf(stderr, "cannot write %s\n", device_path);
        exit(2);
    }
    free(b);
}

static int save(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(data, 1, len, f) == len;
    if (f != NULL) {
        fclose(f);
    }
    return ok ? 0 : -1;
}

static int usage(void)
{
    fprintf(stderr, "usage: fc-oci-pull [--data DIR] [--ca HOST=FILE]... [--auth FILE] [--key FILE]\n"
                    "         fw <image> <out> | layer <image> <arch> <mediaType> <out> |\n"
                    "         expire <image> <arch> <mediaType> <seconds> <out>\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *data = "build/oci-pull";
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        if (i + 1 >= argc) {
            return usage();
        }
        if (strcmp(argv[i], "--data") == 0) {
            data = argv[i + 1];
        }
    }
    fc_port_host_init(data, 0);
    /* The files of a fresh device: nothing from an earlier run. */
    fc_port_remove_file("/etc/containers/auth.json");
    static char *key;
    for (int j = 1; j < i; j += 2) {
        const char *v = argv[j + 1];
        if (strcmp(argv[j], "--ca") == 0) {
            const char *eq = strchr(v, '=');
            if (eq == NULL) {
                return usage();
            }
            char path[256];
            snprintf(path, sizeof(path), CERTS_DIR "%.*s/ca.crt", (int)(eq - v), v);
            put(path, eq + 1);
        } else if (strcmp(argv[j], "--auth") == 0) {
            put("/etc/containers/auth.json", v);
        } else if (strcmp(argv[j], "--key") == 0) {
            size_t len;
            key = slurp(v, &len);
        } else if (strcmp(argv[j], "--data") != 0) {
            return usage();
        }
    }
    fc_oci_set_signing_key(key);
    if (i >= argc) {
        return usage();
    }
    const char *cmd = argv[i];
    char digest[80] = "", err[256] = "";
    int rc;
    if (strcmp(cmd, "fw") == 0 && argc - i == 3) {
        rc = fc_oci_fetch_firmware(argv[i + 1], digest, sizeof(digest), err, sizeof(err));
        if (rc == 0) {
            char ota[600];
            snprintf(ota, sizeof(ota), "%s/ota-next.bin", data);
            rc = rename(ota, argv[i + 2]) == 0 ? 0 : -1;
            if (rc != 0) {
                snprintf(err, sizeof(err), "no %s", ota);
            }
        }
    } else if (strcmp(cmd, "layer") == 0 && argc - i == 5) {
        unsigned char *out = NULL;
        size_t len = 0;
        rc = fc_oci_fetch_layer(argv[i + 1], argv[i + 2], argv[i + 3], 4 * 1024 * 1024, &out, &len, digest,
                                sizeof(digest), err, sizeof(err));
        if (rc == 0) {
            rc = save(argv[i + 4], out, len);
        }
        free(out);
    } else if (strcmp(cmd, "expire") == 0 && argc - i == 6) {
        oci_ref_t r;
        rc = reg_open(argv[i + 1], &r, err, sizeof(err));
        char blob[72];
        long size = 0;
        if (rc == 0) {
            /* An empty mediaType: the manifest's first layer, as for firmware. */
            rc = resolve_layer(argv[i + 1], argv[i + 2], argv[i + 3][0] ? argv[i + 3] : NULL, &r, blob, &size,
                               digest, sizeof(digest), err, sizeof(err), NULL);
        }
        if (rc == 0) {
            fprintf(stderr, "resolved with %s; waiting %s s\n", r.authorization ? "a token" : "no token",
                    argv[i + 4]);
            sleep((unsigned)atoi(argv[i + 4]));
            mem_sink_t m = {.buf = malloc((size_t)size), .cap = (size_t)size};
            rc = fetch_blob(&r, blob, size, mem_sink, &m, 0, err, sizeof(err));
            if (rc == 0) {
                rc = save(argv[i + 5], m.buf, m.len);
            }
            free(m.buf);
        }
        reg_close(&r);
    } else {
        return usage();
    }
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    printf("%s\n", digest);
    return 0;
}
