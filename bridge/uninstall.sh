#!/bin/sh
# 卸载：停止、关闭自启、移除 Claude Code / Codex 的 hooks 与 statusline：同 buddy uninstall
exec "$(dirname "$0")/buddy" uninstall "$@"
