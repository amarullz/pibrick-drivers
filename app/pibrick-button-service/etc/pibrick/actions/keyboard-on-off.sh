#!/bin/bash

VID="f10c"
PID="0001"

STATE_FILE="/run/pibrick-keyboard-disabled"

find_keyboard()
{
    for dev in /sys/bus/usb/devices/*; do
        [ -f "$dev/idVendor" ] || continue
        [ -f "$dev/idProduct" ] || continue

        [ "$(cat "$dev/idVendor")" = "$VID" ] || continue
        [ "$(cat "$dev/idProduct")" = "$PID" ] || continue

        echo "$dev"
        return 0
    done

    return 1
}

disable_keyboard()
{
    local dev

    dev=$(find_keyboard)

    if [ -z "$dev" ]; then
        echo "piBrick keyboard not found"
        return 1
    fi

    echo "Disabling piBrick keyboard: $dev"

    echo 0 > "$dev/authorized"

    touch "$STATE_FILE"
}

enable_keyboard()
{
    local dev

    dev=$(find_keyboard)

    if [ -z "$dev" ]; then
        echo "piBrick keyboard not found"
        rm -f "$STATE_FILE"
        return 1
    fi

    echo "Enabling piBrick keyboard: $dev"

    echo 1 > "$dev/authorized"

    rm -f "$STATE_FILE"
}

if [ -f "$STATE_FILE" ]; then
    enable_keyboard
else
    disable_keyboard
fi

