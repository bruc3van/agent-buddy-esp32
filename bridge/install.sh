#!/bin/sh
# 首次安装：等同项目根目录的 setup.sh（建 Python 环境、注册 hooks、检测板子、开机自启）
exec "$(dirname "$0")/../setup.sh" "$@"
