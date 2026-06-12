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

#include <limits.h>
#include <stdlib.h>

#include "downsample.h"

static const int BUCKETS[] = {60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400};
#define NBUCKETS ((int)(sizeof(BUCKETS) / sizeof(BUCKETS[0])))

/* Return the best bucket size in seconds, or 0 for raw (no aggregation).
 * Iterates ascending so ties naturally resolve to the smaller bucket. */
int pick_bucket(long range_sec, int interval_sec, int points, int actual_count)
{
    /* Default 240 = a sane trend resolution for clients that omit `points`
     * (curl, external API). The bundled dashboard passes an explicit value, so
     * this default only applies when the parameter is missing. */
    if (points <= 0)
        points = 240;
    if (actual_count >= 0 && actual_count <= points)
        return 0; /* fewer rows than target; show raw for progressive resolution */
    long ideal = range_sec / (long)points;
    if (ideal <= interval_sec)
        return 0; /* raw */

    int  best = -1;
    long best_diff = LONG_MAX;
    for (int i = 0; i < NBUCKETS; i++) {
        int b = BUCKETS[i];
        if (b % interval_sec != 0)
            continue;
        long diff = labs(range_sec / b - (long)points);
        if (diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    return (best >= 0) ? BUCKETS[best] : interval_sec;
}
