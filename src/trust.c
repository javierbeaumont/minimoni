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

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "trust.h"

/* Go's list (crypto/x509/root_linux.go), which covers the major distributions. */
static const char *const SYSTEM_CA_FILES[] = {
    "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Gentoo and others */
    "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL 6 */
    "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
    "/etc/pki/tls/cacert.pem",                           /* OpenELEC */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* CentOS, RHEL 7 */
    "/etc/ssl/cert.pem",                                 /* Alpine */
};

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
    int            failed;
} bytes_t;

/* BearSSL's output callback: append to a growing buffer, recording a failed allocation. */
static void bytes_append(void *ctx, const void *src, size_t len)
{
    bytes_t *b = ctx;
    if (b->failed)
        return;

    if (len > b->cap - b->len) {
        size_t cap = b->cap ? b->cap : 1024;
        while (len > cap - b->len)
            cap *= 2;

        unsigned char *p = realloc(b->data, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->data = p;
        b->cap = cap;
    }

    memcpy(b->data + b->len, src, len);
    b->len += len;
}

static unsigned char *copy(const unsigned char *src, size_t len)
{
    unsigned char *p = malloc(len);
    if (p)
        memcpy(p, src, len);

    return p;
}

static void free_anchor(br_x509_trust_anchor *ta)
{
    free(ta->dn.data);
    if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
        free(ta->pkey.key.rsa.n);
        free(ta->pkey.key.rsa.e);
    } else {
        free(ta->pkey.key.ec.q);
    }
}

/* Decode one DER certificate into the next anchor: its subject, its key and whether it is a CA.
 * The key is copied out of the decoder, which owns it. Returns 0, or -1 to skip the certificate. */
static int add_anchor(trust_t *t, const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    bytes_t                 dn = {0};

    br_x509_decoder_init(&dc, bytes_append, &dn);
    br_x509_decoder_push(&dc, der, len);

    const br_x509_pkey  *pk = br_x509_decoder_get_pkey(&dc);
    br_x509_trust_anchor ta = {.dn = {dn.data, dn.len}};

    if (!pk || dn.failed) {
        free(dn.data);
        return -1;
    }

    ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta.pkey.key_type = pk->key_type;

    int copied = 0;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        br_rsa_public_key *rsa = &ta.pkey.key.rsa;
        rsa->n = copy(pk->key.rsa.n, pk->key.rsa.nlen);
        rsa->nlen = pk->key.rsa.nlen;
        rsa->e = copy(pk->key.rsa.e, pk->key.rsa.elen);
        rsa->elen = pk->key.rsa.elen;
        copied = rsa->n && rsa->e;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        br_ec_public_key *ec = &ta.pkey.key.ec;
        ec->curve = pk->key.ec.curve;
        ec->q = copy(pk->key.ec.q, pk->key.ec.qlen);
        ec->qlen = pk->key.ec.qlen;
        copied = ec->q != NULL;
    }

    br_x509_trust_anchor *grown = copied ? realloc(t->anchors, (t->count + 1) * sizeof(ta)) : NULL;

    if (!grown) {
        free_anchor(&ta);
        return -1;
    }

    t->anchors = grown;
    t->anchors[t->count++] = ta;

    return 0;
}

/* A PEM file being read: the decoder, the certificate it is decoding and what came out so far. */
typedef struct {
    br_pem_decoder_context pc;
    bytes_t                der;
    trust_t               *t;
    int                    in_object;
    int                    in_cert;
    int                    loaded;
    int                    bad;
} pem_read_t;

static void pem_push(pem_read_t *r, const unsigned char *p, size_t n)
{
    while (n > 0 && !r->bad) {
        size_t used = br_pem_decoder_push(&r->pc, p, n);
        p += used;
        n -= used;

        switch (br_pem_decoder_event(&r->pc)) {
        case BR_PEM_BEGIN_OBJ:
            r->in_object = 1;
            r->in_cert = strcmp(br_pem_decoder_name(&r->pc), "CERTIFICATE") == 0;
            r->der.len = 0;
            r->der.failed = 0;
            br_pem_decoder_setdest(&r->pc, r->in_cert ? bytes_append : NULL, &r->der);
            break;
        case BR_PEM_END_OBJ:
            if (r->in_cert && !r->der.failed && add_anchor(r->t, r->der.data, r->der.len) == 0)
                r->loaded++;
            r->in_object = r->in_cert = 0;
            break;
        case BR_PEM_ERROR:
            r->bad = 1;
            break;
        default:
            break;
        }
    }
}

int trust_load(trust_t *t, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    pem_read_t    r = {.t = t};
    unsigned char chunk[4096];
    size_t        n;

    br_pem_decoder_init(&r.pc);
    do {
        n = fread(chunk, 1, sizeof(chunk), f);
        pem_push(&r, chunk, n);
    } while (n == sizeof(chunk) && !r.bad);

    /* One newline ends a last line that has none, as brssl does. */
    pem_push(&r, (const unsigned char *)"\n", 1);

    int bad = r.bad || r.in_object || ferror(f);
    fclose(f);
    free(r.der.data);

    return bad ? -1 : r.loaded;
}

const char *trust_system_file(void)
{
    for (size_t i = 0; i < sizeof(SYSTEM_CA_FILES) / sizeof(SYSTEM_CA_FILES[0]); i++)
        if (access(SYSTEM_CA_FILES[i], R_OK) == 0)
            return SYSTEM_CA_FILES[i];

    return NULL;
}

void trust_free(trust_t *t)
{
    for (size_t i = 0; i < t->count; i++)
        free_anchor(&t->anchors[i]);

    free(t->anchors);
    t->anchors = NULL;
    t->count = 0;
}
