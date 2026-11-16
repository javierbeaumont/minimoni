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

/* Fixtures shared by the config suites. Include it after the config sources: it calls their static
 * functions. */

#ifndef MINIMONI_TEST_CONFIG_HELPERS_H
#define MINIMONI_TEST_CONFIG_HELPERS_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_tmpcfg_path[256];

/* Write `toml` to a temp file, run config_load on it. Returns config_load's
 * return code. Leaves cfg in whatever state config_load left it. */
static int load_cfg(config_t *cfg, const char *toml)
{
    snprintf(g_tmpcfg_path, sizeof(g_tmpcfg_path), "/tmp/minimoni-test-%d.toml", getpid());
    FILE *f = fopen(g_tmpcfg_path, "w");
    if (!f)
        return -2;
    fputs(toml, f);
    fclose(f);

    config_defaults(cfg);
    int rc = config_load(cfg, g_tmpcfg_path);
    unlink(g_tmpcfg_path);
    return rc;
}

/* load_cfg, also capturing what config_load wrote to stderr into `err`. */
static int load_cfg_err(config_t *cfg, const char *toml, char *err, size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-test-err-%d.txt", getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return -2;

    int rc = load_cfg(cfg, toml);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return -2;

    FILE  *f = fopen(path, "r");
    size_t n = f ? fread(err, 1, cap - 1, f) : 0;
    err[n] = '\0';

    if (f)
        fclose(f);
    unlink(path);

    return rc;
}

/* Parse `toml` from memory and count the unknown keys it warns about. */
static int count_unknown(const char *toml)
{
    toml_result_t r = toml_parse(toml, (int)strlen(toml));
    if (!r.ok)
        return -9;
    int n = config_warn_unknown(r.toptab);
    toml_free(r);
    return n;
}

/* `n` copies of 'x'; buf must hold n + 1. */
static const char *xs(char *buf, size_t n)
{
    memset(buf, 'x', n);
    buf[n] = '\0';

    return buf;
}

#endif /* MINIMONI_TEST_CONFIG_HELPERS_H */
