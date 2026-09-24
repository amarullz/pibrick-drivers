#!/bin/bash
export XDG_RUNTIME_DIR=/run/user/1000

if [[ $(wlr-randr | grep "Enabled: no") ]]; then
    echo "Display is off turning on."
    wlr-randr --output DSI-2 --on
else
    echo "Display is on turning off."
    wlr-randr --output DSI-2 --off
fi
