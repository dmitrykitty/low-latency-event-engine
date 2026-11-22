#!/usr/bin/env bash
# Run the whole engine on one host as four separate processes:
#
#   lle-demo-producer -> ingress SHM -> lle-sender -> UDP 127.0.0.1 -> lle-receiver
#                     -> egress SHM -> lle-demo-consumer
#
# Start order follows ownership: the receiver creates egress before the consumer attaches,
# and the producer creates ingress before the sender attaches. Shutdown: the producer closes
# ingress, the sender drains it and exits, then the receiver gets SIGINT, closes egress, and
# the consumer drains it and prints its summary.
#
# Settings (environment, defaults in brackets):
#   LLE_DEMO_BIN_DIR [build/dev/apps]  LLE_DEMO_COUNT [100000]  LLE_DEMO_RATE [100000]
#   LLE_DEMO_PAYLOAD [64]  LLE_DEMO_CPUS ["" or producer,sender,receiver,consumer]
#   LLE_DEMO_STRICT [0]  1 = fail unless every event arrived intact
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
bin_dir="${LLE_DEMO_BIN_DIR:-$repo_dir/build/dev/apps}"
count="${LLE_DEMO_COUNT:-100000}"
rate="${LLE_DEMO_RATE:-100000}"
payload="${LLE_DEMO_PAYLOAD:-64}"
strict="${LLE_DEMO_STRICT:-0}"
ingress="/lle-demo-in-$$"
egress="/lle-demo-out-$$"
logs="$(mktemp -d)"

cpu_args=("" "" "" "")
if [[ -n ${LLE_DEMO_CPUS:-} ]]; then
    IFS=',' read -ra cpus <<< "$LLE_DEMO_CPUS"
    for i in 0 1 2 3; do cpu_args[i]="--cpu=${cpus[i]}"; done
fi

pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2> /dev/null || true; done
    rm -f "/dev/shm$ingress" "/dev/shm$egress"
    rm -rf "$logs"
}
trap cleanup EXIT

# wait until a process prints a line matching $2 (or exits).
wait_for() { # log file, pattern, pid
    for _ in $(seq 500); do
        if grep -q "$2" "$1" 2> /dev/null; then return 0; fi
        if ! kill -0 "$3" 2> /dev/null; then break; fi
        sleep 0.01
    done
    echo "timed out waiting for '$2' in $1:" >&2
    cat "$1" >&2
    return 1
}

# 1. receiver: binds a free UDP port and creates egress.
"$bin_dir/lle-receiver" --listen=127.0.0.1:0 --egress="$egress" ${cpu_args[2]} \
    > "$logs/receiver.log" 2>&1 &
receiver=$!
pids+=("$receiver")
wait_for "$logs/receiver.log" "listening on" "$receiver"
port="$(sed -n 's/.*listening on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$logs/receiver.log")"

# 2. consumer: attaches to egress.
"$bin_dir/lle-demo-consumer" --egress="$egress" ${cpu_args[3]} > "$logs/consumer.log" 2>&1 &
consumer=$!
pids+=("$consumer")
wait_for "$logs/consumer.log" "attached to" "$consumer"

# 3. producer: creates ingress, then gives the sender time to attach before publishing.
"$bin_dir/lle-demo-producer" --ingress="$ingress" --count="$count" --rate="$rate" \
    --payload="$payload" --start-delay-ms=300 ${cpu_args[0]} > "$logs/producer.log" 2>&1 &
producer=$!
pids+=("$producer")
wait_for "$logs/producer.log" "ready" "$producer"

# 4. sender: attaches to ingress and sends to the receiver's port.
"$bin_dir/lle-sender" --ingress="$ingress" --dest="127.0.0.1:$port" ${cpu_args[1]} \
    > "$logs/sender.log" 2>&1 &
sender=$!
pids+=("$sender")

status=0
wait "$producer" || status=1
wait "$sender" || status=1      # exits by itself once ingress is closed and drained
sleep 0.2                       # let the last datagrams reach the receiver
kill -INT "$receiver"
wait "$receiver" || status=1
wait "$consumer" || status=1
pids=()

for name in producer sender receiver consumer; do
    cat "$logs/$name.log"
done

if [[ $strict == 1 ]]; then
    if ! grep -q "received=$count missing=0 invalid=0" "$logs/consumer.log"; then
        echo "strict check failed: expected received=$count missing=0 invalid=0" >&2
        status=1
    fi
fi
exit "$status"
