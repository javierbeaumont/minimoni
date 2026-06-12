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

#ifndef MINIMONI_DOWNSAMPLE_H
#define MINIMONI_DOWNSAMPLE_H

/*
 * Choose the query downsampling bucket for /api/metrics.
 *
 *   range_sec     requested time span in seconds
 *   interval_sec  collect interval in seconds
 *   points        target data points per chart; <= 0 uses the default (480)
 *   actual_count  rows available for the range, or < 0 if unknown
 *
 * Returns the bucket size in seconds to aggregate into, or 0 for raw (no
 * aggregation) when the range already has fewer rows than the target or the
 * ideal bucket is at or below the collect interval. The chosen bucket is
 * always a multiple of interval_sec (or interval_sec itself as a fallback).
 */
int pick_bucket(long range_sec, int interval_sec, int points, int actual_count);

#endif /* MINIMONI_DOWNSAMPLE_H */
