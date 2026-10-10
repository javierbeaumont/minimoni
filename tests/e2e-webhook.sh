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

# Webhook delivery over the wire: asserts that an `https://` webhook completes a real TLS handshake
# and that the POST arrives. Needs socat to terminate TLS. The config gate is asserted in e2e-cli.sh
# instead: it trips before any socket is opened, so it needs no server and always runs.
set -u

BIN=./minimoni
TLS_PORT=18101
TCP_PORT=18102
EXPIRED_PORT=18103
pass=0
fail=0

for tool in socat openssl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  $tool not found in PATH; install it to run webhook tests" >&2
        exit 2
    fi
done

check_has() { # DESC HAYSTACK NEEDLE
    if printf '%s' "$2" | grep -qF -- "$3"; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s (missing: %s)\n' "$1" "$3"
    fi
}

check_lacks() { # DESC HAYSTACK NEEDLE
    if printf '%s' "$2" | grep -qF -- "$3"; then
        fail=$((fail + 1))
        printf '  FAIL  %s (unexpected: %s)\n' "$1" "$3"
    else
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    fi
}

work=$(mktemp -d)
listeners=""
cleanup() {
    for pid in $listeners; do
        kill "$pid" 2>/dev/null
    done
    rm -rf "$work"
}

trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$work/key.pem" -out "$work/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
cat "$work/cert.pem" "$work/key.pem" >"$work/both.pem"

openssl req -x509 -newkey rsa:2048 -nodes \
    -not_before 20200101000000Z -not_after 20200201000000Z \
    -keyout "$work/expired-key.pem" -out "$work/expired-cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
cat "$work/expired-cert.pem" "$work/expired-key.pem" >"$work/expired.pem"

received="$work/received.txt"
cat >"$work/reply.sh" <<EOF
#!/bin/sh
timeout 2 cat >>"$received"
printf 'HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok'
EOF
chmod +x "$work/reply.sh"

socat "OPENSSL-LISTEN:$TLS_PORT,reuseaddr,fork,cert=$work/both.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "TCP-LISTEN:$TCP_PORT,reuseaddr,fork" "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$EXPIRED_PORT,reuseaddr,fork,cert=$work/expired.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

# Poll instead of sleeping blind: socat needs a moment to bind.
ready=0
attempt=0
while [ "$attempt" -lt 40 ]; do
    if openssl s_client -connect "127.0.0.1:$TLS_PORT" </dev/null >/dev/null 2>&1; then
        ready=1
        break
    fi
    attempt=$((attempt + 1))
    sleep 0.25
done

if [ "$ready" -ne 1 ]; then
    echo "  socat did not come up on port $TLS_PORT" >&2
    exit 2
fi

fire() { # URL INSECURE(0|1) [NAME] [TITLE] -> sets $out (stderr) and $got (bytes the server read)
    : >"$received"
    rm -f "$work/metrics.db"
    cat >"$work/config.toml" <<EOF
[collect]
db = "$work/metrics.db"
[[alert]]
name = "${3:-probe}"
metric = "uptime_seconds"
operator = ">"
threshold = 0
webhook = "$1"
cooldown = "1h"
EOF
    if [ "$2" = 1 ]; then
        printf '[webhook]\ninsecure_skip_verify = true\n' >>"$work/config.toml"
    fi
    if [ -n "${4:-}" ]; then
        printf '[dashboard]\ntitle = "%s"\n' "$4" >>"$work/config.toml"
    fi

    out=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 | grep -v 'firing')
    sleep 3 # the handler holds the connection for up to 2s before replying
    got=$(cat "$received" 2>/dev/null)
}

echo "Webhook delivery tests:"

fire "https://127.0.0.1:$TLS_PORT/hook" 1
check_has "https webhook completes the handshake and arrives" "$got" "POST /hook"
check_has "the delivered payload names the alert" "$got" '"alert":"probe"'
check_has "the delivered payload names the metric" "$got" '"metric":"uptime_seconds"'
check_lacks "a delivered webhook reports no failure" "$out" "not delivered"

fire "https://127.0.0.1:$TCP_PORT/hook" 1
check_lacks "a failed handshake delivers nothing" "$got" "POST"
check_has "a failed handshake is reported, not silent" "$out" "not delivered"

# Deliberate: the opt-out drops the trust verdict, not the validity dates. br_x509_minimal withholds
# the key on an expired chain, so the handshake cannot complete.
fire "https://127.0.0.1:$EXPIRED_PORT/hook" 1
check_lacks "an expired certificate is refused even under the opt-out" "$got" "POST"
# BR_ERR_X509_EXPIRED, pinned so this cannot pass on a dead listener or any other handshake failure.
check_has "an expired certificate fails on its dates" "$out" "not delivered, TLS error 54"

fire "http://127.0.0.1:$TCP_PORT/hook" 0
check_has "a plain http webhook still arrives" "$got" "POST /hook"

# Regression: a request longer than its 1024-byte buffer was sent at its full length, reading past
# the end of the buffer. A long path plus a name and title of quotes, which escape to twice their
# size, take it to about 1100 bytes.
long_path=$(printf '%480s' '' | tr ' ' a)
quotes63=$(printf '%63s' '' | sed 's/ /\\"/g')
quotes127=$(printf '%127s' '' | sed 's/ /\\"/g')
fire "http://127.0.0.1:$TCP_PORT/$long_path" 0 "$quotes63" "$quotes127"
check_lacks "a webhook that does not fit its buffer sends nothing" "$got" "POST"
check_has "a webhook that does not fit its buffer says so" "$out" "does not fit in 1024 bytes"

# Regression: a host longer than its 255-byte field was cut and the request went to another name.
fire "http://$(printf '%256s' '' | tr ' ' a)/hook" 0
check_lacks "a webhook host that does not fit sends nothing" "$got" "POST"
check_has "a webhook host that does not fit says so" "$out" "host, port or path does not fit"

printf '\n  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
