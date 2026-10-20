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

#ifndef MINIMONI_TRUST_H
#define MINIMONI_TRUST_H

#include <stddef.h>

#include "bearssl.h"

/* Trust anchors for the https webhook; trust_free releases them. */
typedef struct {
    br_x509_trust_anchor *anchors;
    size_t                count;
} trust_t;

/* Load every certificate in the PEM file at `path` into `t`, which must start zeroed. A certificate
 * BearSSL cannot use is skipped. Returns the number loaded, or -1 when the file cannot be read or
 * is not valid PEM. Either way, trust_free releases what was loaded. */
int trust_load(trust_t *t, const char *path);

/* The host's CA bundle: the first of the usual Linux paths that can be read, or NULL. */
const char *trust_system_file(void);

/* Free every anchor and leave `t` zeroed. */
void trust_free(trust_t *t);

#endif /* MINIMONI_TRUST_H */
