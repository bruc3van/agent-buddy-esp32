#!/usr/bin/env python3
"""Claude Code / Codex hook（Codex 调用时带参数 codex）：把事件 JSON 原样转发给 Agent Buddy bridge，bridge 不在时静默忽略。
与 hook.sh 作用相同；Windows 上没有 sh，用这个。只用标准库。"""
import sys
import urllib.request

try:
    data = sys.stdin.buffer.read()
    agent = sys.argv[1] if len(sys.argv) > 1 else ""
    req = urllib.request.Request("http://127.0.0.1:47823/hook" + (f"?agent={agent}" if agent else ""), data=data,
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=2).read()
except Exception:  # noqa: BLE001 —— 任何情况都不能影响 Claude Code / Codex
    pass
sys.exit(0)
