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

# Webhook delivery over the wire: asserts that an `https://` webhook completes a real TLS handshake,
# verified against a test CA or unverified under the opt-out, and that the POST arrives. Needs socat
# to terminate TLS.
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
CA_PORT=18112
CA_EXPIRED_PORT=18113
SELF_CA_PORT=18114
SELF_LEAF_PORT=18115
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
    -not_before 20000101000000Z -not_after 20000101010000Z \
    -keyout "$work/expired-key.pem" -out "$work/expired-cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
cat "$work/expired-cert.pem" "$work/expired-key.pem" >"$work/expired.pem"

# A test CA and the leaves it signs for localhost, the name the verified URLs use: a certificate is
# matched against the host's DNS name, never against an IP address.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$work/ca-key.pem" -out "$work/ca.pem" -subj "/CN=minimoni test CA" >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -keyout /dev/null \
    -out "$work/other-ca.pem" -subj "/CN=minimoni other CA" >/dev/null 2>&1
leaf() { # NAME [OPENSSL ARGS...]: NAME.pem holds the certificate and its key
    name=$1
    shift
    openssl req -x509 -newkey rsa:2048 -nodes "$@" -CA "$work/ca.pem" -CAkey "$work/ca-key.pem" \
        -keyout "$work/$name-key.pem" -out "$work/$name-cert.pem" -subj "/CN=localhost" \
        -addext basicConstraints=CA:FALSE -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1
    cat "$work/$name-cert.pem" "$work/$name-key.pem" >"$work/$name.pem"
}
leaf ca-leaf -days 1
leaf ca-expired -not_before 20000101000000Z -not_after 20000101010000Z

# Self-signed for localhost: one marked as a CA, openssl's default, and one not.
selfsigned() { # NAME [OPENSSL ARGS...]: NAME.pem holds the certificate and its key
    name=$1
    shift
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 "$@" \
        -keyout "$work/$name-key.pem" -out "$work/$name-cert.pem" -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1
    cat "$work/$name-cert.pem" "$work/$name-key.pem" >"$work/$name.pem"
}
selfsigned self-ca
selfsigned self-leaf -addext basicConstraints=CA:FALSE

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

socat "OPENSSL-LISTEN:$CA_PORT,reuseaddr,fork,cert=$work/ca-leaf.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$CA_EXPIRED_PORT,reuseaddr,fork,cert=$work/ca-expired.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$SELF_CA_PORT,reuseaddr,fork,cert=$work/self-ca.pem,verify=0" \
    "SYSTEM:$work/reply.sh" >/dev/null 2>&1 &
listeners="$listeners $!"

socat "OPENSSL-LISTEN:$SELF_LEAF_PORT,reuseaddr,fork,cert=$work/self-leaf.pem,verify=0" \
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

ca_file=""
fire() { # URL INSECURE(0|1) [NAME] [TITLE] [FORMAT] -> sets $out (stderr) and $got (bytes received)
    # $ca_file, when set, goes under [alerts] too.
    : >"$received"
    rm -f "$work/metrics.db"
    cat >"$work/config.toml" <<EOF
[collect]
db = "$work/metrics.db"
[[alert]]
name = "${3:-probe}"
when = "uptime_seconds > 0"
webhook = "$1"
cooldown = "1h"
EOF
    if [ -n "${5:-}" ]; then
        printf 'format = "%s"\n' "$5" >>"$work/config.toml"
    fi
    printf '[alerts]\n' >>"$work/config.toml"
    if [ "$2" = 1 ]; then
        printf 'insecure_skip_verify = true\n' >>"$work/config.toml"
    fi
    if [ -n "$ca_file" ]; then
        printf 'ca_file = "%s"\n' "$ca_file" >>"$work/config.toml"
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
check_has "the delivered payload says the alert is firing" "$got" '"status":"firing"'
check_has "the json format is sent as application/json" "$got" "Content-Type: application/json"
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
alert_firing() { # NAME: an alert table whose condition always holds, missing its action
    printf '[[alert]]\nname = "%s"\nwhen = "uptime_seconds > 0"\n' "$1"
}

reset_after_handshake() { # -> sets $rc (collect's exit status) and $ran (yes if the last alert ran)
    rm -f "$work/metrics.db" "$work/ran"
    {
        printf '[collect]\ndb = "%s"\n[alerts]\ninsecure_skip_verify = true\n' "$work/metrics.db"
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
        printf '[collect]\ndb = "%s"\n[alerts]\ninsecure_skip_verify = true\n' "$work/metrics.db"
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

# An alert counts as firing only once an action succeeds: a webhook that gets a 2xx reply, or a
# command that exits 0. Each case runs collect twice on the same database.
twice() { # WEBHOOK|- COMMAND|- -> sets $first and $second (stderr of each run)
    rm -f "$work/metrics.db"
    {
        printf '[collect]\ndb = "%s"\n' "$work/metrics.db"
        alert_firing cool
        [ "$1" = - ] || printf 'webhook = "%s"\n' "$1"
        [ "$2" = - ] || printf 'command = "%s"\n' "$2"
    } >"$work/config.toml"

    first=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
    second=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
}

twice "http://127.0.0.1:$TCP_PORT/hook" -
check_has "a webhook that gets a 2xx reply fires" "$first" "firing 'cool'"
check_lacks "and the next run sends nothing while it holds" "$second" "firing 'cool'"
twice "http://127.0.0.1:$FAIL_PORT/hook" -
check_has "a webhook that gets a 500 reports it" "$first" "answered HTTP 500"
check_has "and the next run fires again" "$second" "firing 'cool'"
twice - /bin/false
check_has "a failing command fires again on the next run" "$second" "firing 'cool'"
twice "http://127.0.0.1:$FAIL_PORT/hook" /bin/true
check_lacks "one successful action is enough to count it as firing" "$second" "firing 'cool'"

fire "http://127.0.0.1:$TCP_PORT/hook" 0 cool web-01 text
check_has "the text format is sent as plain text" "$got" "Content-Type: text/plain; charset=utf-8"
check_has "its line names the alert" "$got" "[firing] cool: uptime_seconds "
check_has "then the condition and the host" "$got" " > 0 on web-01"
check_lacks "and no JSON body" "$got" '"alert":'

fire "http://127.0.0.1:$TCP_PORT/hook" 0 cool web-01 alertmanager
check_has "the alertmanager format is sent as JSON" "$got" "Content-Type: application/json"
check_has "its body is Alertmanager's, version 4, grouped by the alert and the host" "$got" \
    '"version":"4","groupKey":"{}:{alertname=\"cool\", instance=\"web-01\"}"'
check_has "a firing alert has no end" "$got" '"endsAt":"0001-01-01T00:00:00Z"'

fire "http://127.0.0.1:$TCP_PORT/hook" 0 cool web-01 grafana
check_has "the grafana format is sent as JSON" "$got" "Content-Type: application/json"
check_has "its body is Grafana's, version 1" "$got" '"version":"1","groupKey":"{}:{alertname='
check_has "with a title, a state and a message" "$got" \
    '"title":"cool on web-01","state":"alerting","message":"uptime_seconds '

fire "http://127.0.0.1:$TCP_PORT/hook" 0 cool 'web \"01\"' slack
check_has "the slack format is sent as JSON" "$got" "Content-Type: application/json"
check_has "its text is the text format's line" "$got" '{"text":"[firing] cool: uptime_seconds '
check_has "then the condition and the host, escaped" "$got" ' &gt; 0 on web \"01\""}'

# One alert across runs on the same database, its condition changed between them:
# `uptime_seconds > 0` always holds, `< 0` never.
alert_run() { # WHEN COOLDOWN|- WEBHOOK|- COMMAND|- [FORMAT] -> sets $out (stderr of one collect)
    {
        printf '[collect]\ndb = "%s"\n' "$work/metrics.db"
        printf '[[alert]]\nname = "cool"\nwhen = "%s"\n' "$1"
        [ "$2" = - ] || printf 'cooldown = "%s"\n' "$2"
        [ "$3" = - ] || printf 'webhook = "%s"\n' "$3"
        [ "$4" = - ] || printf 'command = "%s"\n' "$4"
        [ -z "${5:-}" ] || printf 'format = "%s"\n' "$5"
    } >"$work/config.toml"

    out=$(timeout 30 "$BIN" collect --config "$work/config.toml" 2>&1 >/dev/null)
}

rm -f "$work/metrics.db"
: >"$received"
alert_run "uptime_seconds > 0" - "http://127.0.0.1:$TCP_PORT/hook" -
alert_run "uptime_seconds < 0" - "http://127.0.0.1:$TCP_PORT/hook" -
check_has "a firing alert resolves once its condition stops holding" "$out" "resolved 'cool'"
sleep 3 # the handler holds the connection for up to 2s before replying
check_has "the payload says the alert is resolved" "$(tr -d '\000' <"$received")" \
    '"status":"resolved"'
alert_run "uptime_seconds < 0" - "http://127.0.0.1:$TCP_PORT/hook" -
check_lacks "and the next run sends nothing more" "$out" "resolved 'cool'"

# An alertmanager resolve starts when its firing did, seconds earlier.
rm -f "$work/metrics.db"
: >"$received"
alert_run "uptime_seconds > 0" - "http://127.0.0.1:$TCP_PORT/hook" - alertmanager
sleep 3 # the handler holds the connection for up to 2s before replying
started=$(tr -d '\000' <"$received" | grep -o '"startsAt":"[^"]*"')
: >"$received"
alert_run "uptime_seconds < 0" - "http://127.0.0.1:$TCP_PORT/hook" - alertmanager
sleep 3
resolved=$(tr -d '\000' <"$received")
check_has "an alertmanager firing has a start" "$started" '"startsAt":"20'
check_has "its resolve keeps that start" "$resolved" "$started"
check_lacks "and has an end" "$resolved" '"endsAt":"0001-01-01T00:00:00Z"'

rm -f "$work/metrics.db"
alert_run "uptime_seconds > 0" 1h - /bin/true
check_has "an alert with a cooldown fires" "$out" "firing 'cool'"
alert_run "uptime_seconds < 0" 1h - /bin/true
check_has "and resolves" "$out" "resolved 'cool'"
alert_run "uptime_seconds > 0" 1h - /bin/true
check_lacks "an alert that holds again within its cooldown sends nothing" "$out" "firing 'cool'"

rm -f "$work/metrics.db"
alert_run "uptime_seconds > 0" - - /bin/true
alert_run "uptime_seconds < 0" - - /bin/false
alert_run "uptime_seconds < 0" - - /bin/false
check_has "a resolved message that no action delivers is sent again on the next run" "$out" \
    "resolved 'cool'"

# A firing alert left out of the config is resolved: back in it, it fires again.
rm -f "$work/metrics.db"
alert_run "uptime_seconds > 0" - - /bin/true
printf '[collect]\ndb = "%s"\n' "$work/metrics.db" >"$work/config.toml"
timeout 30 "$BIN" collect --config "$work/config.toml" >/dev/null 2>&1
alert_run "uptime_seconds > 0" - - /bin/true
check_has "a firing alert removed from the config is resolved" "$out" "firing 'cool'"

# Deliberate: the opt-out drops the trust verdict, not the validity dates. br_x509_minimal withholds
# the key on an expired chain, so the handshake cannot complete.
fire "https://127.0.0.1:$EXPIRED_PORT/hook" 1
check_lacks "an expired certificate is refused even under the opt-out" "$got" "POST"
# BR_ERR_X509_EXPIRED, pinned so this cannot pass on a dead listener or any other handshake failure.
check_has "an expired certificate fails on its dates" "$out" "not delivered, TLS error 54"

# Without the opt-out, the certificate must chain to ca_file, or to the system store when ca_file is
# not set.
ca_file="$work/ca.pem"
fire "https://localhost:$CA_PORT/hook" 0
check_has "a certificate signed by the ca_file CA is verified and the webhook arrives" "$got" \
    "POST /hook"
check_lacks "and reports no failure" "$out" "not delivered"

fire "https://localhost:$CA_EXPIRED_PORT/hook" 0
check_lacks "an expired certificate from the ca_file CA is refused" "$got" "POST"
check_has "and the reason is named" "$out" "TLS error 54 (certificate expired or not yet valid)"

fire "https://127.0.0.1:$CA_PORT/hook" 0
check_lacks "a certificate for another name is refused" "$got" "POST"
check_has "and the reason is named" "$out" "TLS error 56 (certificate does not match the host name)"

ca_file="$work/other-ca.pem"
fire "https://localhost:$CA_PORT/hook" 0
check_lacks "a certificate from a CA outside ca_file is refused" "$got" "POST"
check_has "and the reason is named" "$out" "TLS error 62 (certificate not trusted)"

ca_file=""
fire "https://localhost:$CA_PORT/hook" 0
check_lacks "without ca_file, a certificate from a CA outside the system store is refused" \
    "$got" "POST"
check_has "and that is reported" "$out" "webhook to localhost not delivered"

# A self-signed endpoint is trusted when ca_file is its own certificate, CA or not.
ca_file="$work/self-ca-cert.pem"
fire "https://localhost:$SELF_CA_PORT/hook" 0
check_has "a self-signed certificate marked as a CA is trusted through ca_file" "$got" \
    "POST /hook"
ca_file="$work/self-leaf-cert.pem"
fire "https://localhost:$SELF_LEAF_PORT/hook" 0
check_has "a self-signed certificate not marked as a CA is trusted through ca_file" "$got" \
    "POST /hook"
fire "https://localhost:$SELF_CA_PORT/hook" 0
check_lacks "and it trusts no other self-signed certificate" "$got" "POST"

# The opt-out wins over ca_file, and the load says so.
ca_file="$work/other-ca.pem"
fire "https://localhost:$CA_PORT/hook" 1
check_has "insecure_skip_verify delivers even when ca_file would refuse the certificate" "$got" \
    "POST /hook"
check_has "and the load warns that ca_file is ignored" "$out" "alerts.ca_file is ignored"
ca_file=""

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

# A long path and a long body fit the request together.
long_path=$(printf '%480s' '' | tr ' ' a)
fire "http://127.0.0.1:$TCP_PORT/$long_path" 0 "$(printf '%63s' '' | tr ' ' n)" \
    "$(printf '%127s' '' | tr ' ' h)" alertmanager
check_has "a webhook with a long path and a long body is sent" "$got" "POST /$long_path HTTP"
check_has "and arrives whole" "$got" '"truncatedAlerts":0}'

# Regression: a host longer than its 255-byte field was cut and the request went to another name.
fire "http://$(printf '%256s' '' | tr ' ' a)/hook" 0
check_lacks "a webhook host that does not fit sends nothing" "$got" "POST"
check_has "a webhook host that does not fit says so" "$out" "host, port or path does not fit"

printf '\n  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
