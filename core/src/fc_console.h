/*
 * The remote console: `flightctl console device/<name>` and the UI's terminal.
 *
 * Flight Control puts a session in the rendered spec (spec.consoles: its
 * id, and metadata with the terminal and the command); the device opens a
 * gRPC stream for it, flightctl.v1.RouterService/Stream, on the agent's
 * connection (the agent endpoint serves gRPC too), and the server joins it
 * to the user's. The bytes on it are Kubernetes' remote command protocol,
 * v5.channel.k8s.io: a channel byte (0 stdin, 1 stdout, 3 the exit status,
 * 4 a resize, 255 a channel closed) and the data.
 *
 * A microcontroller has no shell: the device answers a closed set of
 * commands (help, status, apps, reboot, exit), one per session or line by
 * line at a prompt. One session at a time, in a thread of its own while it
 * lasts; a second gets an error.
 */
#pragma once

#include "cJSON.h"
#include "fc_http.h"

/* The device's name in Flight Control (the client-name the stream says). */
void fc_console_set_device(const char *name);

/* spec.consoles of a rendered spec: opens the stream of a session not seen
 * before, on t's connection (t is used only now), and starts it. Sessions
 * are remembered across restarts, so a `reboot` does not run again. Returns
 * 1 when the spec lists a session (new or not): the server then answers the
 * long-poll at once, and the caller waits a little before the next one. */
int fc_console_sync(const cJSON *consoles, const fc_http_target_t *t);

/* Ends the session, if any, and waits for its thread: before the agent lets
 * go of its connection (a download's turn), so that it is the only one. */
void fc_console_stop(void);
