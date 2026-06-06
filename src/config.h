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
    char listen[64]; /* default: "0.0.0.0:8080" */

    /* [collect] */
    long interval_seconds; /* default: 60 */
    char db_path[256];     /* default: "./metrics.db" */
    char disk_path[256];   /* default: "/" */

    /* [dashboard] */
    char  title[128];             /* default: "minimoni" */
    char  theme[8];               /* "auto" | "light" | "dark", default: "auto" */
    int   show_footer;            /* 1 = show project footer, 0 = hide, default: 1 */
    int   refresh_seconds;        /* default: 30 */
    char  memory_card_unit[16];   /* "%" | "mb" | "gb", default: "%" */
    char  memory_chart_unit[16];  /* "mb" | "gb" | "%", default: "mb" */
    char  disk_card_unit[16];     /* "%" | "gb" | "tb", default: "%" */
    char  disk_chart_unit[16];    /* "gb" | "tb" | "%", default: "gb" */
    char  temp_card_unit[4];      /* "c" | "f" | "%", default: "c" */
    char  temp_chart_unit[4];     /* "c" | "f" | "%", default: "c" */
    float temp_max;               /* 100% reference when temp_*_unit="%", default: 100.0 */
    char  cpu_load_card_unit[4];  /* "abs" | "%", default: "abs" */
    char  cpu_load_chart_unit[4]; /* "abs" | "%", default: "abs" */
    char  net_card_unit[8];       /* "mb"|"gb"|"mbps"|"gbps", default: "mb" */
    char  net_chart_unit[8];      /* "mb"|"gb"|"mbps"|"gbps", default: "mb" */
    char  uptime_unit[8];         /* "h"|"d"|"auto", default: "auto" */
    /* charts/cards: count=0 means show all in default order */
    char charts[MAX_CHARTS][16]; /* "cpu_load"|"cpu_usage"|"memory"|"disk"|"temp"|"net" */
    int  chart_count;
    char cards[MAX_CARDS][16]; /* same as charts plus "uptime" */
    int  card_count;
    /* ranges: display tabs + retention (last/max value); count=0 uses defaults */
    char ranges[MAX_RANGES][8]; /* e.g. "1d", "7d"; units: h or d only */
    int  range_count;
    int  points; /* target data points per chart query, default: 300 */

    /* [[alert]] */
    int         alert_count;
    alert_cfg_t alerts[MAX_ALERTS];
} config_t;

/*
 * Fill cfg with built-in defaults. Always call before config_load or config_open.
 */
void config_defaults(config_t *cfg);

/*
 * Load and merge TOML values from path into cfg.
 * cfg must have been initialised with config_defaults() first.
 * Returns 0 on success, -1 if the file cannot be opened or parsed
 * (message written to stderr).
 */
int config_load(config_t *cfg, const char *path);

/*
 * Apply config_defaults then find and load a config file.
 * If explicit_path is non-NULL it is used as-is (returns -1 on failure).
 * Otherwise tries "./config.toml", then "/etc/minimoni/config.toml",
 * then silently keeps defaults. Always returns 0 when explicit_path is NULL.
 */
int config_open(config_t *cfg, const char *explicit_path);

#endif /* MINIMONI_CONFIG_H */
