#!/usr/bin/env bash
#
# piBrick Battery Monitor
# Human-readable battery, fuel-gauge and charger diagnostics.
#
# Usage:
#   ./pibrick-battery-dump.sh
#   ./pibrick-battery-dump.sh --watch
#   ./pibrick-battery-dump.sh --watch 3
#
# Reads Linux power_supply sysfs only. It does not access I2C directly.

set -u

BATTERY="/sys/class/power_supply/battery"
WATCH=0
INTERVAL=2

usage() {
    cat <<'EOF'
piBrick Battery Monitor

Usage:
  ./pibrick-battery-dump.sh             Show one report
  ./pibrick-battery-dump.sh --watch     Refresh every 2 seconds
  ./pibrick-battery-dump.sh --watch 5   Refresh every 5 seconds
  ./pibrick-battery-dump.sh --help     Show this help
EOF
}

while (($#)); do
    case "$1" in
        --watch|-w)
            WATCH=1
            if [[ "${2:-}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
                INTERVAL="$2"
                shift
            fi
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
    shift
done

read_value() {
    local file="$1"
    if [[ -r "$file" ]]; then
        tr -d '\n' < "$file"
    else
        printf 'N/A'
    fi
}

number() {
    [[ "${1:-}" =~ ^-?[0-9]+([.][0-9]+)?$ ]]
}

scaled() {
    local value="$1" divisor="$2" unit="$3" digits="${4:-2}"
    if number "$value"; then
        awk -v v="$value" -v d="$divisor" -v u="$unit" -v n="$digits" \
            'BEGIN { printf "%.*f %s", n, v/d, u }'
    else
        printf 'N/A'
    fi
}

pretty_status() {
    case "$1" in
        Charging)       printf 'Charging' ;;
        Discharging)    printf 'Discharging' ;;
        Full)           printf 'Fully charged' ;;
        Not\ charging)  printf 'Not charging' ;;
        Unknown)        printf 'Unknown' ;;
        *)              printf '%s' "$1" ;;
    esac
}

line() {
    printf '  %-27s %s\n' "$1" "$2"
}

section() {
    printf '\n%s\n' "$1"
    printf '%s\n' '----------------------------------------------------------------'
}

report() {
    local voltage current soc status temp charge_now charge_full
    local energy_now energy_full power_uw power_abs_uw model vendor
    local current_desc power_desc

    [[ -d "$BATTERY" ]] || {
        echo "ERROR: Linux battery device not found: $BATTERY"
        echo
        echo "Detected power supplies:"
        ls -1 /sys/class/power_supply/ 2>/dev/null || true
        return 1
    }

    voltage="$(read_value "$BATTERY/voltage_now")"
    current="$(read_value "$BATTERY/current_now")"
    soc="$(read_value "$BATTERY/capacity")"
    status="$(read_value "$BATTERY/status")"
    temp="$(read_value "$BATTERY/temp")"
    charge_now="$(read_value "$BATTERY/charge_now")"
    charge_full="$(read_value "$BATTERY/charge_full")"
    energy_now="$(read_value "$BATTERY/energy_now")"
    energy_full="$(read_value "$BATTERY/energy_full")"
    model="$(read_value "$BATTERY/model_name")"
    vendor="$(read_value "$BATTERY/manufacturer")"

    if number "$current"; then
        current_desc="$(scaled "$current" 1000000 "A" 3)"
        if (( current < 0 )); then
            current_desc+=" (negative)"
        elif (( current > 0 )); then
            current_desc+=" (positive)"
        fi
    else
        current_desc="N/A"
    fi

    if number "$voltage" && number "$current"; then
        power_uw="$(awk -v v="$voltage" -v i="$current" \
            'BEGIN { printf "%.0f", v*i/1000000 }')"
        power_abs_uw="$(awk -v p="$power_uw" \
            'BEGIN { if (p<0) p=-p; printf "%.0f", p }')"
        power_desc="$(scaled "$power_abs_uw" 1000000 "W" 2)"
    else
        power_desc="N/A"
    fi

    clear 2>/dev/null || true
    printf '\n============================================================\n'
    printf '                 piBrick BATTERY REPORT\n'
    printf '============================================================\n'
    line "Time" "$(date '+%Y-%m-%d %H:%M:%S %Z')"
    line "Device" "$(hostname)"
    line "Kernel" "$(uname -r)"
    line "Fuel gauge" "${vendor} ${model}"

    section "BATTERY STATUS"
    line "Status" "$(pretty_status "$status")"
    if number "$soc"; then
        line "State of charge (SOC)" "${soc}%"
    else
        line "State of charge (SOC)" "N/A"
    fi
    line "Battery voltage" "$(scaled "$voltage" 1000000 "V" 3)"
    line "Battery current" "$current_desc"
    line "Power (calculated)" "$power_desc"

    section "BATTERY CAPACITY"
    line "Remaining charge" "$(scaled "$charge_now" 1000000 "Ah" 3)"
    line "Full charge capacity" "$(scaled "$charge_full" 1000000 "Ah" 3)"
    line "Remaining energy" "$(scaled "$energy_now" 1000000 "Wh" 2)"
    line "Full energy capacity" "$(scaled "$energy_full" 1000000 "Wh" 2)"
    if number "$temp"; then
        # Linux power_supply temperature is conventionally in tenths of °C.
        line "Battery temperature" "$(awk -v t="$temp" 'BEGIN { printf "%.1f °C", t/10 }')"
    else
        line "Battery temperature" "Not reported"
    fi
    line "Cycle count" "$(read_value "$BATTERY/cycle_count")"
    line "Capacity level" "$(read_value "$BATTERY/capacity_level")"

    section "CHARGER / POWER INPUT"
    local found_charger=0
    for dev in /sys/class/power_supply/*; do
        [[ -d "$dev" ]] || continue
        [[ "$dev" == "$BATTERY" ]] && continue
        local type name online current_max voltage_max
        type="$(read_value "$dev/type")"
        name="$(basename "$dev")"
        [[ "$type" == "Mains" || "$type" == "USB" || "$name" == *charger* ]] || continue
        found_charger=1
        printf '  %s\n' "$name"
        line "  Type" "$type"
        online="$(read_value "$dev/online")"
        [[ "$online" == "1" ]] && online="Connected" || \
            [[ "$online" == "0" ]] && online="Disconnected"
        line "  Input status" "$online"
        current_max="$(read_value "$dev/current_max")"
        voltage_max="$(read_value "$dev/voltage_max")"
        line "  Input current limit" "$(scaled "$current_max" 1000000 "A" 3)"
        line "  Input voltage limit" "$(scaled "$voltage_max" 1000000 "V" 2)"
    done
    (( found_charger )) || line "Charger supply" "No separate input supply reported"

    section "FUEL-GAUGE / DRIVER DETAILS"
    line "Battery sysfs path" "$BATTERY"
    line "Present" "$(read_value "$BATTERY/present")"
    line "Technology" "$(read_value "$BATTERY/technology")"
    line "Manufacturer" "$vendor"
    line "Model" "$model"

    printf '\n  All exported battery properties:\n'
    for file in "$BATTERY"/*; do
        [[ -f "$file" && -r "$file" ]] || continue
        printf '    %-25s %s\n' "$(basename "$file")" "$(read_value "$file")"
    done

    printf '\n  Charger power-supply properties:\n'
    for dev in /sys/class/power_supply/*; do
        [[ -d "$dev" ]] || continue
        [[ "$dev" == "$BATTERY" ]] && continue
        name="$(basename "$dev")"
        [[ "$name" == *charger* || "$(read_value "$dev/type")" == "Mains" || \
           "$(read_value "$dev/type")" == "USB" ]] || continue
        printf '\n    [%s]\n' "$name"
        for file in "$dev"/*; do
            [[ -f "$file" && -r "$file" ]] || continue
            printf '      %-23s %s\n' "$(basename "$file")" "$(read_value "$file")"
        done
    done

    section "DRIVER DETECTION LOG"
    if command -v dmesg >/dev/null 2>&1; then
        dmesg 2>/dev/null | grep -iE \
            'stc3115|max17048|battery provider|pibrick.*battery|bq2589' | tail -n 12 \
            || echo "  No matching messages (try running with sudo)."
    else
        echo "  dmesg command not available."
    fi

    printf '\n============================================================\n'
    printf 'Notes:\n'
    printf '  - Power is calculated from battery voltage x battery current.\n'
    printf '  - Current sign is displayed as the driver reports it; do not\n'
    printf '    assume positive/negative means charge/discharge without checking.\n'
    printf '  - Charger current properties are shown when exported by the driver.\n'
    printf '  - This measures battery-side power, including converter losses\n'
    printf '    and all loads connected to the battery.\n'
    printf '============================================================\n'
}

if (( WATCH )); then
    while true; do
        report
        sleep "$INTERVAL"
    done
else
    report
fi
