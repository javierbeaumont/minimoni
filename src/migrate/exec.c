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

#include "exec.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Read up to (cap-1) bytes from fd into buf, NUL-terminate. Drains until
 * EOF so the writer never blocks. Bytes beyond cap-1 are discarded. */
static void drain_fd(int fd, char *buf, size_t cap)
{
    size_t len = 0;
    char   scratch[1024];
    for (;;) {
        ssize_t n;
        if (len + 1 < cap) {
            n = read(fd, buf + len, cap - 1 - len);
        } else {
            n = read(fd, scratch, sizeof(scratch));
        }
        if (n <= 0)
            break;
        if (len + 1 < cap)
            len += (size_t)n;
    }
    if (cap > 0)
        buf[len < cap ? len : cap - 1] = '\0';
}

int migrate_exec(const char *minimoni_exec, const char *db_path, const char *sql, char *out_buf,
                 size_t out_size, char *err_buf, size_t err_size)
{
    if (out_size > 0)
        out_buf[0] = '\0';
    if (err_size > 0)
        err_buf[0] = '\0';

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0)
        return -1;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: rewire stdout/stderr to pipe write ends, then exec. */
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);

        char *argv[6];
        int   i = 0;
        argv[i++] = (char *)minimoni_exec;
        argv[i++] = (char *)"db";
        argv[i++] = (char *)"exec";
        argv[i++] = (char *)db_path;
        argv[i++] = (char *)sql;
        argv[i] = NULL;
        execv(minimoni_exec, argv);
        /* execv only returns on failure */
        _exit(127);
    }

    /* Parent: close write ends, drain both pipes, wait for child. */
    close(out_pipe[1]);
    close(err_pipe[1]);
    drain_fd(out_pipe[0], out_buf, out_size);
    drain_fd(err_pipe[0], err_buf, err_size);
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (WIFSIGNALED(status))
        return -3;
    if (!WIFEXITED(status))
        return -3;
    int rc = WEXITSTATUS(status);
    if (rc == 127)
        return -2;
    return rc;
}
