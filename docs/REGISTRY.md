# Registry

Where devices get their firmware and apps: an OCI registry, reached over TLS and HTTP/2, with a password when it asks for one.

## The rule

A device reaches a registry over TLS, with the CA its fleet gives it, and HTTP/2. Without the CA the pull fails, and the error names the file the CA is expected in. Plain HTTP is not spoken.

The CA and the credentials reach the device as config files in its fleet. The paths are the ones containers use on Linux, and so does Flight Control's Linux agent:

| File | What it is |
|---|---|
| `/etc/containers/certs.d/<host>/ca.crt` | The registry's CA (PEM), for any port of that host; port 443 when the image reference names no port. Flight Control refuses a `:` in a config file's path, so `certs.d/<host:port>/ca.crt`, which containers use, cannot come from a fleet. The device still reads it first, for a file put there another way. |
| `/etc/ostree/auth.json`, `/etc/containers/auth.json`, `/root/.config/containers/auth.json` | The credentials, in podman's format. The device uses the first of these files that has an entry for the registry. |

```json
{"auths": {"registry.example.com:5443": {"auth": "<base64 of user:password>"}}}
```

A key may also name a repository, as in `registry.example.com:5443/flightctl-mcu`; the longest key that matches wins. `username` and `password` are taken in place of `auth`.

The device sends the credentials only when the registry asks for them with a 401. It supports two schemes:

- **Basic**, which is what `registry:3` with a password file asks for.
- **Bearer tokens**, the scheme of Harbor, Quay, GitLab and most hosted registries. The token comes from the realm the challenge names, on https, with the credentials as Basic, or with none for a public repository (an anonymous token). A realm on another host needs that host's CA too (`certs.d/<that host>/ca.crt`) and takes a second connection while the token is asked for; one on the registry's own host and port uses the same connection. The device asks for one token per pull, for the scope the registry names, and replaces it once if it runs out during the pull.

A robot account of Harbor or Quay goes into `auth.json` as any account: its name (`<organization>+<robot>` in Quay, `robot$<project>+<robot>` in Harbor) as the user and its token as the password:

```json
{"auths": {"quay.example.com": {"username": "flightctl-mcu+device", "password": "<the robot's token>"}}}
```

A 401 that remains once the device has authenticated says why: `credentials refused` (Basic, or the token service refused the password), `no pull access with these credentials` (a token that does not open this repository: an account, such as a robot account, without access to it), `no pull access without credentials` (no `auth.json` entry for this registry or repository), `the registry refused the token: <error>` (a token the registry does not take, such as one signed with a key it does not know: `invalid_token`).

The credentials never appear in the device's errors or its log. A registry that refuses them shows up in `status.updated.info` as `HTTP 401`.

## What was verified

The device's registry client, built for the host, against Quay 3.17 with a robot account, and against `registry:3` with [docker_auth](https://github.com/cesanta/docker_auth) as the token service, with the realm on another host and on the registry's own host behind nginx, as Harbor has it ([TESTING.md](TESTING.md#registry-tokens)): a firmware index with the C3 and S3 images (1.1 MB) and a signed WASM layer pulled, each with one token; the refusals above; a token that ran out during a pull replaced, the image intact. The tokens were RS256, about 1 KB from Quay for a pull-only robot account and 0.8 KB from docker_auth. Not run: Harbor (no arm64 image), and Bearer tokens on a board (the same code).

## The local registry

`tools/registry/up.sh` starts one storage with two doors:

| Door | Address | For |
|---|---|---|
| Plain HTTP | `<your-ip>:5001` | The tools on this machine, to push. Devices do not use it. Anyone on the LAN can read and push. |
| TLS with a password | `registry.<your-ip>.nip.io:5443` | Devices, in a deployment |

The first run makes `.secrets/registry/` with:

- the CA;
- the server certificate, made again when the LAN IP changes;
- a random password for the user `device`;
- `auth.json` holding that password;
- `host`, the TLS door's address.

An image pushed through one door can be pulled through the other.

The push tools (`tools/fw-push.py`, `tools/signing/sign.py`, `wasm-apps/push.sh`, with the shared logic in `tools/registry/registry_client.py`) use TLS for a registry they have a CA for, and plain HTTP otherwise:

- the TLS door, named in `.secrets/registry/host`: its CA and password from `.secrets/registry/`;
- another registry: `$REGISTRY_CA` and `$REGISTRY_AUTH_FILE`;
- the plain door (`<your-ip>:5001`, as in the [quick start](QUICKSTART.md)): neither.

```bash
tools/fw-push.py $(cat .secrets/registry/host)/flightctl-mcu/fw:<tag>
```

```bash
REGISTRY=$(cat .secrets/registry/host) tools/fleet/apply.sh tools/fleet/zephyr-lab.yaml
```

When the registry has a CA, `apply.sh` adds both files to the fleet's template as a config provider named `registry` (`tools/fleet/registry-config.py`).

## Fleets kept in Git

Flight Control can take fleets from a Git repository: a `ResourceSync`, or Import Fleet in the UI. It applies the files as they are, so they cannot hold `${REGISTRY}`, and the password should not be in them. `tools/fleet/git-export.py` writes them for that: the registry spelled out, and the CA and `auth.json` mounted from two Kubernetes secrets (config of type `secretRef`), which `tools/fleet/registry-secrets.sh` makes from `.secrets/registry`. Flight Control's worker reads the secrets when it renders each device's spec; the device gets the same two files as with `apply.sh`. The Helm chart does not let the worker read secrets (`worker.clusterLevelSecretAccess: false`), and the fleet is then refused as invalid; the script also adds a Role that lets it read these two secrets only.

```bash
tools/fleet/registry-secrets.sh
```

```bash
tools/fleet/git-export.py $(cat .secrets/registry/host) <your repo>/fleets tools/fleet/idf-lab.yaml
```

Commit the files, then point a `Repository` and a `ResourceSync` at them:

```yaml
apiVersion: flightctl.io/v1beta1
kind: Repository
metadata:
  name: fleets
spec:
  type: git
  url: https://git.example.com/you/fleets.git
---
apiVersion: flightctl.io/v1beta1
kind: ResourceSync
metadata:
  name: fleets
spec:
  repository: fleets
  targetRevision: main
  path: /fleets
```

Or, in the UI, Import fleets asks for the same two:

![Import fleets in Flight Control's UI](img/import-fleets.png)
*Import fleets: after the repository, a resource sync with its revision and path.*

A fleet that a `ResourceSync` owns is changed through Git only: delete one applied with `apply.sh` before the first sync. The secrets are in the `flightctl` namespace by default (`FLEET_SECRETS_NAMESPACE`). The end-to-end test runs this against a repository served by `git daemon`.

## Moving a fleet to the TLS door

Devices pull through the TLS door only. Changing the registry's address in a fleet's `os.image` makes every device download its firmware again and restart, as Flight Control compares `os.image` as a string ([limitation 11](FLIGHTCTL-LIMITATIONS.md#11-the-server-compares-osimage-only-as-a-string)): push the release with a delta from the running one (`--delta-from`, [DELTA-UPDATES.md](DELTA-UPDATES.md)) to keep that download small.

## Memory

The spec's long-poll and the status share one HTTP/2 connection, and firmware and apps come from the registry over HTTP/2; which boards this was checked on is in [RESULTS.md](RESULTS.md#http2).

A TLS connection costs about 25 KB of RAM on these boards, 16.7 KB of it the buffer that receives the largest record the server may send. Neither Flight Control nor `registry:3` accepts smaller records (Go's TLS has neither `record_size_limit` nor `max_fragment_length`), so a connection cannot cost less, and the device holds one at a time. The agent's connection, where the spec's long-poll and the status share one HTTP/2 connection, is closed while a download from the registry runs, with manifests, signatures and blobs over one connection of its own: a status already on its way goes out first, the next one waits for the end of the download, and the agent then opens its connection again, with a new handshake. One receive buffer is reserved when the agent starts ([fc_tls_pool.h](../core/include/fc_tls_pool.h)), so a connection does not depend on finding 16.7 KB in one piece in a heap shared with WiFi and the apps. On ESP-IDF the TLS buffers are otherwise sized to each record and freed once it is read (dynamic buffers): only full-size records take the reserved one, and an idle connection holds little. Against two connections with a buffer each, that leaves about 20 KB more in the largest free block. A token service on another host is a second connection for the length of its request, with its buffer from the heap.

## What it does not do

- **No plain HTTP, no HTTP/1.1.** The registry, or the proxy in front of it, must offer HTTP/2 over TLS (ALPN `h2`), as `registry:3` does.
- **No public CA bundle.** The device trusts only the CA that the fleet gives it. A registry with a public certificate, such as `ghcr.io` or `quay.io`, takes its CA chain as `ca.crt`.
- **No redirects.** A registry that sends blobs from another host, such as S3 or a CDN, gets a 307 redirect that the device does not follow. Quay and Harbor do this when their storage is an object store, unless told to serve blobs themselves: `FEATURE_PROXY_STORAGE: true` in Quay's config, `storage_service.redirect.disable: true` in Harbor's `harbor.yml`. Serve blobs from the registry itself.
- **No read-only account in `registry:3`.** Any account can push to it. A deployment should give its devices a pull-only account, such as a robot account in Harbor or Quay. Signatures still stop a device from installing an image it did not expect.
- **The credentials stay in the fleet.** Inline config files are part of the fleet's spec, so whoever can read the fleet can read them. Flight Control 1.4 encrypts rendered config at rest. The files can also come from a Kubernetes secret (`secretRef` in the fleet): the server reads the secret and sends its keys to the device as files, as it does with inline ones, once the worker may read it ([Fleets kept in Git](#fleets-kept-in-git); tried in the end-to-end test).
- **A file removed from the fleet is not removed from the device.** The device keeps the last `auth.json` it was given until a new one replaces it.
