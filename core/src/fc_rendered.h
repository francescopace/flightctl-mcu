#pragma once

#include <stddef.h>
#include "cJSON.h"

/* HTTP gives the rendered response to fc_rendered_feed in order. No single
 * allocation depends on its wire length. */
typedef struct fc_rendered_page fc_rendered_page_t;
typedef struct {
    fc_rendered_page_t *first, *last;
    size_t size;
} fc_rendered_body_t;

int fc_rendered_feed(const void *data, size_t len, void *ctx);
void fc_rendered_free(fc_rendered_body_t *body);

#define RENDERED_ANNOTATION "device-controller/renderedVersion"

typedef struct {
    cJSON *version, *consoles, *spec;
    int same;
} fc_rendered_t;

/* Reads the version and consoles first. Only a version that might need to
 * be applied keeps a spec. Consumes the body, including on error. */
int fc_rendered_read(fc_rendered_body_t *body, const char *known, int refresh, fc_rendered_t *out);
void fc_rendered_clear(fc_rendered_t *rendered);
