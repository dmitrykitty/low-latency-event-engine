#!/usr/bin/env bash
# Put the CPU into a low-noise benchmark state, or restore the saved state.
#   sudo scripts/bench/tune.sh apply     # save current settings, then tune
#   sudo scripts/bench/tune.sh restore   # put the saved settings back
#   scripts/bench/tune.sh status         # show current settings (no sudo)
# Changes: cpufreq governor -> performance (intel_pstate then forces EPP to
# performance), turbo off, SMT off. Nothing persists across a reboot.
# Core isolation needs kernel boot parameters; see benchmarks/micro/README.md.
set -euo pipefail

# /run is root-only and cleared at boot, like the settings themselves.
state_file="/run/lle-bench-tune.state"
cpu_root="/sys/devices/system/cpu"
no_turbo="$cpu_root/intel_pstate/no_turbo"
smt="$cpu_root/smt/control"

status() {
    printf 'governor: %s\n' "$(cat "$cpu_root"/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | xargs)"
    printf 'epp:      %s\n' "$(cat "$cpu_root"/cpu*/cpufreq/energy_performance_preference 2>/dev/null | sort | uniq -c | xargs)"
    printf 'no_turbo: %s\n' "$(cat "$no_turbo" 2>/dev/null || echo n/a)"
    printf 'smt:      %s\n' "$(cat "$smt" 2>/dev/null || echo n/a)"
    printf 'online:   %s\n' "$(cat "$cpu_root/online")"
}

require_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "run with sudo: sudo $0 $1" >&2
        exit 1
    fi
}

# offline CPUs (e.g. SMT siblings after "smt off") keep their cpufreq files,
# but reading or writing them fails with EBUSY, so only touch online CPUs.
online_cpu_dirs() {
    local dir
    for dir in "$cpu_root"/cpu[0-9]*; do
        if [[ ! -e $dir/online || $(cat "$dir/online") == 1 ]]; then
            echo "$dir"
        fi
    done
}

set_governor() {
    local dir
    for dir in $(online_cpu_dirs); do
        echo "$1" > "$dir/cpufreq/scaling_governor"
    done
}

set_epp() {
    local dir
    for dir in $(online_cpu_dirs); do
        if [[ -e $dir/cpufreq/energy_performance_preference ]]; then
            echo "$1" > "$dir/cpufreq/energy_performance_preference"
        fi
    done
}

case "${1:-}" in
apply)
    require_root apply
    if [[ -e $state_file ]]; then
        echo "saved state already exists ($state_file); run restore first" >&2
        exit 1
    fi
    {
        echo "governor=$(cat "$cpu_root/cpu0/cpufreq/scaling_governor")"
        echo "epp=$(cat "$cpu_root/cpu0/cpufreq/energy_performance_preference" 2>/dev/null || true)"
        echo "no_turbo=$(cat "$no_turbo" 2>/dev/null || true)"
        echo "smt=$(cat "$smt" 2>/dev/null || true)"
    } > "$state_file"
    # SMT first so the governor is also set on every CPU that stays online.
    if [[ -w $smt ]]; then echo off > "$smt"; fi
    set_governor performance
    if [[ -w $no_turbo ]]; then echo 1 > "$no_turbo"; fi
    echo "applied; previous settings saved to $state_file"
    status
    ;;
restore)
    require_root restore
    if [[ ! -e $state_file ]]; then
        echo "no saved state at $state_file; nothing to restore" >&2
        exit 1
    fi
    # parse known keys only; never execute the state file.
    saved_governor="" saved_epp="" saved_no_turbo="" saved_smt=""
    while IFS='=' read -r key value; do
        [[ $value =~ ^[A-Za-z0-9_]*$ ]] || continue
        case "$key" in
        governor) saved_governor=$value ;;
        epp) saved_epp=$value ;;
        no_turbo) saved_no_turbo=$value ;;
        smt) saved_smt=$value ;;
        esac
    done < "$state_file"
    # SMT first so restored CPUs also get the governor and EPP below.
    if [[ -n $saved_smt && -w $smt ]]; then echo "$saved_smt" > "$smt"; fi
    if [[ -n $saved_governor ]]; then set_governor "$saved_governor"; fi
    # intel_pstate only accepts EPP changes once the governor is no longer performance.
    if [[ -n $saved_epp && $saved_governor != performance ]]; then set_epp "$saved_epp"; fi
    if [[ -n $saved_no_turbo && -w $no_turbo ]]; then echo "$saved_no_turbo" > "$no_turbo"; fi
    rm -f "$state_file"
    echo "restored"
    status
    ;;
status)
    status
    ;;
*)
    echo "usage: sudo $0 apply | sudo $0 restore | $0 status" >&2
    exit 2
    ;;
esac
