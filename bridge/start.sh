#!/bin/sh
# 手动启动 bridge（后台运行，意外退出自动重启）：同 buddy start
exec "$(dirname "$0")/buddy" start "$@"
