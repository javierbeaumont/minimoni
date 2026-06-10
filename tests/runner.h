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

/*
 * Shared test harness for the tests/unit-*.c suites.
 *
 * Each suite is its own standalone binary (tests/unit-config.c,
 * tests/unit-db.c, ...). They all use the same trivial runner via this
 * header: define your test functions returning 0 on pass / 1 on fail,
 * list them in an ALL_TESTS array using the T() macro, and call
 * run_tests() from main.
 *
 * Helpers specific to a single suite (temp DBs, config fixtures, ...)
 * live in the suite's own .c. Only the runner harness - common to
 * every suite - is here.
 */

#ifndef MINIMONI_TEST_RUNNER_H
#define MINIMONI_TEST_RUNNER_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *name;
    int (*fn)(void);
} test_t;

/* Build a test_t entry whose function is `test_<n>`. */
#define T(n) {#n, test_##n}

/* Walk `tests`, run each, print pass/fail. Returns 0 if all passed, 1 if
 * any failed. Use from main() like:
 *
 *   int main(void) {
 *       freopen("/dev/null", "w", stderr);   // optional
 *       return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
 *   }
 */
static inline int run_tests(const test_t *tests, size_t n)
{
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        printf("  %-45s ", tests[i].name);
        fflush(stdout);
        int r = tests[i].fn();
        if (r) {
            failed++;
            printf("FAIL\n");
        } else {
            printf("ok\n");
        }
    }
    printf("\n  %zu/%zu tests passed\n", n - failed, n);
    return failed ? 1 : 0;
}

#endif /* MINIMONI_TEST_RUNNER_H */
