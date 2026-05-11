#!/usr/bin/env bash
set -euo pipefail

log() {
    echo "[BOOT-TUNE] $*"
}

set_governor_performance() {
    local changed=0
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [[ -w "$gov" ]]; then
            echo performance > "$gov"
            changed=1
        fi
    done
    if [[ "$changed" -eq 1 ]]; then
        log "CPU governor set to performance"
    else
        log "No writable cpu governor controls found"
    fi
}

set_irq_affinity() {
    local mask="${LTC_IRQ_CPU_MASK:-4}"
    local irqs="${LTC_IRQ_LIST:-142 149 187}"
    local applied=0

    for irq in $irqs; do
        local path="/proc/irq/${irq}/smp_affinity"
        if [[ -w "$path" ]]; then
            echo "$mask" > "$path"
            log "IRQ ${irq} affinity -> ${mask}"
            applied=1
        fi
    done

    if [[ "$applied" -eq 0 ]]; then
        log "No target IRQ affinity files writable"
    fi
}

set_governor_performance
set_irq_affinity
