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

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_internal.h"
#include "tomlc17.h"

static const char *const METRIC_NAMES[] = {ALERT_METRICS, NULL};

static int valid_op(const char *s)
{
    return strcmp(s, ">") == 0 || strcmp(s, "<") == 0 || strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0;
}

/* One [[alert]]. Returns -1 when the load must fail; a value too long for its field sets *too_long
 * instead. */
static int load_alert(alert_cfg_t *alert, toml_datum_t table, int index, int *too_long)
{
    if (table.type != TOML_TABLE) {
        fprintf(stderr, "config: alert[%d]: not a table; aborting\n", index);
        return -1;
    }

    toml_datum_t name = toml_get(table, "name");
    toml_datum_t metric = toml_get(table, "metric");
    toml_datum_t op = toml_get(table, "operator");
    toml_datum_t threshold = toml_get(table, "threshold");

    if (name.type != TOML_STRING || metric.type != TOML_STRING || op.type != TOML_STRING) {
        fprintf(stderr, "config: alert[%d]: missing name, metric or operator; aborting\n", index);
        return -1;
    }

    /* A measured value almost never equals a threshold exactly, and no metric is below 0, so "is
     * zero" is <= 0. */
    if (strcmp(op.u.s, "==") == 0) {
        fprintf(stderr,
                "config: alert '%s': '==' is not supported; use '<= 0' to check for zero, or '>=' "
                "or '<='; aborting\n",
                name.u.s);
        return -1;
    }

    if (!valid_op(op.u.s)) {
        fprintf(stderr, "config: alert '%s': unknown operator '%s'; aborting\n", name.u.s, op.u.s);
        return -1;
    }

    if (!in_list(metric.u.s, METRIC_NAMES)) {
        fprintf(stderr, "config: alert '%s': unknown metric '%s'; aborting\n", name.u.s,
                metric.u.s);
        return -1;
    }

    if (threshold.type != TOML_FP64 && threshold.type != TOML_INT64) {
        fprintf(stderr, "config: alert '%s': missing threshold; aborting\n", name.u.s);
        return -1;
    }

    memset(alert, 0, sizeof(*alert));
    *too_long |= str_copy(alert->name, sizeof(alert->name), name, "alert[%d].name", index) != 0;
    *too_long |=
        str_copy(alert->metric, sizeof(alert->metric), metric, "alert[%d].metric", index) != 0;
    *too_long |= str_copy(alert->op, sizeof(alert->op), op, "alert[%d].operator", index) != 0;
    alert->threshold = threshold.type == TOML_FP64 ? threshold.u.fp64 : (double)threshold.u.int64;
    *too_long |= str_copy(alert->webhook, sizeof(alert->webhook), toml_get(table, "webhook"),
                          "alert[%d].webhook", index) != 0;
    *too_long |= str_copy(alert->command, sizeof(alert->command), toml_get(table, "command"),
                          "alert[%d].command", index) != 0;

    toml_datum_t cooldown = toml_get(table, "cooldown");

    if (cooldown.type == TOML_STRING) {
        long seconds = parse_duration(cooldown.u.s);

        if (seconds > 0)
            alert->cooldown_seconds = seconds;
        else
            fprintf(stderr,
                    "config: alert '%s': invalid cooldown '%s'; no cooldown, it fires every "
                    "cycle\n",
                    alert->name, cooldown.u.s);
    } else if (cooldown.type != TOML_UNKNOWN) {
        fprintf(stderr,
                "config: alert '%s': cooldown is not a duration string such as \"5m\"; no "
                "cooldown, it fires every cycle\n",
                alert->name);
    }

    if (!alert->webhook[0] && !alert->command[0]) {
        fprintf(stderr, "config: alert '%s': needs a webhook or a command; aborting\n",
                alert->name);
        return -1;
    }

    return 0;
}

int load_alerts(config_t *cfg, toml_datum_t root, int *too_long)
{
    toml_datum_t list = toml_seek(root, "alert");

    if (list.type != TOML_ARRAY && list.type != TOML_UNKNOWN) {
        fprintf(stderr,
                "config: [alert] must be an array of tables, written [[alert]]; aborting\n");
        return -1;
    }

    int table_count = list.type == TOML_ARRAY ? list.u.arr.size : 0;

    if (table_count > MAX_ALERTS) {
        fprintf(stderr, "config: %d [[alert]] tables; the maximum is %d; aborting\n", table_count,
                MAX_ALERTS);
        return -1;
    }

    for (int i = 0; i < table_count; i++) {
        if (load_alert(&cfg->alerts[i], list.u.arr.elem[i], i, too_long) != 0)
            return -1;

        cfg->alert_count = i + 1;
    }

    return 0;
}
