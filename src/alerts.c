/*
 * minimoni - zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"

#include "alerts.h"
#include "json.h"

/* --- Metric lookup --- */

static double get_metric_value(const db_row_t *row, const char *m, int *valid)
{
    *valid = 1;
    if (!strcmp(m, "load_1m"))
        return row->load_1m;
    if (!strcmp(m, "load_5m"))
        return row->load_5m;
    if (!strcmp(m, "load_15m"))
        return row->load_15m;
    if (!strcmp(m, "cpu_user_percent")) {
        *valid = row->cpu_valid;
        return row->cpu_user_percent;
    }
    if (!strcmp(m, "cpu_system_percent")) {
        *valid = row->cpu_valid;
        return row->cpu_system_percent;
    }
    if (!strcmp(m, "cpu_idle_percent")) {
        *valid = row->cpu_valid;
        return row->cpu_idle_percent;
    }
    if (!strcmp(m, "mem_total_mb"))
        return row->mem_total_mb;
    if (!strcmp(m, "mem_used_mb"))
        return row->mem_used_mb;
    if (!strcmp(m, "mem_available_mb"))
        return row->mem_available_mb;
    if (!strcmp(m, "mem_percent"))
        return row->mem_percent;
    if (!strcmp(m, "disk_total_gb"))
        return row->disk_total_gb;
    if (!strcmp(m, "disk_used_gb"))
        return row->disk_used_gb;
    if (!strcmp(m, "disk_free_gb"))
        return row->disk_free_gb;
    if (!strcmp(m, "disk_percent"))
        return row->disk_percent;
    if (!strcmp(m, "temp_celsius")) {
        *valid = row->temp_valid;
        return row->temp_celsius;
    }
    if (!strcmp(m, "net_rx_bps")) {
        *valid = row->net_valid;
        return row->net_rx_bps;
    }
    if (!strcmp(m, "net_tx_bps")) {
        *valid = row->net_valid;
        return row->net_tx_bps;
    }
    if (!strcmp(m, "uptime_seconds"))
        return row->uptime_seconds;
    *valid = 0;
    fprintf(stderr, "alerts: unknown metric '%s'\n", m);
    return 0.0;
}

/* --- Condition evaluation --- */

static int eval_op(double value, const char *op, double threshold)
{
    if (!strcmp(op, ">"))
        return value > threshold;
    if (!strcmp(op, "<"))
        return value < threshold;
    if (!strcmp(op, ">="))
        return value >= threshold;
    if (!strcmp(op, "<="))
        return value <= threshold;
    if (!strcmp(op, "=="))
        return value == threshold;
    return 0;
}

/* --- Webhook (HTTP and HTTPS) --- */

static int parse_url(const char *url, char *host, size_t hsz, char *port_str, size_t psz,
                     char *path, size_t pasz)
{
    int         https = 0;
    const char *start;

    if (strncmp(url, "https://", 8) == 0) {
        https = 1;
        start = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    } else {
        fprintf(stderr, "alerts: webhook requires http:// or https:// (got: %.64s)\n", url);
        return -1;
    }

    const char *slash = strchr(start, '/');
    const char *colon = memchr(start, ':', slash ? (size_t)(slash - start) : strlen(start));

    if (colon) {
        snprintf(host, hsz, "%.*s", (int)(colon - start), start);
        int plen = slash ? (int)(slash - colon - 1) : (int)strlen(colon + 1);
        snprintf(port_str, psz, "%.*s", plen, colon + 1);
    } else {
        int hlen = slash ? (int)(slash - start) : (int)strlen(start);
        snprintf(host, hsz, "%.*s", hlen, start);
        snprintf(port_str, psz, https ? "443" : "80");
    }
    snprintf(path, pasz, "%s", slash ? slash : "/");
    return https;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0)
            break;
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

/* Encrypt without authenticating, for insecure_skip_verify. The real engine still parses the chain,
 * which is where the public key comes from. Passing no server name is how br_x509_minimal is told
 * to skip the name check; the SNI the engine sends is unaffected. */
static void xi_start_chain(const br_x509_class **ctx, const char *server_name)
{
    (void)server_name;
    br_x509_minimal_vtable.start_chain(ctx, NULL);
}

static unsigned xi_end_chain(const br_x509_class **ctx)
{
    unsigned err = br_x509_minimal_vtable.end_chain(ctx);
    return err == BR_ERR_X509_NOT_TRUSTED ? 0 : err;
}

static int tls_sock_read(void *ctx, unsigned char *buf, size_t len)
{
    return (int)read(*(int *)ctx, buf, len);
}
static int tls_sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    return (int)write(*(int *)ctx, buf, len);
}

static void post_webhook(const config_t *cfg, const alert_cfg_t *a, double value,
                         const char *timestamp, const char *hostname)
{
    char host[256], port_str[8], path[512];
    int  scheme =
        parse_url(a->webhook, host, sizeof(host), port_str, sizeof(port_str), path, sizeof(path));
    if (scheme < 0)
        return;

    if (scheme == 1 && !cfg->insecure_skip_verify) {
        fprintf(stderr,
                "alerts: '%s' uses https but minimoni cannot verify certificates; set "
                "insecure_skip_verify = true under [webhook] to send it unverified\n",
                a->name);
        return;
    }

    /* Not op: valid_op() checks it at load. The other three are free-form. */
    char ename[256], emetric[256], ehost[512];
    if (json_escape(ename, sizeof(ename), a->name) < 0 ||
        json_escape(emetric, sizeof(emetric), a->metric) < 0 ||
        json_escape(ehost, sizeof(ehost), hostname) < 0) {
        fprintf(stderr, "alerts: '%.32s' does not fit escaped; webhook skipped\n", a->name);
        return;
    }

    char body[1024];
    int  blen = snprintf(body, sizeof(body),
                         "{\"alert\":\"%s\",\"metric\":\"%s\",\"value\":%.6g,"
                         "\"threshold\":%.6g,\"operator\":\"%s\","
                         "\"timestamp\":\"%s\",\"hostname\":\"%s\"}",
                         ename, emetric, value, a->threshold, a->op, timestamp, ehost);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "alerts: webhook dns failed for %s\n", host);
        return;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        fprintf(stderr, "alerts: webhook connect to %s failed\n", host);
        close(fd);
        return;
    }
    if (rc != 0) {
        fd_set         wfds;
        struct timeval ctv = {5, 0};
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        if (select(fd + 1, NULL, &wfds, NULL, &ctv) <= 0) {
            fprintf(stderr, "alerts: webhook connect to %s timed out\n", host);
            close(fd);
            return;
        }
        int       err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            fprintf(stderr, "alerts: webhook connect to %s failed\n", host);
            close(fd);
            return;
        }
    }
    fcntl(fd, F_SETFL, flags);

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[1024];
    int  rlen = snprintf(req, sizeof(req),
                         "POST %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "%s",
                         path, host, blen, body);

    if (scheme == 0) {
        write_all(fd, req, (size_t)rlen);
        char buf[256];
        while (read(fd, buf, sizeof(buf)) > 0)
            ;
    } else {
        br_ssl_client_context   sc;
        br_x509_minimal_context xc;
        unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
        br_sslio_context        ioc;

        /* Not a constant expression, so this cannot be a static initialiser. */
        br_x509_class insecure_vt = br_x509_minimal_vtable;
        insecure_vt.start_chain = xi_start_chain;
        insecure_vt.end_chain = xi_end_chain;

        br_ssl_client_init_full(&sc, &xc, NULL, 0);
        xc.vtable = &insecure_vt;
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
        br_ssl_client_reset(&sc, host, 0);
        br_sslio_init(&ioc, &sc.eng, tls_sock_read, &fd, tls_sock_write, &fd);

        /* Only delivery is checked: plenty of endpoints close without close_notify, which BearSSL
         * reports as an error even though the POST did arrive. */
        if (br_sslio_write_all(&ioc, req, (size_t)rlen) != 0 || br_sslio_flush(&ioc) != 0) {
            fprintf(stderr, "alerts: webhook to %s not delivered, TLS error %d\n", host,
                    br_ssl_engine_last_error(&sc.eng));
        } else {
            char buf[256];
            while (br_sslio_read(&ioc, buf, sizeof(buf)) > 0)
                ;
            br_sslio_close(&ioc);
        }
    }

    close(fd);
}

/* --- Command execution --- */

static void run_command(const alert_cfg_t *a, double value)
{
    char val_str[32], thr_str[32];
    snprintf(val_str, sizeof(val_str), "%.6g", value);
    snprintf(thr_str, sizeof(thr_str), "%.6g", a->threshold);

    setenv("MINIMONI_ALERT_NAME", a->name, 1);
    setenv("MINIMONI_ALERT_METRIC", a->metric, 1);
    setenv("MINIMONI_ALERT_VALUE", val_str, 1);
    setenv("MINIMONI_ALERT_THRESHOLD", thr_str, 1);
    setenv("MINIMONI_ALERT_OPERATOR", a->op, 1);

    /* a->command is a static value from the operator's config; the dynamic alert
     * data is exported via the MINIMONI_ALERT_* env vars above, never the string. */
    /* NOLINTNEXTLINE(bugprone-command-processor) no untrusted input reaches the command */
    int ret = system(a->command);
    if (ret != 0)
        fprintf(stderr, "alerts: command '%s' exited %d\n", a->command, ret);

    unsetenv("MINIMONI_ALERT_NAME");
    unsetenv("MINIMONI_ALERT_METRIC");
    unsetenv("MINIMONI_ALERT_VALUE");
    unsetenv("MINIMONI_ALERT_THRESHOLD");
    unsetenv("MINIMONI_ALERT_OPERATOR");
}

/* --- Public API --- */

int alerts_evaluate(db_t *db, const config_t *cfg, const db_row_t *row)
{
    if (cfg->alert_count == 0)
        return 0;

    char hostname[128];
    if (cfg->title[0])
        snprintf(hostname, sizeof(hostname), "%s", cfg->title);
    else if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");

    for (int i = 0; i < cfg->alert_count; i++) {
        const alert_cfg_t *a = &cfg->alerts[i];

        int    valid;
        double value = get_metric_value(row, a->metric, &valid);
        if (!valid)
            continue;

        if (!eval_op(value, a->op, a->threshold))
            continue;

        if (db_alert_on_cooldown(db, a->name, a->cooldown_seconds) != 0)
            continue;

        fprintf(stderr, "alerts: firing '%s' (%.6g %s %.6g)\n", a->name, value, a->op,
                a->threshold);

        if (a->webhook[0])
            post_webhook(cfg, a, value, row->timestamp, hostname);
        if (a->command[0])
            run_command(a, value);

        if (db_alert_log_fire(db, a->name) != 0)
            fprintf(stderr,
                    "alerts: '%s' fired but its cooldown was not recorded; it will fire "
                    "again next cycle\n",
                    a->name);
    }

    return 0;
}
