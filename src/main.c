/*
 * minimoni — zero-dependency system monitoring
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "alerts.h"
#include "config.h"
#include "db.h"
#include "http.h"
#include "metrics.h"

#define VERSION "1.0.0"

static volatile sig_atomic_t shutdown_flag = 0;

static void handle_signal(int sig)
{
    (void)sig;
    shutdown_flag = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s serve    [--config PATH]\n"
            "  %s collect  [--config PATH]\n"
            "  %s --version\n"
            "  %s --help\n",
            prog, prog, prog, prog);
}

static const char *parse_config_flag(int argc, char **argv, int start)
{
    for (int i = start; i < argc - 1; i++) {
        if (strcmp(argv[i], "--config") == 0)
            return argv[i + 1];
    }
    return NULL;
}

/* Convert a range string ("1d", "24h", …) to days (ceiling for hours). */
static int range_to_days(const char *r)
{
    int  n = (int)strtol(r, NULL, 10);
    char u = r[strlen(r) - 1];
    if (u == 'd')
        return n;
    if (u == 'h')
        return (n + 23) / 24;
    return 1;
}

static int retention_days(const config_t *cfg)
{
    if (cfg->range_count == 0)
        return 90;
    int max = 0;
    for (int i = 0; i < cfg->range_count; i++) {
        int d = range_to_days(cfg->ranges[i]);
        if (d > max)
            max = d;
    }
    return max > 0 ? max : 90;
}

/* -------------------------------------------------------------------------
 * collect: oneshot — seed CPU snapshot, sleep 250 ms, collect, insert
 * ---------------------------------------------------------------------- */

static int run_collect(const char *config_path)
{
    config_t cfg;
    if (config_open(&cfg, config_path) != 0)
        return 1;

    db_t db;
    if (db_open(&db, cfg.db_path) != 0)
        return 1;

    metrics_t m;

    /* First call seeds the static CPU snapshot; cpu_valid will be 0. */
    if (metrics_collect(&m, cfg.disk_path) != 0) {
        fprintf(stderr, "minimoni: metrics_collect failed\n");
        db_close(&db);
        return 1;
    }

    struct timespec ts = {0, 250000000L};
    nanosleep(&ts, NULL);

    /* Second call computes the CPU delta; cpu_valid is now 1. */
    if (metrics_collect(&m, cfg.disk_path) != 0) {
        fprintf(stderr, "minimoni: metrics_collect failed\n");
        db_close(&db);
        return 1;
    }

    int ret = 0;
    if (db_insert(&db, &m) != 0) {
        ret = 1;
    } else {
        db_prune(&db, retention_days(&cfg));
        db_row_t row;
        if (db_current(&db, &row) == 0)
            alerts_evaluate(&db, &cfg, &row);
    }

    db_close(&db);
    return ret;
}

/* -------------------------------------------------------------------------
 * serve: daemon — HTTP server + drift-free collect loop
 * ---------------------------------------------------------------------- */

static int run_serve(const char *config_path)
{
    config_t cfg;
    if (config_open(&cfg, config_path) != 0)
        return 1;

    db_t db;
    if (db_open(&db, cfg.db_path) != 0)
        return 1;

    http_ctx_t http;
    if (http_start(&http, &cfg, &db) != 0) {
        db_close(&db);
        return 1;
    }

    /* Seed the CPU delta snapshot before the first timed cycle. */
    metrics_t m;
    metrics_collect(&m, cfg.disk_path);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    next.tv_sec += cfg.interval_seconds;

    int days = retention_days(&cfg);

    while (!shutdown_flag) {
        int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (shutdown_flag)
            break;
        if (r != 0) /* EINTR — signal arrived; recheck flag */
            continue;

        next.tv_sec += cfg.interval_seconds;

        if (metrics_collect(&m, cfg.disk_path) != 0)
            continue;

        if (db_insert(&db, &m) != 0)
            continue;

        db_prune(&db, days);

        db_row_t row;
        if (db_current(&db, &row) == 0)
            alerts_evaluate(&db, &cfg, &row);
    }

    http_stop(&http);
    db_close(&db);
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("minimoni %s\n", VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "collect") == 0)
        return run_collect(parse_config_flag(argc, argv, 2));

    if (strcmp(argv[1], "serve") == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        signal(SIGHUP, SIG_IGN);
        return run_serve(parse_config_flag(argc, argv, 2));
    }

    fprintf(stderr, "minimoni: unknown subcommand '%s'\n", argv[1]);
    usage(argv[0]);
    return 1;
}
