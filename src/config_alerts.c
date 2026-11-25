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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "config_internal.h"
#include "tomlc17.h"

static const char *const METRIC_NAMES[] = {ALERT_METRICS, NULL};

/* Shared by [alerts] and [[alert]]: load_alerts and load_alert reject a key in the wrong one. */
static const char *const ALERT_KEYS[] = {
    "ca_file", "command", "cooldown", "format", "insecure_skip_verify", "name", "webhook", "when"};

static const char *const FORMAT_NAMES[] = {
    [ALERT_FORMAT_JSON] = "json",
    [ALERT_FORMAT_ALERTMANAGER] = "alertmanager",
    [ALERT_FORMAT_SLACK] = "slack",
    [ALERT_FORMAT_TEXT] = "text",
};

/* 1 when a line opens a PEM certificate, 0 when none does, -1 with errno when the file cannot be
 * read. Only a first check: the anchors are decoded at delivery. */
static int has_certificate(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[64];
    int  found = 0;
    while (!found && fgets(line, sizeof(line), f))
        found = strncmp(line, "-----BEGIN CERTIFICATE-----", 27) == 0;

    int err = ferror(f) ? errno : 0;
    fclose(f);

    if (err) {
        errno = err;
        return -1;
    }

    return found;
}

static int valid_op(const char *s)
{
    return strcmp(s, ">") == 0 || strcmp(s, "<") == 0 || strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0;
}

/* A condition's three parts, as written, and the metric's place and the number's length, to
 * report a part too long. */
typedef struct {
    char        metric[sizeof(((alert_cfg_t *)0)->metric)];
    char        op[sizeof(((alert_cfg_t *)0)->op)];
    char        num[24];
    const char *metric_at;
    size_t      metric_len;
    size_t      num_len;
} when_t;

/* What split_when found wrong, if anything. */
enum { WHEN_OK, WHEN_MALFORMED, WHEN_NEGATIVE, WHEN_METRIC_TOO_LONG, WHEN_NUMBER_TOO_LONG };

/* The name an alert gets from its condition always fits. */
static_assert(sizeof(((when_t *)0)->metric) + sizeof(((when_t *)0)->op) +
                  sizeof(((when_t *)0)->num) <=
              sizeof(((alert_cfg_t *)0)->name));

/* How a message names the alert at `index` before alert->name is set. */
static void alert_label(char *label, size_t size, toml_datum_t name, int index)
{
    if (name.type == TOML_STRING && name.u.s[0])
        snprintf(label, size, "alert '%s'", name.u.s);
    else
        snprintf(label, size, "alert[%d]", index);
}

/* Copy the `len` characters at `src`; -1 when `len` is 0 or more than `dst` holds. */
static int take(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len == 0 || len >= dst_size)
        return -1;

    memcpy(dst, src, len);
    dst[len] = '\0';

    return 0;
}

/* Split "metric operator number", with spaces around the operator optional and the number a plain
 * decimal. Returns WHEN_OK, or what is wrong. */
static int split_when(const char *text, when_t *parts)
{
    const char *cursor = text + strspn(text, " \t");
    parts->metric_at = cursor;
    parts->metric_len = strcspn(cursor, " \t<>=!");

    if (parts->metric_len >= sizeof(parts->metric))
        return WHEN_METRIC_TOO_LONG;

    if (take(parts->metric, sizeof(parts->metric), cursor, parts->metric_len) != 0)
        return WHEN_MALFORMED;

    cursor += parts->metric_len;
    cursor += strspn(cursor, " \t");
    size_t op_len = strspn(cursor, "<>=!");

    if (take(parts->op, sizeof(parts->op), cursor, op_len) != 0)
        return WHEN_MALFORMED;

    cursor += op_len;
    cursor += strspn(cursor, " \t");

    if (cursor[0] == '-' && cursor[1] >= '0' && cursor[1] <= '9')
        return WHEN_NEGATIVE;

    const char *number = cursor;
    parts->num_len = strspn(number, "0123456789");
    if (parts->num_len > 0 && number[parts->num_len] == '.' && number[parts->num_len + 1] >= '0' &&
        number[parts->num_len + 1] <= '9')
        parts->num_len += 1 + strspn(number + parts->num_len + 1, "0123456789");

    cursor = number + parts->num_len;
    cursor += strspn(cursor, " \t");

    if (parts->num_len == 0 || *cursor != '\0')
        return WHEN_MALFORMED;

    if (take(parts->num, sizeof(parts->num), number, parts->num_len) != 0)
        return WHEN_NUMBER_TOO_LONG;

    return WHEN_OK;
}

/* An unknown key in [alerts] or an [[alert]] may be a misnamed webhook or command, which would
 * leave the alert another destination or none. Returns 1, having said so, when `table` has one. */
static int has_unknown_alert_key(toml_datum_t table, const char *label)
{
    for (int i = 0; i < table.u.tab.size; i++) {
        const char *key = table.u.tab.key[i];

        if (is_known(key, ALERT_KEYS, N_KEYS(ALERT_KEYS)))
            continue;

        const char *hint = closest(key, ALERT_KEYS, N_KEYS(ALERT_KEYS));
        if (hint)
            fprintf(stderr, "config: %s: unknown key '%s' (typo of '%s'?); aborting\n", label, key,
                    hint);
        else
            fprintf(stderr, "config: %s: unknown key '%s'; aborting\n", label, key);

        return 1;
    }

    return 0;
}

/* A cooldown in seconds: a duration string, or "" for none. Anything else warns, naming `who` and
 * what it is `left_with`, and gives `fallback`. */
static long cooldown_value(toml_datum_t value, const char *who, long fallback,
                           const char *left_with)
{
    if (value.type == TOML_STRING && value.u.s[0] == '\0')
        return 0;

    if (value.type != TOML_STRING) {
        fprintf(stderr, "config: %s: cooldown is not a duration string such as \"5m\"; %s\n", who,
                left_with);
        return fallback;
    }

    long seconds = parse_duration(value.u.s);

    if (seconds <= 0) {
        fprintf(stderr, "config: %s: invalid cooldown '%s'; %s\n", who, value.u.s, left_with);
        return fallback;
    }

    return seconds;
}

/* Returns 1, after an error naming `who` and `key`, when `value` is set but is not a string. */
static int not_a_string(toml_datum_t value, const char *who, const char *key)
{
    if (value.type == TOML_STRING || value.type == TOML_UNKNOWN)
        return 0;

    fprintf(stderr, "config: %s: %s is not a string; aborting\n", who, key);

    return 1;
}

/* Sets *format to the one `value` names, if set. Returns -1, after an error naming `who`, when
 * `value` is not a string or names no format. */
static int format_value(toml_datum_t value, const char *who, alert_format_t *format)
{
    if (value.type == TOML_UNKNOWN)
        return 0;

    if (not_a_string(value, who, "format"))
        return -1;

    for (int i = 0; i < N_KEYS(FORMAT_NAMES); i++) {
        if (strcmp(value.u.s, FORMAT_NAMES[i]) == 0) {
            *format = (alert_format_t)i;
            return 0;
        }
    }

    fprintf(stderr, "config: %s: unknown format '%s' (use ", who, value.u.s);
    for (int i = 0; i < N_KEYS(FORMAT_NAMES); i++)
        fprintf(stderr, "%s%s", i ? ", " : "", FORMAT_NAMES[i]);
    fprintf(stderr, "); aborting\n");

    return -1;
}

/* [alerts]: the TLS keys every webhook uses. Returns -1 when the load must fail; a value too long
 * for its field sets *too_long instead. */
static int load_tls(config_t *cfg, toml_datum_t alerts_table, int *too_long)
{
    toml_datum_t skip = toml_get(alerts_table, "insecure_skip_verify");
    if (skip.type == TOML_BOOLEAN)
        cfg->insecure_skip_verify = skip.u.boolean ? 1 : 0;
    else if (skip.type != TOML_UNKNOWN)
        fprintf(stderr, "config: alerts: insecure_skip_verify is not a boolean; certificates stay "
                        "verified\n");

    toml_datum_t ca_file = toml_get(alerts_table, "ca_file");

    if (not_a_string(ca_file, "alerts", "ca_file"))
        return -1;

    int ca_ok = str_copy(cfg->ca_file, sizeof(cfg->ca_file), ca_file, "alerts.ca_file") == 0;
    *too_long |= !ca_ok;

    if (ca_file.type != TOML_STRING || !ca_ok)
        return 0;

    if (cfg->insecure_skip_verify) {
        fprintf(stderr, "config: alerts.ca_file is ignored while insecure_skip_verify is true\n");
        return 0;
    }

    int found = has_certificate(cfg->ca_file);

    if (found < 0) {
        fprintf(stderr, "config: alerts.ca_file '%s': %s; aborting\n", cfg->ca_file,
                strerror(errno));
        return -1;
    }

    if (found == 0) {
        fprintf(stderr, "config: alerts.ca_file '%s' has no certificate; aborting\n", cfg->ca_file);
        return -1;
    }

    return 0;
}

/* One [[alert]], built on `defaults`. Returns -1 when the load must fail; a name, webhook or
 * command too long for its field sets *too_long instead, so that every such value is reported. */
static int load_alert(alert_cfg_t *alert, const alert_cfg_t *defaults, toml_datum_t table,
                      int index, int *too_long)
{
    if (table.type != TOML_TABLE) {
        fprintf(stderr, "config: alert[%d]: not a table; aborting\n", index);
        return -1;
    }

    toml_datum_t name = toml_get(table, "name");
    char         label[96];
    alert_label(label, sizeof(label), name, index);

    static const char *const TLS_KEYS[] = {"ca_file", "insecure_skip_verify"};
    for (int i = 0; i < N_KEYS(TLS_KEYS); i++) {
        if (toml_get(table, TLS_KEYS[i]).type != TOML_UNKNOWN) {
            fprintf(stderr, "config: %s: %s goes in [alerts]; aborting\n", label, TLS_KEYS[i]);
            return -1;
        }
    }

    toml_datum_t when = toml_get(table, "when");

    if (when.type == TOML_UNKNOWN) {
        fprintf(stderr, "config: %s: missing when; aborting\n", label);
        return -1;
    }

    if (when.type != TOML_STRING) {
        fprintf(stderr, "config: %s: when is not a string such as \"load_1m > 2\"; aborting\n",
                label);
        return -1;
    }

    if (has_unknown_alert_key(table, label))
        return -1;

    when_t parts = {0};
    int    split = split_when(when.u.s, &parts);

    if (split == WHEN_MALFORMED) {
        fprintf(stderr, "config: %s: when \"%s\" is not \"metric operator number\"; aborting\n",
                label, when.u.s);
        return -1;
    }

    if (split == WHEN_NEGATIVE) {
        fprintf(stderr, "config: %s: when \"%s\": the number cannot be negative; aborting\n", label,
                when.u.s);
        return -1;
    }

    if (split == WHEN_METRIC_TOO_LONG) {
        fprintf(stderr, "config: %s: unknown metric '%.*s'; aborting\n", label,
                (int)parts.metric_len, parts.metric_at);
        return -1;
    }

    if (split == WHEN_NUMBER_TOO_LONG) {
        fprintf(stderr,
                "config: %s: when \"%s\": the number is %zu characters; the maximum is %zu; "
                "aborting\n",
                label, when.u.s, parts.num_len, sizeof(parts.num) - 1);
        return -1;
    }

    /* A measured value almost never equals a threshold exactly. Every metric but the temperatures
     * is 0 or more, where "is zero" is <= 0. */
    if (strcmp(parts.op, "==") == 0) {
        if (strncmp(parts.metric, "temp_", 5) == 0)
            fprintf(stderr, "config: %s: '==' is not supported; use '>=' or '<='; aborting\n",
                    label);
        else
            fprintf(stderr,
                    "config: %s: '==' is not supported; use '<= 0' to check for zero, or '>=' or "
                    "'<='; aborting\n",
                    label);

        return -1;
    }

    if (!valid_op(parts.op)) {
        fprintf(stderr, "config: %s: unknown operator '%s'; aborting\n", label, parts.op);
        return -1;
    }

    if (!in_list(parts.metric, METRIC_NAMES)) {
        fprintf(stderr, "config: %s: unknown metric '%s'; aborting\n", label, parts.metric);
        return -1;
    }

    *alert = *defaults;
    memcpy(alert->metric, parts.metric, sizeof(parts.metric));
    memcpy(alert->op, parts.op, sizeof(parts.op));
    alert->threshold = strtod(parts.num, NULL);

    snprintf(alert->name, sizeof(alert->name), "%s %s %s", parts.metric, parts.op, parts.num);

    if (name.type != TOML_STRING && name.type != TOML_UNKNOWN)
        fprintf(stderr, "config: %s: name is not a string; it is named '%s'\n", label, alert->name);
    else if (name.type == TOML_STRING && name.u.s[0]) {
        alert->named = 1;
        *too_long |= str_copy(alert->name, sizeof(alert->name), name, "alert[%d].name", index) != 0;
    }

    char who[96];
    snprintf(who, sizeof(who), "alert '%s'", alert->name);

    toml_datum_t webhook = toml_get(table, "webhook");
    toml_datum_t command = toml_get(table, "command");

    if (not_a_string(webhook, who, "webhook") || not_a_string(command, who, "command"))
        return -1;

    *too_long |=
        str_copy(alert->webhook, sizeof(alert->webhook), webhook, "alert[%d].webhook", index) != 0;
    *too_long |=
        str_copy(alert->command, sizeof(alert->command), command, "alert[%d].command", index) != 0;

    toml_datum_t cooldown = toml_get(table, "cooldown");
    if (cooldown.type != TOML_UNKNOWN)
        alert->cooldown_seconds = cooldown_value(
            cooldown, who, defaults->cooldown_seconds,
            defaults->cooldown_seconds > 0 ? "it keeps the one in [alerts]" : "it has no cooldown");

    if (format_value(toml_get(table, "format"), who, &alert->format) != 0)
        return -1;

    /* A refused value already fails the load, and a refused webhook or command would look
     * missing. */
    if (!*too_long && !alert->webhook[0] && !alert->command[0]) {
        fprintf(stderr, "config: %s: needs a webhook or a command; aborting\n", who);
        return -1;
    }

    return 0;
}

int load_alerts(config_t *cfg, toml_datum_t root, int *too_long)
{
    toml_datum_t alerts_table = toml_seek(root, "alerts");

    if (alerts_table.type != TOML_TABLE && alerts_table.type != TOML_UNKNOWN) {
        fprintf(stderr, "config: alerts must be a table, written [alerts]; each alert is an "
                        "[[alert]]; aborting\n");
        return -1;
    }

    static const char *const ALERT_ONLY_KEYS[] = {"name", "when"};
    for (int i = 0; i < N_KEYS(ALERT_ONLY_KEYS); i++) {
        if (toml_get(alerts_table, ALERT_ONLY_KEYS[i]).type != TOML_UNKNOWN) {
            fprintf(stderr, "config: alerts.%s goes in an [[alert]]; aborting\n",
                    ALERT_ONLY_KEYS[i]);
            return -1;
        }
    }

    if (has_unknown_alert_key(alerts_table, "alerts"))
        return -1;

    if (load_tls(cfg, alerts_table, too_long) != 0)
        return -1;

    toml_datum_t webhook = toml_get(alerts_table, "webhook");
    toml_datum_t command = toml_get(alerts_table, "command");

    if (not_a_string(webhook, "alerts", "webhook") || not_a_string(command, "alerts", "command"))
        return -1;

    alert_cfg_t defaults = {0};
    *too_long |=
        str_copy(defaults.webhook, sizeof(defaults.webhook), webhook, "alerts.webhook") != 0;
    *too_long |=
        str_copy(defaults.command, sizeof(defaults.command), command, "alerts.command") != 0;

    toml_datum_t cooldown = toml_get(alerts_table, "cooldown");
    if (cooldown.type != TOML_UNKNOWN)
        defaults.cooldown_seconds =
            cooldown_value(cooldown, "alerts", 0, "alerts without their own have no cooldown");

    if (format_value(toml_get(alerts_table, "format"), "alerts", &defaults.format) != 0)
        return -1;

    toml_datum_t list = toml_seek(root, "alert");

    if (list.type != TOML_ARRAY && list.type != TOML_UNKNOWN) {
        fprintf(stderr,
                "config: [alert] must be an array of tables, written [[alert]] (the defaults go "
                "in [alerts]); aborting\n");
        return -1;
    }

    int table_count = list.type == TOML_ARRAY ? list.u.arr.size : 0;

    if (table_count > MAX_ALERTS) {
        fprintf(stderr, "config: %d [[alert]] tables; the maximum is %d; aborting\n", table_count,
                MAX_ALERTS);
        return -1;
    }

    for (int i = 0; i < table_count; i++) {
        if (load_alert(&cfg->alerts[i], &defaults, list.u.arr.elem[i], i, too_long) != 0)
            return -1;

        cfg->alert_count = i + 1;
    }

    /* A refused value already fails the load, and an alert whose name is refused is named after its
     * condition, which another alert may share. */
    if (*too_long)
        return 0;

    /* alert_log keeps each alert's state by name: two alerts with one name would share it. */
    for (int i = 0; i < cfg->alert_count; i++) {
        for (int j = i + 1; j < cfg->alert_count; j++) {
            if (strcmp(cfg->alerts[i].name, cfg->alerts[j].name) == 0) {
                fprintf(stderr, "config: two alerts are named '%s'; aborting\n",
                        cfg->alerts[i].name);
                return -1;
            }
        }
    }

    return 0;
}
