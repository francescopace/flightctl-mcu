#!/usr/bin/env bash
# The apps' MQTT client (core/src/fc_mqtt.c) against mosquitto in Docker,
# without and with TLS (tests/unit/mqtt_broker_check.c says what is checked).
# Needs docker, openssl, cmake, ninja and mbedTLS 4 (MbedTLS_DIR); about two
# minutes. The broker's container and its files are removed at the end.
# Usage: tests/mqtt-broker.sh [cmake args...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMAGE=eclipse-mosquitto:2.0.22
NAME=fc-mqtt-test-$$
USER_NAME=fc
PASS=s3cret-$$
WORK="$(mktemp -d)"
cleanup() {
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}
PLAIN_PORT=$(free_port)
TLS_PORT=$(free_port)

echo "== certificates"
cert() { # name, subject, issuer (empty: self-signed), SAN
    openssl ecparam -name prime256v1 -genkey -noout -out "$WORK/$1.key" 2>/dev/null
    if [ -z "$3" ]; then
        openssl req -x509 -new -key "$WORK/$1.key" -subj "$2" -days 2 -out "$WORK/$1.crt" \
            -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null
    else
        openssl req -new -key "$WORK/$1.key" -subj "$2" -out "$WORK/$1.csr" 2>/dev/null
        printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\n' "$4" >"$WORK/$1.ext"
        openssl x509 -req -in "$WORK/$1.csr" -CA "$WORK/$3.crt" -CAkey "$WORK/$3.key" -CAcreateserial \
            -days 2 -out "$WORK/$1.crt" -extfile "$WORK/$1.ext" 2>/dev/null
    fi
}
cert ca "/CN=fc-mqtt-test CA" "" ""
cert wrong "/CN=another CA" "" ""
cert server "/CN=localhost" ca "DNS:localhost,IP:127.0.0.1"

mkdir -p "$WORK/config"
cp "$WORK/ca.crt" "$WORK/server.crt" "$WORK/server.key" "$WORK/config/"
cat >"$WORK/config/mosquitto.conf" <<EOF
per_listener_settings false
allow_anonymous false
password_file /mosquitto/config/passwd
log_dest stdout
listener 1883
listener 8883
cafile /mosquitto/config/ca.crt
certfile /mosquitto/config/server.crt
keyfile /mosquitto/config/server.key
EOF
docker run --rm -v "$WORK/config:/mosquitto/config" "$IMAGE" \
    mosquitto_passwd -b -c /mosquitto/config/passwd "$USER_NAME" "$PASS"
chmod 0644 "$WORK/config/"*

echo "== broker $IMAGE on $PLAIN_PORT (plain) and $TLS_PORT (TLS)"
docker run -d --name "$NAME" -p "127.0.0.1:$PLAIN_PORT:1883" -p "127.0.0.1:$TLS_PORT:8883" \
    -v "$WORK/config:/mosquitto/config" "$IMAGE" >/dev/null
for _ in $(seq 50); do
    docker exec "$NAME" mosquitto_sub -h localhost -p 1883 -u "$USER_NAME" -P "$PASS" -t x -W 1 -C 1 \
        >/dev/null 2>&1 && break
    [ $? -eq 27 ] && break # the wait timed out: the broker answers
    sleep 0.2
done

echo "== build"
cmake -S . -B build/mqtt -G Ninja -DFC_UNIT=ON "$@" >/dev/null
cmake --build build/mqtt --target mqtt-broker-check >/dev/null

echo "== run"
MQTT_PLAIN="localhost:$PLAIN_PORT" MQTT_TLS="localhost:$TLS_PORT" MQTT_CA="$WORK/ca.crt" \
    MQTT_WRONG_CA="$WORK/wrong.crt" MQTT_USER="$USER_NAME" MQTT_PASS="$PASS" MQTT_CONTAINER="$NAME" \
    build/mqtt/mqtt-broker-check || { docker logs "$NAME" 2>&1 | tail -30; exit 1; }
