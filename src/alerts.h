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

#ifndef MINIMONI_ALERTS_H
#define MINIMONI_ALERTS_H

#include "config.h"
#include "db.h"

/*
 * Evaluate all configured alerts against the current metrics snapshot.
 * For each alert whose condition is met and whose cooldown has expired:
 *   - POSTs the webhook (if configured)
 *   - executes the command via system() with MINIMONI_ALERT_* env vars set
 *   - logs the fire event in alert_log
 * Individual failures are logged to stderr; the function always returns 0.
 */
int alerts_evaluate(db_t *db, const config_t *cfg, const db_row_t *row);

#endif /* MINIMONI_ALERTS_H */
