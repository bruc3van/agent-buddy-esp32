#!/bin/sh
# 停止 bridge（不影响开机自启的设置）：同 buddy stop
exec "$(dirname "$0")/buddy" stop "$@"
