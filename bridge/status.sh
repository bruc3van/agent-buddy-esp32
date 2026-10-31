#!/bin/sh
# 查看状态：进程、串口、板子固件、hooks、magpie：同 buddy status
exec "$(dirname "$0")/buddy" status "$@"
