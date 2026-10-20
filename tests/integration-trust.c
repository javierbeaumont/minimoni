/*
 * minimoni: zero-dependency system monitoring
 *
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This file is part of minimoni.
 *
 * minimoni is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * minimoni is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with minimoni. If not, see <https://www.gnu.org/licenses/>.
 */

/* Integration tests for src/trust.c against the real BearSSL decoders and br_x509_minimal engine,
 * in process and with no sockets. Run with: make test-integration.
 *
 * The Makefile generates two CAs, one RSA and one EC, and a leaf the RSA one signed for
 * minimoni.test; each test writes the PEM file it needs from them. */

#include <stdio.h>
#include <string.h>

#include "runner.h"
#include "trust.h"

#define PEM_FILE "build/trust-test.pem"

static char          g_ca[8192], g_ec[8192];
static unsigned char g_leaf[8192];
static size_t        g_leaf_len;

static const char KEY_PEM[] = "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n";
/* Valid base64, but not the DER of a certificate. */
static const char NOT_DER_PEM[] = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
/* '!' is not base64. */
static const char BAD_PEM[] = "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n";

/* Write the concatenation of `parts` (NULL-terminated) to PEM_FILE, then load it. */
static int load_parts(trust_t *t, const char *const *parts)
{
    FILE *f = fopen(PEM_FILE, "w");
    if (!f)
        return -2;
    for (; *parts; parts++)
        fputs(*parts, f);
    fclose(f);

    return trust_load(t, PEM_FILE);
}

/* br_x509_minimal's verdict on the leaf alone, checked against the anchors in `t`. */
static unsigned verify_leaf(const trust_t *t, const char *name)
{
    br_x509_minimal_context xc;
    br_x509_minimal_init_full(&xc, t->anchors, t->count);

    const br_x509_class **vt = &xc.vtable;
    (*vt)->start_chain(vt, name);
    (*vt)->start_cert(vt, (uint32_t)g_leaf_len);
    (*vt)->append(vt, g_leaf, g_leaf_len);
    (*vt)->end_cert(vt);

    return (*vt)->end_chain(vt);
}

static int test_loads_each_certificate(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){g_ca, g_ec, NULL});
    int ok = n == 2 && t.count == 2 && t.anchors[0].pkey.key_type == BR_KEYTYPE_RSA &&
             t.anchors[1].pkey.key_type == BR_KEYTYPE_EC && (t.anchors[0].flags & BR_X509_TA_CA) &&
             (t.anchors[1].flags & BR_X509_TA_CA);
    trust_free(&t);

    return ok ? 0 : 1;
}

/* Bundles carry comment lines, and a PEM file may hold other objects, such as a key. */
static int test_skips_text_and_other_objects(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){"# a comment\n", KEY_PEM, g_ca, NULL});
    trust_free(&t);

    return n == 1 ? 0 : 1;
}

static int test_no_certificate_loads_none(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){KEY_PEM, NULL});
    trust_free(&t);

    return n == 0 ? 0 : 1;
}

/* Valid PEM whose content is not a certificate: skipped, the rest still load. */
static int test_skips_an_undecodable_certificate(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){NOT_DER_PEM, g_ca, NULL});
    trust_free(&t);

    return n == 1 ? 0 : 1;
}

static int test_missing_file_fails(void)
{
    trust_t t = {0};
    int     n = trust_load(&t, "build/trust-missing.pem");
    trust_free(&t);

    return n == -1 ? 0 : 1;
}

static int test_invalid_pem_fails(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){BAD_PEM, NULL});
    trust_free(&t);

    return n == -1 ? 0 : 1;
}

static int test_unfinished_certificate_fails(void)
{
    char cut[sizeof(g_ca)];
    snprintf(cut, sizeof(cut), "%s", g_ca);
    *strstr(cut, "-----END") = '\0';

    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){cut, NULL});
    trust_free(&t);

    return n == -1 ? 0 : 1;
}

static int test_last_line_needs_no_newline(void)
{
    char bare[sizeof(g_ca)];
    snprintf(bare, sizeof(bare), "%s", g_ca);
    bare[strlen(bare) - 1] = '\0';

    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){bare, NULL});
    trust_free(&t);

    return n == 1 ? 0 : 1;
}

static int test_anchors_verify_the_leaf(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){g_ec, g_ca, NULL});
    int     ok = n == 2 && verify_leaf(&t, "minimoni.test") == 0;
    trust_free(&t);

    return ok ? 0 : 1;
}

static int test_name_mismatch_is_refused(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){g_ca, NULL});
    int     ok = n == 1 && verify_leaf(&t, "other.test") == BR_ERR_X509_BAD_SERVER_NAME;
    trust_free(&t);

    return ok ? 0 : 1;
}

static int test_another_ca_is_not_trusted(void)
{
    trust_t t = {0};
    int     n = load_parts(&t, (const char *const[]){g_ec, NULL});
    int     ok = n == 1 && verify_leaf(&t, "minimoni.test") == BR_ERR_X509_NOT_TRUSTED;
    trust_free(&t);

    return ok ? 0 : 1;
}

/* The toolchain image ships a CA bundle; every certificate in it must load. */
static int test_system_store_loads_whole(void)
{
    const char *path = trust_system_file();
    if (!path)
        return 1;

    FILE *f = fopen(path, "r");
    if (!f)
        return 1;
    char line[256];
    int  certs = 0;
    while (fgets(line, sizeof(line), f))
        certs += strcmp(line, "-----BEGIN CERTIFICATE-----\n") == 0;
    fclose(f);

    trust_t t = {0};
    int     n = trust_load(&t, path);
    trust_free(&t);

    return certs > 0 && n == certs ? 0 : 1;
}

static int test_free_leaves_it_zeroed(void)
{
    trust_t t = {0};
    load_parts(&t, (const char *const[]){g_ca, NULL});
    trust_free(&t);
    trust_free(&t);

    return t.anchors == NULL && t.count == 0 ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(loads_each_certificate),       T(skips_text_and_other_objects),
    T(no_certificate_loads_none),    T(skips_an_undecodable_certificate),
    T(missing_file_fails),           T(invalid_pem_fails),
    T(unfinished_certificate_fails), T(last_line_needs_no_newline),
    T(anchors_verify_the_leaf),      T(name_mismatch_is_refused),
    T(another_ca_is_not_trusted),    T(system_store_loads_whole),
    T(free_leaves_it_zeroed),
};

/* Read a whole fixture into `buf`; returns its length, or 0 when missing, empty or too large. */
static size_t slurp(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  cannot open %s; the Makefile generates it\n", path);
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);

    return n < cap ? n : 0;
}

int main(void)
{
    size_t ca = slurp("build/trust-ca.pem", g_ca, sizeof(g_ca) - 1);
    size_t ec = slurp("build/trust-ec.pem", g_ec, sizeof(g_ec) - 1);
    g_leaf_len = slurp("build/trust-leaf.der", g_leaf, sizeof(g_leaf));
    if (ca == 0 || ec == 0 || g_leaf_len == 0)
        return 2;

    int rc = run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
    remove(PEM_FILE);

    return rc;
}
