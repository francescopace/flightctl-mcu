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
 * commands (help, status, apps, log, reboot, exit), one per session, line by
 * line at a prompt, or read from stdin as `bash -s` would. For the UI's Logs
 * tab, which runs journalctl in such a session, it also takes the little of
 * a shell's that the tab sends: quotes, ;, &&, ||, a pipe to tail, sudo,
 * bash -c, printf, echo, test, and journalctl's options, answered from the
 * log kept in RAM (fc_log.h). One session at a time, in a thread of its own
 * while it lasts; a second gets an error.
 */
#pragma once

#include "cJSON.h"
#include "fc_http.h"

/* The device's name in Flight Control (the client-name the stream says). */
void fc_console_set_device(const char *name);

/* spec.consoles of a rendered spec: opens the stream of a session not seen
 * before, on t's connection (t is used only now), and starts it. Sessions
 * are remembered across restarts, so a `reboot` does not run again. Returns
 * 0 when the spec lists no session; 1 when it lists only sessions seen
 * before, none running (Flight Control 1.4 keeps listing a session whose
 * user left before the device came); 2 when one is new or a session runs.
 * While any is listed the server answers the long-poll at once, and the
 * caller waits a little before the next one. */
int fc_console_sync(const cJSON *consoles, const fc_http_target_t *t);

/* Lock-free: the spec loop can reclaim a finished thread and its stack. */
int fc_console_finished(void);

/* Ends the session, if any, and waits for its thread: before the agent lets
 * go of its connection (a download's turn), so that it is the only one. */
void fc_console_stop(void);
