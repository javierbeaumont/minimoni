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

#define _POSIX_C_SOURCE 200809L

#include "exec.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* One captured stream: up to cap-1 bytes are kept, the rest is read and dropped. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sink_t;

/* One read into `s`. Returns 0 at end of stream or on an error, 1 otherwise. */
static int sink_read(int fd, sink_t *s)
{
    char    scratch[1024];
    int     keep = s->len + 1 < s->cap;
    ssize_t n =
        keep ? read(fd, s->buf + s->len, s->cap - 1 - s->len) : read(fd, scratch, sizeof(scratch));

    if (n <= 0)
        return 0;

    if (keep)
        s->len += (size_t)n;

    return 1;
}

/* Both pipes at once, each until its end of stream: reading one to the end first deadlocks when
 * the child fills the other and blocks writing to it. */
static void drain_both(int out_fd, sink_t *out, int err_fd, sink_t *err)
{
    struct pollfd pfd[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
    sink_t       *sink[2] = {out, err};

    /* poll skips an fd set to -1, which is how a finished stream leaves the loop. */
    while (pfd[0].fd >= 0 || pfd[1].fd >= 0) {
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < 2; i++)
            if (pfd[i].revents && !sink_read(pfd[i].fd, sink[i]))
                pfd[i].fd = -1;
    }

    for (int i = 0; i < 2; i++)
        if (sink[i]->cap > 0)
            sink[i]->buf[sink[i]->len] = '\0';
}

int migrate_exec(const char *minimoni_exec, const char *db_path, const char *sql, const char *value,
                 char *out_buf, size_t out_size, char *err_buf, size_t err_size)
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
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);

        char *argv[7];
        int   i = 0;
        argv[i++] = (char *)minimoni_exec;
        argv[i++] = (char *)"db";
        argv[i++] = (char *)"exec";
        argv[i++] = (char *)db_path;
        argv[i++] = (char *)sql;
        if (value)
            argv[i++] = (char *)value;
        argv[i] = NULL;

        execvp(minimoni_exec, argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    sink_t out = {out_buf, out_size, 0};
    sink_t err = {err_buf, err_size, 0};
    drain_both(out_pipe[0], &out, err_pipe[0], &err);
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (!WIFEXITED(status))
        return -3;

    int rc = WEXITSTATUS(status);

    if (rc == 127)
        return -2;

    return rc;
}
