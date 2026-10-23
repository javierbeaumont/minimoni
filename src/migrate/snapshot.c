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

#include "snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"

/* Like unlink, but a missing file counts as removed. Returns 0, or -1 with the reason reported. */
static int remove_file(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;

    fprintf(stderr, "migrate: cannot remove '%s': %s\n", path, strerror(errno));

    return -1;
}

/* Remove the files SQLite keeps beside a database (-journal, -wal, -shm). A writer that crashed
 * leaves them behind, and whoever opens the database next applies them to it. Returns 0, or -1 with
 * the reason reported. */
static int remove_sidecars(const char *path)
{
    static const char *const SUFFIXES[] = {"-journal", "-wal", "-shm"};
    char                     side[1100];

    for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
        int n = snprintf(side, sizeof(side), "%s%s", path, SUFFIXES[i]);
        if (n < 0 || (size_t)n >= sizeof(side)) {
            fprintf(stderr, "migrate: the path '%s' is too long\n", path);
            return -1;
        }

        if (remove_file(side) != 0)
            return -1;
    }

    return 0;
}

int migrate_snapshot(const char *minimoni_exec, const char *src_path, const char *dst_path)
{
    /* The copy is written under a temporary name and renamed over dst only once complete, so a
     * failed copy leaves an earlier one as it was. */
    char tmp[1100];
    int  n = snprintf(tmp, sizeof(tmp), "%s.tmp", dst_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        fprintf(stderr, "migrate: the path '%s' is too long\n", dst_path);
        return 1;
    }

    if (remove_file(tmp) != 0 || remove_sidecars(tmp) != 0)
        return 1;

    /* Created here to give the copy mode 0600; VACUUM INTO fills an empty file like a new one. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "migrate: cannot create '%s': %s\n", tmp, strerror(errno));
        return 1;
    }
    close(fd);

    char out[64], err[1024];
    int  rc = migrate_exec(minimoni_exec, src_path, "VACUUM INTO ?1", tmp, out, sizeof(out), err,
                           sizeof(err));

    if (rc != 0) {
        fprintf(stderr, "migrate: copying '%s' to '%s' failed (db exec rc=%d)\n", src_path, tmp,
                rc);
        if (err[0])
            fputs(err, stderr);
        unlink(tmp);

        return 1;
    }

    /* dst's sidecars belong to the file being replaced; left beside the copy, they would be applied
     * to it. rename replaces dst itself, never what a symlink there points to. */
    if (remove_sidecars(dst_path) != 0) {
        unlink(tmp);
        return 1;
    }

    if (rename(tmp, dst_path) != 0) {
        fprintf(stderr, "migrate: cannot replace '%s': %s\n", dst_path, strerror(errno));
        unlink(tmp);

        return 1;
    }

    return 0;
}
