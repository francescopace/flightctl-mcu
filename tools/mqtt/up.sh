#!/usr/bin/env bash
# MQTT broker for the lab's apps (docs/APPS.md#mqtt): mosquitto in Docker, on
# TLS_PORT (8883) of this machine as mqtt.<LAN IP>.nip.io, with a password.
# Its plain listener (1883) is not published: watch it from inside, e.g.
#   docker exec fc-mqtt mosquitto_sub -p 1883 -u device -P "$(cat .secrets/mqtt/password)" -v -t 'flightctl/#'
# The CA, the server certificate and the password are made once in
# .secrets/mqtt/ (SECRETS); the certificate again when the LAN IP has changed.
# The user "device" may only use flightctl/#. tools/fleet/mqtt-config.py
# turns these into a fleet's /etc/fc/mqtt.json and /etc/fc/mqtt-ca.crt.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/tools/local-ip.sh"
IP="${HOST_IP:-$(local_ip)}"
TLS_PORT="${TLS_PORT:-8883}"
SECRETS="${SECRETS:-$ROOT/.secrets/mqtt}"
IMAGE="${IMAGE:-docker.io/library/eclipse-mosquitto:2.1.4-alpine}"
NAME=fc-mqtt
HOST="mqtt.$IP.nip.io"
USER_NAME=device
mkdir -p "$SECRETS/config"
chmod 700 "$SECRETS"
cd "$SECRETS"

if [[ ! -f ca.key ]]; then
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ca.key 2>/dev/null
  openssl req -x509 -new -key ca.key -sha256 -days 3650 -subj "/CN=flightctl-mcu MQTT CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt
  rm -f server.crt
fi
if [[ ! -f server.crt ]] || ! openssl x509 -in server.crt -noout -checkhost "$HOST" 2>/dev/null | grep -q 'does match'; then
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out server.key 2>/dev/null
  openssl req -new -key server.key -subj "/CN=$HOST" -out server.csr
  printf 'subjectAltName=DNS:%s,DNS:localhost,IP:%s,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature\nbasicConstraints=critical,CA:FALSE\n' \
    "$HOST" "$IP" > server.ext
  openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -sha256 \
    -extfile server.ext -out server.crt 2>/dev/null
  rm -f server.csr server.ext
fi
if [[ ! -f password ]]; then
  (umask 077; openssl rand -hex 16 > password)
fi
echo "$HOST:$TLS_PORT" > host

# What the container reads: readable by its mosquitto user.
cp ca.crt server.crt server.key config/
cat > config/mosquitto.conf <<EOF
per_listener_settings false
allow_anonymous false
password_file /mosquitto/config/passwd
acl_file /mosquitto/config/acl
log_dest stdout
log_type error
log_type warning
log_type notice
listener 1883
listener 8883
cafile /mosquitto/config/ca.crt
certfile /mosquitto/config/server.crt
keyfile /mosquitto/config/server.key
EOF
printf 'user %s\ntopic readwrite flightctl/#\n' "$USER_NAME" > config/acl
rm -f config/passwd
docker run --rm -v "$SECRETS/config:/mosquitto/config" "$IMAGE" \
  mosquitto_passwd -b -c /mosquitto/config/passwd "$USER_NAME" "$(cat password)"
chmod 0644 config/*

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --restart unless-stopped \
  -p "0.0.0.0:${TLS_PORT}:8883" -v "$SECRETS/config:/mosquitto/config" "$IMAGE" >/dev/null
echo "MQTT broker on mqtts://$HOST:$TLS_PORT (user $USER_NAME, $SECRETS)"
