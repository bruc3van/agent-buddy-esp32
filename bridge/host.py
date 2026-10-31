"""bridge 用到的系统能力，按平台实现（macOS / Windows）。都只读「有没有、多久、几次」，不读内容。

- idle()          -> (多久没按键, 多久没任何输入)，秒；拿不到返回 None
- key_count()     -> 累计按键次数（只有次数）；拿不到返回 None
- mic_in_use()    -> 有程序正在用麦克风（开会 / 语音输入）
- tts_wav(text, path) 生成 16kHz / 16bit / 单声道 WAV；不支持时抛 OSError
- lock_exclusive(f) 对打开的文件加非阻塞排他锁，拿不到抛 OSError
- env(name)       环境变量 AGENT_BUDDY_<name>（旧名字 CLAUDE_BUDDY_<name> 也认）
"""

import ctypes
import os
import subprocess
import sys
import threading
import time

IS_MAC = sys.platform == "darwin"
IS_WIN = sys.platform == "win32"


def env(name, default=None):
    return os.environ.get("AGENT_BUDDY_" + name) or os.environ.get("CLAUDE_BUDDY_" + name) or default


if IS_WIN:
    import msvcrt
    import winreg
    from ctypes import wintypes
else:
    import fcntl


def lock_exclusive(f):
    if IS_WIN:
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)


# ---------------- macOS ----------------
if IS_MAC:
    # CGEventSourceSecondsSinceLastEventType / CounterForEventType 不需要辅助功能权限
    try:
        _cg = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        _idle = _cg.CGEventSourceSecondsSinceLastEventType
        _idle.restype, _idle.argtypes = ctypes.c_double, [ctypes.c_int32, ctypes.c_uint32]
        _keycount = _cg.CGEventSourceCounterForEventType
        _keycount.restype, _keycount.argtypes = ctypes.c_uint32, [ctypes.c_int32, ctypes.c_uint32]
    except (OSError, AttributeError):
        _idle = _keycount = None

    class _AOAddr(ctypes.Structure):
        _fields_ = [("sel", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("elem", ctypes.c_uint32)]

    def _fcc(s):
        return int.from_bytes(s.encode(), "big")

    try:
        _ca = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreAudio.framework/CoreAudio")
        _aget, _asize = _ca.AudioObjectGetPropertyData, _ca.AudioObjectGetPropertyDataSize
        _aget.argtypes = [ctypes.c_uint32, ctypes.POINTER(_AOAddr), ctypes.c_uint32, ctypes.c_void_p,
                          ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        _asize.argtypes = [ctypes.c_uint32, ctypes.POINTER(_AOAddr), ctypes.c_uint32, ctypes.c_void_p,
                           ctypes.POINTER(ctypes.c_uint32)]
    except OSError:
        _ca = None

    def idle():
        if not _idle:
            return None
        return _idle(1, 10), _idle(1, 0xFFFFFFFF)  # 10 = 按键；~0 = 任意输入

    def key_count():
        return _keycount(1, 10) if _keycount else None  # 10 = keyDown

    def _objects(obj, sel):
        """读一个 AudioObjectID 列表属性；拿不到返回 None。"""
        a, sz = _AOAddr(_fcc(sel), _fcc("glob"), 0), ctypes.c_uint32(0)
        if _asize(obj, ctypes.byref(a), 0, None, ctypes.byref(sz)) != 0:
            return None
        ids = (ctypes.c_uint32 * (sz.value // 4))()
        if _aget(obj, ctypes.byref(a), 0, None, ctypes.byref(sz), ids) != 0:
            return None
        return list(ids)

    def mic_in_use():
        """有程序正在录音（同菜单栏的橙点）。Mac mini 没接麦克风时总是 False。"""
        if not _ca:
            return False
        try:
            # macOS 14+：逐个进程看「是否在跑输入」；只看设备是否在跑，双工声卡 / 耳麦光放音乐也会算成开会
            procs = _objects(1, "prs#")
            if procs is not None:
                for p in procs:
                    ra, v, vs = _AOAddr(_fcc("piri"), _fcc("glob"), 0), ctypes.c_uint32(0), ctypes.c_uint32(4)
                    if _aget(p, ctypes.byref(ra), 0, None, ctypes.byref(vs), ctypes.byref(v)) == 0 and v.value:
                        return True
                return False
            a, sz = _AOAddr(_fcc("dev#"), _fcc("glob"), 0), ctypes.c_uint32(0)
            _asize(1, ctypes.byref(a), 0, None, ctypes.byref(sz))
            devs = (ctypes.c_uint32 * (sz.value // 4))()
            _aget(1, ctypes.byref(a), 0, None, ctypes.byref(sz), devs)
            for d in devs:
                si, ns = _AOAddr(_fcc("stm#"), _fcc("inpt"), 0), ctypes.c_uint32(0)
                _asize(d, ctypes.byref(si), 0, None, ctypes.byref(ns))
                if not ns.value:
                    continue
                ra, v, vs = _AOAddr(_fcc("gone"), _fcc("glob"), 0), ctypes.c_uint32(0), ctypes.c_uint32(4)
                if _aget(d, ctypes.byref(ra), 0, None, ctypes.byref(vs), ctypes.byref(v)) == 0 and v.value:
                    return True
        except (OSError, ValueError):
            pass
        return False

    SAY_VOICE = env("VOICE", "Tingting")  # macOS 自带的普通话女声

    def tts_wav(text, path):
        # "--"：文本以 - 开头时不会被当成 say 的选项（例如 -f 文件）
        subprocess.run(["say", "-v", SAY_VOICE, "-o", path, "--data-format=LEI16@16000", "--", text],
                       check=True, timeout=30, capture_output=True)


# ---------------- Windows ----------------
elif IS_WIN:
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class _LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]

    _kernel32.GetTickCount.restype = ctypes.c_uint
    _keys = {"n": 0, "at": time.time(), "started": False, "ok": False}
    _keys_lock = threading.Lock()

    def _key_hook_thread():
        """低级键盘钩子：只在按下时把计数 +1、记下时间，不看按的是哪个键。需要一个消息循环所在的线程。"""
        LRESULT = ctypes.c_ssize_t
        HOOKPROC = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)
        _user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
        _user32.SetWindowsHookExW.restype = wintypes.HHOOK
        _user32.CallNextHookEx.argtypes = [wintypes.HHOOK, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
        _user32.CallNextHookEx.restype = LRESULT
        _kernel32.GetModuleHandleW.restype = wintypes.HMODULE

        def proc(code, wparam, lparam):
            if code == 0 and wparam in (0x0100, 0x0104):  # WM_KEYDOWN / WM_SYSKEYDOWN
                _keys["n"] = (_keys["n"] + 1) & 0xFFFFFFFF
                _keys["at"] = time.time()
            return _user32.CallNextHookEx(None, code, wparam, lparam)

        cb = HOOKPROC(proc)
        if not _user32.SetWindowsHookExW(13, cb, _kernel32.GetModuleHandleW(None), 0):  # 13 = WH_KEYBOARD_LL
            return
        _keys["ok"] = True
        msg = wintypes.MSG()
        while _user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            _user32.TranslateMessage(ctypes.byref(msg))
            _user32.DispatchMessageW(ctypes.byref(msg))

    def _ensure_hook():
        with _keys_lock:
            if not _keys["started"]:
                _keys["started"] = True
                threading.Thread(target=_key_hook_thread, name="key_hook", daemon=True).start()
                time.sleep(0.2)

    def idle():
        lii = _LASTINPUTINFO(ctypes.sizeof(_LASTINPUTINFO), 0)
        if not _user32.GetLastInputInfo(ctypes.byref(lii)):
            return None
        inp = ((_kernel32.GetTickCount() - lii.dwTime) & 0xFFFFFFFF) / 1000
        _ensure_hook()
        key = time.time() - _keys["at"] if _keys["ok"] else inp
        return key, inp

    def key_count():
        _ensure_hook()
        return _keys["n"] if _keys["ok"] else None

    _MIC_KEY = r"Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone"

    def _mic_walk(key):
        i = 0
        while True:
            try:
                name = winreg.EnumKey(key, i)
            except OSError:
                return False
            i += 1
            with winreg.OpenKey(key, name) as sub:
                if name == "NonPackaged":
                    if _mic_walk(sub):
                        return True
                    continue
                try:
                    start = winreg.QueryValueEx(sub, "LastUsedTimeStart")[0]
                    stop = winreg.QueryValueEx(sub, "LastUsedTimeStop")[0]
                except OSError:
                    continue
                if start and not stop:  # 开始用了还没停 = 正在用
                    return True

    def mic_in_use():
        """系统的「麦克风隐私」记录：某个程序 LastUsedTimeStop 为 0 表示正在使用。"""
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _MIC_KEY) as k:
                return _mic_walk(k)
        except OSError:
            return False

    # 系统自带的 SAPI：优先选中文语音（中文版 Windows 自带 Huihui / Yaoyao），文本走环境变量，不拼进命令行
    _PS_TTS = (
        "Add-Type -AssemblyName System.Speech;"
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        "$v = $s.GetInstalledVoices() | Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -like 'zh*' } | Select-Object -First 1;"
        "if ($v) { $s.SelectVoice($v.VoiceInfo.Name) };"
        "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono);"
        "$s.SetOutputToWaveFile($env:BUDDY_TTS_OUT, $f); $s.Speak($env:BUDDY_TTS_TEXT); $s.Dispose()"
    )

    def tts_wav(text, path):
        env = {**os.environ, "BUDDY_TTS_TEXT": text, "BUDDY_TTS_OUT": path}
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", _PS_TTS], env=env,
                       check=True, timeout=30, capture_output=True, creationflags=0x08000000)  # CREATE_NO_WINDOW


# ---------------- 其他系统：没有这些能力，bridge 其余功能照常 ----------------
else:
    def idle():
        return None

    def key_count():
        return None

    def mic_in_use():
        return False

    def tts_wav(text, path):
        raise OSError("这个系统不支持语音播报")
