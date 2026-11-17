/*
 * The device's log in RAM (fc_log.h): a byte ring of entries, each a header
 * (the text's length, the level, the uptime) and the text, without its NUL.
 */
#include "fc_log.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define HDR 7 /* length (2), level (1), uptime (4) */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_ring[FC_LOG_RING];
static size_t s_head, s_used;          /* the oldest entry's offset; bytes in use */
static uint32_t s_head_seq, s_end_seq; /* the oldest entry's number; the next one's */

uint32_t fc_log_uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static void put_bytes(size_t at, const void *src, size_t n)
{
    const uint8_t *p = src;
    for (size_t i = 0; i < n; i++) {
        s_ring[(at + i) % FC_LOG_RING] = p[i];
    }
}

static void get_bytes(size_t at, void *dst, size_t n)
{
    uint8_t *p = dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = s_ring[(at + i) % FC_LOG_RING];
    }
}

static size_t entry_size(size_t at)
{
    uint8_t h[2];
    get_bytes(at, h, 2);
    return HDR + ((size_t)h[0] | (size_t)h[1] << 8);
}

void fc_log_put(fc_log_level_t level, const char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (len > FC_LOG_TEXT_MAX) {
        len = FC_LOG_TEXT_MAX;
    }
    uint32_t up = fc_log_uptime();
    uint8_t h[HDR] = {(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)level,
                      (uint8_t)up, (uint8_t)(up >> 8), (uint8_t)(up >> 16), (uint8_t)(up >> 24)};
    pthread_mutex_lock(&s_lock);
    while (s_used + HDR + len > FC_LOG_RING) {
        size_t n = entry_size(s_head);
        s_head = (s_head + n) % FC_LOG_RING;
        s_used -= n;
        s_head_seq++;
    }
    size_t at = (s_head + s_used) % FC_LOG_RING;
    put_bytes(at, h, HDR);
    put_bytes(at + HDR, text, len);
    s_used += HDR + len;
    s_end_seq++;
    pthread_mutex_unlock(&s_lock);
}

int fc_log_next(uint32_t *seq, fc_log_entry_t *e)
{
    pthread_mutex_lock(&s_lock);
    /* Numbers wrap: what counts is the distance from the oldest. */
    uint32_t want = *seq - s_head_seq > s_end_seq - s_head_seq ? s_head_seq : *seq;
    if (want == s_end_seq) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    size_t at = s_head;
    for (uint32_t s = s_head_seq; s != want; s++) {
        at = (at + entry_size(at)) % FC_LOG_RING;
    }
    uint8_t h[HDR];
    get_bytes(at, h, HDR);
    size_t len = (size_t)h[0] | (size_t)h[1] << 8;
    e->seq = want;
    e->level = h[2];
    e->up_s = (uint32_t)h[3] | (uint32_t)h[4] << 8 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 24;
    get_bytes(at + HDR, e->text, len);
    e->text[len] = '\0';
    *seq = want + 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

uint32_t fc_log_end(void)
{
    pthread_mutex_lock(&s_lock);
    uint32_t end = s_end_seq;
    pthread_mutex_unlock(&s_lock);
    return end;
}
