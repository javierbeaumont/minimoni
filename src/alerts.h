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

#ifndef MINIMONI_ALERTS_H
#define MINIMONI_ALERTS_H

#include "config.h"
#include "db.h"

/* Evaluate all configured alerts against the current metrics snapshot.
 * For each alert whose condition is met and whose cooldown has expired:
 *   - POSTs the webhook (if configured)
 *   - executes the command via system() with MINIMONI_ALERT_* env vars set
 *   - logs the fire event in alert_log
 * `temp_critical` is the critical trip point in °C that temp_ref resolves: `temp_percent` is
 * measured against it. `net_ref` is the link ceiling in bytes/s that net_ref_bps resolves:
 * `net_rx_percent` and `net_tx_percent` are measured against it. `cores` is the CPUs online that
 * metrics_num_cores counts, at least 1: the `load_*_percent` are measured against them.
 * Individual failures are logged to stderr; the function always returns 0. */
int alerts_evaluate(db_t *db, const config_t *cfg, const db_row_t *row, double temp_critical,
                    double net_ref, int cores);

#endif /* MINIMONI_ALERTS_H */
