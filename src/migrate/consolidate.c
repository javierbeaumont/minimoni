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

#define _POSIX_C_SOURCE 200809L

#include "consolidate.h"

#include <stdio.h>
#include <stdlib.h>

#include "exec.h"

/* Tier ladder, duplicated on purpose from db.c (TIER_TRANSITIONS / ADR-0005):
 * the SQL is bound to the schema version, which migrate owns, so it changes
 * only when a new migration lands. */
struct tier_transition {
    int  bucket_sec;    /* destination bucket size (seconds) */
    long threshold_sec; /* consolidate when bucket_end <= now - this */
};

static const struct tier_transition TIER_TRANSITIONS[] = {
    {5, 2L * 3600},        /* Raw -> T1:  5s buckets, threshold   2h */
    {30, 12L * 3600},      /* T1  -> T2: 30s buckets, threshold  12h */
    {300, 5L * 86400},     /* T2  -> T3:  5m buckets, threshold   5d */
    {3600, 60L * 86400},   /* T3  -> T4:  1h buckets, threshold  60d */
    {21600, 365L * 86400}, /* T4  -> T5:  6h buckets, threshold 365d */
};
#define NUM_TIERS (sizeof(TIER_TRANSITIONS) / sizeof(TIER_TRANSITIONS[0]))

/* Build the consolidate SQL once: BEGIN IMMEDIATE; (5 x INSERT/DELETE); COMMIT;
 * Heap-allocated string (caller frees), or NULL on allocation failure. Mirror
 * of db.c:build_consolidate_sql; keep them identical. */
static char *build_consolidate_sql(void)
{
    enum { SQL_BUF_SIZE = 8192 };
    char *sql = malloc(SQL_BUF_SIZE);
    if (!sql)
        return NULL;

    int   remaining = SQL_BUF_SIZE;
    char *p = sql;
    int   n;

    n = snprintf(p, remaining, "BEGIN IMMEDIATE;");
    if (n < 0 || n >= remaining)
        goto fail;
    p += n;
    remaining -= n;

    for (size_t i = 0; i < NUM_TIERS; i++) {
        int  bs = TIER_TRANSITIONS[i].bucket_sec;
        long th = TIER_TRANSITIONS[i].threshold_sec;

        n = snprintf(
            p, remaining,
            "INSERT INTO metrics ("
            "  timestamp, load_1m, load_5m, load_15m,"
            "  cpu_user_percent, cpu_system_percent, cpu_idle_percent,"
            "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
            "  disk_total_gb, disk_used_gb, disk_free_gb, disk_percent,"
            "  temp_celsius, net_rx_bps, net_tx_bps, uptime_seconds, bucket_sec"
            ") SELECT"
            "  strftime('%%Y-%%m-%%dT%%H:%%M:%%SZ',"
            "           (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d, 'unixepoch'),"
            "  AVG(load_1m), AVG(load_5m), AVG(load_15m),"
            "  AVG(cpu_user_percent), AVG(cpu_system_percent), AVG(cpu_idle_percent),"
            "  AVG(mem_total_mb), AVG(mem_used_mb), AVG(mem_available_mb), AVG(mem_percent),"
            "  AVG(disk_total_gb), AVG(disk_used_gb), AVG(disk_free_gb), AVG(disk_percent),"
            "  AVG(temp_celsius), AVG(net_rx_bps), AVG(net_tx_bps), AVG(uptime_seconds),"
            "  %d"
            " FROM metrics"
            " WHERE (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d + %d"
            "         <= CAST(strftime('%%s','now') AS INTEGER) - %ld"
            "   AND (bucket_sec IS NULL OR bucket_sec < %d)"
            " GROUP BY (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d;"
            "DELETE FROM metrics"
            " WHERE (CAST(strftime('%%s',timestamp) AS INTEGER)/%d)*%d + %d"
            "         <= CAST(strftime('%%s','now') AS INTEGER) - %ld"
            "   AND (bucket_sec IS NULL OR bucket_sec < %d);",
            bs, bs,              /* SELECT strftime bucket expression */
            bs,                  /* INSERT bucket_sec value */
            bs, bs, bs, th, bs,  /* INSERT WHERE */
            bs, bs,              /* GROUP BY */
            bs, bs, bs, th, bs); /* DELETE WHERE */
        if (n < 0 || n >= remaining)
            goto fail;
        p += n;
        remaining -= n;
    }

    n = snprintf(p, remaining, "COMMIT;");
    if (n < 0 || n >= remaining)
        goto fail;

    return sql;

fail:
    free(sql);
    return NULL;
}

int migrate_consolidate_and_vacuum(const char *minimoni_exec, const char *db_path)
{
    char *sql = build_consolidate_sql();
    if (!sql) {
        fprintf(stderr, "migrate: out of memory building the consolidate SQL\n");
        return 2;
    }

    char out[256], err[1024];

    /* One BEGIN/5-tier/COMMIT per pass; the cascade advances at most one tier per
     * pass, so NUM_TIERS folds the deepest backlog and +3 covers straddles as cheap
     * no-ops. db exec reports no row counts, so the count is fixed, not detected. */
    fprintf(stderr, "migrate: consolidating tiers\n");
    for (size_t i = 0; i < NUM_TIERS + 3; i++) {
        int rc = migrate_exec(minimoni_exec, db_path, sql, out, sizeof(out), err, sizeof(err));
        if (rc != 0) {
            fprintf(stderr, "migrate: consolidate failed (db exec rc=%d)\n", rc);
            if (err[0])
                fputs(err, stderr);
            free(sql);
            return 2;
        }
    }
    free(sql);

    /* VACUUM returns the pages freed by consolidation to the filesystem. It runs
     * outside any transaction, so it is a separate db exec. */
    fprintf(stderr, "migrate: VACUUM\n");
    int rc = migrate_exec(minimoni_exec, db_path, "VACUUM", out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "migrate: VACUUM failed (db exec rc=%d)\n", rc);
        if (err[0])
            fputs(err, stderr);
        return 2;
    }
    return 0;
}
