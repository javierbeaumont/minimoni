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
#include <unistd.h>

#include "config.h"
#include "db.h"
#include "metrics.h"

int main(void)
{
    config_t cfg;
    if (config_open(&cfg, NULL) != 0)
        return 1;
    printf("config: listen=%s  interval=%lds  db=%s  ranges=%d  alerts=%d\n", cfg.listen,
           cfg.interval_seconds, cfg.db_path, cfg.range_count, cfg.alert_count);

    metrics_t m;
    db_t      db;

    /* First collect — seeds the CPU delta snapshot */
    if (metrics_collect(&m, "/") != 0) {
        fprintf(stderr, "metrics_collect failed\n");
        return 1;
    }

    sleep(1);

    /* Second collect — CPU delta now valid */
    if (metrics_collect(&m, "/") != 0) {
        fprintf(stderr, "metrics_collect failed\n");
        return 1;
    }

    if (db_open(&db, "/tmp/minimoni-test.db") != 0)
        return 1;

    if (db_insert(&db, &m) != 0) {
        db_close(&db);
        return 1;
    }
    printf("row inserted\n");

    if (db_prune(&db, 90) != 0) {
        db_close(&db);
        return 1;
    }
    printf("prune ok\n");

    db_close(&db);
    printf("verify: sqlite3 /tmp/minimoni-test.db 'SELECT * FROM metrics'\n");
    return 0;
}
