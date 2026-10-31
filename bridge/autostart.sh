#!/bin/sh
# 开机自启：./autostart.sh on | off，同 buddy autostart
exec "$(dirname "$0")/buddy" autostart "$@"
