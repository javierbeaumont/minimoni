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

#include "snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int migrate_snapshot(const char *src_path, const char *dst_path)
{
    int src = open(src_path, O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "migrate: cannot open '%s' for read: %s\n", src_path, strerror(errno));
        return 1;
    }
    int dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dst < 0) {
        fprintf(stderr, "migrate: cannot create '%s': %s\n", dst_path, strerror(errno));
        close(src);
        return 1;
    }

    char    buf[64 * 1024];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, (size_t)(n - off));
            if (w < 0) {
                fprintf(stderr, "migrate: write to '%s' failed: %s\n", dst_path, strerror(errno));
                close(src);
                close(dst);
                return 1;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "migrate: read from '%s' failed: %s\n", src_path, strerror(errno));
        close(src);
        close(dst);
        return 1;
    }

    if (fsync(dst) != 0) {
        fprintf(stderr, "migrate: fsync '%s' failed: %s\n", dst_path, strerror(errno));
        close(src);
        close(dst);
        return 1;
    }

    close(src);
    close(dst);
    return 0;
}
