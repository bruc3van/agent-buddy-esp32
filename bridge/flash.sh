#!/bin/sh
# 编译当前代码并烧录（开发用，需要 arduino-cli）：同 buddy flash --build；只烧预编译固件用 buddy flash
exec "$(dirname "$0")/buddy" flash --build "$@"
