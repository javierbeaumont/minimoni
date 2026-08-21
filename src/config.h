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

#ifndef MINIMONI_CONFIG_H
#define MINIMONI_CONFIG_H

#define MAX_ALERTS 16
#define MAX_CHARTS 16
#define MAX_CARDS 16
#define MAX_RANGES 8

typedef struct {
    char   name[64];
    char   metric[32];
    char   op[4]; /* ">", "<", ">=", "<=", "==" */
    double threshold;
    char   webhook[512];
    char   command[256];
    long   cooldown_seconds;
} alert_cfg_t;

typedef struct {
    /* [server] */
    char listen[64];            /* default: "0.0.0.0:8080" */
    int  max_dashboards;        /* live dashboards allowed at once, default: 4, min: 1 */
    int  sse_keepalive_seconds; /* SSE keepalive interval, default: 1; inactive if >= refresh */

    /* [collect] */
    long interval_seconds; /* default: 60 */
    char db_path[256];     /* default: "./metrics.db" */
    char disk_path[256];   /* default: "/" */

    /* [dashboard] */
    char  title[128];             /* default: "minimoni" */
    char  theme[8];               /* "auto" | "light" | "dark", default: "auto" */
    int   show_footer;            /* 1 = show project footer, 0 = hide, default: 1 */
    int   refresh_seconds;        /* default: 30 */
    char  memory_card_unit[16];   /* "%" | "auto", default: "%" */
    char  memory_chart_unit[16];  /* "%" | "auto", default: "auto" */
    char  disk_card_unit[16];     /* "%" | "auto", default: "%" */
    char  disk_chart_unit[16];    /* "%" | "auto", default: "auto" */
    char  temp_card_unit[4];      /* "c" | "f" | "%", default: "c" */
    char  temp_chart_unit[4];     /* "c" | "f" | "%", default: "c" */
    float temp_critical_fallback; /* temp percent 100% ref when sysfs critical absent; def: 85 */
    char  cpu_load_card_unit[4];  /* "abs" | "%", default: "abs" */
    char  cpu_load_chart_unit[4]; /* "abs" | "%", default: "abs" */
    char  net_card_unit[8];       /* "%" | "bytes" | "bits", default: "bytes" */
    char  net_chart_unit[8];      /* "%" | "bytes" | "bits", default: "bytes" */
    int   net_max_speed;          /* net % 100% ref (Mbit/s); 0 = use the detected link */
    char  uptime_unit[8];         /* "h"|"d"|"auto", default: "auto" */
    /* charts/cards: count=0 means show all in default order */
    char charts[MAX_CHARTS][16]; /* "cpu_load"|"cpu_usage"|"memory"|"disk"|"temp"|"net" */
    int  chart_count;
    char cards[MAX_CARDS][16]; /* same as charts plus "uptime" */
    int  card_count;
    /* ranges: display tabs + retention (largest value); count=0 uses defaults */
    char ranges[MAX_RANGES][8]; /* e.g. "1d", "7d"; units: m, h, d */
    int  range_count;

    /* [[alert]] */
    int         alert_count;
    alert_cfg_t alerts[MAX_ALERTS];
} config_t;

/* Fill cfg with built-in defaults. Always call before config_load or config_open. */
void config_defaults(config_t *cfg);

/* Load and merge TOML values from path into cfg.
 * cfg must have been initialised with config_defaults() first.
 * Returns 0 on success, -1 if the file cannot be opened or parsed
 * (message written to stderr). */
int config_load(config_t *cfg, const char *path);

/* Apply config_defaults then find and load a config file.
 * If explicit_path is non-NULL it is used as-is (returns -1 on failure).
 * Otherwise tries "./config.toml", then "/etc/minimoni/config.toml",
 * then silently keeps defaults. Always returns 0 when explicit_path is NULL. */
[[nodiscard]] int config_open(config_t *cfg, const char *explicit_path);

/* Membership test for the charts/cards visibility lists: 1 if `name` is
 * configured (or the list is the default show-all, count==0), 0 otherwise
 * (including count<0, an explicit empty list = hide all). */
int config_has(const char list[][16], int count, const char *name);

#endif /* MINIMONI_CONFIG_H */
