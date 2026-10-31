#!/bin/sh
# Claude Code / Codex hook（Codex 调用时带参数 codex）：把事件 JSON 原样转发给 Agent Buddy bridge，bridge 不在时静默忽略
curl -s -m 2 -X POST -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:47823/hook${1:+?agent=$1} >/dev/null 2>&1
exit 0
