#!/bin/bash

get_current_count() {
    cat /sys/class/backlight/pibrick-backlight/brightness
}

current_count=$(get_current_count)
new_brightness=$((current_count + 8000))

if [ $new_brightness -gt 65534 ]; then
    new_brightness=8000
fi

echo $new_brightness | sudo tee /sys/class/backlight/pibrick-backlight/brightness
