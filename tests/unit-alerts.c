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

/* Unit tests for the X.509 policy in src/alerts.c (zero-dependency, no framework).
 * Build with: make test. The module is #included directly.
 *
 * BearSSL is replaced at the link seam, the way unit-http.c replaces civetweb: a fake vtable can
 * report any verdict, where the real engine only produces one per certificate it is handed. That
 * the policy plus real BearSSL actually delivers a webhook is asserted in tests/e2e-webhook.sh. */

#include "runner.h"

#include "alerts.h"
#include "bearssl.h"

/* --- BearSSL link seam --- */

static unsigned    g_verdict;
static const char *g_seen_name;

static void fake_start_chain(const br_x509_class **ctx, const char *server_name)
{
    (void)ctx;
    g_seen_name = server_name;
}

static void fake_start_cert(const br_x509_class **ctx, uint32_t len)
{
    (void)ctx;
    (void)len;
}

static void fake_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
}

static void fake_end_cert(const br_x509_class **ctx) { (void)ctx; }

static unsigned fake_end_chain(const br_x509_class **ctx)
{
    (void)ctx;
    return g_verdict;
}

static const br_x509_pkey *fake_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    (void)ctx;
    (void)usages;
    return NULL;
}

const br_x509_class br_x509_minimal_vtable = {sizeof(br_x509_minimal_context),
                                              fake_start_chain,
                                              fake_start_cert,
                                              fake_append,
                                              fake_end_cert,
                                              fake_end_chain,
                                              fake_get_pkey};

/* Only referenced from post_webhook, which these tests do not reach; defined so the module links
 * without libbearssl.a. */
void br_ssl_client_init_full(br_ssl_client_context *cc, br_x509_minimal_context *xc,
                             const br_x509_trust_anchor *ta, size_t num)
{
    (void)cc;
    (void)xc;
    (void)ta;
    (void)num;
}

int br_ssl_client_reset(br_ssl_client_context *cc, const char *name, int resume)
{
    (void)cc;
    (void)name;
    (void)resume;
    return 0;
}

void br_ssl_engine_set_buffer(br_ssl_engine_context *cc, void *iobuf, size_t len, int bidi)
{
    (void)cc;
    (void)iobuf;
    (void)len;
    (void)bidi;
}

void br_sslio_init(br_sslio_context *ctx, br_ssl_engine_context           *eng,
                   int (*rf)(void *, unsigned char *, size_t), void       *rc,
                   int (*wf)(void *, const unsigned char *, size_t), void *wc)
{
    (void)ctx;
    (void)eng;
    (void)rf;
    (void)rc;
    (void)wf;
    (void)wc;
}

int br_sslio_write_all(br_sslio_context *ctx, const void *src, size_t len)
{
    (void)ctx;
    (void)src;
    (void)len;
    return -1;
}

int br_sslio_flush(br_sslio_context *ctx)
{
    (void)ctx;
    return -1;
}

int br_sslio_read(br_sslio_context *ctx, void *dst, size_t len)
{
    (void)ctx;
    (void)dst;
    (void)len;
    return -1;
}

int br_sslio_close(br_sslio_context *ctx)
{
    (void)ctx;
    return -1;
}

/* json and db seams: src/json.c pulls units.c and config.c, and config.c pulls vendor/tomlc17.c, so
 * the real ones would drag vendored code into a unit build. */
int json_escape(char *dst, size_t cap, const char *src)
{
    (void)src;
    if (cap)
        dst[0] = '\0';
    return 0;
}

int db_alert_on_cooldown(db_t *db, const char *alert_name, long cooldown_seconds)
{
    (void)db;
    (void)alert_name;
    (void)cooldown_seconds;
    return 1;
}

int db_alert_log_fire(db_t *db, const char *alert_name)
{
    (void)db;
    (void)alert_name;
    return 0;
}

#include "../src/alerts.c"

/* --- The policy: which verdicts are dropped --- */

static unsigned verdict_for(unsigned reported)
{
    const br_x509_class *vt = &br_x509_minimal_vtable;
    g_verdict = reported;
    return xi_end_chain(&vt);
}

static int test_drops_not_trusted(void)
{
    return verdict_for(BR_ERR_X509_NOT_TRUSTED) == 0 ? 0 : 1;
}

/* br_x509_minimal reports a good chain as 0, never as BR_ERR_X509_OK. */
static int test_keeps_success(void) { return verdict_for(0) == 0 ? 0 : 1; }

/* An unverified endpoint is still not a free pass: everything else propagates. */
static int test_propagates_expired(void)
{
    return verdict_for(BR_ERR_X509_EXPIRED) == BR_ERR_X509_EXPIRED ? 0 : 1;
}

static int test_propagates_bad_server_name(void)
{
    return verdict_for(BR_ERR_X509_BAD_SERVER_NAME) == BR_ERR_X509_BAD_SERVER_NAME ? 0 : 1;
}

static int test_propagates_empty_chain(void)
{
    return verdict_for(BR_ERR_X509_EMPTY_CHAIN) == BR_ERR_X509_EMPTY_CHAIN ? 0 : 1;
}

static int test_drops_the_server_name(void)
{
    const br_x509_class *vt = &br_x509_minimal_vtable;
    g_seen_name = "sentinel";
    xi_start_chain(&vt, "example.com");
    return g_seen_name == NULL ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(drops_not_trusted),          T(keeps_success),          T(propagates_expired),
    T(propagates_bad_server_name), T(propagates_empty_chain), T(drops_the_server_name),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
