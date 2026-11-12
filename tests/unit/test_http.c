/* HTTP client: chunked bodies, headers, framing and keep-alive decisions. */
#include "../../core/src/fc_http.c"

#include <unistd.h>

#include "port_stub.h"
#include "test.h"

/* Reads a response from bytes, as if a server had sent them. */
static int read_from(const char *raw, size_t len, fc_http_response_t *resp, int *keep)
{
    FILE *f = tmpfile();
    fwrite(raw, 1, len, f);
    fflush(f);
    lseek(fileno(f), 0, SEEK_SET);
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.plain = 1;
    c.net.fd = fileno(f);
    fc_http_target_t t = {.host = "test", .port = 80, .plain = 1, .timeout_s = 1};
    int got_any;
    memset(resp, 0, sizeof(*resp));
    int rc = read_response(&c, &t, "GET", "/", resp, keep, &got_any);
    fclose(f);
    return rc;
}

#define READ(lit, resp, keep) read_from(lit, sizeof(lit) - 1, resp, keep)

static void chunk_header_parses_sizes_and_extensions(void)
{
    size_t n = 0;
    CHECK_INT(chunk_header("1a\r\n", 4, 0, &n), 4);
    CHECK_INT(n, 26);
    CHECK_INT(chunk_header("A;name=x\r\n", 10, 0, &n), 10);
    CHECK_INT(n, 10);
    CHECK_INT(chunk_header("1a", 2, 0, &n), -1);        /* incomplete */
    CHECK_INT(chunk_header("\r\n", 2, 0, &n), -2);      /* no digits */
    CHECK_INT(chunk_header("zz\r\n", 4, 0, &n), -2);    /* not hex */
    CHECK_INT(chunk_header("12\n", 3, 0, &n), -2);      /* bare LF */
    char huge[64];
    snprintf(huge, sizeof(huge), "%s\r\n", "ffffffffffffffffffff");
    CHECK_INT(chunk_header(huge, strlen(huge), 0, &n), -2); /* absurd size */
}

static void chunked_length_waits_for_the_whole_body(void)
{
    const char *body = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    size_t len = strlen(body);
    CHECK_INT(chunked_length(body, len), (long)len);
    CHECK_INT(chunked_length(body, len - 1), -1);
    CHECK_INT(chunked_length(body, 10), -1);
    CHECK_INT(chunked_length("5\r\nhelloXX0\r\n\r\n", 15), -2); /* no CRLF after data */
}

static void dechunk_joins_chunks(void)
{
    char body[] = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    CHECK_INT(dechunk(body, strlen(body)), 11);
    CHECK_STR(body, "hello world");
}

static void header_has_stays_on_its_line(void)
{
    CHECK(header_has("Connection: Close\r\nX: y", "close"));
    CHECK(!header_has("Connection: keep-alive\r\nX-Note: close", "close"));
    CHECK(header_has("Transfer-Encoding: gzip, chunked", "chunked"));
}

static void response_with_content_length_is_kept_alive(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", &r, &keep), 0);
    CHECK_INT(r.status, 200);
    CHECK_STR(r.body, "{}");
    CHECK_INT(keep, 1);
    fc_http_free(&r);
}

static void connection_close_and_http10_are_not_kept(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 1\r\n\r\nx", &r, &keep), 0);
    CHECK_INT(keep, 0);
    fc_http_free(&r);
    CHECK_INT(READ("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\nx", &r, &keep), 0);
    CHECK_INT(keep, 0);
    fc_http_free(&r);
}

static void bytes_past_the_response_drop_the_connection(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxGARBAGE", &r, &keep), 0);
    CHECK_STR(r.body, "x");
    CHECK_INT(keep, 0);
    fc_http_free(&r);
}

static void chunked_response_is_decoded(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", &r, &keep), 0);
    CHECK_STR(r.body, "abc");
    CHECK_INT(r.body_len, 3);
    CHECK_INT(keep, 1);
    fc_http_free(&r);
}

static void no_content_has_no_body(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 204 No Content\r\n\r\n", &r, &keep), 0);
    CHECK_INT(r.status, 204);
    CHECK_INT(r.body_len, 0);
    CHECK_INT(keep, 1);
    fc_http_free(&r);
}

static void body_until_close_without_length(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 200 OK\r\n\r\nuntil close", &r, &keep), 0);
    CHECK_STR(r.body, "until close");
    CHECK_INT(keep, 0);
    fc_http_free(&r);
}

static void a_challenge_is_kept(void)
{
    fc_http_response_t r;
    int keep;
    CHECK_INT(READ("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n"
                   "Www-Authenticate:  Bearer realm=\"https://r/token\",service=\"r\"\r\n\r\n", &r, &keep), 0);
    CHECK_INT(r.status, 401);
    CHECK_STR(r.www_authenticate, "Bearer realm=\"https://r/token\",service=\"r\"");
    fc_http_free(&r);
    CHECK(r.www_authenticate == NULL);
    CHECK_INT(READ("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &r, &keep), 0);
    CHECK(r.www_authenticate == NULL);
    fc_http_free(&r);
}

/* What goes out: the Authorization header, but never in the clear. */
static void credentials_are_not_sent_over_plain_http(void)
{
    FILE *f = tmpfile();
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.plain = 1;
    c.net.fd = fileno(f);
    fc_http_target_t t = {.host = "reg", .port = 5001, .plain = 1, .authorization = "Basic dTpw"};
    CHECK_INT(send_request(&c, &t, "GET", "/v2/", NULL, NULL, 0, 0), -1);
    char buf[1024] = "";
    fflush(f);
    lseek(fileno(f), 0, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    CHECK(strstr(buf, "dTpw") == NULL);
    t.authorization = NULL;
    CHECK_INT(send_request(&c, &t, "GET", "/v2/", NULL, NULL, 0, 0), 0);
    fclose(f);
}

static void truncated_and_malformed_responses_fail(void)
{
    fc_http_response_t r;
    int keep;
    CHECK(READ("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", &r, &keep) != 0);
    fc_http_free(&r);
    CHECK(READ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", &r, &keep) != 0);
    fc_http_free(&r);
    CHECK(READ("garbage\r\n\r\n", &r, &keep) != 0);
    fc_http_free(&r);
}

static void cert_id_tells_certificates_apart(void)
{
    CHECK(cert_id("cert A") == cert_id("cert A"));
    CHECK(cert_id("cert A") != cert_id("cert B"));
    CHECK(cert_id(NULL) == cert_id(""));
}

int main(void)
{
    RUN(chunk_header_parses_sizes_and_extensions);
    RUN(chunked_length_waits_for_the_whole_body);
    RUN(dechunk_joins_chunks);
    RUN(header_has_stays_on_its_line);
    RUN(response_with_content_length_is_kept_alive);
    RUN(connection_close_and_http10_are_not_kept);
    RUN(bytes_past_the_response_drop_the_connection);
    RUN(chunked_response_is_decoded);
    RUN(no_content_has_no_body);
    RUN(body_until_close_without_length);
    RUN(truncated_and_malformed_responses_fail);
    RUN(a_challenge_is_kept);
    RUN(credentials_are_not_sent_over_plain_http);
    RUN(cert_id_tells_certificates_apart);
    return TEST_RESULT();
}
