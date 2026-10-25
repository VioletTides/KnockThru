#!/usr/bin/env bash
# Prints each KnockThru thread's CPU use and wakeups on the Frame over a window.
# Usage: scripts/measure.sh [SECONDS] [SSH_HOST]   (defaults: 60, frame)
# Wakeups are voluntary context switches: a thread that sleeps and wakes again.
# Each one can keep a core out of deep idle, which costs battery beyond CPU time.
set -euo pipefail

seconds="${1:-60}"
host="${2:-frame}"

ssh "$host" bash -s -- "$seconds" <<'EOF'
set -euo pipefail
seconds="$1"
pid="$(pgrep -f '^/.*/knockthru --run$' | head -n 1 || true)"
if [ -z "$pid" ]; then
    echo "KnockThru is not running" >&2
    exit 1
fi
ticks="$(getconf CLK_TCK)"

# One line per thread: tid name cpu_ticks wakeups
snapshot() {
    for task in /proc/"$pid"/task/*; do
        local tid name cpu wakeups
        tid="${task##*/}"
        name="$(cat "$task/comm")"
        # utime and stime are fields 14 and 15. Fields after the ")" of comm start at 3.
        cpu="$(sed 's/.*) //' "$task/stat" | awk '{print $12 + $13}')"
        wakeups="$(awk '/^voluntary_ctxt_switches/ {print $2}' "$task/status")"
        echo "$tid $name $cpu $wakeups"
    done
}

before="$(snapshot)"
sleep "$seconds"
after="$(snapshot)"

echo "KnockThru pid $pid over $seconds s"
printf '%-8s %-16s %10s %12s\n' tid thread "cpu %" "wakeups/s"
join <(echo "$before" | sort) <(echo "$after" | sort) | awk -v s="$seconds" -v hz="$ticks" '
    { cpu = ($6 - $3) / hz / s * 100; wake = ($7 - $4) / s
      total_cpu += cpu; total_wake += wake
      printf "%-8s %-16s %10.2f %12.0f\n", $1, $2, cpu, wake }
    END { printf "%-8s %-16s %10.2f %12.0f\n", "", "total", total_cpu, total_wake }'
EOF
