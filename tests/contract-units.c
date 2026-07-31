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

/* Reference output for the unit-conversion contract: prints one `case|value`
 * line per case. tools/devserver/units.js is a hand-written mirror of this
 * module, and nothing else would notice it drifting, so
 * tests/contract-units.test.js runs this binary and asserts the JS agrees.
 * Keep the case list in step with the one in that test. */

#include <stdio.h>

#include "../src/units.c"

static void p(const char *name, double v) { printf("%s|%.6f\n", name, v); }

int main(void)
{
    p("net kb", net_convert(1024.0, "kb", 0.0));
    p("net mb", net_convert(1048576.0, "mb", 0.0));
    p("net gb", net_convert(1073741824.0, "gb", 0.0));
    p("net kbps", net_convert(1000.0, "kbps", 0.0));
    p("net mbps", net_convert(1.0e6, "mbps", 0.0));
    p("net gbps", net_convert(1.0e9, "gbps", 0.0));
    p("net default", net_convert(1048576.0, "", 0.0));
    p("net unknown", net_convert(1048576.0, "zz", 0.0));
    p("net small", net_convert(1.0, "kb", 0.0));

    p("net ref configured wins", net_ref_bps(100, 1000));
    p("net ref detected", net_ref_bps(0, 300));
    p("net ref default gbe", net_ref_bps(0, 0));
    p("net pct half", net_convert(62500000.0, "%", net_ref_bps(1000, 0)));
    p("net pct full", net_convert(12500000.0, "%", net_ref_bps(100, 0)));
    p("net pct no ref", net_convert(1.0e6, "%", 0.0));

    p("mem mb", mem_convert(512.0, "mb"));
    p("mem gb", mem_convert(1024.0, "gb"));
    p("mem pct passthrough", mem_convert(1024.0, "%"));

    p("disk gb", disk_convert(50.0, "gb"));
    p("disk tb", disk_convert(1024.0, "tb"));

    p("temp c", temp_convert(50.0, "c", 85.0));
    p("temp f", temp_convert(100.0, "f", 85.0));
    p("temp f zero", temp_convert(0.0, "f", 85.0));
    p("temp pct at ref", temp_convert(85.0, "%", 85.0));
    p("temp pct half", temp_convert(42.5, "%", 85.0));
    p("temp pct zero ref", temp_convert(50.0, "%", 0.0));
    p("temp ref crit", temp_ref(1, 105.0, 85.0));
    p("temp ref fallback", temp_ref(0, 105.0, 85.0));

    p("load abs", load_convert(2.0, 4, "abs"));
    p("load pct", load_convert(2.0, 4, "%"));
    p("load pct zero cores", load_convert(2.0, 0, "%"));
    return 0;
}
