#!/usr/bin/env python3
"""Agent Buddy bridge：把 Claude Code / Codex / DeepSeek Harness 的 hooks、statusline 数据转发到 ESP32-S3 屏幕。

- HTTP 127.0.0.1:47823
    POST /hook    —— hook 事件 JSON（hook.sh 转发）
    POST /usage   —— statusline JSON（statusline.sh 转发），返回一行状态栏文本
    GET  /status  —— 调试用，查看当前汇总状态
- USB 串口：自动寻找 Espressif 设备（VID 0x303A），断线自动重连，每行一个 JSON
- 切换页面：POST /ui?screen=0|1|2&tab=0..3&period=0..3（首页 / Buddy / magpie）
- 屏幕校色：POST /gamma?g=1.6（>1 加深，1 关闭）
- DeepSeek：dsh 的 hooks-claude-code 插件转发事件（?agent=deepseek）；余额和今天的用量从 magpie 读，没有 magpie 时用 API Key 查余额
- magpie：定时读 magpie 本地网关（127.0.0.1:3425/v1/magpie/*）和 `magpie ls`，整理成额度 / 用量 / 路由 / Agent 页面数据下发
- 录屏：POST /record?seconds=N 录 N 秒（输出 MP4 + GIF），POST /snap 截一张 PNG，GET /record 查进度
"""

import base64
import datetime as dt
import glob
import hashlib
import json
import math
import os
import queue
import re
import secrets
import select
import socket
import shutil
import sqlite3
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unicodedata
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.request
from urllib.parse import parse_qs, urlparse

import numpy as np
import serial
from PIL import Image
from serial.tools import list_ports

import host

try:  # Wi-Fi 链路的认证 / 加密（esptool 已经带了 cryptography；缺了就只走 USB）
    from cryptography.hazmat.primitives import hashes, hmac as chmac
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
except ImportError:  # pragma: no cover
    AESGCM = None

PORT = 47823
ESPRESSIF_VID = 0x303A
REC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "recordings")
SCREEN_W, SCREEN_H = 480, 272
FRAME_MAGIC = b"\xa5\x5aFR"
OUT_FPS = 25
FFMPEG = shutil.which("ffmpeg") or ("/opt/homebrew/bin/ffmpeg" if host.IS_MAC else "ffmpeg")
STATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "state")
USAGE_FILE = os.path.join(STATE_DIR, "usage.json")
LOG_FILE = os.path.join(STATE_DIR, "bridge.log")  # buddy.py 启动时传 --log，launchd 也会把 stdout 追加到这里
LOCK_FILE = os.path.join(STATE_DIR, "bridge.lock")
DEVICE_FILE = os.path.join(STATE_DIR, "device.json")  # Wi-Fi 配对：板子 id、本机 host_id、共享密钥（只给自己看）
NET_PORT = 47824
PID_FILE = os.path.join(STATE_DIR, "bridge.pid")
LOG_MAX = 5 * 1024 * 1024
MAX_BODY = 32 * 1024 * 1024  # hook 的 tool_input 可能带整份文件内容

DONE_HOLD = 5          # 完成动画保持秒数
ERROR_HOLD = 6
STALE_ACTIVE = 15 * 60  # 活动状态超过这么久没新事件，视为已经停了
STALE_WAITING = 2 * 3600  # 等你确认：人走开了也一直在等，单独给长一点
SESSION_TTL = 3 * 3600  # 会话超过这么久没事件就移除
SLEEP_AFTER = 10 * 60   # 全部空闲多久后进入睡觉动画
DETAIL_WIDTH = 32       # 细节文字最大显示宽度（中文算 2）

PRIORITY = {"waiting": 6, "working": 5, "thinking": 4, "compacting": 4, "error": 3, "done": 2, "idle": 1}

lock = threading.Lock()
sessions = {}  # session_id -> dict(state, label, detail, tool, ts)
usage = {}     # h5, h5r, d7, d7r, model
magpie_usage_at = 0  # 最近一次从 magpie 拿到 Claude 额度的时间；5 分钟内以它为准，忽略 statusline
last_event_ts = {"claude": time.time(), "codex": time.time(), "deepseek": time.time()}  # 每个 Agent 最近一次事件
AGENT_NAME = {"claude": "Claude", "codex": "Codex", "deepseek": "DeepSeek"}
SFX_PREFIX = {"codex": "c", "deepseek": "d"}  # 板子上每个 Agent 一套音色：音效名前加这个前缀
done_n = {"claude": 0, "codex": 0, "deepseek": 0}  # bridge 启动以来每个 Agent 收到几次 Stop；板子按增量记「今天完成」，汇总状态会把并发完成合并掉


_log_lock = threading.Lock()


def log(*args):
    with _log_lock:
        print(time.strftime("%H:%M:%S"), *args, flush=True)
        _rotate_log()


def _rotate_log():
    """stdout 是 state/bridge.log（追加方式打开）时：超过 5MB 复制一份 .1，再原地清空。launchd 模式也能轮转。"""
    try:
        st = os.fstat(sys.stdout.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_size < LOG_MAX or not os.path.samestat(st, os.stat(LOG_FILE)):
            return
        shutil.copyfile(LOG_FILE, LOG_FILE + ".1")
        os.chmod(LOG_FILE + ".1", 0o600)
        os.truncate(LOG_FILE, 0)
    except (OSError, ValueError, AttributeError):  # pythonw 下没有 stdout
        pass


def write_atomic(path, text):
    """先写临时文件再改名：多个线程同时写、或写到一半被杀，都不会留下半截文件。"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.{threading.get_ident()}.tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def forever(fn, *args):
    """后台线程守护：循环里意外抛异常时记日志、2 秒后重新进入，不让某个功能悄悄停掉；正常返回则不再重启。"""
    def run():
        while True:
            try:
                fn(*args)
                return
            except Exception as e:  # noqa: BLE001
                log(f"{fn.__name__} 线程出错，2 秒后重启：{e!r}"[:300])
                log("  " + " | ".join(traceback.format_exc().strip().splitlines()[-4:-1])[:400])
                time.sleep(2)
    threading.Thread(target=run, name=fn.__name__, daemon=True).start()


# 屏幕和日志上的细节文字（prompt、命令行）先去掉明显的密钥
SECRET_RES = [
    (re.compile(r"(?i)\b(bearer|basic|token)(\s+)[\w.~+/=-]{8,}"), r"\1\2***"),
    (re.compile(r"(?i)([\w-]*(?:token|secret|passw(?:or)?d|pwd|api[_-]?key|access[_-]?key|private[_-]?key|auth)[\w-]*"
                r"[\"']?\s*[=:]\s*[\"']?)[^\s\"'&,;]+"), r"\1***"),
    (re.compile(r"\b(sk-|sk_|rk_|ghp_|gho_|ghs_|github_pat_|glpat-|xox[abps]-|AKIA|AIza)[\w-]{6,}"), r"\1***"),
    (re.compile(r"(://[^/\s:@]+:)[^@\s/]+@"), r"\1***@"),  # URL 里的 user:password@
]


def redact(text):
    text = str(text or "")
    for rx, rep in SECRET_RES:
        text = rx.sub(rep, text)
    return scrub(text) if mask_enabled() else text


def truncate(text, width=DETAIL_WIDTH):
    text = " ".join(str(text or "").split())
    out, w = "", 0
    for ch in text:
        cw = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        if w + cw > width - 1:
            return out + "…"
        out += ch
        w += cw
    return out


def tool_label(tool, tin):
    """根据工具名和参数生成 (状态文字, 细节)。"""
    tin = tin if isinstance(tin, dict) else {}  # hook 载荷形状不对时当成没有参数，不让整条事件丢掉
    base = lambda p: os.path.basename(str(p or "").rstrip("/"))
    if tool == "apply_patch":  # Codex 改文件：补丁里的第一个文件名
        m = re.search(r"\*\*\* (?:Update|Add|Delete) File: (.+)", str(tin.get("command") or tin.get("input") or ""))
        return "编写代码", base(m.group(1)) if m else ""
    if tool in ("Bash", "BashOutput", "KillShell", "exec_command", "shell"):
        c = tin.get("command") or tin.get("cmd") or tin.get("description") or ""  # Codex exec_command 用 cmd
        if isinstance(c, list):  # Codex shell：["bash", "-lc", "命令"]
            c = c[-1] if c else ""
        cmd = str(c).strip().splitlines()
        return "运行命令", cmd[0] if cmd else ""
    if tool in ("Edit", "MultiEdit", "Write", "NotebookEdit"):
        return "编写代码", base(tin.get("file_path") or tin.get("notebook_path"))
    if tool == "Read":
        return "阅读文件", base(tin.get("file_path"))
    if tool in ("Grep", "Glob", "LS"):
        return "搜索代码", tin.get("pattern") or tin.get("path") or ""
    if tool == "WebFetch":
        return "浏览网页", urlparse(str(tin.get("url", ""))).netloc
    if tool == "WebSearch":
        return "联网搜索", tin.get("query", "")
    if tool in ("Agent", "Task"):
        return "派出分身", tin.get("description", "")
    if tool == "TodoWrite":
        return "整理计划", ""
    if tool == "AskUserQuestion":
        return "有问题问你", ""
    if tool == "Skill":
        return "使用技能", tin.get("skill", "")
    if tool.startswith("mcp__Claude_Browser") or tool.startswith("mcp__claude-in-chrome"):
        return "操作网页", tool.split("__")[-1]
    if tool.startswith("mcp__"):
        return "调用工具", tool.split("__")[-1]
    return "调用工具", tool


# ---------------- 声音：8-bit 音效 + 语音播报 ----------------
audio_cfg = {"voice": False, "sfx": True, "mute": False, "dsec": 20,  # 默认只响提示音；以板子设置页为准（串口 "CFG {...}"）
             "greet": True, "mmute": True, "talk": False}
say_q = queue.Queue()
SAY_STALE = 30  # 排队超过这么久还没轮到的播报就不念了（念出来也过时了）
link_ref = None
alock = threading.Lock()  # 保护下面这些提醒用的表（HTTP 线程和 pump 线程都会改）
turn_start, waiting_since, alert_at, alert_n = {}, {}, {}, {}
quota_said = set()


def forget(sid):
    """会话结束 / 过期：清掉它的提醒记录。"""
    with alock:
        for d in (turn_start, waiting_since, alert_at, alert_n):
            d.pop(sid, None)


def _sfx(name):
    if audio_cfg["sfx"] and not audio_cfg["mute"] and link_ref:
        link_ref.send({"t": "sfx", "n": name})


def say(text):
    if audio_cfg["voice"] and not audio_cfg["mute"] and not (audio_cfg["mmute"] and (audio_cfg["talk"] or act["mic"])):
        say_q.put((time.time(), text))


def say_worker():
    """排队生成语音（16kHz / 16bit / 单声道），分块经串口发给板子；等一句播完再说下一句。"""
    while True:
        t0, text = say_q.get()
        if time.time() - t0 > SAY_STALE:
            log("say skipped (stale)", text)
            continue
        try:
            with tempfile.TemporaryDirectory() as d:
                path = os.path.join(d, "s.wav")
                host.tts_wav(text, path)  # macOS 用 say，Windows 用系统自带的 SAPI
                with wave.open(path) as w:
                    pcm = w.readframes(w.getnframes())
            for i in range(0, len(pcm), 3000):
                link_ref.send({"t": "au", "d": base64.b64encode(pcm[i:i + 3000]).decode()})
                time.sleep(0.02)
            log("say", text)
            time.sleep(len(pcm) / 32000 + 0.3)
        except Exception as e:  # noqa: BLE001 —— 播报失败不影响其他功能
            log("say failed:", repr(e)[:120])


def spoken_duration(sec):
    sec = int(sec)
    if sec < 60:
        return f"{sec}秒"
    if sec < 3600:
        return f"{sec // 60}分" + (f"{sec % 60}秒" if sec % 60 else "钟")
    return f"{sec // 3600}小时{sec % 3600 // 60}分"


def announce(name, sid, ev, agent="claude"):
    """Claude Code / Codex / DeepSeek 事件 → 音效与语音。Codex、DeepSeek 的音效在板子上各换一种音色（名字前加 c / d）。"""
    with alock:
        _announce(name, sid, ev, agent)


def _announce(name, sid, ev, agent):
    now = time.time()
    who = AGENT_NAME.get(agent, "Claude")
    sfx = (lambda n: _sfx(SFX_PREFIX.get(agent, "") + n))
    if name == "UserPromptSubmit":
        turn_start[sid] = now
        sfx("start")
    elif name == "PermissionRequest" or (name == "Notification" and ev.get("notification_type") == "permission_prompt"):
        if now - alert_at.get(sid, 0) < 15:  # PermissionRequest 之后紧跟的 Notification 不重复
            return
        alert_at[sid], alert_n[sid], waiting_since[sid] = now, 0, now
        label = tool_label(ev.get("tool_name") or "", ev.get("tool_input"))[0] if ev.get("tool_name") else ""
        sfx("alert")
        say(f"{who} 需要你确认" + (f"：{label}" if label else ""))
    elif name in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
        waiting_since.pop(sid, None)  # 已经批准，继续干活了
    elif name == "Stop":
        waiting_since.pop(sid, None)
        dur = now - turn_start.pop(sid, now)
        sfx("done")
        done_times.append((now, agent))
        del done_times[:-50]
        dsec = audio_cfg["dsec"]
        if dsec and dur >= dsec:  # 短任务只响音效，门槛在板子设置页选
            say(f"{who} 任务完成，用时{spoken_duration(dur)}")
    elif name == "StopFailure":
        waiting_since.pop(sid, None)
        sfx("error")
        say(f"{who} 出错了，请看一下屏幕")


def audio_tick():
    """每秒：等确认的提醒（每 30 秒叮咚一次，最多 3 次）；Claude 5 小时额度降到 20% / 10% 时播报。"""
    now = time.time()
    with lock:
        waiting = {sid: s.get("agent") for sid, s in sessions.items() if s["state"] == "waiting"}
        h5 = usage.get("h5")
    with alock:
        for sid in list(waiting_since):
            if sid not in waiting:
                waiting_since.pop(sid, None)
                continue
            if now - alert_at.get(sid, 0) > 30 and alert_n.get(sid, 0) < 3:
                alert_at[sid] = now
                alert_n[sid] = alert_n.get(sid, 0) + 1
                _sfx(SFX_PREFIX.get(waiting[sid], "") + "alert")
    if h5 is None:
        return
    left = 100 - h5
    if left > 30:
        quota_said.clear()
    for th in (20, 10):
        if left <= th and th not in quota_said:
            quota_said.add(th)
            say(f"Claude 五小时额度只剩百分之{int(left)}")
            break


# ---------------- 你在电脑前做什么：键鼠空闲时间 + 麦克风是否被占用 ----------------
# 只读「多久没按键」「按了几次」，不读按了什么（见 host.py）
AWAY_SEC = 300
act = {"key": -1.0, "inp": -1.0, "mic": False}
done_times = []
def away_words(sec):
    m = int(sec) // 60
    return f"{m}分钟" if m < 60 else f"{m // 60}小时" + (f"{m % 60}分" if m % 60 else "")


def key_loop(link):
    """真实按键节奏：系统的累计按键计数（只有次数，没有按了什么），每 0.1 秒看一次，
    有新按键就发 {"t":"key","n":增量}，板子据此让 Clawd 一键一下地跟着敲。"""
    last = host.key_count()
    if last is None:
        return
    while True:
        time.sleep(0.1)
        c = host.key_count()
        if c != last:
            n = (c - last) & 0xFFFFFFFF
            last = c
            if 0 < n < 100:
                link.send({"t": "key", "n": n})


radar = {"p": None, "n": 0, "pv": 0, "seat": 1, "ts": 0.0}  # 板子雷达报来的：在不在座位、几个人、有没有人靠近
RADAR_AWAY_SEC = 180  # 有雷达时：座位没人 3 分钟以上再回来才打招呼


def radar_ok():
    return radar["p"] is not None and radar["seat"] and time.time() - radar["ts"] < 25


def act_loop(link):
    """每秒把键鼠空闲时间发给板子；离开再回来时打招呼。
    有雷达时以「座位上有没有人」为准（看视频不碰键盘也不算离开），没有雷达时看键鼠 5 分钟没动。"""
    away_max = 0.0
    last_mic = None
    away_since = None
    seen = False
    while True:
        time.sleep(1)
        idle = host.idle()
        if idle is None:
            if not seen:  # 这台电脑拿不到键鼠空闲时间：不跑这个功能
                return
            continue  # 拿到过、只是这一次失败（如 Windows 锁屏瞬间）：下一秒再试
        seen = True
        key, inp = idle
        mic = host.mic_in_use()
        act.update(key=key, inp=inp, mic=mic)
        if mic != last_mic:
            if last_mic is not None:
                log("电脑麦克风", "被占用（会议 / 语音）" if mic else "已释放")
            last_mic = mic
        link.send({"t": "act", "k": round(min(key, 99999), 1), "i": int(min(inp, 99999)), "m": int(mic)})
        if radar_ok():
            if not radar["p"]:
                away_since = away_since or time.time()
                if time.time() - away_since >= RADAR_AWAY_SEC:
                    away_max = time.time() - away_since + 10  # 板子报座位没人前有 8 秒宽限，加上雷达目标保留 1.5 秒
                back = False
            else:
                back = bool(away_max)
                away_since = None
        else:
            away_since = None
            if inp >= AWAY_SEC:
                away_max = max(away_max, inp)
            back = bool(away_max) and inp < 5
        if back:  # 回来了
            now = time.time()
            with alock:
                done = list(done_times)
            n_done = sum(1 for t, _ in done if t > now - away_max)
            n_cx = sum(1 for t, a in done if t > now - away_max and a == "codex")
            n_ds = sum(1 for t, a in done if t > now - away_max and a == "deepseek")
            n_cl = n_done - n_cx - n_ds
            with lock:  # 与屏幕一致：按时间衰减后的状态算
                n_wait = sum(1 for x in sessions.values() if effective(x, now) == "waiting")
            link.send({"t": "back", "a": int(away_max), "d": n_cl, "c": n_cx, "s": n_ds, "w": n_wait})
            log(f"欢迎回来：离开 {int(away_max)} 秒，完成 {n_done}，等确认 {n_wait}")
            if audio_cfg["greet"]:
                msg = f"欢迎回来，你离开了{away_words(away_max)}"
                if n_cl:
                    msg += f"，Claude 完成了{n_cl}个任务"
                if n_cx:
                    msg += f"，Codex 完成了{n_cx}个任务"
                if n_ds:
                    msg += f"，DeepSeek 完成了{n_ds}个任务"
                if n_wait:
                    msg += f"，有{n_wait}个在等你确认"
                say(msg)
            away_max = 0.0


# DeepSeek Harness（dsh）的工具名 → Claude Code 的同类工具，状态文字和屏幕上的工具图标照 Claude Code 的来
DSH_TOOLS = {"bash": "Bash", "pwsh": "Bash", "read": "Read", "write": "Write", "edit": "Edit", "str_replace_editor": "Edit",
             "grep": "Grep", "glob": "Glob", "web_fetch": "WebFetch", "web_search": "WebSearch", "todo_write": "TodoWrite",
             "subagent": "Agent", "ask_user_question": "AskUserQuestion"}
# DeepSeek 的状态文字照 dsh 桌面版对话里的步骤提示（packages/client/ui-chat 的 message.stepProcess.*）
DSH_STEP = {"Read": "正在读取文件", "read_image": "正在读取图片", "Write": "正在写入文件", "Edit": "正在编辑文件",
            "Grep": "正在搜索代码", "Glob": "正在搜索代码", "Bash": "正在运行命令", "run_code": "正在运行代码",
            "WebSearch": "正在搜索网页", "WebFetch": "正在访问网页", "Agent": "正在协调子智能体", "TodoWrite": "正在更新计划"}


def handle_hook(ev, agent="claude"):
    name = ev.get("hook_event_name", "")
    sid = ev.get("session_id") or "default"
    now = time.time()
    tool = ev.get("tool_name", "") or ""
    if agent == "deepseek":
        tool = DSH_TOOLS.get(tool, tool)
        if name == "PreToolUse" and tool == "AskUserQuestion":  # dsh 的 hooks 没有 PermissionRequest：它问你问题时当成「等你确认」
            name = "PermissionRequest"
            ev = {**ev, "tool_name": tool}
    rec = None
    with lock:
        prev = sessions.get(sid)
    # 是否在一轮对话里：空闲时后台压缩上下文也会发 PreCompact / SubagentStop / PostCompact，
    # 之后没有 Stop 收尾，不能把它们当成「思考中」，否则要等 STALE_ACTIVE 才回到待命
    busy = bool(prev and prev.get("busy"))

    if name == "SessionStart":
        if ev.get("source") == "compact" and busy:
            return  # 一轮对话中途压缩完重开，状态照旧
        rec = ("idle", "待命中", "新会话已开始", "")
        busy = False
    elif name == "UserPromptSubmit":
        rec = ("thinking", "思考中", ev.get("prompt", ""), "")
        busy = True
    elif name == "PreToolUse":
        label, detail = tool_label(tool, ev.get("tool_input"))
        if agent == "deepseek":
            label = DSH_STEP.get(tool, "正在调用工具")
        rec = ("working", label, detail, tool)
        busy = True
    elif name in ("PostToolUse", "PostToolBatch", "PostCompact", "SubagentStop"):
        rec = ("thinking", "思考中", "", "") if busy else ("idle", "待命中", "", "")
    elif name == "PostToolUseFailure":
        rec = ("thinking", "思考中", f"{tool} 失败，想想办法", "")
    elif name == "PermissionRequest":
        label, detail = tool_label(tool, ev.get("tool_input"))
        rec = ("waiting", "等你确认", f"{label}：{detail}" if detail else label, tool)
        if agent == "deepseek":  # 只有它向你提问时才会走到这里，文字同 dsh 的「等待你的操作」
            rec = ("waiting", "等待你的操作", "有问题要问你", tool)
        busy = True
    elif name == "Notification":
        ntype = ev.get("notification_type", "")
        msg = ev.get("message", "")
        if ntype in ("permission_prompt", "elicitation_dialog", "agent_needs_input") or "permission" in msg.lower():
            rec = ("waiting", "等你确认", msg, "")
        elif ntype == "idle_prompt":
            rec = ("idle", "待命中", msg, "")
            busy = False
    elif name == "Stop":
        rec = ("done", "已完成" if agent == "deepseek" else "完成啦", "", "")
        busy = False
    elif name == "StopFailure":
        rec = ("error", "出错了", ev.get("error", "") or "API 错误", "")
        busy = False
    elif name == "PreCompact":
        rec = ("compacting", "整理记忆", "压缩上下文中", "")
    elif name == "SessionEnd":
        with lock:
            sessions.pop(sid, None)
            last_event_ts[agent] = now
        forget(sid)
        log("session end", sid[:8])
        return

    try:
        announce(name, sid, ev, agent)
    except Exception as e:  # noqa: BLE001
        log("announce failed:", repr(e)[:120])
    if rec is None:
        return
    state, label, detail, tool = rec
    detail = truncate(redact(detail))
    with lock:
        sessions[sid] = {"state": state, "label": label, "detail": detail, "tool": tool, "ts": now, "agent": agent, "busy": busy}
        last_event_ts[agent] = now
        if state == "done":
            done_n[agent] = done_n.get(agent, 0) + 1
    log(f"{name:<18} {agent[:2]} {sid[:8]} -> {state} {label} {detail}")


def effective(s, now):
    """考虑时间衰减后的会话状态。"""
    age = now - s["ts"]
    st = s["state"]
    if st == "done" and age > DONE_HOLD:
        return "idle"
    if st == "error" and age > ERROR_HOLD:
        return "idle"
    if st == "waiting" and age > STALE_WAITING:
        return "idle"
    if st in ("thinking", "working", "compacting") and age > STALE_ACTIVE:
        return "idle"
    return st


def aggregate(agent="claude"):
    now = time.time()
    with lock:
        gone = [k for k, v in sessions.items() if now - v["ts"] > SESSION_TTL]
        for sid in gone:
            sessions.pop(sid)
        mine = [s for s in sessions.values() if s.get("agent", "claude") == agent]
        best, best_state = None, "idle"
        for s in mine:
            st = effective(s, now)
            if best is None or (PRIORITY[st], s["ts"]) > (PRIORITY[best_state], best["ts"]):
                best, best_state = s, st
        active = sum(1 for s in mine if now - s["ts"] < 30 * 60)
        idle_for = now - last_event_ts[agent]
        dn = done_n.get(agent, 0)
    for sid in gone:
        forget(sid)

    if best_state == "idle":
        if idle_for > SLEEP_AFTER:
            return {"state": "sleep", "label": "休息中", "detail": f"已空闲 {int(idle_for // 60)} 分钟", "tool": "", "sessions": active,
                    "dn": dn}
        label = best["label"] if best and best["state"] == "idle" else "待命中"
        detail = best["detail"] if best and best["state"] == "idle" else "随时可以开始"
        return {"state": "idle", "label": label, "detail": detail, "tool": "", "sessions": active, "dn": dn}
    return {"state": best_state, "label": best["label"], "detail": best["detail"], "tool": best["tool"], "sessions": active,
            "dn": dn}


def handle_usage(data):
    rl = data.get("rate_limits") or {}
    if time.time() - magpie_usage_at < 300:
        rl = {}  # magpie 的数据更及时（直接读订阅），statusline 只在终端会话发请求时才更新
    changed = False
    with lock:
        for key, short in (("five_hour", "h5"), ("seven_day", "d7")):
            w = rl.get(key)
            if w and w.get("used_percentage") is not None:
                usage[short] = float(w["used_percentage"])
                usage[short + "r"] = int(w.get("resets_at") or 0)
                changed = True
        model = (data.get("model") or {}).get("display_name")
        if model:
            usage["model"] = model
        snapshot = dict(usage)
    if changed:
        body = json.dumps(snapshot, sort_keys=True)
        if body != _usage_saved[0]:
            try:
                write_atomic(USAGE_FILE, body)
                _usage_saved[0] = body
            except OSError as e:
                log("usage save failed:", e)
    return statusline_text(snapshot)


_usage_saved = [""]


def statusline_text(u):
    parts = [u.get("model") or "Claude"]
    if "h5" in u:
        parts.append(f"5h 剩{100 - u['h5']:.0f}%")
    if "d7" in u:
        parts.append(f"7d 剩{100 - u['d7']:.0f}%")
    return " · ".join(parts)


def load_usage():
    try:
        with open(USAGE_FILE) as f:
            usage.update(json.load(f))
    except (OSError, ValueError):
        pass


# ---------------- 串口 ----------------
class Link:
    """多个线程共用一个串口：打开、写入、断开都在 wlock 里做，写失败的线程负责关掉，别的线程不会拿到半关的端口。"""

    def __init__(self):
        self.ser = None
        self.wlock = threading.RLock()

    def find_port(self):
        if host.env("PORT"):  # 同时插着几块 ESP32 时手动指定
            dev = host.env("PORT")
            return dev if os.path.exists(dev) else None
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID:
                return p.device.replace("/dev/tty.", "/dev/cu.")
        return None

    def ensure(self):
        with self.wlock:
            return self._open()

    def drop(self, s, why):
        """关掉出错的端口（只关仍是当前端口的那个），下一次 ensure 再重连。"""
        with self.wlock:
            if self.ser is not s or s is None:
                return
            log("serial lost", why)
            try:
                s.close()
            except Exception:  # noqa: BLE001
                pass
            self.ser = None

    def _open(self):
        if self.ser:
            return True
        dev = self.find_port()
        if not dev:
            return False
        try:
            s = serial.Serial()
            s.port = dev
            s.baudrate = 115200
            s.timeout = 0
            s.write_timeout = 1
            s.dtr = False  # 不拉 DTR/RTS，避免打开串口时把板子复位
            s.rts = False
            s.open()
            self.ser = s
            log("serial connected", dev)
            return True
        except (serial.SerialException, OSError, ValueError) as e:
            log("serial open failed", e)
            return False

    def send(self, obj):
        line = encode_line(obj)
        if line is None:
            return False
        with self.wlock:
            if not self._open():
                return False
            s = self.ser
            try:
                s.write(line)
                return True
            except Exception as e:  # noqa: BLE001 —— 任何写失败都只断开重连，不能把调用方的线程带崩
                self.drop(s, e)
                return False


def encode_line(obj):
    obj = {**obj, "now": int(time.time()),
           "tz": time.localtime().tm_gmtoff}  # 本地时区偏移（秒），屏幕首页的时钟用
    try:  # NaN / inf 会生成固件解析不了的 JSON
        return (json.dumps(obj, ensure_ascii=False, allow_nan=False) + "\n").encode()
    except (TypeError, ValueError) as e:
        log("send skipped:", e, str(obj)[:80])
        return None


# ---------------- Wi-Fi 链路（docs/无线通讯方案.md）----------------
# 两个方向：listen（macOS 默认）板子主动连过来，接受入站连接不需要「本地网络」授权，launchd 自启也稳定；
#          dial（Windows 默认）bridge 去连板子的 47824，出站不过防火墙。先试上次的 IP，再 agent-buddy-<id>.local，再 UDP 广播探测。
# 环境变量 AGENT_BUDDY_NET=listen|dial 或 device.json 里的 "mode" 可以改。
# 握手：B「BUDDY/2 板子id nB」→ H「HOST host_id nH macH」→ B「OK macB」，之后每帧 AES-256-GCM（格式见 net_link.ino）。
# 板子同时连着几台电脑时只有一台是控制者：板子发 {"t":"role","r":"active|standby"}，待命时只收发心跳。
def load_device():
    try:
        with open(DEVICE_FILE, encoding="utf-8") as f:
            d = json.load(f)
        d["k"] = base64.b64decode(d["key"])
        return d if len(d["k"]) == 32 else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save_device(d):
    os.makedirs(STATE_DIR, exist_ok=True)
    write_atomic(DEVICE_FILE, json.dumps({k: v for k, v in d.items() if k != "k"}, indent=2))
    try:
        os.chmod(DEVICE_FILE, 0o600)
    except OSError:
        pass


def net_mode(dev=None):
    m = host.env("NET") or (dev or load_device() or {}).get("mode") or "auto"
    if m not in ("listen", "dial"):
        m = "dial" if host.IS_WIN else "listen"
    return m


def hmac_hex(k, msg):
    h = chmac.HMAC(k, hashes.SHA256())
    h.update(msg.encode())
    return h.finalize().hex()


def hkdf32(k, salt, info):
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=salt, info=info).derive(k)


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        b = s.recv(n - len(buf))
        if not b:
            raise ConnectionError("closed")
        buf += b
    return bytes(buf)


def recv_line(s, limit=256):
    buf = bytearray()
    while True:
        b = s.recv(1)
        if not b:
            raise ConnectionError("closed")
        if b == b"\n":
            return buf.decode(errors="replace")
        buf += b
        if len(buf) > limit:
            raise ValueError("line too long")


class NetLink:
    """Wi-Fi 会话：同一时间只保留一条（新的认证通过就顶掉旧的）。"""

    def __init__(self):
        self.lock = threading.RLock()
        self.sock = None
        self.peer = None
        self.tx = None
        self.txn = 0
        self.active = True  # 板子说的角色；旧固件不发 role，当作控制者

    def serve(self):
        if AESGCM is None:
            log("wifi: 缺少 cryptography 包，只走 USB")
            return
        threading.Thread(target=self.heartbeat, daemon=True).start()
        if net_mode() == "dial":
            self.dial_loop()
            return
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", NET_PORT))
        srv.listen(4)
        log(f"wifi: listening on :{NET_PORT}")
        while True:
            s, addr = srv.accept()
            threading.Thread(target=self.session, args=(s, addr[0]), daemon=True).start()

    def dial_loop(self):
        """电脑 → 板子：断了就按 1、2、5、5…… 秒重找、重连。"""
        log("wifi: dial mode（bridge 去连板子）")
        back = 1
        while True:
            dev = load_device()
            if not dev:
                time.sleep(5)
                continue
            s = None
            for ip in board_addrs(dev):
                try:
                    s = socket.create_connection((ip, NET_PORT), timeout=2)
                    break
                except OSError:
                    s = None
            if not s:
                time.sleep(back)
                back = min(back * 2, 5)
                continue
            back = 1
            if not self.session(s, ip):  # 认证没过（板子上删掉了这台电脑）：别一直敲门
                time.sleep(30)
            time.sleep(1)

    def session(self, s, ip):
        """认证并收数据，断开才返回；认证没过返回 False。"""
        try:
            s.settimeout(5)
            dev = load_device()
            hello = recv_line(s).split()
            if not dev or len(hello) != 3 or hello[0] != "BUDDY/2" or hello[1] != dev.get("board_id"):
                s.sendall(b"ERR unknown\n")
                raise ConnectionError("unpaired board " + " ".join(hello[:2]))
            bid, nb = hello[1], hello[2]
            nh = secrets.token_hex(16)
            k = dev["k"]
            s.sendall(f"HOST {dev['host_id']} {nh} {hmac_hex(k, f'H|{bid}|{nb}|{nh}')}\n".encode())
            ok = recv_line(s).split()
            if ok[:1] == ["ERR"]:
                raise ConnectionError("板子拒绝：" + " ".join(ok[1:]) + "（板子上删掉了这台电脑？插 USB 或 buddy pair --wifi 重新配对）")
            if len(ok) != 2 or ok[0] != "OK" or not secrets.compare_digest(ok[1], hmac_hex(k, f"B|{dev['host_id']}|{nh}|{nb}")):
                raise ConnectionError("auth failed")
            salt = bytes.fromhex(nb + nh)
            tx, rx = AESGCM(hkdf32(k, salt, b"buddy h2b")), AESGCM(hkdf32(k, salt, b"buddy b2h"))
        except (OSError, ValueError, ConnectionError) as e:
            log("wifi: 拒绝", ip, e)
            s.close()
            return False
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(10)  # 板子每 2 秒发心跳，10 秒没动静就断开
        with self.lock:
            if self.sock:
                self.sock.close()
            self.sock, self.peer, self.tx, self.txn, self.active = s, ip, tx, 0, True
        log("wifi connected", ip)
        if dev.get("ip") != ip:  # 记下板子的 IP，dial 模式下次先试它
            save_device({**dev, "ip": ip})
        rxn = 0
        try:
            while True:
                n = struct.unpack("<I", recv_exact(s, 4))[0]
                if n < 17 or n > 65536:
                    raise ValueError(f"bad frame {n}")
                pt = rx.decrypt(b"B2H\0" + struct.pack("<Q", rxn), recv_exact(s, n), None)
                rxn += 1
                if pt[:1] != b"\x01":
                    continue
                if pt[1:].startswith(b'{"t":"role"'):
                    act = json.loads(pt[1:]).get("r") == "active"
                    if act != self.active:
                        log("wifi role", "控制中" if act else "待命（板子在给别的电脑 / USB 用）")
                    self.active = act
                else:
                    device_line(pt[1:], "wifi")
        except Exception as e:  # noqa: BLE001 —— 断线、超时、校验失败都只关掉这条
            self.drop(s, e)
        return True

    def drop(self, s, why):
        with self.lock:
            if self.sock is s:
                self.sock = self.peer = None
                log("wifi lost", why)
        try:
            s.close()
        except OSError:
            pass

    def send_raw(self, rtype, data=b""):
        with self.lock:
            s = self.sock
            if not s:
                return False
            try:
                ct = self.tx.encrypt(b"H2B\0" + struct.pack("<Q", self.txn), rtype + data, None)
                self.txn += 1
                s.sendall(struct.pack("<I", len(ct)) + ct)
                return True
            except Exception as e:  # noqa: BLE001
                self.drop(s, e)
                return False

    def heartbeat(self):
        while True:  # 走 USB 时 Wi-Fi 这条也保持着，拔线后马上能用
            time.sleep(2)
            self.send_raw(b"\x04")


def lan_ips():
    """本机的局域网 IPv4（10 / 172.16~31 / 192.168），跳过回环和代理软件的虚拟网卡（198.18.x 之类）。"""
    out = []
    try:
        if host.IS_MAC:
            txt = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=3).stdout
            cand = re.findall(r"inet (\d+\.\d+\.\d+\.\d+)", txt)
        else:
            cand = [a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)]
    except (OSError, subprocess.SubprocessError):
        cand = []
    for ip in cand:
        a, b = (int(x) for x in ip.split(".")[:2])
        if (a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)) and ip not in out:
            out.append(ip)
    return out


def local_dial(board_ip=None):
    """告诉板子怎么连本机：局域网 IP（和板子同一网段的排前面）+ Bonjour 主机名（IP 变了板子用 mDNS 找回来）。
    不用「UDP connect 看出口」：开着代理软件的 TUN 模式时，出口是 198.18.0.1 这种虚拟地址，板子连不到。"""
    ips = lan_ips()
    if board_ip:
        pre = board_ip.rsplit(".", 1)[0] + "."
        ips.sort(key=lambda ip: not ip.startswith(pre))
    out = ips[:3]
    name = ""
    if host.IS_MAC:
        try:
            name = subprocess.run(["scutil", "--get", "LocalHostName"], capture_output=True, text=True, timeout=3).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    name = name or socket.gethostname().split(".")[0]
    if name:
        out.append(name + ".local")
    return out


class Links:
    """USB 优先，没有串口时走 Wi-Fi；对外的 send / ensure 和原来的 Link 一样。"""

    def __init__(self):
        self.usb = Link()
        self.net = NetLink()

    @property
    def ser(self):
        return self.usb.ser

    def ensure(self):
        if self.usb.ensure():
            return "usb"
        return "wifi" if self.net.sock and self.net.active else None

    def kind(self):
        if self.usb.ser:
            return "usb"
        if self.net.sock:
            return "wifi" if self.net.active else "standby"
        return None

    def send(self, obj):
        if self.usb.ensure():
            return self.usb.send(obj)
        if not self.net.active:  # 待命：板子不收
            return False
        line = encode_line(obj)
        if line is None or len(line) > 16000:  # 板子单条记录上限 16KB（超过会断开重连）；串口那边本来也只收 8KB 以内的行
            return False
        return self.net.send_raw(b"\x01", line.rstrip(b"\n"))


pending_pair = {}  # 已经发给板子、等它回 PAIRED 的配对


def board_addrs(dev):
    """dial 模式下找板子：上次的 IP → agent-buddy-<id>.local（系统的 mDNS 解析）→ UDP 广播探测。"""
    out = [dev["ip"]] if dev.get("ip") else []
    try:
        ip = socket.getaddrinfo(f"agent-buddy-{dev['board_id']}.local", NET_PORT, socket.AF_INET)[0][4][0]
        if ip not in out:
            out.append(ip)
    except (OSError, IndexError):
        pass
    for ip, _bid, _open in discover(dev["board_id"], 1.0):
        if ip not in out:
            out.append(ip)
    return out


def discover(board_id="*", wait=1.5):
    """UDP 广播「BUDDY? <id|*>」，板子回「BUDDY! <id> <port> <配对窗口开着 1/0>」。返回 [(ip, id, 开着窗口)]。"""
    # 每块局域网网卡一个 socket，绑在网卡地址上发 255.255.255.255：不然 Windows 只从一块网卡发（可能是 WSL / 代理的虚拟网卡），
    # 而且子网不一定是 /24（例如 10.30.0.0/22 的广播地址是 10.30.3.255），受限广播不用知道掩码
    socks = []
    for lip in lan_ips() or ["0.0.0.0"]:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.setblocking(False)
        try:
            s.bind((lip, 0))
        except OSError:
            s.close()
            continue
        socks.append((s, {"255.255.255.255", lip.rsplit(".", 1)[0] + ".255"}))
    q = f"BUDDY? {board_id}\n".encode()
    found, end, next_send = {}, time.time() + wait, 0
    try:
        while time.time() < end:
            if time.time() >= next_send:  # UDP 会丢：每 0.5 秒再喊一遍
                for s, targets in socks:
                    for t in targets:
                        try:
                            s.sendto(q, (t, NET_PORT))
                        except OSError:
                            pass
                next_send = time.time() + 0.5
            ready = select.select([s for s, _ in socks], [], [], 0.2)[0] if socks else []
            for s in ready:
                try:
                    data, (ip, _port) = s.recvfrom(128)
                except OSError:
                    continue
                f = data.decode(errors="replace").split()
                if len(f) >= 4 and f[0] == "BUDDY!" and (board_id == "*" or f[1] == board_id):
                    found[ip] = (ip, f[1], f[3] == "1")
    finally:
        for s, _ in socks:
            s.close()
    return list(found.values())


def wifi_pair_cli():
    """buddy pair --wifi：没有 USB 时和板子配对。板子上先打开 设置 → 电脑 → 添加电脑；两边显示同一个 6 位验证码，在板子上点「允许」。"""
    if AESGCM is None:
        print("缺少 cryptography 包：先运行 setup 或 pip install -r requirements.txt")
        return 1
    print("在局域网里找板子……")
    boards = [b for b in discover("*", 2.0) if b[2]]
    if not boards:
        print("没找到打开了配对窗口的板子：在板子上进入 设置 → 电脑 → 添加电脑，再运行一次（两边要在同一个 Wi-Fi 里）")
        return 1
    ip, bid, _ = boards[0]
    print(f"找到板子 {bid}（{ip}）")
    dev = load_device()
    hid = dev["host_id"] if dev else secrets.token_hex(8)
    mode = net_mode(dev)
    dial = local_dial(ip) if mode == "listen" else []
    name = socket.gethostname().split(".")[0][:30]
    sk = X25519PrivateKey.generate()
    pub_h = sk.public_key().public_bytes_raw()
    try:
        s = socket.create_connection((ip, NET_PORT), timeout=5)
        s.settimeout(5)
        hello = recv_line(s).split()
        if len(hello) != 3 or hello[0] != "BUDDY/2":
            print("板子回的不对：", " ".join(hello))
            return 1
        nb = hello[2]
        line = f"PAIR {hid} {pub_h.hex()} {NET_PORT} {base64.b64encode(name.encode()).decode()} {','.join(dial) or '-'}"
        s.sendall((line + "\n").encode())
        r = recv_line(s).split()
        if len(r) != 2 or r[0] != "PAIRKEY":
            print("板子拒绝：", " ".join(r))
            return 1
        pub_b = bytes.fromhex(r[1])
        sh = sk.exchange(X25519PublicKey.from_public_bytes(pub_b))
        mac = bytes.fromhex(hmac_hex(sh, f"sas|{bid}|{nb}|{line}|{r[1]}"))
        sas = int.from_bytes(mac[:4], "big") % 1000000
        print(f"\n  验证码：{sas // 1000:03d} {sas % 1000:03d}\n")
        print("核对板子屏幕上的验证码一样，再在板子上点「允许」（90 秒内）……")
        s.settimeout(95)
        r = recv_line(s).split()
        s.close()
    except (OSError, ValueError, ConnectionError) as e:
        print("连接板子失败：", e)
        return 1
    if r[:1] != ["PAIROK"]:
        print("没配对上：", " ".join(r))
        return 1
    k = hkdf32(sh, pub_h + pub_b, b"buddy pair")
    save_device({"board_id": bid, "host_id": hid, "key": base64.b64encode(k).decode(), "ip": ip,
                 **({"mode": dev["mode"]} if dev and dev.get("mode") else {})})
    print(f"已配对板子 {bid}。" + ("板子会来连这台电脑。" if mode == "listen" else "bridge 会去连板子。"))
    return 0


def on_hello(info, link):
    """插着 USB 时板子回的 HELLO：新固件带 net 字段。没配对 / 配的不是本机 → 自动配对（USB 是可信通道）；配过 → 更新本机地址。"""
    if "net" not in info or AESGCM is None:
        return
    dev = load_device()
    mode = net_mode(dev)
    dial = local_dial(info.get("ip") or None) if mode == "listen" else []  # dial 模式：由 bridge 去连板子，板子不用拨号
    hosts = info.get("hosts") or [info.get("host")]
    if dev and dev.get("board_id") == info.get("id") and dev.get("host_id") in hosts:
        link.usb.send({"t": "net", "id": dev["host_id"], "dial": dial})
        if info.get("ip") and info["ip"] != dev.get("ip"):
            save_device({**dev, "ip": info["ip"]})
            log("wifi: 板子的 IP", info["ip"])
        return
    hid = dev["host_id"] if dev else secrets.token_hex(8)
    k = secrets.token_bytes(32)
    pending_pair.clear()
    pending_pair.update(board_id=info.get("id"), host_id=hid, key=base64.b64encode(k).decode())
    name = socket.gethostname().split(".")[0]
    if link.usb.send({"t": "pair", "id": hid, "name": name, "k": pending_pair["key"], "dial": dial, "port": NET_PORT}):
        log("wifi: 正在和板子配对", info.get("id"))


FULL_RESEND = 30  # 状态平时只在变化时发；每隔这么久全量重发一次，防止某一行在串口上丢了屏幕一直停在旧状态
HELLO_EVERY = 20  # 插着 USB 时每隔这么久问一次 HELLO，跟上板子的 Wi-Fi IP


def pump(link):
    last_state, last_usage_sent, last_ping, last_usage = None, 0, 0, None
    last_cx, last_cx_at, last_cxu = None, 0, None
    last_ds, last_ds_at, last_dsu = None, 0, None
    was_connected, last_full, last_hello = False, time.time(), 0
    while True:
        connected = link.ensure()
        if connected and connected != was_connected:  # 新连上、或 USB / Wi-Fi 换了：全量重发
            last_state, last_usage_sent, last_usage = None, 0, None
            last_cx, last_cxu = None, None
            last_ds, last_dsu = None, None
            magpie_resend.set()
            last_hello = 0
        if connected == "usb" and time.time() - last_hello > HELLO_EVERY:
            # 新固件回 HELLO 带 Wi-Fi 配对信息和板子 IP（on_hello）。插着线时定时再问：板子可能是插上之后才连的 Wi-Fi，
            # 或者换了 IP；拔线前记下最新的 IP，开了客户端隔离、广播和 mDNS 都不通的网络也能直接连上
            link.send({"t": "hello"})
            last_hello = time.time()
        was_connected = connected
        if connected:
            now = time.time()
            if now - last_full > FULL_RESEND:
                last_state, last_cxu, last_dsu, last_full = None, None, None, now
            st = aggregate()
            ok = True
            if st != last_state:
                ok = link.send({"t": "state", **st})
                if ok:
                    last_state = st
            cx = aggregate("codex")
            if ok and (cx != last_cx or now - last_cx_at > 5):  # Codex：同样的字段，消息类型 cx
                if link.send({"t": "cx", **cx}):
                    last_cx, last_cx_at = cx, now
            ds = aggregate("deepseek")
            if ok and (ds != last_ds or now - last_ds_at > 5):  # DeepSeek：同样的字段，消息类型 ds（旧固件不认识，直接忽略）
                if link.send({"t": "ds", **ds}):
                    last_ds, last_ds_at = ds, now
            with lock:
                u = dict(usage)
                cxu = {k: v for k, v in codex_usage.items() if k != "_at"}
                dsu = dict(ds_balance)
            cur = {k: u[k] for k in ("h5", "h5r", "d7", "d7r") if k in u}
            if ok and "h5" in u and (cur != last_usage or now - last_usage_sent > 60):  # 变了立刻发
                ok = link.send({"t": "usage", **cur})
                if ok:
                    last_usage_sent, last_usage = now, cur
            if ok and cxu and cxu != last_cxu and link.send({"t": "cusage", **cxu}):
                last_cxu = cxu
            if ok and dsu and dsu != last_dsu and link.send({"t": "dsusage", **dsu}):
                last_dsu = dsu
            audio_tick()
            if ok and now - last_ping > 2:
                link.send({"t": "ping"})
                last_ping = now
            time.sleep(0.2)
        else:
            time.sleep(2)

# ---------------- DeepSeek 余额 / 今天的用量 ----------------
# DeepSeek 按量计费，没有 5 小时 / 7 天额度。余额优先从 magpie 的额度卡读（带赠送 / 充值、走势、每天花多少）；
# 没有 magpie 时，设了 API Key（环境变量 DEEPSEEK_API_KEY，或 bridge/state/deepseek_key 文件）就每 5 分钟查一次接口。
DS_KEY_FILE = os.path.join(STATE_DIR, "deepseek_key")
ds_balance = {}  # bal 余额文字，ok 1 能用 / 0 余额不足，g / tp 赠送 / 充值，pd 每天大约花多少，tr 最近的余额走势
ds_magpie_at = 0  # 最近一次从 magpie 拿到余额的时间；10 分钟内不再查接口


def ds_from_magpie(c):
    global ds_magpie_at
    split = {x.get("label"): x.get("text") for x in c.get("balanceSplit") or []}
    trend = c.get("balanceTrend") or {}
    pts = sorted(trend.get("points") or [], key=lambda x: x.get("at") or "")
    d = {"bal": c["balance"], "ok": 1, "g": split.get("Granted", ""), "tp": split.get("Topped up", ""),
         "pd": round(float(trend.get("perDay") or 0), 2), "tr": [round(float(x.get("amount") or 0), 2) for x in pts[-24:]]}
    with lock:
        ds_balance.clear()
        ds_balance.update(d)
    ds_magpie_at = time.time()


def build_ds_today():
    """magpie 里 DeepSeek Harness（agent=dsh）今天的用量：请求数、输入 / 输出 token、缓存命中率、每小时请求数。"""
    d = mget("usage/requests?period=today&limit=1&agent=dsh")
    inp, out = d.get("input") or 0, d.get("output") or 0
    cr, cw = d.get("cache_read") or 0, d.get("cache_write") or 0
    per = [x.get("calls") or 0 for x in d.get("series") or []]
    top = max(per or [0])
    hour = dt.datetime.now().hour
    return {"t": "dstoday", "n": d.get("calls") or 0, "tin": short_num(inp + cr + cw), "tout": short_num(out),
            "hit": f"{round(100 * cr / (inp + cr + cw))}%" if inp + cr + cw else "—",
            "bars": [round(1000 * v / top) if top else 0 for v in per][:24], "hn": hour}


def deepseek_key():
    k = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not k and os.path.exists(DS_KEY_FILE):
        with open(DS_KEY_FILE, encoding="utf-8") as f:
            k = f.read().strip()
    return k


def deepseek_loop():
    while True:
        key = deepseek_key()
        if key and time.time() - ds_magpie_at > 600:
            try:
                req = urllib.request.Request("https://api.deepseek.com/user/balance",
                                             headers={"Authorization": f"Bearer {key}", "Accept": "application/json"})
                with urllib.request.urlopen(req, timeout=10) as r:
                    d = json.loads(r.read())
                infos = d.get("balance_infos") or []
                b = next((i for i in infos if i.get("currency") == "CNY"), infos[0] if infos else None)
                if b:
                    sym = {"CNY": "¥", "USD": "$"}.get(b.get("currency"), "")
                    with lock:
                        ds_balance.clear()
                        ds_balance.update(bal=f"{sym}{float(b.get('total_balance') or 0):.2f}", ok=int(bool(d.get("is_available"))))
            except Exception as e:  # noqa: BLE001 —— 查不到就沿用上次的数
                log("deepseek balance failed:", repr(e)[:120])
        time.sleep(300)


# ---------------- magpie ----------------
MAGPIE_API = "http://127.0.0.1:3425/v1/magpie/"
MAGPIE_BIN = shutil.which("magpie") or "/Applications/magpie.app/Contents/MacOS/magpie"
_magpie_exe = {"path": None}


def magpie_bin():
    """magpie 程序的路径。Windows 上 magpie 是个单独的 exe（例如放在下载目录），通常不在 PATH 里：从正在运行的 magpie 进程找。"""
    if not host.IS_WIN or shutil.which("magpie"):
        return MAGPIE_BIN
    if not (_magpie_exe["path"] and os.path.exists(_magpie_exe["path"])):
        ps = "(Get-Process magpie* | Where-Object { $_.Path -and $_.ProcessName -notlike 'magpie-bun*' } | Select-Object -First 1).Path"
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=15,
                           creationflags=0x08000000)
        _magpie_exe["path"] = r.stdout.strip() or None
    return _magpie_exe["path"] or MAGPIE_BIN
PERIODS = ["today", "7d", "30d", "all"]
MONTHS = {m: i + 1 for i, m in enumerate("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split())}
magpie_resend = threading.Event()  # 设备重新连上时置位：下一轮全量重发


def mget(path, raw=False):
    with urllib.request.urlopen(MAGPIE_API + path, timeout=5) as r:
        data = json.load(r)
    # 在源头打码：之后的截断、拼接都基于打过码的数据，截断后的邮箱也不会漏（raw=True 时由调用方自己打码）
    return scrub(data) if mask_enabled() and not raw else data


def cells(text):
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in text)


def fit(text, width):
    """按显示宽度截断（中文算 2 格），超出加 "…"（unifont 缺这个字时由屏幕手画）。"""
    text = " ".join(str(text or "").split())
    if cells(text) <= width:
        return text
    out, w = "", 0
    for ch in text:
        cw = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        if w + cw > width - 2:
            break
        out += ch
        w += cw
    return out + "…"


def short_num(n, lower=False):
    """与 magpie app.js 的 fmtN 一致；lower=True 时 K 写成 k（路由列表的 token 数）。"""
    n = n or 0
    if n >= 1e9:
        out = f"{n / 1e9:.2f}B"
    elif n >= 1e7:
        out = f"{round(n / 1e6)}M"
    elif n >= 1e6:
        out = f"{n / 1e6:.1f}M"
    elif n >= 1e5:
        out = f"{round(n / 1e3)}K"
    elif n >= 1e3:
        out = f"{n / 1e3:.1f}K"
    else:
        out = str(int(n))
    return out.replace("K", "k") if lower else out


def fmt_cost(c):
    """与 magpie app.js 的 fmtCost 一致（美元）。"""
    return "$" + (f"{c:.0f}" if c >= 100 else f"{c:.2f}" if c >= 1 else f"{c:.3f}")


def nice_max(v):
    if v <= 0:
        return 1
    k = 10 ** int(len(str(int(v))) - 1)
    for m in (1, 2, 2.5, 5, 10):
        if m * k >= v:
            return m * k
    return 10 * k


def local(ts):
    return dt.datetime.fromisoformat(str(ts).replace("Z", "+00:00")).astimezone()


def when(t):
    """重置时间：今天 → 02:19，明天 → 明天 23:52，更远 → 10月19日 18:59。"""
    today = dt.datetime.now().astimezone().date()
    hm = t.strftime("%H:%M")
    if t.date() == today:
        return hm
    if t.date() == today + dt.timedelta(days=1):
        return "明天 " + hm
    return f"{t.month}月{t.day}日 {hm}"


def window_label(name):
    return {"5 hours": "5h", "7 days": "7d", "1 day": "1d", "30 days": "30d"}.get(name, name)


def axis_label(label, bucket):
    if bucket == "hour":
        return f"{int(label):02d}:00" if str(label).isdigit() else str(label)
    parts = str(label).split()
    if len(parts) == 2 and parts[0] in MONTHS:
        return f"{MONTHS[parts[0]]}/{parts[1]}"
    return str(label)


codex_usage = {}
build_round = 0
codex_cards = []   # 本轮读到的 Codex 账号：[(账号 id, 显示名, 5h 已用, 7d 已用, 5h 重置时刻, 7d 重置时刻)]，读写都在 lock 里
CX_PICK_FILE = os.path.join(STATE_DIR, "codex_account.txt")
try:
    with open(CX_PICK_FILE) as _f:
        codex_pick = _f.read().strip()  # 板子上选定的账号 id；空 = 自动（5 小时剩余最多的）
except OSError:
    codex_pick = ""


def acct_id(user):
    """账号的稳定标识：未打码邮箱的哈希。显示名会随 magpie 的「隐藏账号」变化，也可能截断后重名，不能拿来认账号。"""
    return hashlib.sha1(str(user or "").encode()).hexdigest()[:12]


def build_quotas():
    global build_round
    build_round += 1
    mask = mask_enabled()
    cards = mget("quotas/cards", raw=True).get("data") or []
    groups, order, cx = {}, [], []
    for c in cards:
        cid = acct_id(c.get("user") or c.get("name"))
        if mask:
            c = scrub(c)
        key = c.get("provider") or c.get("name")
        if key not in groups:
            groups[key] = {"n": c.get("name") or key, "i": c.get("icon") or key, "plan": c.get("plan") or "", "c": []}
            order.append(key)
        wins, parts, resets = [], [], {}
        for i, w in enumerate(c.get("windows") or []):
            lab = window_label(w.get("name", ""))
            if w.get("resetsAt"):
                resets[lab] = int(local(w["resetsAt"]).timestamp())
            used = float(w.get("used") or 0)
            wins.append({"l": lab, "left": max(0, min(100, round(100 - used)))})
            if w.get("resetsAt"):
                t = when(local(w["resetsAt"]))
                parts.append(t if i == 0 else f"{lab} {t}")
            # Claude 的额度顺便同步给 Agent Buddy 页（比 statusline 更及时，桌面 App 下也有）
            if c.get("provider") == "claude" and lab in ("5h", "7d"):
                short = "h5" if lab == "5h" else "d7"
                global magpie_usage_at
                magpie_usage_at = time.time()
                with lock:  # 窗口刚重置时 magpie 不给重置时间，数值照样要更新（否则卡在旧值）
                    usage[short] = used
                    usage[short + "r"] = int(local(w["resetsAt"]).timestamp()) if w.get("resetsAt") else 0
        groups[key]["c"].append({"u": fit(c.get("user") or c.get("name"), 36), "r": fit(" · ".join(parts), 46), "w": wins})
        if c.get("provider") == "deepseek" and c.get("balance"):  # DeepSeek 余额（按量计费，没有额度窗口）
            ds_from_magpie(c)
        if c.get("provider") == "codex" and wins:  # Codex 额度：记下每个账号，最后按板子上的选择（或自动）挑一个
            left = {w["l"]: w["left"] for w in wins}  # 按窗口名认周期，不按顺序（可能只有 7 天窗口）
            cx.append((cid, fit(c.get("user") or c.get("name") or "", 24),
                       100 - left["5h"] if "5h" in left else -1, 100 - left["7d"] if "7d" in left else -1,
                       resets.get("5h", 0), resets.get("7d", 0)))
    with lock:
        codex_cards[:] = cx
        if cx:  # 旧版本按显示名记的选择也认
            pick = next((i for i, c in enumerate(cx) if codex_pick in (c[0], c[1])), -1) if codex_pick else -1
            k = pick if pick >= 0 else min(range(len(cx)), key=lambda i: cx[i][2] if cx[i][2] >= 0 else 101)  # 自动：5 小时剩余最多
            _, u, h5, d7, h5r, d7r = cx[k]
            codex_usage.clear()
            codex_usage.update(h5=h5, d7=d7, h5r=h5r, d7r=d7r, acct=u, auto=int(pick < 0), n=len(cx), _at=build_round)
        else:  # 读成功但已经没有 Codex 账号：发「暂无数据」覆盖板子上的旧额度（读失败会抛异常，走不到这里）
            codex_usage.clear()
            codex_usage.update(h5=-1, d7=-1, acct="", auto=1, n=0, _at=build_round)
    return {"t": "mq", "g": [groups[k] for k in order]}


def build_usage(i, period):
    """与 magpie 面板「用量」页同口径：网关 + 会话日志，取接口给的总数。"""
    d = mget(f"usage/requests?period={period}&limit=1")
    s = d.get("series") or []
    inp, out = d.get("input") or 0, d.get("output") or 0
    cr, cw = d.get("cache_read") or 0, d.get("cache_write") or 0
    errs, cost = d.get("errors") or 0, d.get("cost") or 0
    cached, prompt = cr + cw, inp + cr + cw
    per = [(x.get("input") or 0) + (x.get("output") or 0) + (x.get("cache_read") or 0) + (x.get("cache_write") or 0) for x in s]
    top = nice_max(max(per or [0]))
    n = len(s)
    idx = sorted({min(n - 1, k * n // 4) for k in range(4)} | {n - 1}) if n > 1 else list(range(n))
    tsub = f"输入 {short_num(inp)} · 输出 {short_num(out)}" + (f" · 缓存 {short_num(cached)}" if cached else "")
    th = d.get("through") or {}  # 只算经过网关的：首页「今天 N 次请求」和路由页同口径（会话日志里的本地会话不算）
    return {
        "t": "mu", "p": i,
        "tok": short_num(inp + out + cached), "tsub": tsub,
        "calls": str(d.get("calls") or 0), "csub": "没有失败" if not errs else f"{errs} 次失败",
        "cost": fmt_cost(cost) if cost else "—",
        "gcalls": str(th.get("calls") or 0), "gcost": fmt_cost(th["cost"]) if th.get("cost") else "—",
        "hit": f"{round(100 * cr / prompt)}%" if prompt else "—",
        "ymax": short_num(top), "bars": [round(1000 * v / top) for v in per][-60:],
        "xl": [axis_label(s[k].get("label"), d.get("bucket")) for k in idx],
    }


# magpie 路由页里每个 Agent 的颜色（routing.js 的 HUES / SPARE / hueOf）
HUES = {"claude": "#d97757", "codex": "#6366f1", "gemini": "#0ea5e9", "copilot": "#a855f7", "cursor": "#14b8a6",
        "opencode": "#eab308", "crush": "#ec4899", "goose": "#84cc16", "pi": "#10b981", "omp": "#d946ef", "omo": "#7c3aed",
        "dsh": "#06b6d4", "commandcode": "#f97316", "mimocode": "#3b82f6", "deveco": "#3c99cc"}
SPARE = ["#8b5cf6", "#22c55e", "#0d9488", "#0891b2", "#ca8a04", "#db2777", "#2563eb", "#65a30d"]
# 请求用途标签（magpie purposes.js + 中文翻译）
KINDS = {"agent_job": "子代理", "ambient_suggestion_safety": "提示词建议", "ambient_suggestions": "提示词建议",
         "auto_review": "审批判定", "collab_spawn": "子代理", "compact": "压缩", "composer_predictions": "预测",
         "guardian": "审批判定", "guardian_review": "审批判定", "luna_reserve": "Luna 储备", "memgen": "记忆",
         "memory": "记忆", "memory_consolidation": "记忆", "review": "审查", "routing": "路由分类器",
         "thread_description": "描述", "thread_spawn": "子代理", "thread_title": "标题",
         "thread_title_reconsideration": "标题", "title": "标题", "title_generation": "标题", "vision": "图片描述",
         "web_search": "联网搜索"}


def hue565(agent):
    h = HUES.get(agent)
    if not h:
        x = 0
        for c in str(agent or ""):
            x = (x * 31 + ord(c)) & 0xFFFFFFFF
        h = SPARE[x % len(SPARE)]
    r, g, b = int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def took(ms):
    """与屏幕上的时长格式一致：<1 分钟「N.N 秒」，<1 小时「N 分 N 秒」，再往上「N 小时 N 分」。"""
    sec = (ms or 0) / 1000
    if sec < 60:
        return f"{sec:.1f} 秒"
    if sec < 3600:
        return f"{int(sec // 60)} 分 {int(sec % 60)} 秒"
    return f"{int(sec // 3600)} 小时 {int(sec % 3600 // 60)} 分"


route_seen = {}  # 已经播放过的请求 id（按插入顺序，只留最近 500 个）
route_primed = False


def build_routes(_today_tokens=None):
    """与 magpie 面板「路由」页同口径：只看经过网关的请求。"""
    global route_primed
    today = mget("usage?period=today")
    head = f"今天 {today.get('calls') or 0} 次调用 · {short_num((today.get('input') or 0) + (today.get('output') or 0))} token"
    d = mget("usage/requests?period=today&limit=12&via=through")
    raw = d.get("rows") or []

    rows = []
    for r in raw[:8]:
        agent = fit(r.get("agentName") or r.get("agent"), 14)
        kind = KINDS.get(r.get("kind") or "", r.get("kind") or "")
        room = max(8, (330 - cells(agent) * 8 - (cells(kind) * 6 + 22 if kind else 0)) // 6)
        status = r.get("status") or 0
        bad = status >= 400 or bool(r.get("error"))
        where = (r.get("providerName") or r.get("provider") or "") + (" · " + r["providerAccount"] if r.get("providerAccount") else "")
        served = r.get("served") or ""
        toks = sum(r.get(k) or 0 for k in ("in", "out", "cache_read", "cache_write"))
        meta = " · ".join(x for x in (f"{short_num(toks, lower=True)} token" if toks else "", took(r.get("ms"))) if x)
        rows.append({"a": agent, "i": r.get("icon") or "", "m": fit(r.get("req") or r.get("model"), room), "k": kind,
                     "tm": local(r["t"]).strftime("%H:%M:%S"),
                     "sv": fit(f"{status} {r.get('error') or '失败'}" if bad else where + (f" · {served}" if served else ""), 40),
                     "info": meta, "err": bad})

    # 连线图：左边最近的 3 个 Agent，右边最近的 3 个服务方（provider + 账号）
    ags, dst = [], []
    for r in raw:
        if r.get("agent") not in [a["id"] for a in ags] and len(ags) < 3:
            ags.append({"id": r.get("agent"), "n": fit(r.get("agentName") or r.get("agent"), 14), "i": r.get("icon") or "",
                        "c": hue565(r.get("agent"))})
        key = r.get("provider") or "?"
        if key not in [x["id"] for x in dst] and len(dst) < 3:
            bad = (r.get("status") or 0) >= 400 or bool(r.get("error"))
            dst.append({"id": key, "n": fit(r.get("providerName") or key, 10), "s": fit(r.get("providerAccount") or "", 14),
                        "st": 2 if bad else 1})

    # 新完成的请求：交给屏幕按 magpie 的方式播放（光点 Agent → magpie → 服务方 → 等回答 → 原路返回）
    trips = []
    for r in reversed(raw):
        rid = r.get("route_id") or r.get("response_id") or r.get("t")
        if rid in route_seen:
            continue
        route_seen[rid] = 1
        while len(route_seen) > 500:
            route_seen.pop(next(iter(route_seen)), None)  # /replay 可能同时在删
        if not route_primed:
            continue
        ai = next((i for i, a in enumerate(ags) if a["id"] == r.get("agent")), -1)
        di = next((i for i, x in enumerate(dst) if x["id"] == (r.get("provider") or "?")), -1)
        if ai >= 0 and di >= 0:
            trips.append({"a": ai, "d": di, "ok": not ((r.get("status") or 0) >= 400 or r.get("error")), "ms": r.get("ms") or 0})
    route_primed = True

    for a in ags:
        a.pop("id")
    for x in dst:
        x.pop("id")
    return {"t": "mr", "head": head, "rows": rows, "ags": ags, "dst": dst, "trips": trips[-4:]}


AGENT_LINE = re.compile(r"^\s{2}(\S.*?)\s{2,}(.+?)\s{2,}((?:[~/]|[A-Za-z]:[\\/]|\\\\).*?)\s*$")  # 路径里可能有空格；Windows 盘符 / UNC


def build_agents():
    out = subprocess.run([magpie_bin(), "ls"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20,
                         creationflags=0x08000000 if host.IS_WIN else 0).stdout  # Windows：不弹控制台窗口
    agents = []
    for line in out.splitlines():
        m = AGENT_LINE.match(line)
        if not m:
            continue
        name, mid = m.group(1).strip(), m.group(2)
        hidden = name.endswith(" hidden")
        name = name.removesuffix(" hidden")
        mid = re.sub(r"\s+·\s+", " · ", mid).strip()
        agents.append({"n": fit(name, 22), "i": "", "m": fit(mid, 40), "h": hidden})  # 图标由屏幕按 magpie 的 Agent 名 → 图标表查
    agents.sort(key=lambda a: a["h"])
    return {"t": "ma", "a": agents}


# ---- 邮箱打码：跟随 magpie 的「隐藏账号」（Routing / Usage 页的 Hide accounts）----
EMAIL_RE = re.compile(r"([\w.+-]{1,2})[\w.+-]*@([\w-]+(?:\.[\w-]+)+)")
_mask_cache = {"at": 0, "on": False, "blocked": not host.IS_MAC, "saved": None}


MAGPIE_WEBKIT = os.path.expanduser("~/Library/WebKit/com.yetone.magpie")
MASK_FILE = os.path.join(STATE_DIR, "mask.json")  # 上次读到的「隐藏账号」开关


def _mask_from_webkit():
    """magpie 存在它 WebKit 本地存储里的 magpie.maskEmails（"1" 打码 / "0" 不打码）。
    这是别的 App 的私有数据：从终端启动时继承终端的权限能读；launchd 自启时 macOS 会拦下并弹「数据访问被阻止」。
    所以只要被拦过一次，这个进程就不再尝试，改用上次读到的值。返回 True / False，没选过或读不了返回 None。"""
    if _mask_cache["blocked"] or not os.path.isdir(MAGPIE_WEBKIT):
        return None
    try:
        os.listdir(MAGPIE_WEBKIT)
    except PermissionError:
        _mask_cache["blocked"] = True
        log("magpie 的本地存储无权读取（launchd 自启时正常），「隐藏账号」改用上次读到的设置")
        return None
    except OSError:
        return None
    for db in glob.glob(os.path.join(MAGPIE_WEBKIT, "WebsiteData", "**", "localstorage.sqlite3"), recursive=True):
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=1)
            try:
                row = con.execute("select value from ItemTable where key='magpie.maskEmails'").fetchone()
            finally:
                con.close()
        except sqlite3.Error as e:
            if "authoriz" in str(e) or "unable to open" in str(e):
                _mask_cache["blocked"] = True
                return None
            continue
        if row:
            return row[0].decode("utf-16-le") == "1" if isinstance(row[0], bytes) else str(row[0]) == "1"
    return None


def mask_enabled():
    """跟随 magpie 的「隐藏账号」：先读 magpie 自己存的开关（读到就记进 state/mask.json），
    读不了用上次记下的值，都没有时跟随 ~/.config/magpie/settings.json 的 redactPersonal。每 30 秒读一次，只读。"""
    now = time.time()
    if now - _mask_cache["at"] < 30:
        return _mask_cache["on"]
    on = _mask_from_webkit()
    if on is not None:
        if on != _mask_cache["saved"]:
            try:
                write_atomic(MASK_FILE, json.dumps({"mask": on}))
                _mask_cache["saved"] = on
            except OSError:
                pass
    else:
        if _mask_cache["saved"] is None:
            try:
                with open(MASK_FILE) as f:
                    _mask_cache["saved"] = bool(json.load(f).get("mask"))
            except (OSError, ValueError):
                pass
        on = _mask_cache["saved"]
    if on is None:
        try:
            with open(os.path.expanduser("~/.config/magpie/settings.json")) as f:
                on = bool(json.load(f).get("redactPersonal"))
        except (OSError, ValueError):
            on = False
    _mask_cache.update(at=now, on=on)
    return on


def scrub(v):
    """把消息里所有邮箱打码成 an***@gmail.com（保留前两个字符和域名）。"""
    if isinstance(v, str):
        return EMAIL_RE.sub(lambda m: f"{m.group(1)}***@{m.group(2)}", v)
    if isinstance(v, list):
        return [scrub(x) for x in v]
    if isinstance(v, dict):
        return {k: scrub(x) for k, x in v.items()}
    return v


def magpie_loop(link):
    sent = {}          # 每类消息上次发出的内容，没变就不重发
    due = {}           # 每类任务下次执行时间
    tasks = [("routes", 3), ("usage0", 10), ("quotas", 30), ("dstoday", 30), ("usage", 60), ("agents", 60)]
    last_full = time.time()
    online = True  # 以最近一次实际探测（routes）为准；没轮到探测的那一秒沿用上次结果
    while True:
        if magpie_resend.is_set():
            magpie_resend.clear()
            sent.clear()
            due.clear()
        now = time.time()
        if now - last_full > 120:  # 隔一阵子即使没变也重发一次，防串口丢行
            sent.clear()
            last_full = now
        for name, every in tasks:
            if now < due.get(name, 0):
                continue
            due[name] = now + every
            try:
                if name == "quotas":
                    msgs = [build_quotas()]
                elif name == "usage0":
                    msgs = [build_usage(0, "today")]
                elif name == "usage":
                    msgs = [build_usage(i, p) for i, p in enumerate(PERIODS) if i]
                elif name == "routes":
                    msgs = [build_routes()]
                elif name == "dstoday":
                    msgs = [build_ds_today()]
                else:
                    msgs = [build_agents()]
                if name == "routes":
                    online = True
            except Exception as e:  # noqa: BLE001 —— magpie 没开或接口变了，不影响 Agent Buddy
                if name == "routes":
                    online = False
                    log("magpie unreachable:", repr(e)[:120])
                due[name] = now + max(every, 10)
                continue
            for m in msgs:
                key = m["t"] + str(m.get("p", ""))
                body = json.dumps(m, ensure_ascii=False, sort_keys=True)
                if (sent.get(key) != body or m.get("trips")) and link.send(dict(m)):
                    sent[key] = body
        if sent.get("mst") != str(online) and link.send({"t": "mst", "on": online}):
            sent["mst"] = str(online)
        time.sleep(1)


# ---------------- 录屏 ----------------
class Recorder:
    def __init__(self):
        self.active = False   # 是否在收帧
        self.frames = []      # [(设备 millis, RLE 数据)]
        self.status = {"status": "idle"}
        self.lock = threading.Lock()  # 两个录屏请求同时到达时只开一个


rec = Recorder()


def device_line(line, src="usb"):
    """设备的一行文字输出：设置同步（CFG）、雷达（PRES）、换 Codex 账号（CXACC）、Wi-Fi 配对（HELLO / PAIRED）、
    性能 / 扫描 / 调试日志；其余丢弃。src：从 USB 还是 Wi-Fi 收到的（配对只认 USB）。"""
    global codex_pick
    if line.startswith(b"HELLO ") and src == "usb":
        try:
            on_hello(json.loads(line[6:]), link_ref)
        except (ValueError, AttributeError):
            pass
    elif line.startswith(b"PAIRED ") and src == "usb":
        try:
            r = json.loads(line[7:])
            if pending_pair and r.get("host") == pending_pair["host_id"]:
                dev = load_device() or {}
                save_device({**pending_pair, "board_id": r.get("id"),
                             **{k: dev[k] for k in ("mode",) if k in dev}})
                log("wifi: 已配对板子", r.get("id"), "，拔掉 USB 后自动走 Wi-Fi")
                pending_pair.clear()
        except (ValueError, OSError):
            pass
    elif line.startswith(b"CXACC"):
        with lock:
            ids = [c[0] for c in codex_cards]
            names = {c[0]: c[1] for c in codex_cards}
        if not ids:
            return
        cur = next((c for c in ids if codex_pick in (c, names[c])), None)  # 兼容旧版按显示名记的选择
        i = ids.index(cur) if cur else -1
        codex_pick = ids[i + 1] if i + 1 < len(ids) else ""  # 自动 → 账号 1 → 账号 2 → 自动
        try:
            write_atomic(CX_PICK_FILE, codex_pick)
        except OSError:
            pass
        log("Codex 额度账号：", names.get(codex_pick) or "自动")
        magpie_resend.set()
    elif line.startswith(b"PRES "):
        try:
            pr = json.loads(line[5:])
            if pr.get("p") != radar["p"] and radar["p"] is not None:
                log("雷达：", "你回到座位" if pr.get("p") else "座位没人", f"· {pr.get('n')} 人")
            if pr.get("pv") != radar["pv"]:
                log("雷达：", "有人靠近" if pr.get("pv") else "靠近的人走了")
            radar.update(p=pr.get("p"), n=pr.get("n", 0), pv=pr.get("pv", 0), seat=pr.get("seat", 1), ts=time.time())
        except (ValueError, AttributeError):
            pass
    elif line.startswith(b"CFG "):
        try:
            cfg = json.loads(line[4:])
            audio_cfg.update(voice=bool(cfg["voice"]), sfx=bool(cfg["sfx"]),
                             mute=bool(cfg["mute"]), dsec=int(cfg["dsec"]),
                             greet=bool(cfg.get("greet", 1)), mmute=bool(cfg.get("mmute", 1)),
                             talk=bool(cfg.get("talk", 0)))
        except (ValueError, KeyError, TypeError):
            pass
    elif line.startswith((b"PERF", b"SCAN", b"MIC", b"SPK", b"RADAR", b"BAT")):
        log(line.decode(errors="replace").strip())


def reader(link):
    """持续读串口：按行处理设备的文字输出；录屏期间先从字节流里切出画面帧，帧之间的字节照样当文字处理。"""
    buf = bytearray()
    carry = b""

    def text(data):
        nonlocal carry
        lines = (carry + data).split(b"\n")
        carry = lines.pop()[-512:]  # 半行留到下次拼上
        for line in lines:
            device_line(line.rstrip(b"\r"))

    while True:
        s = link.ser
        if not s:
            buf.clear()
            carry = b""
            time.sleep(0.5)
            continue
        try:
            chunk = s.read(65536)
        except (serial.SerialException, OSError, TypeError, AttributeError) as e:
            link.drop(s, f"read failed: {e}")  # 板子拔掉了：断开，等 pump 重连
            time.sleep(0.5)
            continue
        if not chunk:
            time.sleep(0.005)
            continue
        if not rec.active:
            buf.clear()  # 录屏结束时剩下的半帧
            text(bytes(chunk))
            continue
        buf += chunk
        while True:
            i = buf.find(FRAME_MAGIC)
            if i < 0:
                if len(buf) > 3:  # 末尾 3 字节可能是半个帧头，留着
                    text(bytes(buf[:-3]))
                    del buf[:-3]
                break
            if i:
                text(bytes(buf[:i]))
                del buf[:i]
            if len(buf) < 12:
                break
            ms, ln = struct.unpack_from("<II", buf, 4)
            if ln > SCREEN_W * SCREEN_H * 3:
                del buf[:4]
                continue
            if len(buf) < 12 + ln:
                break
            rec.frames.append((ms, bytes(buf[12:12 + ln])))
            del buf[:12 + ln]


def decode_frame(rle):
    a = np.frombuffer(rle, dtype=np.uint8).reshape(-1, 3)
    colors = a[:, 1].astype(np.uint16) | (a[:, 2].astype(np.uint16) << 8)
    px = np.repeat(colors, a[:, 0].astype(np.int64))
    if px.size != SCREEN_W * SCREEN_H:
        return None
    r = ((px >> 11) & 0x1F).astype(np.uint32) * 255 // 31
    g = ((px >> 5) & 0x3F).astype(np.uint32) * 255 // 63
    b = (px & 0x1F).astype(np.uint32) * 255 // 31
    return np.stack([r, g, b], axis=-1).astype(np.uint8).reshape(SCREEN_H, SCREEN_W, 3)


def encode_video(frames, base):
    """按设备时间戳重采样成固定 25fps，放大 2 倍（最近邻，保持像素风）后编码 MP4，再转 GIF。"""
    mp4, gif = base + ".mp4", base + ".gif"
    t0, t1 = frames[0][0], frames[-1][0]
    proc = subprocess.Popen(
        [FFMPEG, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{SCREEN_W}x{SCREEN_H}",
         "-r", str(OUT_FPS), "-i", "-", "-vf", f"scale={SCREEN_W * 2}:{SCREEN_H * 2}:flags=neighbor",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", "-movflags", "+faststart", mp4],
        stdin=subprocess.PIPE)
    idx, cache_idx, cache = 0, -1, None
    for k in range(int((t1 - t0) * OUT_FPS / 1000) + 1):
        t = t0 + k * 1000 // OUT_FPS
        while idx + 1 < len(frames) and frames[idx + 1][0] <= t:
            idx += 1
        if idx != cache_idx:
            img = decode_frame(frames[idx][1])
            if img is not None:
                cache = img
            cache_idx = idx
        if cache is not None:
            proc.stdin.write(cache.tobytes())
    proc.stdin.close()
    try:
        proc.wait(timeout=120)  # ffmpeg 卡住时不能让录屏一直停在 "encoding"
    except subprocess.TimeoutExpired:
        proc.kill()
        raise
    subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-i", mp4, "-vf",
                    "fps=15,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=none", gif],
                   check=False, timeout=120)
    return mp4, gif


def capture(link, seconds, video=True):
    with rec.lock:
        if rec.active or rec.status.get("status") == "encoding":
            return {"error": "正在录制中"}
        rec.frames = []
        rec.active = True
        rec.status = {"status": "recording", "seconds": seconds}
    if link.kind() != "usb" or not link.send({"t": "cap", "ms": int(seconds * 1000) + 500}):  # 录屏只走 USB
        rec.active = False
        rec.status = {"status": "error", "error": "录屏需要插 USB" if link.kind() in ("wifi", "standby") else "串口未连接"}
        return rec.status

    def finish():
        time.sleep(seconds)
        link.send({"t": "cap", "ms": 0})
        time.sleep(0.6)
        rec.active = False
        frames = rec.frames
        if not frames:
            rec.status = {"status": "error", "error": "没有收到画面帧"}
            return
        os.makedirs(REC_DIR, exist_ok=True)
        base = os.path.join(REC_DIR, "buddy-" + time.strftime("%Y%m%d-%H%M%S"))
        span = (frames[-1][0] - frames[0][0]) / 1000 or 1
        info = {"frames": len(frames), "device_fps": round(len(frames) / span, 1)}
        try:
            if video:
                rec.status = {"status": "encoding", **info}
                mp4, gif = encode_video(frames, base)
                rec.status = {"status": "done", "mp4": mp4, "gif": gif, **info}
            else:
                Image.fromarray(decode_frame(frames[-1][1])).save(base + ".png")
                rec.status = {"status": "done", "png": base + ".png", **info}
        except Exception as e:  # noqa: BLE001 —— 编码失败要把原因报给调用方
            rec.status = {"status": "error", "error": repr(e)}
        log("capture", rec.status)

    threading.Thread(target=finish, daemon=True).start()
    return rec.status


LOCAL_HOSTS = {"127.0.0.1", "localhost", "[::1]"}


def qnum(q, key, default, lo, hi):
    """查询参数转数字：不是数字 / NaN / inf 用默认值，再夹到 [lo, hi]。"""
    try:
        v = float(q.get(key, [default])[0])
    except (TypeError, ValueError):
        v = default
    return min(max(v if math.isfinite(v) else default, lo), hi)


class Handler(BaseHTTPRequestHandler):
    timeout = 10  # 慢吞吞不发完的连接 10 秒后断开，不占着线程

    def _allowed(self):
        """只接受本机命令行（curl / hook 脚本）的请求。浏览器发的跨站请求一定带 Origin，
        DNS rebinding 的 Host 是攻击者的域名；这两种都拒绝，网页就没法让喇叭说话、伪造状态或改屏幕设置。"""
        if self.headers.get("Origin") or self.headers.get("Sec-Fetch-Site") not in (None, "none"):
            return False
        host = (self.headers.get("Host") or "").lower()
        name, _, port = host.rpartition(":") if not host.endswith("]") else (host, "", "")
        if not name:
            name, port = host, ""
        return name in LOCAL_HOSTS and port in ("", str(PORT))

    def _body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n < 0 or n > MAX_BODY:
            raise ValueError(f"bad Content-Length {n}")
        raw = self.rfile.read(n) if n else b""
        try:
            data = json.loads(raw or b"{}")
        except ValueError:
            return {}
        return data if isinstance(data, dict) else {}

    def _reply(self, code, text, ctype="text/plain; charset=utf-8"):
        data = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._reply(code, json.dumps(obj, ensure_ascii=False), "application/json; charset=utf-8")

    def do_POST(self):
        self._dispatch(self._post)

    def do_GET(self):
        self._dispatch(self._get)

    def _dispatch(self, fn):
        if not self._allowed():
            self._reply(403, "forbidden")
            return
        u = urlparse(self.path)
        try:
            fn(u.path, parse_qs(u.query))
        except Exception as e:  # noqa: BLE001 —— 出错也要回一个响应，调用方不至于等到超时
            log(f"http {self.command} {u.path} failed: {e!r}"[:200])
            try:
                self._reply(400 if isinstance(e, ValueError) else 500, f"error: {e}")
            except OSError:
                pass

    def _post(self, path, q):
        if path == "/hook":  # ?agent=codex / ?agent=deepseek 表示 Codex / DeepSeek Harness 的事件
            agent = q.get("agent", ["claude"])[0]
            handle_hook(self._body(), agent if agent in AGENT_NAME else "claude")
            self._reply(200, "ok")
        elif path == "/usage":
            self._reply(200, handle_usage(self._body()))
        elif path == "/record":
            self._json(capture(link, qnum(q, "seconds", 15, 1, 300)))
        elif path == "/gamma":
            g = qnum(q, "g", 1.6, 0.5, 3.0)
            ok = link.send({"t": "gamma", "g": g})
            self._reply(200 if ok else 503, f"gamma={g}" if ok else "串口未连接")
        elif path == "/ui":
            cmd = {k: int(v[0]) for k, v in q.items() if v[0].lstrip("-").isdigit() and len(v[0]) < 10}
            ok = link.send({"t": "ui", **cmd})
            self._reply(200 if ok else 503, "ok" if ok else "串口未连接")
        elif path == "/replay":  # 调试：把最近 3 条网关请求当作新请求，再播放一遍飞行动画
            for r in (mget("usage/requests?period=today&limit=3&via=through").get("rows") or []):
                route_seen.pop(r.get("route_id") or r.get("response_id") or r.get("t"), None)
            self._reply(200, "ok")
        elif path == "/audio":  # 开关：/audio?voice=0&sfx=1&mute=0&dsec=60（转给板子保存，同设置页）
            cmd = {k: int(q[k][0]) for k in ("voice", "sfx", "mute", "dsec") if k in q and q[k][0].isdigit() and len(q[k][0]) < 6}
            if cmd:
                link.send({"t": "ui", **cmd})
                audio_cfg.update({k: (v if k == "dsec" else bool(v)) for k, v in cmd.items()})
            self._json(audio_cfg)
        elif path == "/reset":  # 调试 / 录屏：清空所有会话，画面回到待命
            with lock:
                gone = list(sessions)
                sessions.clear()
            for sid in gone:
                forget(sid)
            self._reply(200, "ok")
        elif path == "/back":  # 测试：模拟离开 25 分钟后回来
            link.send({"t": "back", "a": 1500, "d": 3, "w": 1})
            say("欢迎回来，你离开了25分钟，Claude 完成了3个任务，有1个在等你确认")
            self._reply(200, "ok")
        elif path == "/say":  # 测试：/say?text=你好
            say(q.get("text", ["你好，我是 Agent Buddy"])[0][:200])
            self._reply(200, "ok")
        elif path == "/snap":
            self._json(capture(link, 0.4, video=False))
        else:
            self._reply(404, "not found")

    def _get(self, path, q):
        if path == "/status":
            with lock:
                u = dict(usage)
            self._reply(200, json.dumps({"display": aggregate(), "usage": u, "serial": bool(link.ser), "link": link.kind(),
                                    "wifi": link.net.peer}, ensure_ascii=False, indent=2),
                        "application/json; charset=utf-8")
        elif path == "/record":
            self._json(rec.status)
        else:
            self._reply(404, "not found")

    def log_message(self, *args):
        pass


def single_instance():
    """同一时间只跑一个 bridge：两个实例会抢串口、抢端口。拿不到锁就说明已经有一个在跑，正常退出（launchd 不会因此反复重启）。"""
    os.makedirs(STATE_DIR, exist_ok=True)
    f = open(LOCK_FILE, "a+")
    try:
        host.lock_exclusive(f)
    except OSError:
        try:
            with open(PID_FILE) as p:
                pid = p.read().strip()
        except OSError:
            pid = "?"
        log(f"已有一个 bridge 在运行（pid {pid}），本次退出")
        sys.exit(0)
    write_atomic(PID_FILE, str(os.getpid()))  # 锁文件本身在 Windows 上被锁住时别人读不了，pid 单独存
    return f  # 调用方要一直拿着，文件关了锁就没了


def serve():
    """端口被别的程序占着时每 10 秒重试，而不是直接崩溃退出、被 launchd 拉起来再崩。"""
    while True:
        try:
            return ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
        except OSError as e:
            log(f"端口 {PORT} 打不开（{e.strerror}），10 秒后重试")
            time.sleep(10)


def redirect_log():
    """--log：自己把 stdout / stderr 追加到 state/bridge.log（Windows 用 pythonw 后台运行时没有控制台可写）。"""
    if "--log" not in sys.argv:
        return
    os.makedirs(STATE_DIR, exist_ok=True)
    fd = os.open(LOG_FILE, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.dup2(fd, 1)
    os.dup2(fd, 2)
    os.close(fd)
    sys.stdout = open(1, "w", encoding="utf-8", errors="replace", buffering=1, closefd=False)
    sys.stderr = open(2, "w", encoding="utf-8", errors="replace", buffering=1, closefd=False)


if __name__ == "__main__":
    redirect_log()
    _lock_file = single_instance()
    for f in (LOG_FILE, LOG_FILE + ".1"):  # 日志里有命令行和 prompt 片段：只给自己看
        try:
            os.chmod(f, 0o600)
        except OSError:
            pass
    load_usage()
    link = Links()
    link_ref = link
    forever(say_worker)
    forever(act_loop, link)
    forever(key_loop, link)
    forever(pump, link)
    forever(reader, link.usb)
    forever(link.net.serve)
    forever(magpie_loop, link)
    forever(deepseek_loop)
    server = serve()
    log(f"Agent Buddy bridge listening on 127.0.0.1:{PORT}")
    server.serve_forever()
