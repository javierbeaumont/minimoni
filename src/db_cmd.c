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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "db_cmd.h"
#include "sqlite3.h"

/* --- Helpers ------------------------------------------------------------ */

/* Render `bytes` as the most human-readable unit it fits into.
 * Output: "X.X GB", "X.X MB", "X.X KB", or "N bytes" for very small files. */
static void format_size(long bytes, char *out, size_t out_size)
{
    if (bytes >= 1024L * 1024L * 1024L)
        snprintf(out, out_size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(out, out_size, "%ld bytes", bytes);
}

/* Get file size in bytes; -1 if path does not exist or is not a regular file. */
static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Run a single-statement query and return its first column as text in `out`.
 * Returns  0 on row,  1 on no rows, -1 on prepare/step error. */
static int query_scalar_text(sqlite3 *db, const char *sql, char *out, size_t out_size)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        snprintf(out, out_size, "%s", txt ? (const char *)txt : "");
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 1 : -1;
}

/* Same as query_scalar_text but returns an integer. -1 on any error. */
static long query_scalar_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    long v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = (long)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

/* Same shape as query_scalar_int but returns a double. -1.0 on any error. */
static double query_scalar_double(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1.0;
    double v = -1.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

/* Render a Unix epoch as "YYYY-MM-DD HH:MM:SS TZ" using the system's
 * configured local time when available (TZ env var or /etc/localtime). On
 * systems without a configured zone, falls back to UTC. The zone label is
 * normalised: an empty %Z or "GMT" is rewritten to "UTC" for clarity. */
static void format_timestamp(time_t t, char *out, size_t out_size)
{
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[24];
    char tz[16] = "";
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    strftime(tz, sizeof(tz), "%Z", &tm);
    if (tz[0] == '\0' || strcmp(tz, "GMT") == 0)
        snprintf(tz, sizeof(tz), "UTC");
    snprintf(out, out_size, "%s %s", ts, tz);
}

/* Render a duration in seconds with the largest sensible unit:
 * <60 s   → "N seconds"
 * <60 m   → "N minutes"
 * <24 h   → "N.N hours"
 * <365 d  → "N.N days"
 * ≥ 1 y   → "N.NN years" (using 365.25 d/y). */
static void format_duration(double seconds, char *out, size_t out_size)
{
    if (seconds < 0)
        snprintf(out, out_size, "0 seconds");
    else if (seconds < 60.0)
        snprintf(out, out_size, "%.0f seconds", seconds);
    else if (seconds < 3600.0)
        snprintf(out, out_size, "%.0f minutes", seconds / 60.0);
    else if (seconds < 86400.0)
        snprintf(out, out_size, "%.1f hours", seconds / 3600.0);
    else if (seconds < 86400.0 * 365.0)
        snprintf(out, out_size, "%.1f days", seconds / 86400.0);
    else
        snprintf(out, out_size, "%.2f years", seconds / (86400.0 * 365.25));
}

/* Does the table exist in the schema? */
static int table_exists(sqlite3 *db, const char *table)
{
    char          sql[256];
    sqlite3_stmt *stmt = NULL;
    snprintf(sql, sizeof(sql), "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

/* Does the column exist in the named table? */
static int column_exists(sqlite3 *db, const char *table, const char *column)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    int found = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && strcmp((const char *)name, column) == 0) {
            found = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

/* Render the application_id + user_version pair as a compact identifier.
 * When application_id matches minimoni's magic (0x6D6F6E69 = "moni"), render
 * the friendly label; for any other value, render as plain hex. We never
 * decode arbitrary application_id bytes as text — that would create a path
 * for terminal-control or other byte sequences from a foreign / corrupted
 * DB to influence the output. Hex is always safe. */
static void format_application_id(long app_id, long version, char *out, size_t out_size)
{
    if (app_id == 0x6D6F6E69L)
        snprintf(out, out_size, "moni v%ld", version);
    else
        snprintf(out, out_size, "0x%X v%ld (not minimoni)", (unsigned int)(app_id & 0xFFFFFFFFL),
                 version);
}

/* Bucket size → short human label: "1s", "5s", "30s", "1m", "5m", "1h", "6h", "1d", ...
 * Used purely for the tier-distribution table cosmetics. */
static void format_bucket_label(long bucket_sec, char *out, size_t out_size)
{
    if (bucket_sec < 60)
        snprintf(out, out_size, "%lds", bucket_sec);
    else if (bucket_sec < 3600)
        snprintf(out, out_size, "%ldm", bucket_sec / 60);
    else if (bucket_sec < 86400)
        snprintf(out, out_size, "%ldh", bucket_sec / 3600);
    else
        snprintf(out, out_size, "%ldd", bucket_sec / 86400);
}

/* --- Public API ---------------------------------------------------------- */

int db_cmd_info(const char *db_path)
{
    /* Pre-flight: file sizes via stat() — gives a clearer error than
     * sqlite3_open if the file is missing entirely. */
    long db_size = file_size(db_path);
    if (db_size < 0) {
        fprintf(stderr, "db info: cannot stat '%s'\n", db_path);
        return 1;
    }
    char wal_path[1024], shm_path[1024];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    long wal_size = file_size(wal_path);
    long shm_size = file_size(shm_path);

    /* Open READ-ONLY: do not touch PRAGMAs, do not prepare daemon statements. */
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "db info: cannot open '%s': %s\n", db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    /* File / size header */
    char db_size_str[32];
    format_size(db_size, db_size_str, sizeof(db_size_str));
    printf("File:           %s\n", db_path);
    printf("Size on disk:   %s", db_size_str);
    if (wal_size >= 0 || shm_size >= 0) {
        char extras[128] = "";
        char buf[32];
        if (wal_size >= 0) {
            format_size(wal_size, buf, sizeof(buf));
            snprintf(extras + strlen(extras), sizeof(extras) - strlen(extras), "+ %s WAL", buf);
        }
        if (shm_size >= 0) {
            format_size(shm_size, buf, sizeof(buf));
            snprintf(extras + strlen(extras), sizeof(extras) - strlen(extras), "%s%s SHM",
                     (wal_size >= 0) ? ", " : "+ ", buf);
        }
        printf("  (%s)", extras);
    }
    printf("\n");

    /* Format identifier (compact application_id + user_version). SQLite-
     * level internals (journal_mode, page_size) are intentionally omitted —
     * they are implementation details rather than something the operator
     * inspecting a DB needs to know. */
    long appid = query_scalar_int(db, "PRAGMA application_id");
    long ver = query_scalar_int(db, "PRAGMA user_version");
    char fmt[64];
    format_application_id(appid, ver, fmt, sizeof(fmt));
    printf("Format:         %s\n", fmt);

    printf("\n");

    /* Metrics table */
    if (!table_exists(db, "metrics")) {
        printf("Metrics:        no metrics table\n");
        sqlite3_close(db);
        return 0;
    }

    long row_count = query_scalar_int(db, "SELECT COUNT(*) FROM metrics");
    printf("Metrics:\n");
    printf("  Total rows:   %ld\n", row_count < 0 ? 0L : row_count);
    if (row_count > 0) {
        /* Pull MIN/MAX as Unix epoch seconds so we can format in local time. */
        long oldest_epoch = query_scalar_int(
            db, "SELECT CAST(strftime('%s', MIN(timestamp)) AS INTEGER) FROM metrics");
        long newest_epoch = query_scalar_int(
            db, "SELECT CAST(strftime('%s', MAX(timestamp)) AS INTEGER) FROM metrics");
        if (oldest_epoch >= 0 && newest_epoch >= 0) {
            char ts[48];
            format_timestamp((time_t)oldest_epoch, ts, sizeof(ts));
            printf("  Oldest:       %s\n", ts);
            format_timestamp((time_t)newest_epoch, ts, sizeof(ts));
            printf("  Newest:       %s\n", ts);

            double span_sec = query_scalar_double(
                db, "SELECT (julianday(MAX(timestamp)) - julianday(MIN(timestamp))) * 86400.0"
                    " FROM metrics");
            if (span_sec >= 0.0) {
                char span[32];
                format_duration(span_sec, span, sizeof(span));
                printf("  Time span:    %s\n", span);
            }
        }
    }
    printf("\n");

    /* Tier distribution */
    if (!column_exists(db, "metrics", "bucket_sec")) {
        printf("Tier distribution:  schema has no bucket_sec column (pre-v0.2)\n");
    } else if (row_count > 0) {
        printf("Tier distribution (only buckets with rows shown):\n");
        sqlite3_stmt *stmt = NULL;
        const char   *q = "SELECT bucket_sec, COUNT(*) FROM metrics"
                          " GROUP BY bucket_sec ORDER BY bucket_sec";
        if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int         bucket_type = sqlite3_column_type(stmt, 0);
                long        n = (long)sqlite3_column_int64(stmt, 1);
                const char *unit = (n == 1) ? "row" : "rows";
                if (bucket_type == SQLITE_NULL) {
                    printf("  NULL: %5ld %s\n", n, unit);
                } else {
                    long b = (long)sqlite3_column_int64(stmt, 0);
                    char label[16];
                    format_bucket_label(b, label, sizeof(label));
                    printf("  %3s: %5ld %s\n", label, n, unit);
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    printf("\n");

    /* Alerts */
    if (table_exists(db, "alert_log")) {
        long alert_count = query_scalar_int(db, "SELECT COUNT(*) FROM alert_log");
        printf("Alert log:\n");
        printf("  Rows:         %ld\n", alert_count);
        if (alert_count > 0) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db,
                                   "SELECT CAST(strftime('%s', fired_at) AS INTEGER),"
                                   "  alert_name FROM alert_log"
                                   " ORDER BY fired_at DESC LIMIT 1",
                                   -1, &stmt, NULL) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW) {
                long                 epoch = (long)sqlite3_column_int64(stmt, 0);
                const unsigned char *name = sqlite3_column_text(stmt, 1);
                char                 ts[48];
                format_timestamp((time_t)epoch, ts, sizeof(ts));
                printf("  Most recent:  %s (%s)\n", ts, name ? (const char *)name : "?");
            }
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    return 0;
}

/* --- db_cmd_exec -------------------------------------------------------- */

/* Print one column value to stdout in TSV-safe form:
 *  - NULL                 -> "NULL" literal
 *  - SQLITE_BLOB          -> X'<hex>' (no decoding of opaque bytes)
 *  - SQLITE_INTEGER/REAL  -> text representation via sqlite3_column_text
 *  - SQLITE_TEXT          -> raw bytes (caller-side caveat — see header)
 */
static void print_value(sqlite3_stmt *stmt, int col)
{
    int type = sqlite3_column_type(stmt, col);
    if (type == SQLITE_NULL) {
        fputs("NULL", stdout);
        return;
    }
    if (type == SQLITE_BLOB) {
        const unsigned char *b = sqlite3_column_blob(stmt, col);
        int                  n = sqlite3_column_bytes(stmt, col);
        fputs("X'", stdout);
        for (int i = 0; i < n; i++)
            printf("%02X", b[i]);
        fputc('\'', stdout);
        return;
    }
    /* INTEGER, REAL, TEXT: let SQLite stringify */
    const unsigned char *t = sqlite3_column_text(stmt, col);
    if (t)
        fputs((const char *)t, stdout);
}

int db_cmd_exec(const char *db_path, const char *sql)
{
    if (!db_path || !sql) {
        fprintf(stderr, "db exec: missing db path or SQL\n");
        return 2;
    }

    sqlite3 *db = NULL;
    int      rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db exec: cannot open '%s': %s\n", db_path,
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db)
            sqlite3_close(db);
        return 2;
    }

    /* Iterate statements via the pzTail mechanism so a multi-statement script
     * is executed one prepare/step/finalize at a time. Counter `idx` is used
     * only for error messages. */
    int         idx = 0;
    const char *tail = sql;
    const char *next = NULL;

    while (tail && *tail) {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, tail, -1, &stmt, &next);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db exec: error at statement %d: %s\n", idx + 1, sqlite3_errmsg(db));
            if (stmt)
                sqlite3_finalize(stmt);
            sqlite3_close(db);
            return 1;
        }
        /* prepare on whitespace/comment-only tail returns NULL stmt — skip */
        if (!stmt) {
            tail = next;
            continue;
        }
        idx++;

        int ncols = sqlite3_column_count(stmt);
        for (;;) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW) {
                fprintf(stderr, "db exec: error at statement %d: %s\n", idx, sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return 1;
            }
            for (int c = 0; c < ncols; c++) {
                if (c > 0)
                    fputc('\t', stdout);
                print_value(stmt, c);
            }
            fputc('\n', stdout);
        }

        sqlite3_finalize(stmt);
        tail = next;
    }

    sqlite3_close(db);
    return 0;
}
