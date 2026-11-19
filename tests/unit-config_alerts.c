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

/* Unit tests for the alert settings. Zero-dependency, no framework. Build with:
 *   make test
 *
 * The config sources are `#include`d directly so static helpers are exercisable. Each test returns
 * 0 on pass / 1 on fail; the shared harness lives in runner.h. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runner.h"

#include "../src/config.c"
#include "../src/config_alerts.c"

#include "config-helpers.h"

/* --- [alerts] insecure_skip_verify --- */

static int test_insecure_skip_verify_default_false(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;
    return cfg.insecure_skip_verify == 0 ? 0 : 1;
}

static int test_insecure_skip_verify_true(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[alerts]\ninsecure_skip_verify = true\n") != 0)
        return 1;

    return cfg.insecure_skip_verify == 1 ? 0 : 1;
}

static int test_insecure_skip_verify_explicit_false(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[alerts]\ninsecure_skip_verify = false\n") != 0)
        return 1;

    return cfg.insecure_skip_verify == 0 ? 0 : 1;
}

/* A quoted "true" is not a boolean: it warns, and https stays verified. */
static int test_insecure_skip_verify_wrong_type_ignored(void)
{
    config_t cfg;
    char     err[512];

    if (load_cfg_err(&cfg, "[alerts]\ninsecure_skip_verify = \"true\"\n", err, sizeof(err)) != 0)
        return 1;

    return cfg.insecure_skip_verify == 0 &&
                   strstr(err, "config: alerts: insecure_skip_verify is not a boolean; "
                               "certificates stay verified")
               ? 0
               : 1;
}

/* --- [alerts] ca_file --- */

#define CA_FILE "/tmp/minimoni-test-ca.pem"
#define CA_TOML "[alerts]\nca_file = \"" CA_FILE "\"\n"

/* load_cfg_err with CA_FILE holding `pem` (NULL: no such file). */
static int load_ca(config_t *cfg, const char *pem, const char *toml, char *err, size_t cap)
{
    unlink(CA_FILE);

    if (pem) {
        FILE *f = fopen(CA_FILE, "w");
        if (!f)
            return -2;
        fputs(pem, f);
        fclose(f);
    }

    int rc = load_cfg_err(cfg, toml, err, cap);
    unlink(CA_FILE);

    return rc;
}

static int test_ca_file_default_empty(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;

    return cfg.ca_file[0] == '\0' ? 0 : 1;
}

static int test_ca_file_with_a_certificate_loads(void)
{
    config_t cfg;
    char     err[512];
    int rc = load_ca(&cfg, "# CA\n-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
                     CA_TOML, err, sizeof(err));

    return rc == 0 && strcmp(cfg.ca_file, CA_FILE) == 0 && err[0] == '\0' ? 0 : 1;
}

static int test_ca_file_missing_fails(void)
{
    config_t cfg;
    char     err[512];

    return load_ca(&cfg, NULL, CA_TOML, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.ca_file '" CA_FILE
                               "': No such file or directory; aborting")
               ? 0
               : 1;
}

static int test_ca_file_without_a_certificate_fails(void)
{
    config_t cfg;
    char     err[512];

    return load_ca(&cfg, "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n", CA_TOML,
                   err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.ca_file '" CA_FILE "' has no certificate; aborting")
               ? 0
               : 1;
}

static int test_ca_file_not_a_string_fails(void)
{
    config_t cfg;
    char     err[512];

    return load_cfg_err(&cfg, "[alerts]\nca_file = [\"/etc/minimoni/ca.pem\"]\n", err,
                        sizeof(err)) == -1 &&
                   strstr(err, "config: alerts: ca_file is not a string; aborting")
               ? 0
               : 1;
}

/* insecure_skip_verify wins, so the file is not even read. */
static int test_ca_file_ignored_while_insecure(void)
{
    config_t cfg;
    char     err[512];

    return load_ca(&cfg, NULL, CA_TOML "insecure_skip_verify = true\n", err, sizeof(err)) == 0 &&
                   strstr(err,
                          "config: alerts.ca_file is ignored while insecure_skip_verify is true")
               ? 0
               : 1;
}

/* --- parse_duration: per-unit 10-year caps --- */

/* With no cooldown in [alerts], an invalid one warns; the alert is kept, with none. */
static int test_alert_invalid_cooldown_without_default_warns(void)
{
    config_t cfg;
    char     err[512];
    int      rc = load_cfg_err(&cfg,
                               "[[alert]]\nname = \"a\"\nwhen = \"load_1m > 1\"\n"
                               "webhook = \"http://x\"\ncooldown = \"999999999d\"\n",
                               err, sizeof(err));

    return rc == 0 && cfg.alert_count == 1 && cfg.alerts[0].cooldown_seconds == 0 &&
                   strstr(err, "config: alert 'a': invalid cooldown '999999999d'; no cooldown, "
                               "it fires every cycle")
               ? 0
               : 1;
}

/* --- [[alert]] --- */

static int test_alert_full_entry(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[[alert]]\nname = \"hot\"\nwhen = \"temp_degrees >= 80.5\"\n"
                       "webhook = \"http://h\"\ncommand = \"echo hi\"\ncooldown = \"5m\"\n") != 0)
        return 1;

    const alert_cfg_t *alert = &cfg.alerts[0];

    return cfg.alert_count == 1 && strcmp(alert->name, "hot") == 0 &&
                   strcmp(alert->metric, "temp_degrees") == 0 && strcmp(alert->op, ">=") == 0 &&
                   alert->threshold == 80.5 && alert->cooldown_seconds == 300 &&
                   strcmp(alert->webhook, "http://h") == 0 && strcmp(alert->command, "echo hi") == 0
               ? 0
               : 1;
}

/* The spaces around the operator are optional, and the name has one on each side. */
static int test_alert_when_spacing(void)
{
    static const struct {
        const char *when;
        const char *op;
    } cases[] = {
        {"load_1m>=0.5", ">="},      {"load_1m >= 0.5", ">="}, {"  load_1m\t>=   0.5  ", ">="},
        {"load_1m >=\t0.5\t", ">="}, {"load_1m<0.5", "<"},     {"load_1m<=0.5", "<="},
        {"\tload_1m>=0.5", ">="},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        config_t cfg;
        char     toml[256], name[32];
        snprintf(toml, sizeof(toml), "[[alert]]\nwhen = \"%s\"\nwebhook = \"w\"\n", cases[i].when);
        snprintf(name, sizeof(name), "load_1m %s 0.5", cases[i].op);

        if (load_cfg(&cfg, toml) != 0)
            return 1;

        const alert_cfg_t *alert = &cfg.alerts[0];

        if (strcmp(alert->metric, "load_1m") != 0 || strcmp(alert->op, cases[i].op) != 0 ||
            alert->threshold != 0.5 || strcmp(alert->name, name) != 0)
            return 1;
    }

    return 0;
}

static int test_alert_without_name_is_named_after_its_condition(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[[alert]]\nwhen = \"load_1m>1\"\nwebhook = \"w\"\n"
                       "[[alert]]\nname = \"\"\nwhen = \"load_5m < 2\"\nwebhook = \"w\"\n") != 0)
        return 1;

    return strcmp(cfg.alerts[0].name, "load_1m > 1") == 0 &&
                   strcmp(cfg.alerts[1].name, "load_5m < 2") == 0
               ? 0
               : 1;
}

/* An alert that cannot work fails the load: skipping it would lose it in silence. True when the
 * load fails and its errors include `want`. */
static int fails_with(const char *toml, const char *want)
{
    config_t cfg;
    char     err[1024];

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 && strstr(err, want) != NULL;
}

static int test_alert_without_when_fails(void)
{
    return fails_with("[[alert]]\nname = \"a\"\nwebhook = \"w\"\n",
                      "config: alert 'a': missing when; aborting") &&
                   fails_with("[[alert]]\nname = \"\"\nwebhook = \"w\"\n",
                              "config: alert[0]: missing when; aborting")
               ? 0
               : 1;
}

static int test_alert_malformed_when_fails(void)
{
    static const char *const whens[] = {"",
                                        "load_1m",
                                        "load_1m >",
                                        "> 1",
                                        "load_1m 1",
                                        "load_1m ~ 1",
                                        "load_1m > 1 x",
                                        "load_1m > 1e3",
                                        "load_1m > inf",
                                        "load_1m > nan",
                                        "load_1m > 0x10",
                                        "load_1m > .5",
                                        "load_1m > 1.",
                                        "load_1m > 10%",
                                        "load_1m > -abc",
                                        "load_1m <<<< 1"};

    for (size_t i = 0; i < sizeof(whens) / sizeof(whens[0]); i++) {
        char toml[256], want[128];
        snprintf(toml, sizeof(toml), "[[alert]]\nwhen = \"%s\"\nwebhook = \"w\"\n", whens[i]);
        snprintf(want, sizeof(want),
                 "config: alert[0]: when \"%s\" is not \"metric operator number\"; aborting",
                 whens[i]);

        if (!fails_with(toml, want))
            return 1;
    }

    return 0;
}

/* Any unknown key fails: a misnamed destination would leave the alert the default one. */
static int test_alert_unknown_key_fails(void)
{
    return fails_with("[alerts]\nwebhook = \"https://ntfy.sh/public\"\n"
                      "[[alert]]\nname = \"private\"\nwhen = \"load_1m > 1\"\n"
                      "url = \"https://internal/hook\"\n",
                      "config: alert 'private': unknown key 'url'; aborting") &&
                   fails_with(
                       "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\ncommands = \"c\"\n",
                       "config: alert[0]: unknown key 'commands' (typo of 'command'?); aborting") &&
                   fails_with(
                       "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\ncoldown = \"5m\"\n",
                       "config: alert[0]: unknown key 'coldown' (typo of 'cooldown'?); aborting")
               ? 0
               : 1;
}

/* In [alerts] too: an alert that sets only a webhook would lose the default command. */
static int test_alerts_unknown_key_fails(void)
{
    return fails_with("[alerts]\ncommands = \"notify\"\n"
                      "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n",
                      "config: alerts: unknown key 'commands' (typo of 'command'?); aborting")
               ? 0
               : 1;
}

static int test_alert_with_negative_number_fails(void)
{
    return fails_with("[[alert]]\nname = \"cold\"\nwhen = \"temp_degrees < -5\"\nwebhook = \"w\"\n",
                      "config: alert 'cold': when \"temp_degrees < -5\": the number cannot be "
                      "negative; aborting") &&
                   fails_with("[[alert]]\nwhen = \"temp_degrees < -0.5\"\nwebhook = \"w\"\n",
                              "the number cannot be negative") &&
                   fails_with("[[alert]]\nwhen = \"temp_degrees < -9\"\nwebhook = \"w\"\n",
                              "the number cannot be negative")
               ? 0
               : 1;
}

static int test_alert_with_unknown_operator_fails(void)
{
    static const char *const ops[] = {"=", "!=", "=<", "<>", ">>"};

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        char toml[256], want[128];
        snprintf(toml, sizeof(toml), "[[alert]]\nwhen = \"load_1m %s 1\"\nwebhook = \"w\"\n",
                 ops[i]);
        snprintf(want, sizeof(want), "config: alert[0]: unknown operator '%s'; aborting", ops[i]);

        if (!fails_with(toml, want))
            return 1;
    }

    return fails_with("[[alert]]\nwhen = \"load_1m!=1\"\nwebhook = \"w\"\n",
                      "config: alert[0]: unknown operator '!='; aborting")
               ? 0
               : 1;
}

static int test_alert_with_equals_operator_fails(void)
{
    return fails_with("[[alert]]\nwhen = \"load_1m == 0\"\nwebhook = \"w\"\n",
                      "config: alert[0]: '==' is not supported; use '<= 0' to check for zero") &&
                   fails_with("[[alert]]\nwhen = \"load_1m==0\"\nwebhook = \"w\"\n",
                              "config: alert[0]: '==' is not supported; use '<= 0' to check for "
                              "zero") &&
                   fails_with("[[alert]]\nwhen = \"temp_degrees == 0\"\nwebhook = \"w\"\n",
                              "config: alert[0]: '==' is not supported; use '>=' or '<='; aborting")
               ? 0
               : 1;
}

static int test_alert_that_is_not_a_table_fails(void)
{
    return fails_with("alert = [{ when = \"load_1m > 1\", webhook = \"w\" }, \"cpu-hot\"]\n",
                      "config: alert[1]: not a table; aborting")
               ? 0
               : 1;
}

static int test_alert_without_action_fails(void)
{
    return fails_with("[[alert]]\nwhen = \"load_1m > 1\"\n",
                      "config: alert 'load_1m > 1': needs a webhook or a command; aborting")
               ? 0
               : 1;
}

static int test_alert_when_not_a_string_fails(void)
{
    return fails_with("[[alert]]\nwhen = 80\nwebhook = \"w\"\n",
                      "config: alert[0]: when is not a string such as \"load_1m > 2\"; aborting")
               ? 0
               : 1;
}

/* The alert is kept, named after its condition. */
static int test_alert_name_not_a_string_warns(void)
{
    config_t cfg;
    char     err[512];

    if (load_cfg_err(&cfg, "[[alert]]\nname = 42\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n", err,
                     sizeof(err)) != 0)
        return 1;

    return strcmp(cfg.alerts[0].name, "load_1m > 1") == 0 &&
                   strstr(err, "config: alert[0]: name is not a string; it is named 'load_1m > 1'")
               ? 0
               : 1;
}

/* An alert in v0.2's format is told only that it has no `when`. */
static int test_alert_with_v02_keys_fails(void)
{
    config_t cfg;
    char     err[1024];
    int rc = load_cfg_err(&cfg,
                          "[[alert]]\nname = \"disk-full\"\nmetric = \"disk_available_percent\"\n"
                          "operator = \"<\"\nthreshold = 10\nwebhook = \"w\"\n",
                          err, sizeof(err));

    return rc == -1 && strstr(err, "config: alert 'disk-full': missing when; aborting") &&
                   !strstr(err, "unknown key")
               ? 0
               : 1;
}

/* --- [alerts]: the defaults every alert takes --- */

static int test_alerts_defaults_taken(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[alerts]\nwebhook = \"http://d\"\ncommand = \"notify\"\ncooldown = \"5m\"\n"
                       "[[alert]]\nwhen = \"load_1m > 1\"\n") != 0)
        return 1;

    const alert_cfg_t *alert = &cfg.alerts[0];

    return strcmp(alert->webhook, "http://d") == 0 && strcmp(alert->command, "notify") == 0 &&
                   alert->cooldown_seconds == 300
               ? 0
               : 1;
}

static int test_alert_overrides_the_defaults(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[alerts]\nwebhook = \"http://d\"\ncommand = \"notify\"\ncooldown = \"5m\"\n"
                       "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"http://a\"\n"
                       "command = \"own\"\ncooldown = \"1h\"\n") != 0)
        return 1;

    const alert_cfg_t *alert = &cfg.alerts[0];

    return strcmp(alert->webhook, "http://a") == 0 && strcmp(alert->command, "own") == 0 &&
                   alert->cooldown_seconds == 3600
               ? 0
               : 1;
}

/* Clearing the cooldown does not warn. */
static int test_alert_empty_string_clears_a_default(void)
{
    config_t cfg;
    char     err[512];

    if (load_cfg_err(&cfg,
                     "[alerts]\nwebhook = \"http://d\"\ncommand = \"notify\"\ncooldown = \"5m\"\n"
                     "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"\"\ncooldown = \"\"\n"
                     "[[alert]]\nwhen = \"load_5m > 1\"\ncommand = \"\"\n",
                     err, sizeof(err)) != 0)
        return 1;

    const alert_cfg_t *alert = &cfg.alerts[0];
    const alert_cfg_t *second = &cfg.alerts[1];

    return alert->webhook[0] == '\0' && strcmp(alert->command, "notify") == 0 &&
                   alert->cooldown_seconds == 0 && strcmp(second->webhook, "http://d") == 0 &&
                   second->command[0] == '\0' && err[0] == '\0'
               ? 0
               : 1;
}

static int test_alert_with_every_destination_cleared_fails(void)
{
    return fails_with("[alerts]\nwebhook = \"http://d\"\n"
                      "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"\"\n",
                      "config: alert 'load_1m > 1': needs a webhook or a command; aborting")
               ? 0
               : 1;
}

static int test_alerts_invalid_cooldown_warns(void)
{
    config_t cfg;
    char     err[512];
    int      rc = load_cfg_err(&cfg,
                               "[alerts]\nwebhook = \"w\"\ncooldown = \"5x\"\n"
                               "[[alert]]\nwhen = \"load_1m > 1\"\n",
                               err, sizeof(err));

    return rc == 0 && cfg.alerts[0].cooldown_seconds == 0 &&
                   strstr(err, "config: alerts: invalid cooldown '5x'; no cooldown, alerts "
                               "without one fire every cycle")
               ? 0
               : 1;
}

/* An invalid cooldown in an alert leaves the one in [alerts] in its place. */
static int test_alert_invalid_cooldown_keeps_the_default(void)
{
    config_t cfg;
    char     err[512];
    int      rc = load_cfg_err(&cfg,
                               "[alerts]\nwebhook = \"w\"\ncooldown = \"1h\"\n"
                               "[[alert]]\nwhen = \"load_1m > 1\"\ncooldown = \"5x\"\n"
                               "[[alert]]\nwhen = \"load_5m > 1\"\ncooldown = 300\n",
                               err, sizeof(err));

    return rc == 0 && cfg.alerts[0].cooldown_seconds == 3600 &&
                   cfg.alerts[1].cooldown_seconds == 3600 &&
                   strstr(err, "config: alert 'load_1m > 1': invalid cooldown '5x'; it keeps the "
                               "one in [alerts]") &&
                   strstr(err, "config: alert 'load_5m > 1': cooldown is not a duration string "
                               "such as \"5m\"; it keeps the one in [alerts]")
               ? 0
               : 1;
}

/* A destination that is not a string fails the load: going on would send the alert to the default
 * one, or nowhere. */
static int test_destination_not_a_string_fails(void)
{
    return fails_with("[alerts]\nwebhook = \"http://d\"\n"
                      "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = 5\n",
                      "config: alert 'load_1m > 1': webhook is not a string; aborting") &&
                   fails_with("[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n"
                              "command = [\"notify\"]\n",
                              "config: alert 'load_1m > 1': command is not a string; aborting") &&
                   fails_with("[alerts]\nwebhook = true\n",
                              "config: alerts: webhook is not a string; aborting") &&
                   fails_with("[alerts]\ncommand = 5\n",
                              "config: alerts: command is not a string; aborting")
               ? 0
               : 1;
}

static int test_alert_decimal_numbers_load(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[[alert]]\nwhen = \"load_1m > 1.05\"\nwebhook = \"w\"\n"
                       "[[alert]]\nwhen = \"load_5m > 1.9\"\nwebhook = \"w\"\n") != 0)
        return 1;

    return cfg.alerts[0].threshold == 1.05 && cfg.alerts[1].threshold == 1.9 ? 0 : 1;
}

static int test_when_number_of_23_characters_loads(void)
{
    config_t cfg;

    if (load_cfg(&cfg, "[[alert]]\nwhen = \"net_rx_bps > 12345678901234567890123\"\n"
                       "webhook = \"w\"\n") != 0)
        return 1;

    return cfg.alerts[0].threshold > 1.2e22 ? 0 : 1;
}

/* A metric of 32 characters, longer than any, is reported as unknown, and a number of 24 as too
 * long. */
static int test_when_parts_too_long_fail(void)
{
    return fails_with("[[alert]]\nwhen = \"disk_available_percent_of_disk_1 > 1\"\n"
                      "webhook = \"w\"\n",
                      "config: alert[0]: unknown metric 'disk_available_percent_of_disk_1'; "
                      "aborting") &&
                   fails_with("[[alert]]\nwhen = \"load_1m > 123456789012345678901234\"\n"
                              "webhook = \"w\"\n",
                              "config: alert[0]: when \"load_1m > 123456789012345678901234\": "
                              "the number is 24 characters; the maximum is 23; aborting")
               ? 0
               : 1;
}

static int test_alerts_as_an_array_fails(void)
{
    return fails_with("[[alerts]]\nwebhook = \"w\"\n",
                      "config: alerts must be a table, written [alerts]; each alert is an "
                      "[[alert]]; aborting")
               ? 0
               : 1;
}

static int test_alert_keys_in_alerts_fail(void)
{
    return fails_with("[alerts]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n",
                      "config: alerts.when goes in an [[alert]]; aborting") &&
                   fails_with("[alerts]\nname = \"a\"\n",
                              "config: alerts.name goes in an [[alert]]; aborting")
               ? 0
               : 1;
}

/* The TLS keys hold for every webhook: in one alert they fail the load. */
static int test_tls_keys_in_an_alert_fail(void)
{
    return fails_with("[[alert]]\nname = \"a\"\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n"
                      "ca_file = \"/x.pem\"\n",
                      "config: alert 'a': ca_file goes in [alerts]; aborting") &&
                   fails_with("[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n"
                              "insecure_skip_verify = true\n",
                              "config: alert[0]: insecure_skip_verify goes in [alerts]; aborting")
               ? 0
               : 1;
}

/* --- Names --- */

static int test_alerts_with_one_name_fail(void)
{
    return fails_with("[[alert]]\nname = \"disk\"\nwhen = \"disk_available_percent < 10\"\n"
                      "webhook = \"w\"\n"
                      "[[alert]]\nname = \"disk\"\nwhen = \"disk_available_gb < 5\"\n"
                      "webhook = \"w\"\n",
                      "config: two alerts are named 'disk'; aborting")
               ? 0
               : 1;
}

/* Two alerts with no name on one condition, however spaced, get one name. */
static int test_alerts_with_one_condition_fail(void)
{
    return fails_with("[[alert]]\nwhen = \"load_1m>1\"\nwebhook = \"w\"\n"
                      "[[alert]]\nwhen = \"load_1m > 1\"\ncommand = \"c\"\n",
                      "config: two alerts are named 'load_1m > 1'; aborting")
               ? 0
               : 1;
}

/* --- Unknown keys --- */

/* A key two edits from a known one is hinted; three edits away, it is not. */
static int test_keys_typo_in_alerts(void)
{
    return fails_with("[alerts]\ncoldwn = \"5m\"\n",
                      "config: alerts: unknown key 'coldwn' (typo of 'cooldown'?); aborting") &&
                   fails_with("[alerts]\ncldwn = \"5m\"\n",
                              "config: alerts: unknown key 'cldwn'; aborting")
               ? 0
               : 1;
}

static int test_keys_refused_with_their_own_error_not_warned(void)
{
    return count_unknown("[alerts]\nwhen = \"x\"\nname = \"x\"\nwebhok = \"w\"\ncommands = \"c\"\n"
                         "[[alert]]\nca_file = \"c\"\ninsecure_skip_verify = true\nwebhok = \"w\"\n"
                         "commands = \"c\"\nurl = \"u\"\n") == 0
               ? 0
               : 1;
}

/* --- Values that do not fit their field --- */

#define ALERT_HEAD "[[alert]]\nname = \"a\"\nwhen = \"load_1m > 1\"\n"

static int test_command_over_max_fails(void)
{
    config_t cfg;
    char     cmd[300], toml[512], err[512];
    snprintf(toml, sizeof(toml), ALERT_HEAD "webhook = \"w\"\ncommand = \"%s\"\n", xs(cmd, 256));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alert[0].command is 256 bytes; the maximum is 255")
               ? 0
               : 1;
}

static int test_command_at_max_loads(void)
{
    config_t cfg;
    char     cmd[300], toml[512];
    snprintf(toml, sizeof(toml), ALERT_HEAD "command = \"%s\"\n", xs(cmd, 255));

    return load_cfg(&cfg, toml) == 0 && cfg.alert_count == 1 && strlen(cfg.alerts[0].command) == 255
               ? 0
               : 1;
}

static int test_webhook_over_max_fails(void)
{
    config_t cfg;
    char     url[520], toml[700], err[512];
    snprintf(toml, sizeof(toml), ALERT_HEAD "command = \"c\"\nwebhook = \"%s\"\n", xs(url, 512));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alert[0].webhook is 512 bytes; the maximum is 511")
               ? 0
               : 1;
}

static int test_alerts_webhook_over_max_fails(void)
{
    config_t cfg;
    char     url[520], toml[700], err[512];
    snprintf(toml, sizeof(toml), "[alerts]\nwebhook = \"%s\"\n", xs(url, 512));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.webhook is 512 bytes; the maximum is 511")
               ? 0
               : 1;
}

static int test_alerts_command_over_max_fails(void)
{
    config_t cfg;
    char     cmd[300], toml[512], err[512];
    snprintf(toml, sizeof(toml), "[alerts]\ncommand = \"%s\"\n", xs(cmd, 256));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.command is 256 bytes; the maximum is 255")
               ? 0
               : 1;
}

/* Each name past its maximum is reported, and no false duplicate: meanwhile the first alert is
 * named after its condition, which the third shares. */
static int test_alert_names_over_max_report_only_their_length(void)
{
    config_t cfg;
    char     name[80], toml[512], err[1024];
    xs(name, 70);
    snprintf(toml, sizeof(toml),
             "[[alert]]\nname = \"%s\"\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n"
             "[[alert]]\nname = \"%s\"\nwhen = \"load_5m > 1\"\nwebhook = \"w\"\n"
             "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n",
             name, name);

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alert[0].name is 70 bytes; the maximum is 63") &&
                   strstr(err, "config: alert[1].name is 70 bytes; the maximum is 63") &&
                   !strstr(err, "named 'load_1m > 1'")
               ? 0
               : 1;
}

static int test_destinations_over_max_report_only_their_length(void)
{
    config_t cfg;
    char     url[520], cmd[300], own[1024], shared[700], own_err[1024], shared_err[1024];
    snprintf(own, sizeof(own),
             "[[alert]]\nwhen = \"load_1m > 1\"\nwebhook = \"%s\"\n"
             "[[alert]]\nwhen = \"load_5m > 1\"\ncommand = \"%s\"\n",
             xs(url, 512), xs(cmd, 256));
    snprintf(shared, sizeof(shared),
             "[alerts]\nwebhook = \"%s\"\n[[alert]]\nwhen = \"load_1m > 1\"\n", xs(url, 512));

    int own_rc = load_cfg_err(&cfg, own, own_err, sizeof(own_err));
    int shared_rc = load_cfg_err(&cfg, shared, shared_err, sizeof(shared_err));

    return own_rc == -1 &&
                   strstr(own_err, "config: alert[0].webhook is 512 bytes; the maximum is 511") &&
                   strstr(own_err, "config: alert[1].command is 256 bytes; the maximum is 255") &&
                   !strstr(own_err, "needs a webhook") && shared_rc == -1 &&
                   strstr(shared_err, "config: alerts.webhook is 512 bytes; the maximum is 511") &&
                   !strstr(shared_err, "needs a webhook")
               ? 0
               : 1;
}

static int test_ca_file_over_max_reports_only_its_length(void)
{
    config_t cfg;
    char     path[300], url[520], toml[1024], err[1024];
    snprintf(toml, sizeof(toml), "[alerts]\nca_file = \"%s\"\nwebhook = \"%s\"\n", xs(path, 256),
             xs(url, 512));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.ca_file is 256 bytes; the maximum is 255") &&
                   strstr(err, "config: alerts.webhook is 512 bytes; the maximum is 511") &&
                   !strstr(err, "No such file")
               ? 0
               : 1;
}

static int test_ca_file_over_max_fails(void)
{
    config_t cfg;
    char     path[300], toml[512], err[512];
    snprintf(toml, sizeof(toml), "[alerts]\nca_file = \"%s\"\n", xs(path, 256));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: alerts.ca_file is 256 bytes; the maximum is 255")
               ? 0
               : 1;
}

static int test_every_overlong_value_is_reported(void)
{
    config_t cfg;
    char     db[300], cmd[300], toml[1024], err[1024];
    snprintf(toml, sizeof(toml), "[collect]\ndb = \"%s\"\n" ALERT_HEAD "command = \"%s\"\n",
             xs(db, 256), xs(cmd, 256));

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "collect.db is 256 bytes") &&
                   strstr(err, "alert[0].command is 256 bytes")
               ? 0
               : 1;
}

/* --- Cooldown: anything but a duration warns and leaves none --- */

static int test_cooldown_integer_warns(void)
{
    config_t cfg;
    char     err[512];
    int rc = load_cfg_err(&cfg, ALERT_HEAD "webhook = \"w\"\ncooldown = 300\n", err, sizeof(err));

    return rc == 0 && cfg.alert_count == 1 && cfg.alerts[0].cooldown_seconds == 0 &&
                   strstr(err, "config: alert 'a': cooldown is not a duration string")
               ? 0
               : 1;
}

static int test_cooldown_absent_is_silent(void)
{
    config_t cfg;
    char     err[512];
    int      rc = load_cfg_err(&cfg, ALERT_HEAD "webhook = \"w\"\n", err, sizeof(err));

    return rc == 0 && cfg.alert_count == 1 && !strstr(err, "cooldown") ? 0 : 1;
}

/* parse_duration once read only the first character after the number: "1mo" was 1m, "500ms" 500m
 * and "1h30m" 1h. */
static int test_cooldown_trailing_text_warns(void)
{
    static const char *const bad[] = {"1mo", "500ms", "1h30m"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        config_t cfg;
        char     toml[256], err[512], want[64];
        snprintf(toml, sizeof(toml), ALERT_HEAD "webhook = \"w\"\ncooldown = \"%s\"\n", bad[i]);
        snprintf(want, sizeof(want), "config: alert 'a': invalid cooldown '%s'", bad[i]);

        if (load_cfg_err(&cfg, toml, err, sizeof(err)) != 0 || cfg.alert_count != 1 ||
            cfg.alerts[0].cooldown_seconds != 0 || !strstr(err, want))
            return 1;
    }

    return 0;
}

/* --- Lists past their limit --- */

/* A single [alert] table was once skipped silently, and every alert with it. */
static int test_alert_as_a_single_table_fails(void)
{
    config_t cfg;
    char     err[512];
    int rc = load_cfg_err(&cfg, "[alert]\nname = \"a\"\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n",
                          err, sizeof(err));

    return rc == -1 && strstr(err, "config: [alert] must be an array of tables, written [[alert]] "
                                   "(the defaults go in [alerts]); aborting")
               ? 0
               : 1;
}

static int test_alerts_at_max_load(void)
{
    config_t cfg;
    char     toml[4096] = "";
    for (int i = 0; i < MAX_ALERTS; i++)
        snprintf(toml + strlen(toml), sizeof(toml) - strlen(toml),
                 "[[alert]]\nname = \"a%d\"\nwhen = \"load_1m > 1\"\nwebhook = \"w\"\n", i);

    return load_cfg(&cfg, toml) == 0 && cfg.alert_count == MAX_ALERTS ? 0 : 1;
}

/* A 17th alert was once dropped silently. */
static int test_alerts_over_max_fail(void)
{
    config_t cfg;
    char     toml[4096] = "", err[512];
    for (int i = 0; i <= MAX_ALERTS; i++)
        snprintf(toml + strlen(toml), sizeof(toml) - strlen(toml),
                 "[[alert]]\nname = \"a%d\"\nwhen = \"load_1m > 1\"\n"
                 "webhook = \"w\"\n",
                 i);

    return load_cfg_err(&cfg, toml, err, sizeof(err)) == -1 &&
                   strstr(err, "config: 17 [[alert]] tables; the maximum is 16")
               ? 0
               : 1;
}

/* An unknown metric was once loaded: the alert never fired, and every cycle logged it. */
static int test_unknown_alert_metric_fails(void)
{
    config_t cfg;
    char     err[512];
    int      rc = load_cfg_err(&cfg,
                               "[[alert]]\nname = \"cpu-high\"\nwhen = \"cpu_pct > 90\"\n"
                               "webhook = \"w\"\n",
                               err, sizeof(err));

    return rc == -1 && strstr(err, "config: alert 'cpu-high': unknown metric 'cpu_pct'") ? 0 : 1;
}

static int test_mem_alert_metrics_accepted(void)
{
    static const char *const names[] = {"mem_available_mb", "mem_available_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"mem\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

/* mem_used_* (the complement of mem_available_*) and the old names fail the load. */
static int test_old_mem_alert_metrics_fail(void)
{
    static const char *const names[] = {"mem_used_mb", "mem_used_percent", "mem_percent",
                                        "mem_total_mb"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"mem\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        char     err[512];
        char     want[96];
        snprintf(want, sizeof(want), "config: alert 'mem': unknown metric '%s'", names[i]);

        if (load_cfg_err(&cfg, toml, err, sizeof(err)) != -1 || !strstr(err, want))
            return 1;
    }

    return 0;
}

static int test_disk_alert_metrics_accepted(void)
{
    static const char *const names[] = {"disk_available_gb", "disk_available_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"disk\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

/* disk_used_* (the complement of disk_available_*) and the old names fail the load; disk_free_gb
 * is now disk_available_gb. */
static int test_old_disk_alert_metrics_fail(void)
{
    static const char *const names[] = {"disk_used_gb", "disk_used_percent", "disk_percent",
                                        "disk_free_gb", "disk_total_gb"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"disk\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        char     err[512];
        char     want[96];
        snprintf(want, sizeof(want), "config: alert 'disk': unknown metric '%s'", names[i]);

        if (load_cfg_err(&cfg, toml, err, sizeof(err)) != -1 || !strstr(err, want))
            return 1;
    }

    return 0;
}

static int test_temp_alert_metrics_accepted(void)
{
    static const char *const names[] = {"temp_degrees", "temp_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"hot\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

/* The old name fails the load: temp_celsius, now temp_degrees in collect.temp_scale. */
static int test_old_temp_alert_metric_fails(void)
{
    config_t cfg;
    char     err[512];

    if (load_cfg_err(&cfg,
                     "[[alert]]\nname = \"hot\"\nwhen = \"temp_celsius > 1\"\n"
                     "webhook = \"w\"\n",
                     err, sizeof(err)) != -1)
        return 1;

    return strstr(err, "config: alert 'hot': unknown metric 'temp_celsius'") ? 0 : 1;
}

/* The old name fails the load: cpu_idle_percent, now 100 minus cpu_total_percent. */
static int test_cpu_idle_alert_metric_fails(void)
{
    config_t cfg;
    char     err[512];

    if (load_cfg_err(&cfg,
                     "[[alert]]\nname = \"busy\"\nwhen = \"cpu_idle_percent < 10\"\n"
                     "webhook = \"w\"\n",
                     err, sizeof(err)) != -1)
        return 1;

    return strstr(err, "config: alert 'busy': unknown metric 'cpu_idle_percent'") ? 0 : 1;
}

static int test_swap_alert_metrics_accepted(void)
{
    static const char *const names[] = {"swap_free_mb", "swap_free_percent", "swap_in_bps",
                                        "swap_out_bps", "swap_in_ram",       "swap_out_ram"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"swap\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

/* swap_used_* (the complement of swap_free_*) fails the load. */
static int test_swap_used_alert_metrics_fail(void)
{
    static const char *const names[] = {"swap_used_mb", "swap_used_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"swap\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        char     err[512];
        char     want[96];
        snprintf(want, sizeof(want), "config: alert 'swap': unknown metric '%s'", names[i]);

        if (load_cfg_err(&cfg, toml, err, sizeof(err)) != -1 || !strstr(err, want))
            return 1;
    }

    return 0;
}

static int test_load_alert_metrics_accepted(void)
{
    static const char *const names[] = {"load_1m",         "load_5m",         "load_15m",
                                        "load_1m_percent", "load_5m_percent", "load_15m_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"load\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

static int test_cpu_alert_metrics_accepted(void)
{
    static const char *const names[] = {"cpu_total_percent", "cpu_user_percent",
                                        "cpu_system_percent", "cpu_steal_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"cpu\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

static int test_net_alert_metrics_accepted(void)
{
    static const char *const names[] = {"net_rx_bps", "net_tx_bps", "net_rx_percent",
                                        "net_tx_percent"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char toml[256];
        snprintf(toml, sizeof(toml),
                 "[[alert]]\nname = \"net\"\nwhen = \"%s > 1\"\n"
                 "webhook = \"w\"\n",
                 names[i]);

        config_t cfg;
        if (load_cfg(&cfg, toml) != 0 || cfg.alert_count != 1 ||
            strcmp(cfg.alerts[0].metric, names[i]))
            return 1;
    }

    return 0;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(alert_invalid_cooldown_without_default_warns),
    /* alerts */
    T(alert_full_entry),
    T(alert_when_spacing),
    T(alert_without_name_is_named_after_its_condition),
    T(alert_without_when_fails),
    T(alert_malformed_when_fails),
    T(alert_unknown_key_fails),
    T(alerts_unknown_key_fails),
    T(alert_with_negative_number_fails),
    T(alert_with_unknown_operator_fails),
    T(alert_with_equals_operator_fails),
    T(alert_that_is_not_a_table_fails),
    T(alert_without_action_fails),
    T(alert_when_not_a_string_fails),
    T(alert_name_not_a_string_warns),
    T(alert_with_v02_keys_fails),
    T(alerts_defaults_taken),
    T(alert_overrides_the_defaults),
    T(alert_empty_string_clears_a_default),
    T(alert_with_every_destination_cleared_fails),
    T(alerts_invalid_cooldown_warns),
    T(alert_invalid_cooldown_keeps_the_default),
    T(destination_not_a_string_fails),
    T(alert_decimal_numbers_load),
    T(when_number_of_23_characters_loads),
    T(when_parts_too_long_fail),
    T(alerts_as_an_array_fails),
    T(alert_keys_in_alerts_fail),
    T(tls_keys_in_an_alert_fail),
    T(alerts_with_one_name_fail),
    T(alerts_with_one_condition_fail),
    /* [alerts] TLS */
    T(insecure_skip_verify_default_false),
    T(insecure_skip_verify_true),
    T(insecure_skip_verify_explicit_false),
    T(insecure_skip_verify_wrong_type_ignored),
    T(ca_file_default_empty),
    T(ca_file_with_a_certificate_loads),
    T(ca_file_missing_fails),
    T(ca_file_without_a_certificate_fails),
    T(ca_file_not_a_string_fails),
    T(ca_file_ignored_while_insecure),
    T(keys_typo_in_alerts),
    T(keys_refused_with_their_own_error_not_warned),
    /* values that do not fit their field */
    T(command_over_max_fails),
    T(command_at_max_loads),
    T(webhook_over_max_fails),
    T(alerts_webhook_over_max_fails),
    T(alerts_command_over_max_fails),
    T(alert_names_over_max_report_only_their_length),
    T(destinations_over_max_report_only_their_length),
    T(ca_file_over_max_fails),
    T(ca_file_over_max_reports_only_its_length),
    T(every_overlong_value_is_reported),
    /* cooldown: anything but a duration */
    T(cooldown_integer_warns),
    T(cooldown_absent_is_silent),
    T(cooldown_trailing_text_warns),
    /* lists past their limit */
    T(alert_as_a_single_table_fails),
    T(alerts_at_max_load),
    T(alerts_over_max_fail),
    T(unknown_alert_metric_fails),
    T(mem_alert_metrics_accepted),
    T(old_mem_alert_metrics_fail),
    T(disk_alert_metrics_accepted),
    T(old_disk_alert_metrics_fail),
    T(temp_alert_metrics_accepted),
    T(old_temp_alert_metric_fails),
    T(cpu_idle_alert_metric_fails),
    T(swap_alert_metrics_accepted),
    T(swap_used_alert_metrics_fail),
    T(load_alert_metrics_accepted),
    T(cpu_alert_metrics_accepted),
    T(net_alert_metrics_accepted),
};

int main(void)
{
    /* Silence config_load's diagnostics during tests so the output stays
     * readable. Re-open later if a test needs to inspect stderr. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
