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

/* Integration tests for src/migrate/exec.c with a real child process standing in for minimoni:
 * what migrate_exec does with the two pipes it reads. Run with: make test-integration. */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runner.h"

#include "../src/migrate/exec.c"

static char g_child[64];

/* Write the shell script migrate_exec runs in minimoni's place. Returns 0, or -1. */
static int write_child(const char *body)
{
    FILE *f = fopen(g_child, "w");
    if (!f)
        return -1;
    fprintf(f, "#!/bin/sh\n%s\n", body);
    fclose(f);

    return chmod(g_child, 0700);
}

/* A hang ends the run with a reason. */
static void hung(int sig)
{
    (void)sig;
    static const char msg[] = "  migrate_exec did not return within 20 s\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

/* The child fills the stderr pipe while its stdout stays open: both pipes are read at once, so
 * neither side waits on the other. 128 KiB is twice the default Linux pipe capacity. */
static int test_reads_both_pipes_at_once(void)
{
    if (write_child("echo row\nhead -c 131072 /dev/zero | tr '\\000' x >&2\nexit 1") != 0)
        return 1;

    char out[64], err[1024];
    alarm(20);
    int rc = migrate_exec(g_child, "db", "SELECT 1", out, sizeof(out), err, sizeof(err));
    alarm(0);

    return rc == 1 && strcmp(out, "row\n") == 0 && strlen(err) == sizeof(err) - 1 && err[0] == 'x'
               ? 0
               : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(reads_both_pipes_at_once),
};

int main(void)
{
    snprintf(g_child, sizeof(g_child), "/tmp/minimoni-int-migrate-%d.sh", getpid());
    signal(SIGALRM, hung);

    int rc = run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
    unlink(g_child);

    return rc;
}
