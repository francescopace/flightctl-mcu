#pragma once

/*
 * MQTT for the apps: one connection to the fleet's broker, shared by all the
 * apps on the device (MQTT 3.1.1, QoS 0 and 1, written here: no library).
 *
 * The broker comes from FC_MQTT_CONFIG, its CA from FC_MQTT_CA_FILE, both
 * written by the fleet as config files:
 *
 *   {"broker": "mqtts://broker.example.com:8883",
 *    "username": "pumps", "password": "...",
 *    "clientId": "{device}",
 *    "keepAliveSeconds": 60,
 *    "devicePrefix": "flightctl/{device}/{app}/",
 *    "fleetPrefix": "flightctl/fleet/pumps/{app}/"}
 *
 * Only "broker" is required. "mqtts://" (8883 by default, or a bare
 * host:port) is TLS, checked against the CA file, which it needs; "mqtt://"
 * (1883) is plain TCP, for a broker on a network the fleet trusts, and is
 * logged as such. {device} is the device's name in Flight Control, {app} the
 * app's. A prefix ends with '/' (one is added) and has no wildcard.
 *
 * Threads: fc_mqtt_configure on the agent's; publish, subscribe and the
 * message calls on any (the apps'); the client has a thread of its own,
 * which connects, reads, keeps the link alive and calls the hooks. Hooks are
 * called with no lock held.
 *
 * RAM: about 1.1 KB of static data (the subscriptions, the prefixes), and
 * nothing more until a broker is configured. Then the thread's stack
 * (FC_MQTT_STACK_SIZE), the receive pool (FC_MQTT_POOL_SIZE, kept from then
 * on), about 1 KB of packet buffers, and the TLS connection: on a port where
 * mbedTLS's receive buffer is the full record (Zephyr), a second reserved
 * block in the TLS pool (fc_tls_pool_extra), since this connection stays
 * open while the agent's gives its block to a download.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FC_MQTT_CONFIG  "/etc/fc/mqtt.json"
#define FC_MQTT_CA_FILE "/etc/fc/mqtt-ca.crt"

/* As in the apps' SDK (wasm-apps/sdk/fc_app.h). */
#define FC_MQTT_DEVICE      0
#define FC_MQTT_FLEET       1
#define FC_MQTT_MAX_TOPIC   64  /* an app's topic, relative to its prefix */
#define FC_MQTT_MAX_PAYLOAD 512
#define FC_MQTT_MAX_PREFIX  160 /* a prefix once {device} and {app} are in */
#define FC_MQTT_MAX_SUBS    16  /* subscriptions, all the apps together */

/* The received messages waiting in the apps' event queues, all the apps
 * together: a message takes its relative topic, its payload and 8 bytes.
 * One app holds at most half of it, so a chatty subscription cannot starve
 * the others. */
#ifndef FC_MQTT_POOL_SIZE
#define FC_MQTT_POOL_SIZE 4096
#endif

/* A received message, handed to its subscriber (on_message). */
typedef struct fc_mqtt_msg fc_mqtt_msg_t;
/* The topic relative to the subscription's prefix, not NUL terminated. */
const char *fc_mqtt_msg_topic(const fc_mqtt_msg_t *m, size_t *len);
const void *fc_mqtt_msg_payload(const fc_mqtt_msg_t *m, size_t *len);
/* Gives it back to the pool; NULL is fine. Any thread. */
void fc_mqtt_msg_free(fc_mqtt_msg_t *m);

typedef struct {
    /* A message for the subscription `sub` of `owner`. Return 0 to keep it
     * (and fc_mqtt_msg_free it later), -1 to refuse it (a full queue, an
     * app stopping): the client frees it and counts it as dropped. */
    int (*on_message)(void *owner, int sub, fc_mqtt_msg_t *m);
    /* The link to the broker came up (1) or went down (0). */
    void (*on_link)(int up);
    /* 1: a TLS handshake may start now; 0: later (one at a time on a small
     * heap, none during a download). NULL: always. Asked again every second. */
    int (*may_connect)(void);
} fc_mqtt_hooks_t;

/* Set before fc_mqtt_configure; the struct is copied. */
void fc_mqtt_set_hooks(const fc_mqtt_hooks_t *h);

/* Reads FC_MQTT_CONFIG and FC_MQTT_CA_FILE again (at boot, and when the
 * fleet changes either) and connects, reconnects to a new broker, or stops.
 * `device` is the device's name, NULL until it has enrolled (no connection
 * until then). Subscriptions are kept across it, their topics under the new
 * prefixes. Returns 1 connecting, 0 no broker configured, -1 a config that
 * cannot be used (logged; the client is stopped). */
int fc_mqtt_configure(const char *device);
/* Stops the client and closes the connection (subscriptions are kept). */
void fc_mqtt_stop(void);

/* Publishes on `app`'s device prefix. Returns 0 once written to the
 * connection (QoS 1 is not sent again if the link drops before the broker's
 * PUBACK), FC_HW_EAGAIN when not connected, FC_HW_EINVAL for a topic with a
 * wildcard, empty or too long, or a payload or QoS out of bounds. */
int fc_mqtt_publish(const char *app, const char *topic, size_t tlen, const void *payload, size_t plen, int qos,
                    int retain);

/* Subscribes `owner` (the app) under one of its prefixes: a subscription
 * handle >= 0, kept across reconnections, or FC_HW_EINVAL (a bad filter or
 * QoS), FC_HW_ENOENT (FC_MQTT_FLEET with no fleetPrefix), FC_HW_ENOSPC (no
 * subscription left). Taken even while disconnected or unconfigured. */
int fc_mqtt_subscribe(void *owner, const char *app, int scope, const char *topic, size_t tlen, int qos);
/* Closes one subscription of `owner`: 0, or FC_HW_EINVAL. */
int fc_mqtt_unsubscribe(void *owner, int sub);
/* Closes every subscription of `owner` (the app stopped or was removed). */
void fc_mqtt_drop_owner(void *owner);

typedef struct {
    int configured, connected;
    unsigned long connects;  /* CONNACKs accepted since boot */
    unsigned long published, received, dropped;
    unsigned long long tx, rx; /* bytes on the wire, TLS included */
    size_t pool_used, pool_peak;
} fc_mqtt_stats_t;
void fc_mqtt_stats(fc_mqtt_stats_t *s);

#ifdef __cplusplus
}
#endif
