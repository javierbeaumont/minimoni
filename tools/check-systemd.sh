#!/bin/sh
# minimoni - zero-dependency system monitoring
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# Usage: check-systemd.sh IMAGE (a Debian image, booted with systemd as PID 1)
set -eu

c=minimoni-check-systemd
pass=0
fail=0

check() { # DESC COMMAND...
    desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n' "$desc"
    fi
}

in_c() { docker exec "$c" sh -c "$1"; }

docker rm -f "$c" >/dev/null 2>&1 || true
trap 'docker rm -f "$c" >/dev/null 2>&1 || true' EXIT

docker run -d --name "$c" --privileged --cgroupns=host -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    --tmpfs /run --tmpfs /run/lock -e DEBIAN_FRONTEND=noninteractive -v "$PWD":/src:ro "$1" \
    sh -c 'apt-get update -qq && apt-get install -y -qq --no-install-recommends systemd \
        systemd-sysv gcc make xxd git libc6-dev wget >/dev/null && exec /sbin/init' >/dev/null

i=0
until in_c 'systemctl is-system-running 2>/dev/null' | grep -qE 'running|degraded'; do
    i=$((i + 1))
    if [ "$i" -gt 120 ] || [ "$(docker inspect -f '{{.State.Running}}' "$c")" != true ]; then
        echo "check-systemd: systemd did not come up" >&2
        docker logs "$c" 2>&1 | tail -5 >&2
        exit 1
    fi
    sleep 2
done

in_c 'cp -r /src /w && cd /w && make clean >/dev/null 2>&1 && make release >/dev/null 2>&1 &&
    install -m755 minimoni /usr/local/bin/minimoni &&
    cp contrib/systemd/*.service contrib/systemd/*.timer /etc/systemd/system/ &&
    systemctl daemon-reload' || {
    echo "check-systemd: building minimoni or installing the units failed" >&2
    exit 1
}

state='/var/lib/private/minimoni'
health='wget -qO- http://127.0.0.1:18080/api/health'
echo "systemd unit checks:"

check "the units pass systemd-analyze verify" in_c \
    "[ -z \"\$(systemd-analyze verify /etc/systemd/system/minimoni*.service \
        /etc/systemd/system/minimoni*.timer 2>&1)\" ]"

in_c 'systemctl start minimoni minimoni-collect; sleep 8'
check "without a config, serve is skipped, not restarted" in_c \
    "[ \"\$(systemctl show minimoni -p ActiveState --value)\" = inactive ] &&
        [ \"\$(systemctl show minimoni -p NRestarts --value)\" = 0 ]"
check "without a config, nothing creates a database" in_c "[ ! -e $state/metrics.db ]"

in_c 'mkdir -p /etc/minimoni && printf "[server]\nlisten = \"127.0.0.1:18080\"\n" \
    >/etc/minimoni/config.toml && systemctl start minimoni; sleep 3'
check "with a config, serve answers" in_c "$health | grep -q '\"status\":\"ok\"'"
check "a relative db lands in the state directory" in_c "[ -s $state/metrics.db ]"

rows="minimoni db exec $state/metrics.db 'SELECT count(*) FROM metrics' | tail -1"
check "collect stores a sample" in_c \
    "n=\$($rows); systemctl start minimoni-collect && [ \"\$($rows)\" -gt \"\$n\" ]"

in_c "systemctl stop minimoni && printf '[server]\nlisten = \"127.0.0.1:18081\"\n' \
    >$state/config.toml && systemctl start minimoni; sleep 3"
check "a config planted in the state directory is ignored" in_c \
    "$health >/dev/null && ! wget -qO- http://127.0.0.1:18081/api/health"

printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
