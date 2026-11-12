# Registry

Where devices get their firmware and apps: an OCI registry, reached over TLS with a password, or over plain HTTP.

## The rule

A device reaches a registry over TLS when it has that registry's CA. Without the CA it uses plain HTTP, which is how every firmware before this one worked.

The CA and the credentials reach the device as config files in its fleet. The paths are the ones containers use on Linux, and so does Flight Control's Linux agent:

| File | What it is |
|---|---|
| `/etc/containers/certs.d/<host>/ca.crt` | The registry's CA (PEM), for any port of that host. Its presence means TLS, on port 443 when the image reference names no port. Flight Control refuses a `:` in a config file's path, so `certs.d/<host:port>/ca.crt`, which containers use, cannot come from a fleet. The device still reads it first, for a file put there another way. |
| `/etc/ostree/auth.json`, `/etc/containers/auth.json`, `/root/.config/containers/auth.json` | The credentials, in podman's format. The device uses the first of these files that has an entry for the registry. |

```json
{"auths": {"registry.example.com:5443": {"auth": "<base64 of user:password>"}}}
```

A key may also name a repository, as in `registry.example.com:5443/flightctl-mcu`; the longest key that matches wins. `username` and `password` are taken in place of `auth`.

The device sends the credentials only when the registry asks for them with a 401, and never over plain HTTP. It supports two schemes:

- **Basic**, which is what `registry:3` with a password file asks for.
- **Bearer tokens**, the scheme of Harbor, Quay, GitLab and most hosted registries. The token comes from the realm named in the challenge, which must be on https. A realm on another host needs that host's CA as well.

The credentials never appear in the device's errors or its log. A registry that refuses them shows up in `status.updated.info` as `HTTP 401`.

The CA covers every port of its host. A registry served over plain HTTP and over TLS needs a different host name for each, as the local one below has.

## The local registry

`tools/registry/up.sh` starts one storage with two doors:

| Door | Address | For |
|---|---|---|
| Plain HTTP | `<your-ip>:5001` | Development, and firmware older than TLS. Anyone on the LAN can read and push. |
| TLS with a password | `registry.<your-ip>.nip.io:5443` | Devices, in a deployment |

The first run makes `.secrets/registry/` with:

- the CA;
- the server certificate, made again when the LAN IP changes;
- a random password for the user `device`;
- `auth.json` holding that password;
- `host`, the TLS door's address.

An image pushed through one door can be pulled through the other.

The push tools follow the devices' rule:

- `tools/fw-push.py`, `tools/signing/sign.py` and `wasm-apps/push.sh` use TLS and the password for the registry named in `.secrets/registry/host`.
- `$REGISTRY_CA` and `$REGISTRY_AUTH_FILE` point them at another registry.
- `tools/registry/registry_client.py` holds the shared logic.

```bash
tools/fw-push.py $(cat .secrets/registry/host)/flightctl-mcu/fw:v3.1
```

```bash
REGISTRY=$(cat .secrets/registry/host) tools/fleet/apply.sh tools/fleet/zephyr-lab.yaml
```

When the registry has a CA, `apply.sh` adds both files to the fleet's template as a config provider named `registry` (`tools/fleet/registry-config.py`).

## Moving a fleet from plain HTTP to TLS

The devices must first run a firmware that knows TLS for the registry. Release it through the plain door, and once the fleet is up to date, apply the fleet again with the TLS door. Changing the registry address in `os.image` makes every device download its firmware again and restart, because Flight Control compares `os.image` as a string. Push a new release with a delta from the running one (`--delta-from`) to keep that download small.

## Memory

A download over TLS opens a TLS session, which on these boards costs about 25 KB of RAM. The agent closes its spec connection before downloading, so the device holds at most two TLS sessions at a time: the status connection and the registry. The spec connection opens again on the next long-poll. One connection to the registry is open at a time: manifests and signatures go over one kept connection, and each blob over a connection of its own.

## What it does not do

- **No public CA bundle.** The device trusts only the CA that the fleet gives it. A registry with a public certificate, such as `ghcr.io` or `quay.io`, takes its CA chain as `ca.crt`.
- **No redirects.** A registry that sends blobs from another host, such as S3 or a CDN, gets a 307 redirect that the device does not follow. Serve blobs from the registry itself.
- **No read-only account in `registry:3`.** Any account can push to it. A deployment should give its devices a pull-only account, such as a robot account in Harbor or Quay. Signatures still stop a device from installing an image it did not expect.
- **The credentials stay in the fleet.** Inline config files are part of the fleet's spec, so whoever can read the fleet can read them. Flight Control 1.4 encrypts rendered config at rest. This agent does not take config from a Kubernetes secret (`secretRef`).
- **A file removed from the fleet is not removed from the device.** The device keeps the last `auth.json` it was given until a new one replaces it.
