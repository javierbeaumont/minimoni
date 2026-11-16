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

/* Private to the config sources. */

#ifndef MINIMONI_CONFIG_INTERNAL_H
#define MINIMONI_CONFIG_INTERNAL_H

#include <stddef.h>

#include "config.h"
#include "tomlc17.h"

/* Seconds in a duration such as "90m" (s, m, h or d, at most 10 years); -1 when it is not one. */
long parse_duration(const char *s);

/* Copy a string value into its fixed field. A value that does not fit is an error, never cut: a
 * truncated path, command or URL is a different one. A missing or non-string value copies nothing,
 * so the field keeps its default. */
[[nodiscard]] [[gnu::format(printf, 4, 5)]] int str_copy(char *dst, size_t dsize, toml_datum_t v,
                                                         const char *key, ...);

/* 1 if `name` is in the NULL-terminated `list`. */
int in_list(const char *name, const char *const *list);

/* Each [[alert]] into cfg. Returns -1 when the load must fail; a value too long for its field sets
 * *too_long instead. */
int load_alerts(config_t *cfg, toml_datum_t root, int *too_long);

#endif /* MINIMONI_CONFIG_INTERNAL_H */
