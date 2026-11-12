#!/usr/bin/env python3
"""How the tools reach a registry, by the rule the devices follow: TLS when
there is a CA for it, plain HTTP otherwise; credentials from an auth.json
(podman's format), only over TLS and only when the registry asks.

The CA is $REGISTRY_CA, or .secrets/registry/ca.crt for the registry named in
.secrets/registry/host (what tools/registry/up.sh makes). The credentials are
in $REGISTRY_AUTH_FILE, or .secrets/registry/auth.json. Stdlib only.

    registry_client.py oras-args HOST:PORT   the oras flags for that registry
"""
import base64
import json
import os
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECRETS = os.environ.get("REGISTRY_SECRETS") or os.path.join(ROOT, ".secrets", "registry")


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def ca_for(hostport):
    """The CA file for this registry, or None (plain HTTP)."""
    if os.environ.get("REGISTRY_CA"):
        return os.environ["REGISTRY_CA"]
    if _read(os.path.join(SECRETS, "host")) == hostport and os.path.exists(os.path.join(SECRETS, "ca.crt")):
        return os.path.join(SECRETS, "ca.crt")
    return None


def auth_file():
    return os.environ.get("REGISTRY_AUTH_FILE") or os.path.join(SECRETS, "auth.json")


def key_matches(key, hostport, repo):
    """Length of an auth.json key that is for this registry and repository, 0 if not."""
    for scheme in ("https://", "http://"):
        if key.startswith(scheme):
            key = key[len(scheme):]
    key = key.rstrip("/")
    if key == hostport:
        return len(key)
    if key.startswith(hostport + "/"):
        ns = key[len(hostport) + 1:]
        if repo == ns or repo.startswith(ns + "/"):
            return len(key)
    return 0


def basic_for(hostport, repo=""):
    """base64 of user:password for this registry, or None."""
    text = _read(auth_file())
    if not text:
        return None
    best, best_len = None, 0
    for key, entry in json.loads(text).get("auths", {}).items():
        n = key_matches(key, hostport, repo)
        if n > best_len:
            best, best_len = entry, n
    if not best:
        return None
    if best.get("auth"):
        return best["auth"]
    if "username" in best and "password" in best:
        return base64.b64encode(f"{best['username']}:{best['password']}".encode()).decode()
    return None


def base_url(hostport):
    return ("https://" if ca_for(hostport) else "http://") + hostport


def _challenge(header):
    scheme, _, rest = (header or "").partition(" ")
    params = {}
    for part in urllib.request.parse_http_list(rest):
        k, _, v = part.partition("=")
        params[k.strip().lower()] = v.strip().strip('"')
    return scheme.lower(), params


_authorization = {}  # hostport -> Authorization header, once a registry has asked


def _context(hostport):
    ca = ca_for(hostport)
    return ssl.create_default_context(cafile=ca) if ca else None


def _authenticate(hostport, repo, header):
    scheme, params = _challenge(header)
    if not ca_for(hostport):
        sys.exit(f"registry {hostport} asks for credentials: they go over TLS only, and there is no CA for it")
    basic = basic_for(hostport, repo)
    if scheme == "basic":
        if not basic:
            sys.exit(f"registry {hostport} asks for credentials, and {auth_file()} has none for it")
        return f"Basic {basic}"
    if scheme == "bearer":
        realm = params.get("realm", "")
        if not realm.startswith("https://"):
            sys.exit(f"registry {hostport}: its token service is not on https")
        query = {k: params[k] for k in ("service", "scope") if k in params}
        url = realm + ("&" if "?" in realm else "?") + urllib.parse.urlencode(query)
        req = urllib.request.Request(url, headers={"Authorization": f"Basic {basic}"} if basic else {})
        token_host = urllib.parse.urlsplit(realm).netloc
        with urllib.request.urlopen(req, context=_context(token_host) or _context(hostport)) as r:
            body = json.loads(r.read())
        return f"Bearer {body.get('token') or body.get('access_token')}"
    sys.exit(f"registry {hostport}: authentication {scheme} is not supported")


def request(method, url, data=None, headers=None):
    """(status, headers, body); raises urllib.error.HTTPError as urlopen does."""
    parts = urllib.parse.urlsplit(url)
    hostport = parts.netloc
    repo = parts.path[len("/v2/"):].split("/manifests/")[0].split("/blobs/")[0] if parts.path.startswith("/v2/") else ""
    for attempt in (0, 1):
        h = dict(headers or {})
        if parts.scheme == "https" and hostport in _authorization:
            h["Authorization"] = _authorization[hostport]
        req = urllib.request.Request(url, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, context=_context(hostport) if parts.scheme == "https" else None) as r:
                return r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            if e.code != 401 or attempt:
                raise
            _authorization[hostport] = _authenticate(hostport, repo, e.headers.get("WWW-Authenticate"))


def oras_args(hostport):
    ca = ca_for(hostport)
    if not ca:
        return ["--plain-http"]
    args = ["--ca-file", ca]
    if os.path.exists(auth_file()):
        args += ["--registry-config", auth_file()]
    return args


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "oras-args":
        print(" ".join(oras_args(sys.argv[2])))
    else:
        sys.exit(__doc__)
