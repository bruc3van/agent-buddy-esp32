#!/usr/bin/env python3
"""Claude Code statusline：把会话 JSON（含 rate_limits 额度）发给 bridge，并显示 bridge 返回的一行文字。
与 statusline.sh 作用相同；Windows 上没有 sh，用这个。只用标准库。"""
import sys
import urllib.request

out = ""
try:
    req = urllib.request.Request("http://127.0.0.1:47823/usage", data=sys.stdin.buffer.read(),
                                 headers={"Content-Type": "application/json"})
    out = urllib.request.urlopen(req, timeout=1).read().decode("utf-8", "replace").strip()
except Exception:  # noqa: BLE001
    pass
sys.stdout.buffer.write(((out or "Claude（buddy bridge 未运行）") + "\n").encode("utf-8"))
