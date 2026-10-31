#!/bin/sh
# Claude Code statusline：把会话 JSON（含 rate_limits 额度）发给 bridge，并显示 bridge 返回的一行文字
out=$(curl -s -m 1 -X POST -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:47823/usage 2>/dev/null)
[ -n "$out" ] && echo "$out" || echo "Claude（buddy bridge 未运行）"
