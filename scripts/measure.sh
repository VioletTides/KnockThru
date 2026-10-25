#!/usr/bin/env bash
# Prints each KnockThru thread's CPU use and wakeups on the Frame over a window,
# and the headset's average battery power while it runs on battery.
# Usage: scripts/measure.sh [SECONDS] [SSH_HOST]   (defaults: 60, frame)
# Wakeups are voluntary context switches: a thread that sleeps and wakes again.
# Each one can keep a core out of deep idle, which costs battery beyond CPU time.
# With KnockThru stopped, it prints power only, as a baseline for KnockThru's share.
set -euo pipefail

seconds="${1:-60}"
host="${2:-frame}"

ssh "$host" bash -s -- "$seconds" <<'EOF'
set -euo pipefail
seconds="$1"
battery=/sys/class/power_supply/max1720x_bat_7-36
pid="$(pgrep -f '^/.*/knockthru --run$' | head -n 1 || true)"
ticks="$(getconf CLK_TCK)"

# The gauge reads charging state while a charger is attached, so power only
# means something on battery.
plugged=""
for online in /sys/class/power_supply/*/online; do
    [ "$(cat "$online" 2>/dev/null)" = 1 ] && plugged=1
done
[ "$(cat "$battery/status")" = Discharging ] || plugged=1

if [ -z "$pid" ] && [ -n "$plugged" ]; then
    echo "KnockThru is not running, and the headset is plugged in, so there is nothing to measure" >&2
    exit 1
fi

# One line per thread: tid name cpu_ticks wakeups
snapshot() {
    [ -n "$pid" ] || return 0
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

# current_now is in uA and voltage_now in uV. The sign of current_now depends
# on the gauge driver, so the power uses its magnitude.
power_samples=""
before="$(snapshot)"
for _ in $(seq "$seconds"); do
    sleep 1
    if [ -z "$plugged" ]; then
        power_samples+="$(cat "$battery/current_now") $(cat "$battery/voltage_now")"$'\n'
    fi
done
after="$(snapshot)"

if [ -n "$pid" ]; then
    echo "KnockThru pid $pid over $seconds s"
    printf '%-8s %-16s %10s %12s\n' tid thread "cpu %" "wakeups/s"
    join <(echo "$before" | sort) <(echo "$after" | sort) | awk -v s="$seconds" -v hz="$ticks" '
        { cpu = ($6 - $3) / hz / s * 100; wake = ($7 - $4) / s
          total_cpu += cpu; total_wake += wake
          printf "%-8s %-16s %10.2f %12.0f\n", $1, $2, cpu, wake }
        END { printf "%-8s %-16s %10.2f %12.0f\n", "", "total", total_cpu, total_wake }'
else
    echo "KnockThru is not running. Power only, over $seconds s"
fi

if [ -n "$plugged" ]; then
    echo "The headset is plugged in, so battery power is unknown. Use the CPU and wakeups above."
else
    echo "$power_samples" | awk 'NF == 2 {
            mw = ($1 < 0 ? -$1 : $1) * $2 / 1e9; sum += mw; n++
            if (n == 1 || mw < low) low = mw
            if (n == 1 || mw > high) high = mw }
        END { if (n) printf "Battery power: %.0f mW average, %.0f to %.0f mW, %d samples\n", sum / n, low, high, n }'
fi
EOF
