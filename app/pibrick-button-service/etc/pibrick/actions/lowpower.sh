#!/bin/bash

STATE_FILE="/run/pibrick-lowpower"
CPUFREQ="/sys/devices/system/cpu/cpufreq"

LOWPOWER_FREQ=800000

enter_lowpower()
{
    echo "Entering piBrick low-power mode"

    for policy in "$CPUFREQ"/policy*; do
        [ -d "$policy" ] || continue

        id="${policy##*/}"

        # Save current settings.
        cat "$policy/scaling_governor" > "/run/pibrick-lowpower-${id}-governor"
        cat "$policy/scaling_min_freq" > "/run/pibrick-lowpower-${id}-min"
        cat "$policy/scaling_max_freq" > "/run/pibrick-lowpower-${id}-max"

        # Limit CPU to minimum frequency.
        echo "$LOWPOWER_FREQ" > "$policy/scaling_min_freq"
        echo "$LOWPOWER_FREQ" > "$policy/scaling_max_freq"
        echo "powersave" > "$policy/scaling_governor"
    done

    touch "$STATE_FILE"
}

exit_lowpower()
{
    echo "Leaving piBrick low-power mode"

    for policy in "$CPUFREQ"/policy*; do
        [ -d "$policy" ] || continue

        id="${policy##*/}"

        governor_file="/run/pibrick-lowpower-${id}-governor"
        min_file="/run/pibrick-lowpower-${id}-min"
        max_file="/run/pibrick-lowpower-${id}-max"

        [ -f "$min_file" ] &&
            cat "$min_file" > "$policy/scaling_min_freq"

        [ -f "$max_file" ] &&
            cat "$max_file" > "$policy/scaling_max_freq"

        [ -f "$governor_file" ] &&
            cat "$governor_file" > "$policy/scaling_governor"

        rm -f \
            "$governor_file" \
            "$min_file" \
            "$max_file"
    done

    rm -f "$STATE_FILE"
}

if [ -f "$STATE_FILE" ]; then
    exit_lowpower
else
    enter_lowpower
fi

