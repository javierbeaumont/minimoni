#!/bin/sh
# minimoni: zero-dependency system monitoring
#
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
#
# This file is part of minimoni.
#
# minimoni is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# minimoni is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with minimoni. If not, see <https://www.gnu.org/licenses/>.

# Webhook delivery over the wire: asserts that an `https://` webhook completes a real TLS handshake
# and that the POST arrives. Needs socat to terminate TLS. The config gate is asserted in e2e-cli.sh
# instead: it trips before any socket is opened, so it needs no server and always runs.
set -u

BIN=./minimoni
TLS_PORT=18101
TCP_PORT=18102
EXPIRED_PORT=18103
CLOSE_PORT=18104
RESET_PORT=18105
TLS_RESET_PORT=18106
DEFAULT_PORT=80
SLOW_PORT=18107
TLS_SLOW_PORT=18108
V4_PORT=18109
NOBODY_PORT=18110
FAIL_PORT=18111
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

socat "TCP-LISTEN:$DEFAULT_PORT,reuseaddr,fork" "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "TCP4-LISTEN:$V4_PORT,reuseaddr,fork" "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

printf '#!/bin/sh\ntimeout 1 cat >/dev/null\nprintf "HTTP/1.0 500 Oops\\r\\n\\r\\n"\n' \
    >"$work/reply500.sh"
chmod +x "$work/reply500.sh"
socat "TCP-LISTEN:$FAIL_PORT,reuseaddr,fork" "SYSTEM:$work/reply500.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

printf '#!/bin/sh\nwhile true; do printf x; sleep 2; done\n' >"$work/slow.sh"
chmod +x "$work/slow.sh"
socat "TCP-LISTEN:$SLOW_PORT,reuseaddr,fork" "EXEC:$work/slow.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$TLS_SLOW_PORT,reuseaddr,fork,cert=$work/both.pem,verify=0" \
    "EXEC:$work/slow.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$EXPIRED_PORT,reuseaddr,fork,cert=$work/expired.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "TCP-LISTEN:$CLOSE_PORT,reuseaddr,fork" EXEC:true >/dev/null 2>&1 &
listeners="$listeners $!"

socat "TCP-LISTEN:$RESET_PORT,reuseaddr,fork,linger=0" EXEC:true >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$TLS_RESET_PORT,reuseaddr,fork,cert=$work/both.pem,verify=0,linger=0" \
    EXEC:true >/dev/null 2>&1 &
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
    # A refused handshake leaves raw TLS bytes, and bash warns on their NULs inside $( ).
    got=$(tr -d '\000' <"$received" 2>/dev/null)
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

# Regression: a peer gone in the handshake read as 0 bytes; BearSSL called again, collect hung.
fire "https://127.0.0.1:$CLOSE_PORT/hook" 1
check_has "a peer that closes in the handshake is reported, not polled" "$out" "not delivered"
fire "https://127.0.0.1:$RESET_PORT/hook" 1
check_has "a peer that resets in the handshake is reported, not polled" "$out" "not delivered"

# Regression: a write after the peer's reset raised SIGPIPE and killed collect; without the signal,
# the TLS close looped forever. A race, so 15 deliveries to a peer that resets after the handshake,
# then an alert that must still run.
alert_firing() { # NAME: an alert table that always fires, missing its action
    printf '[[alert]]\nname = "%s"\nmetric = "uptime_seconds"\noperator = ">"\nthreshold = 0\n' "$1"
}

reset_after_handshake() { # -> sets $rc (collect's exit status) and $ran (yes if the last alert ran)
    rm -f "$work/metrics.db" "$work/ran"
    {
        printf '[collect]\ndb = "%s"\n[webhook]\ninsecure_skip_verify = true\n' "$work/metrics.db"
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            alert_firing "reset$i"
            printf 'webhook = "https://127.0.0.1:%s/hook"\n' "$TLS_RESET_PORT"
        done
        alert_firing after
        printf 'command = "/bin/touch %s/ran"\n' "$work"
    } >"$work/config.toml"

    timeout 60 "$BIN" collect --config "$work/config.toml" >/dev/null 2>&1
    rc=$?
    ran=$([ -e "$work/ran" ] && echo yes)
}

# The delivery deadline: collect gives up on a slow peer and still runs the alerts after it.
slow_peer() { # URL -> sets $rc, $ran, $took (seconds) and $out (stderr)
    rm -f "$work/metrics.db" "$work/ran"
    {
        printf '[collect]\ndb = "%s"\n[webhook]\ninsecure_skip_verify = true\n' "$work/metrics.db"
        alert_firing slow
        printf 'webhook = "%s"\n' "$1"
        alert_firing after
        printf 'command = "/bin/touch %s/ran"\n' "$work"
    } >"$work/config.toml"

    start=$(date +%s)
    out=$(timeout 60 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
    rc=$?
    took=$(($(date +%s) - start))
    ran=$([ -e "$work/ran" ] && echo yes)
}

for url in "http://127.0.0.1:$SLOW_PORT/hook" "https://127.0.0.1:$TLS_SLOW_PORT/hook"; do
    slow_peer "$url"
    check_has "a slow peer is given up within 20 s (${url%%:*})" \
        "exit $rc, $([ "$took" -lt 20 ] && echo within)" "exit 0, within"
    check_has "and says so (${url%%:*})" "$out" "timed out after 10 s"
    check_has "the alert after it still runs (${url%%:*})" "$ran" yes
done

for run in 1 2; do
    reset_after_handshake
    check_has "a peer that resets after the handshake neither kills nor hangs collect ($run)" \
        "exit $rc" "exit 0"
    check_has "the alert after it still runs ($run)" "$ran" yes
done

# The cooldown starts only when an action succeeds: a webhook that gets a 2xx reply, or a command
# that exits 0. Each case runs collect twice on the same database, with a one-hour cooldown.
twice() { # WEBHOOK|- COMMAND|- -> sets $first and $second (stderr of each run)
    rm -f "$work/metrics.db"
    {
        printf '[collect]\ndb = "%s"\n' "$work/metrics.db"
        alert_firing cool
        printf 'cooldown = "1h"\n'
        [ "$1" = - ] || printf 'webhook = "%s"\n' "$1"
        [ "$2" = - ] || printf 'command = "%s"\n' "$2"
    } >"$work/config.toml"

    first=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
    second=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
}

twice "http://127.0.0.1:$TCP_PORT/hook" -
check_has "a webhook that gets a 2xx reply fires" "$first" "firing 'cool'"
check_lacks "and the next run is in cooldown" "$second" "firing 'cool'"
twice "http://127.0.0.1:$FAIL_PORT/hook" -
check_has "a webhook that gets a 500 reports it" "$first" "answered HTTP 500"
check_has "and the next run fires again" "$second" "firing 'cool'"
twice - /bin/false
check_has "a failing command fires again on the next run" "$second" "firing 'cool'"
twice "http://127.0.0.1:$FAIL_PORT/hook" /bin/true
check_lacks "one successful action is enough to start the cooldown" "$second" "firing 'cool'"

# Deliberate: the opt-out drops the trust verdict, not the validity dates. br_x509_minimal withholds
# the key on an expired chain, so the handshake cannot complete.
fire "https://127.0.0.1:$EXPIRED_PORT/hook" 1
check_lacks "an expired certificate is refused even under the opt-out" "$got" "POST"
# BR_ERR_X509_EXPIRED, pinned so this cannot pass on a dead listener or any other handshake failure.
check_has "an expired certificate fails on its dates" "$out" "not delivered, TLS error 54"

fire "http://127.0.0.1:$TCP_PORT/hook" 0
check_has "a plain http webhook still arrives" "$got" "POST /hook"
check_has "Host carries a port that is not the default" "$got" "Host: 127.0.0.1:$TCP_PORT"

fire "http://127.0.0.1:$DEFAULT_PORT/hook" 0
check_has "a webhook on the default port arrives" "$got" "Host: 127.0.0.1"
check_lacks "Host leaves the default port out" "$got" "Host: 127.0.0.1:"

# Every address a name resolves to is tried: here localhost gives ::1 first, and only IPv4 listens.
fire "http://localhost:$V4_PORT/hook" 0
check_has "a webhook by name reaches a listener on its second address" "$got" "POST /hook"
check_lacks "and reports no failure" "$out" "connect to"
fire "http://localhost:$NOBODY_PORT/hook" 0
check_has "with no address listening, the failure is reported" "$out" \
    "webhook connect to localhost failed"

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
