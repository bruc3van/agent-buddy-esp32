"""宣传片素材：脚本驱动板子演示各页面，用 bridge 的录屏接口录下纯屏幕画面（960×544，无状态栏）。

用法：python3 promo/record_screens.py [片段名 ...]   （不带参数录全部）
输出：promo/screens/<片段名>.mp4
"""
import json
import os
import shutil
import sys
import threading
import time
import urllib.request

API = "http://127.0.0.1:47823"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens")


def post(path, body=None):
    data = json.dumps(body).encode() if body is not None else b""
    req = urllib.request.Request(API + path, data=data, method="POST")
    return urllib.request.urlopen(req, timeout=10).read().decode()


def get(path):
    return json.loads(urllib.request.urlopen(API + path, timeout=10).read())


def hook(agent, sid, name, **kw):
    post(f"/hook?agent={agent}", {"hook_event_name": name, "session_id": sid, **kw})


def ui(**kw):
    post("/ui?" + "&".join(f"{k}={v}" for k, v in kw.items()))


def bash(cmd):
    return {"tool_name": "Bash", "tool_input": {"command": cmd}}


def edit(path):
    return {"tool_name": "Edit", "tool_input": {"file_path": path}}


def claude_flow(agent="claude", sid="promo-claude", wait=True):
    t = [
        (0.5, "UserPromptSubmit", {"prompt": "给雷达页加上座位标定"}),
        (3.0, "PreToolUse", {"tool_name": "Read", "tool_input": {"file_path": "hw_radar.ino"}}),
        (5.0, "PreToolUse", edit("ui_radar.ino")),
        (8.0, "PreToolUse", bash("arduino-cli compile")),
    ]
    if wait:
        t.append((11.0, "PermissionRequest", bash("git push")))
        t.append((15.5, "Stop", {}))
    else:
        t.append((11.5, "Stop", {}))
    return [(at, agent, sid, n, kw) for at, n, kw in t]


def codex_flow():
    return [
        (0.5, "codex", "promo-codex", "UserPromptSubmit", {"prompt": "重构串口协议"}),
        (3.0, "codex", "promo-codex", "PreToolUse", {"tool_name": "exec_command", "tool_input": {"cmd": "rg serial"}}),
        (6.0, "codex", "promo-codex", "PreToolUse", {"tool_name": "apply_patch", "tool_input": {"command": "*** Update File: link.py"}}),
        (9.0, "codex", "promo-codex", "PermissionRequest", {"tool_name": "exec_command", "tool_input": {"cmd": "pytest"}}),
        (13.0, "codex", "promo-codex", "Stop", {}),
    ]


def deepseek_flow():
    return [
        (0.5, "deepseek", "promo-ds", "UserPromptSubmit", {"prompt": "写一个天气小组件"}),
        (3.0, "deepseek", "promo-ds", "PreToolUse", {"tool_name": "web_search", "tool_input": {"query": "天气 API"}}),
        (6.0, "deepseek", "promo-ds", "PreToolUse", {"tool_name": "write", "tool_input": {"file_path": "weather.py"}}),
        (9.0, "deepseek", "promo-ds", "PreToolUse", {"tool_name": "bash", "tool_input": {"command": "python weather.py"}}),
        (12.0, "deepseek", "promo-ds", "Stop", {}),
    ]


def split_flow():
    return [(at, a, s, n, kw) for at, a, s, n, kw in claude_flow(wait=False)] + [
        (1.5, "codex", "promo-codex", "UserPromptSubmit", {"prompt": "补单元测试"}),
        (4.0, "codex", "promo-codex", "PreToolUse", {"tool_name": "apply_patch", "tool_input": {"command": "*** Update File: test_link.py"}}),
        (7.5, "codex", "promo-codex", "PreToolUse", {"tool_name": "exec_command", "tool_input": {"cmd": "pytest -q"}}),
        (10.5, "codex", "promo-codex", "Stop", {}),
    ]


# 片段名: (录制秒数, 录前设置, 时间轴事件)
CLIPS = {
    "home_idle": (10, lambda: ui(screen=0), []),
    "home_claude": (16, lambda: ui(screen=0), claude_flow()),
    "home_split": (13, lambda: ui(screen=0), split_flow()),
    "buddy_claude": (16, lambda: ui(screen=1, agent=0), claude_flow()),
    "buddy_codex": (14, lambda: ui(screen=1, agent=1), codex_flow()),
    "buddy_deepseek": (13, lambda: ui(screen=1, agent=2), deepseek_flow()),
    "magpie_agent": (6, lambda: ui(screen=2, tab=0), []),
    "magpie_quota": (6, lambda: ui(screen=2, tab=1), []),
    "magpie_usage": (6, lambda: ui(screen=2, tab=2, period=0), []),
    "magpie_route": (9, lambda: (ui(screen=2, tab=3), post("/replay")), []),
    "radar": (14, lambda: ui(screen=5, rdemo=1), []),
    "standby": (10, lambda: ui(screen=0, standby=1), []),
    "welcome_back": (9, lambda: (ui(screen=0), post("/back")), []),
    "settings_sound": (6, lambda: ui(screen=3, set=0), []),
    "settings_pc": (6, lambda: ui(screen=3, set=1), []),
}


def run_timeline(events):
    t0 = time.time()
    for at, agent, sid, name, kw in sorted(events, key=lambda e: e[0]):
        time.sleep(max(0, t0 + at - time.time()))
        hook(agent, sid, name, **kw)


def record(name):
    secs, setup, events = CLIPS[name]
    post("/reset")
    setup()
    setup()  # 再发一次：刚切页时 tab 偶尔没生效
    time.sleep(2.5)  # 等页面切换动画结束
    st = json.loads(post(f"/record?seconds={secs}"))
    if st.get("error"):
        raise RuntimeError(st["error"])
    th = threading.Thread(target=run_timeline, args=(events,))
    th.start()
    th.join()
    while True:
        time.sleep(1)
        st = get("/record")
        if st.get("status") in ("done", "error"):
            break
    if st["status"] != "done":
        raise RuntimeError(st)
    shutil.copy(st["mp4"], os.path.join(OUT, name + ".mp4"))
    for k in ("mp4", "gif"):  # 录屏目录里不留这批素材
        os.remove(st[k])
    print(name, st["frames"], "帧", st["device_fps"], "fps", flush=True)
    for sid in ("promo-claude", "promo-codex", "promo-ds"):
        hook("claude", sid, "SessionEnd")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for n in sys.argv[1:] or CLIPS:
        record(n)
    post("/reset")
    ui(screen=0)
