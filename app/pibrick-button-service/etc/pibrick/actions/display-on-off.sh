#!/bin/bash

# Get current display status
current_display_status=$(cat `find /sys -name pibrick_display_enable | grep dsi`)

if [ "$current_display_status" -eq 0 ]; then
    # If the display is currently off, turn it on
    echo 1 > `find /sys -name pibrick_display_enable | grep dsi`
else
    # If the display is currently on, turn it off
    echo 0 > `find /sys -name pibrick_display_enable | grep dsi`
fi

exit 0
