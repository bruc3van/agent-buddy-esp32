#!/usr/bin/env python3
"""Agent Buddy 管理工具（macOS / Windows 通用）。新电脑、新板子从项目根目录的 setup.sh / setup.cmd 开始，它会调用这里的 setup。

  buddy setup               一键配置：hooks → 检测板子 → 烧录固件（需要时）→ 开机自启 → 启动 → 自检
  buddy start | stop | restart
  buddy status              进程、串口、板子固件、hooks、magpie 等逐项检查
  buddy logs [-f]           看日志
  buddy autostart on|off    开机自启（macOS：launchd；Windows：当前用户的「启动」注册表项）
  buddy flash [--build] [--erase] [--port P]
                            烧录固件：默认用 firmware/ 里预编译的版本，不需要装 Arduino
  buddy build               用 arduino-cli 编译固件并更新 firmware/（开发用）
  buddy hooks on|off        只注册 / 移除 Claude Code、Codex 与 DeepSeek Harness 的 hooks
  buddy record [秒] | snap | gamma [值]
  buddy pair --wifi         没有 USB 时和板子配对（板子上先打开 设置 → 电脑 → 添加电脑，核对 6 位验证码）
  buddy uninstall
"""

import argparse
import datetime as dt
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import tomllib
import urllib.request

import host

BRIDGE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BRIDGE)
STATE = os.path.join(BRIDGE, "state")
LOG_FILE = os.path.join(STATE, "bridge.log")
PID_FILE = os.path.join(STATE, "bridge.pid")
LOCK_FILE = os.path.join(STATE, "bridge.lock")
SUP_PID_FILE = os.path.join(STATE, "supervisor.pid")
BRIDGE_PY = os.path.join(BRIDGE, "agent_bridge.py")
FW_DIR = os.path.join(ROOT, "firmware")
SKETCH = os.path.join(ROOT, "agent_buddy")
PORT = 47823
URL = f"http://127.0.0.1:{PORT}"
ESPRESSIF_VID = 0x303A

IS_WIN, IS_MAC = host.IS_WIN, host.IS_MAC
VENV = os.path.join(BRIDGE, ".venv")
PY = os.path.join(VENV, "Scripts", "python.exe") if IS_WIN else os.path.join(VENV, "bin", "python")
PYW = os.path.join(VENV, "Scripts", "pythonw.exe") if IS_WIN else PY  # Windows 后台运行不弹黑窗口

LABEL = "com.agent-buddy.bridge"
PLIST = os.path.expanduser(f"~/Library/LaunchAgents/{LABEL}.plist")
WIN_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
WIN_RUN_NAME = "AgentBuddyBridge"
OLD_LABEL, OLD_RUN_NAME = "com.claude-buddy.bridge", "ClaudeBuddyBridge"  # 改名前（Claude Buddy）的开机自启
NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW
DETACHED = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP

FQBN = "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=4M,CDCOnBoot=cdc,USBMode=hwcdc,PartitionScheme=huge_app"
CORE = "esp32:esp32@3.3.12"
CORE_URL = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
LIBS = ["GFX Library for Arduino@1.6.9", "U8g2@2.36.19", "TAMC_GT911@1.0.2", "ArduinoJson@7.4.3"]

CLAUDE_SETTINGS = os.path.expanduser(os.path.join("~", ".claude", "settings.json"))
CODEX_DIR = os.path.expanduser(os.path.join("~", ".codex"))
CODEX_CONFIG = os.path.join(CODEX_DIR, "config.toml")
CLAUDE_EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure", "PermissionRequest",
                 "Notification", "Stop", "StopFailure", "PreCompact", "PostCompact", "SubagentStop", "SessionEnd"]
CODEX_EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PermissionRequest", "PostToolUse",
                "PreCompact", "PostCompact", "Stop", "SubagentStop"]
DSH_HOME = os.environ.get("DSH_HOME") or os.path.expanduser(os.path.join("~", ".dsh"))
DSH_PATCH = os.path.join(DSH_HOME, "cordis.patch.yml")      # DeepSeek Harness 的全局配置层，对所有 profile 生效
DSH_HOOKS = os.path.join(DSH_HOME, "agent-buddy-hooks.json")  # 给 dsh-hooks-claude-code 插件读的 Claude Code 格式 hooks
DSH_EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop", "SubagentStop"]  # 插件支持的事件里我们用得上的
DSH_MARK = "# ---- Agent Buddy（ESP32 桌面屏）"
OUR_HOOK = re.compile(r"bridge[/\\]+hook\.(sh|py)")            # 认得出的本项目 hook（含仓库搬家前的旧路径）
OUR_STATUS = re.compile(r"bridge[/\\]+statusline\.(sh|py)")

ASSUME_YES = False


# ---------------- 小工具 ----------------
def out(msg=""):
    print(msg, flush=True)


def ok(msg):
    out(f"  ✓ {msg}")


def bad(msg):
    out(f"  ✗ {msg}")


def info(msg):
    out(f"  · {msg}")


def step(n, msg):
    out(f"\n[{n}] {msg}")


def ask(question, default=True):
    if ASSUME_YES or not sys.stdin.isatty():
        return default
    hint = "Y/n" if default else "y/N"
    try:
        a = input(f"  ? {question} [{hint}] ").strip().lower()
    except EOFError:
        return default
    return default if not a else a in ("y", "yes", "是", "好")


def http(method, path, timeout=3):
    req = urllib.request.Request(URL + path, method=method, data=b"{}" if method == "POST" else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def bridge_status():
    try:
        return json.loads(http("GET", "/status", 2))
    except Exception:  # noqa: BLE001
        return None


def cmd_path(p):
    """写进 hook 配置的路径：统一用 / （Git Bash、cmd、PowerShell 都认）；有空格才加引号。"""
    p = p.replace("\\", "/") if IS_WIN else p
    if " " not in p:
        return p
    return f'"{p}"' if IS_WIN else shlex.quote(p)


def write_atomic(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".buddy.tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(tmp, path)


def backup(path, tag):
    if os.path.exists(path):
        dst = f"{path}.bak-agent-buddy-{tag}-{time.strftime('%Y%m%d-%H%M%S')}"
        shutil.copy2(path, dst)
        return dst
    return None


# ---------------- 进程管理 ----------------
def running_pid():
    """bridge 运行时一直拿着 state/bridge.lock 的排他锁：拿得到锁 = 没在运行。比 pid 文件可靠（进程号会被复用）。"""
    os.makedirs(STATE, exist_ok=True)
    try:
        f = open(LOCK_FILE, "a+")
    except OSError:
        return None
    try:
        host.lock_exclusive(f)
    except OSError:
        f.close()
        try:
            with open(PID_FILE) as p:
                return int(p.read().strip() or 0) or -1
        except (OSError, ValueError):
            return -1
    f.close()  # 关闭即释放
    return None


def pid_alive(pid):
    if not pid or pid < 0:
        return False
    if IS_WIN:
        r = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                           creationflags=NO_WINDOW)
        return "python" in r.stdout.lower()
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def is_supervisor(pid):
    """pid 文件可能是上次开机留下的、号已经被别的进程用了：核对命令行确实是 buddy.py supervise 再动它。"""
    if not pid_alive(pid):
        return False
    if IS_WIN:
        cmd = ["powershell", "-NoProfile", "-Command", f"(Get-CimInstance Win32_Process -Filter 'ProcessId={pid}').CommandLine"]
        kw = {"creationflags": NO_WINDOW}
    else:
        cmd, kw = ["ps", "-o", "command=", "-p", str(pid)], {}
    try:
        line = subprocess.run(cmd, capture_output=True, text=True, timeout=10, **kw).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return "buddy.py" in line and "supervise" in line


def read_pid(path):
    try:
        with open(path) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def kill_tree(pid):
    if IS_WIN:
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True, creationflags=NO_WINDOW)
    else:
        import signal
        try:
            if os.getpgid(pid) == pid:  # 守护进程自己是组长，它拉起的 bridge 在同一组；别的进程只杀它自己，免得误伤终端
                os.killpg(pid, signal.SIGTERM)
            else:
                os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def launchd_loaded():
    return IS_MAC and subprocess.run(["launchctl", "print", f"gui/{os.getuid()}/{LABEL}"],
                                     capture_output=True).returncode == 0


def plist_args():
    import plistlib
    try:
        with open(PLIST, "rb") as f:
            return plistlib.load(f).get("ProgramArguments")
    except Exception:  # noqa: BLE001 —— 读不了就当过期，重写
        return None


def autostart_enabled():
    if IS_MAC:
        return os.path.exists(PLIST)
    if IS_WIN:
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY) as k:
                winreg.QueryValueEx(k, WIN_RUN_NAME)
                return True
        except OSError:
            return False
    return False


def wait_running(want, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        if bool(running_pid()) == want:
            return True
        time.sleep(0.3)
    return False


def spawn_detached_win(args):
    """Windows：守护进程不能挂在启动它的程序下面。从 Claude 桌面端（MSIX 打包应用）等的终端启动时，
    应用自动更新会用重启管理器把它连带关掉，bridge 就没了，开机自启要到下次登录才再拉起。
    交给 WMI 创建：父进程是系统的 WmiPrvSE，和谁启动的无关；WMI 不可用时退回直接启动。"""
    q = lambda s: s.replace("'", "''")  # noqa: E731  PowerShell 单引号字符串转义
    ps = ("$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
          "@{CommandLine='%s'; CurrentDirectory='%s'}; exit $r.ReturnValue"
          % (q(subprocess.list2cmdline(args)), q(BRIDGE)))
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True,
                           timeout=30, creationflags=NO_WINDOW)
        if r.returncode == 0:
            return
    except (OSError, subprocess.TimeoutExpired):
        pass
    subprocess.Popen(args, creationflags=DETACHED | NO_WINDOW, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, cwd=BRIDGE, close_fds=True)


def start(quiet=False):
    pid = running_pid()
    if pid:
        if not quiet:
            ok(f"bridge 已在运行（pid {pid}）")
        return True
    if not os.path.exists(PY):
        bad("还没安装 Python 环境，先运行项目根目录的 setup")
        return False
    if IS_MAC:  # magpie 的「隐藏账号」开关存在它的私有数据里：launchd 下读不了，趁在终端里先读一次记进 state/mask.json
        subprocess.run([PY, "-c", "import agent_bridge; agent_bridge.mask_enabled()"], cwd=BRIDGE,
                       capture_output=True, timeout=20)
    if IS_MAC and os.path.exists(PLIST):  # 开着自启：交给 launchd 拉起
        uid = os.getuid()
        if plist_args() != [PY, BRIDGE_PY, "--log"]:  # 仓库换了位置：旧 plist 指向的路径已失效，重写一份
            info("开机自启配置里的路径过期了，更新一下")
            if not autostart(True):
                return False
        elif launchd_loaded():
            subprocess.run(["launchctl", "kickstart", f"gui/{uid}/{LABEL}"], capture_output=True)
        else:
            subprocess.run(["launchctl", "bootstrap", f"gui/{uid}", PLIST], capture_output=True)
    elif IS_WIN:
        spawn_detached_win([PYW, os.path.abspath(__file__), "supervise"])
    else:  # 手动启动：拉起一个守护进程，bridge 意外退出时 10 秒后自动重启；关掉终端也继续运行
        subprocess.Popen([PYW, os.path.abspath(__file__), "supervise"], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=BRIDGE, close_fds=True,
                         start_new_session=True)
    if wait_running(True):
        if not quiet:
            ok(f"bridge 已启动（pid {running_pid()}），日志：{LOG_FILE}")
        return True
    bad("启动失败，最后几行日志：")
    tail(5)
    return False


def stop(quiet=False):
    if IS_MAC and launchd_loaded():
        subprocess.run(["launchctl", "bootout", f"gui/{os.getuid()}/{LABEL}"], capture_output=True)
    sup = read_pid(SUP_PID_FILE)
    if sup and is_supervisor(sup):
        kill_tree(sup)
    pid = running_pid()
    if pid and pid > 0:
        kill_tree(pid)
    if wait_running(False):
        for p in (SUP_PID_FILE,):
            try:
                os.remove(p)
            except OSError:
                pass
        if not quiet:
            ok("bridge 已停止" + ("（开机自启仍然开着，下次登录会再启动）" if autostart_enabled() else ""))
        return True
    bad(f"没能停下 bridge（pid {running_pid()}）")
    return False


def supervise():
    """手动启动 / Windows 自启时的守护进程：bridge 崩了 10 秒后重开；正常退出（例如已有一个在跑）就一起退出。"""
    os.makedirs(STATE, exist_ok=True)
    with open(SUP_PID_FILE, "w") as f:
        f.write(str(os.getpid()))
    kw = {"creationflags": NO_WINDOW} if IS_WIN else {}
    while True:
        rc = subprocess.call([PY, BRIDGE_PY, "--log"], cwd=BRIDGE, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)
        if rc == 0:
            try:
                os.remove(SUP_PID_FILE)  # 正常退出：不留 pid 文件，免得号被别的进程复用后误杀
            except OSError:
                pass
            return
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%H:%M:%S')} bridge 退出（code {rc}），10 秒后重启\n")
        time.sleep(10)


def migrate_old_name():
    """项目从 Claude Buddy 改名为 Agent Buddy：旧名字注册的开机自启换成新名字（路径也跟着 agent_bridge.py 更新）。"""
    if IS_MAC:
        old = os.path.expanduser(f"~/Library/LaunchAgents/{OLD_LABEL}.plist")
        if not os.path.exists(old):
            return
        subprocess.run(["launchctl", "bootout", f"gui/{os.getuid()}/{OLD_LABEL}"], capture_output=True)
        os.remove(old)
        info("开机自启从旧名字 Claude Buddy 换成 Agent Buddy")
        autostart(True)
        start(quiet=True)
    elif IS_WIN:
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY, 0, winreg.KEY_ALL_ACCESS) as k:
                winreg.QueryValueEx(k, OLD_RUN_NAME)
                winreg.DeleteValue(k, OLD_RUN_NAME)
        except OSError:
            return
        info("开机自启从旧名字 Claude Buddy 换成 Agent Buddy")
        autostart(True)


def autostart(on):
    if IS_MAC:
        uid = os.getuid()
        if on:
            import plistlib
            was = running_pid()
            if was:
                stop(quiet=True)
            os.makedirs(os.path.dirname(PLIST), exist_ok=True)
            with open(PLIST, "wb") as f:
                plistlib.dump({"Label": LABEL, "ProgramArguments": [PY, BRIDGE_PY, "--log"], "RunAtLoad": True,
                               "KeepAlive": {"SuccessfulExit": False}, "ThrottleInterval": 10,
                               "StandardErrorPath": LOG_FILE, "WorkingDirectory": BRIDGE}, f)
            subprocess.run(["launchctl", "bootout", f"gui/{uid}/{LABEL}"], capture_output=True)
            r = subprocess.run(["launchctl", "bootstrap", f"gui/{uid}", PLIST], capture_output=True, text=True)
            if r.returncode:
                bad(f"launchctl 加载失败：{r.stderr.strip()}")
                return False
            ok("已开启开机自启（launchd，登录后自动运行，崩溃自动重启）")
            info("macOS 会弹一条「App 后台活动：python 可在后台运行」通知，属正常；可在「登录项与扩展」里管理")
        else:
            subprocess.run(["launchctl", "bootout", f"gui/{uid}/{LABEL}"], capture_output=True)
            if os.path.exists(PLIST):
                os.remove(PLIST)
            ok("已关闭开机自启")
        return True
    if IS_WIN:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            if on:
                winreg.SetValueEx(k, WIN_RUN_NAME, 0, winreg.REG_SZ,
                                  f'"{PYW}" "{os.path.abspath(__file__)}" supervise')
                ok("已开启开机自启（登录后自动运行，崩溃自动重启）")
            else:
                try:
                    winreg.DeleteValue(k, WIN_RUN_NAME)
                except OSError:
                    pass
                ok("已关闭开机自启")
        return True
    bad("这个系统暂不支持开机自启")
    return False


def tail(n=50, follow=False):
    try:
        with open(LOG_FILE, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-n:]
            for line in lines:
                out("    " + line.rstrip())
            while follow:
                line = f.readline()
                if line:
                    out(line.rstrip())
                else:
                    time.sleep(0.3)
    except OSError:
        info("还没有日志")
    except KeyboardInterrupt:
        pass


# ---------------- 板子 ----------------
def find_port():
    if host.env("PORT"):
        return host.env("PORT")
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device.replace("/dev/tty.", "/dev/cu.")
    return None


def wait_port(timeout):
    end = time.time() + timeout
    while time.time() < end:
        p = find_port()
        if p:
            return p
        time.sleep(1)
    return None


def probe(port):
    """问板子固件版本：本固件会回 HELLO {"fw":"v3.7"}；出厂固件 / 很老的版本不回，返回 None。"""
    import serial
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False  # 不拉 DTR/RTS，免得复位
        s.open()
    except (serial.SerialException, OSError) as e:
        return f"error:{e}"
    try:
        buf = b""
        for _ in range(3):
            s.write(b'{"t":"hello"}\n')
            end = time.time() + 1.2
            while time.time() < end:
                buf += s.read(4096)
                m = re.search(rb'HELLO (\{[^\n]*\})', buf)
                if m:
                    return json.loads(m.group(1)).get("fw")
                buf = buf[-4096:]
        return None
    finally:
        s.close()


def fw_bundle():
    try:
        with open(os.path.join(FW_DIR, "manifest.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def ver_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", str(v or "")))


def flash(port=None, build=False, erase=False):
    if build and not build_fw():
        return False
    man = fw_bundle()
    if not man:
        bad("firmware/ 里没有固件（manifest.json），可以用 buddy build 自己编译")
        return False
    files = [(off, os.path.join(FW_DIR, name)) for off, name in man["parts"]]
    missing = [p for _, p in files if not os.path.exists(p)]
    if missing:
        bad(f"固件文件缺失：{missing}")
        return False
    was = running_pid()
    if was:
        info("暂停 bridge，释放串口")
        stop(quiet=True)
        time.sleep(1)
    okd = False
    try:
        port = port or wait_port(30)
        if not port:
            bad("没找到板子：用能传数据的 USB-C 线连上电脑（有的线只能充电）")
            return False
        info(f"烧录 {man['version']} → {port}" + ("（先整片擦除：板子上的设置、Wi-Fi 会清空）" if erase else ""))
        base = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port, "--baud", "921600",
                "--before", "default-reset", "--after", "hard-reset"]
        if erase and subprocess.call(base + ["erase-flash"]):
            bad("擦除失败")
            return False
        for i in range(3):
            args = base + ["write-flash", "-z", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
            args[args.index("--port") + 1] = port  # 重试时串口可能重新枚举、换了名字（Windows 常换 COM 号）
            for off, p in files:
                args += [off, p]
            if subprocess.call(args) == 0:  # esptool 自己打印进度；退出码 0 = 写完且校验通过
                okd = True
                break
            out(f"  第 {i + 1} 次烧录失败，2 秒后重试…")
            time.sleep(2)
            port = wait_port(10) or port
        if okd:
            ok(f"烧录完成：{man['version']}")
        else:
            bad("烧录失败。新板子如果连不上：按住板子上的 BOOT 键，点一下 RST，松开 BOOT，再运行 buddy flash")
        return okd
    finally:
        if was:
            start(quiet=True)


def build_fw():
    """编译固件并更新 firmware/：需要 arduino-cli；core 和库按固定版本安装（已装则跳过）。"""
    cli = shutil.which("arduino-cli")
    if not cli:
        bad("需要 arduino-cli：macOS 用 brew install arduino-cli；Windows 用 winget install ArduinoSA.CLI")
        return False
    info("准备 esp32 core 与库（第一次会下载约 1GB，之后跳过）")
    subprocess.run([cli, "core", "update-index", "--additional-urls", CORE_URL], capture_output=True)
    if subprocess.call([cli, "core", "install", CORE, "--additional-urls", CORE_URL]):
        bad(f"安装 {CORE} 失败")
        return False
    if subprocess.call([cli, "lib", "install", *LIBS]):
        bad("安装库失败")
        return False
    outdir = os.path.join(STATE, "build")
    info("编译中…")
    r = subprocess.run([cli, "compile", "-b", FQBN, "--output-dir", outdir, SKETCH], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode:
        for line in (r.stdout + r.stderr).splitlines():
            if "error" in line.lower():
                out("    " + line)
        bad("编译失败" + ("（Apple Silicon 上报 ctags 错误时见 docs/使用手册.md「固件」一节）" if "ctags" in r.stderr else ""))
        return False
    size = next((line for line in r.stdout.splitlines() if "Sketch uses" in line), "")
    props = subprocess.run([cli, "compile", "-b", FQBN, "--show-properties", SKETCH], capture_output=True, text=True).stdout
    core_path = re.search(r"^runtime\.platform\.path=(.+)$", props, re.M).group(1).strip()
    with open(os.path.join(SKETCH, "hw_state.h"), encoding="utf-8") as f:
        version = re.search(r'#define FW_VERSION "([^"]+)"', f.read()).group(1)
    os.makedirs(FW_DIR, exist_ok=True)
    parts = [("0x0", "bootloader.bin", os.path.join(outdir, "agent_buddy.ino.bootloader.bin")),
             ("0x8000", "partitions.bin", os.path.join(outdir, "agent_buddy.ino.partitions.bin")),
             ("0xe000", "boot_app0.bin", os.path.join(core_path, "tools", "partitions", "boot_app0.bin")),
             ("0x10000", "app.bin", os.path.join(outdir, "agent_buddy.ino.bin"))]
    for _, name, src in parts:
        shutil.copyfile(src, os.path.join(FW_DIR, name))
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "agent_buddy"], capture_output=True, text=True).stdout.strip()
    man = {"version": version, "chip": "esp32s3", "fqbn": FQBN, "core": CORE, "libs": LIBS,
           "built": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
           "commit": commit + ("+dirty" if dirty else ""), "parts": [[off, name] for off, name, _ in parts]}
    write_atomic(os.path.join(FW_DIR, "manifest.json"), json.dumps(man, ensure_ascii=False, indent=2) + "\n")
    ok(f"固件 {version} 已更新到 firmware/  {size.strip()}")
    return True


# ---------------- hooks ----------------
def hook_cmd(agent=""):
    if IS_WIN:
        return f"{cmd_path(PY)} {cmd_path(os.path.join(BRIDGE, 'hook.py'))}" + (f" {agent}" if agent else "")
    return cmd_path(os.path.join(BRIDGE, "hook.sh")) + (f" {agent}" if agent else "")


def status_cmd():
    if IS_WIN:
        return f"{cmd_path(PY)} {cmd_path(os.path.join(BRIDGE, 'statusline.py'))}"
    return cmd_path(os.path.join(BRIDGE, "statusline.sh"))


def claude_hooks(on):
    """~/.claude/settings.json：注册 / 移除 hooks 与 statusline。认得出仓库搬家前留下的旧路径，一并换掉。"""
    try:
        with open(CLAUDE_SETTINGS, encoding="utf-8-sig") as f:  # 有的编辑器会存成带 BOM 的 UTF-8
            cfg = json.load(f)
    except FileNotFoundError:
        cfg = {}
    except ValueError as e:
        bad(f"{CLAUDE_SETTINGS} 不是合法 JSON，没有改动：{e}")
        return False
    if not isinstance(cfg, dict) or not isinstance(cfg.get("hooks", {}), dict) \
            or not all(isinstance(v, list) for v in cfg.get("hooks", {}).values()):
        bad(f"{CLAUDE_SETTINGS} 里 hooks 的结构不认识，没有改动：请手动检查")
        return False
    ours, before = hook_cmd(), json.dumps(cfg, sort_keys=True)
    hooks = cfg.setdefault("hooks", {})
    added = removed = 0
    for ev in list(hooks):
        keep = []
        for g in hooks[ev]:
            if not isinstance(g, dict) or not isinstance(g.get("hooks", []), list):  # 不认识的条目原样留着
                keep.append(g)
                continue
            hs = [h for h in g.get("hooks", []) if not isinstance(h, dict) or not OUR_HOOK.search(str(h.get("command", "")))
                  or (on and h.get("command") == ours and ev in CLAUDE_EVENTS)]
            removed += len(g.get("hooks", [])) - len(hs)
            if hs:
                keep.append({**g, "hooks": hs})
        if keep:
            hooks[ev] = keep
        else:
            del hooks[ev]
    if on:
        for ev in CLAUDE_EVENTS:
            arr = hooks.setdefault(ev, [])
            if not any(isinstance(h, dict) and h.get("command") == ours
                       for g in arr if isinstance(g, dict) for h in g.get("hooks", [])):
                arr.append({"hooks": [{"type": "command", "command": ours, "async": True}]})
                added += 1
    if not hooks:
        cfg.pop("hooks", None)
    sl = cfg.get("statusLine") or {}
    if not isinstance(sl, dict):
        sl = {"command": str(sl)}
    note = ""
    if on and (not sl or OUR_STATUS.search(str(sl.get("command", "")))):
        cfg["statusLine"] = {"type": "command", "command": status_cmd(), "refreshInterval": 60}
    elif on:
        note = "（已有别的 statusLine，没有覆盖；额度改从 magpie 读取，不影响使用）"
    elif OUR_STATUS.search(str(sl.get("command", ""))):
        del cfg["statusLine"]
    if json.dumps(cfg, sort_keys=True) == before:
        ok(f"Claude Code hooks {'已是最新' if on else '本来就没有'}{note}")
        return True
    bk = backup(CLAUDE_SETTINGS, "install" if on else "uninstall")
    write_atomic(CLAUDE_SETTINGS, json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")
    ok(f"Claude Code hooks：新增 {added}，移除{'旧路径的 ' if on else ''}{removed}{note}"
       + (f"（原文件备份 {os.path.basename(bk)}）" if bk else ""))
    return True


def _toml_tables(text):
    """把 TOML 文本按表头切块：[(表头或 None, 块文本)]。只用来删除本项目写的 [[hooks.X]] 段，不做通用解析。"""
    chunks, head, buf = [], None, []
    for line in text.splitlines(keepends=True):
        if re.match(r"\s*\[", line) and not line.lstrip().startswith("#"):
            chunks.append((head, "".join(buf)))
            head, buf = line.strip(), [line]
        else:
            buf.append(line)
    chunks.append((head, "".join(buf)))
    return chunks


def codex_hooks(on):
    """~/.codex/config.toml：在末尾追加 / 移除每个事件一段 [[hooks.X]]（async）。没装 Codex 就跳过。"""
    if not os.path.isdir(CODEX_DIR) and not shutil.which("codex"):
        info("没装 Codex，跳过")
        return True
    try:
        with open(CODEX_CONFIG, encoding="utf-8-sig") as f:
            text = f.read()
    except FileNotFoundError:
        text = ""
    try:
        cfg = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        bad(f"{CODEX_CONFIG} 解析失败，没有改动：{e}")
        return False
    ours = hook_cmd("codex")
    have = {ev for ev, groups in (cfg.get("hooks") or {}).items() if isinstance(groups, list)
            for g in groups for h in g.get("hooks", []) if h.get("command") == ours}
    stale = any(OUR_HOOK.search(str(h.get("command", ""))) and h.get("command") != ours
                for groups in (cfg.get("hooks") or {}).values() if isinstance(groups, list)
                for g in groups for h in g.get("hooks", []))
    if on and not stale and have >= set(CODEX_EVENTS):
        ok("Codex hooks 已是最新")
        return True
    new = text
    if stale or not on:  # 删掉本项目写过的段：[[hooks.X.hooks]] 里命令是我们的，连同它前面空的 [[hooks.X]]
        chunks, keep = _toml_tables(text), []
        for head, body in chunks:
            m = re.fullmatch(r"\[\[hooks\.(\w+)\.hooks\]\]", head or "")
            if m and OUR_HOOK.search(body) and (on is False or ours not in body):
                if keep and keep[-1][0] == f"[[hooks.{m.group(1)}]]" and not keep[-1][1].strip().splitlines()[1:]:
                    keep.pop()
                continue
            keep.append((head, body))
        new = "".join(body for _, body in keep)
        if not OUR_HOOK.search(new):
            new = re.sub(r"\n?# ---- (?:Agent|Claude) Buddy（ESP32 桌面屏）[^\n]*\n", "\n", new)
        have = set() if not on else have
    if on:
        block = ["\n# ---- Agent Buddy（ESP32 桌面屏）：把 Codex 事件转发给本机 bridge，bridge 不在时静默忽略 ----\n"]
        for ev in CODEX_EVENTS:
            if ev not in have:
                block.append(f"[[hooks.{ev}]]\n[[hooks.{ev}.hooks]]\ntype = \"command\"\n"
                             f"command = {json.dumps(ours, ensure_ascii=False)}\nasync = true\ntimeout = 5\n\n")
        if len(block) > 1:
            new = new.rstrip("\n") + "\n" + "".join(block)
    try:
        tomllib.loads(new)
    except tomllib.TOMLDecodeError as e:
        bad(f"改完的 config.toml 解析不了，已放弃改动：{e}")
        return False
    if new == text:
        ok("Codex hooks " + ("已是最新" if on else "本来就没有"))
        return True
    bk = backup(CODEX_CONFIG, "install" if on else "uninstall")
    write_atomic(CODEX_CONFIG, new)
    ok(f"Codex hooks 已{'写入' if on else '移除'}" + (f"（原文件备份 {os.path.basename(bk)}）" if bk else "")
       + ("；Codex 提示审核 / 信任新 hook 时同意即可" if on else ""))
    return True


def dsh_hook_cmd():
    """dsh 跑 hook 命令：macOS 上是 bash -c，Windows 上是 pwsh -Command。路径一律用 /、都加引号；
    PowerShell 里带引号的程序路径要用 & 调用。stdin 直接交给 python（不要用 $input 转，pwsh 会按 GBK 解码弄乱中文）。"""
    q = lambda p: '"' + p.replace("\\", "/") + '"'
    if IS_WIN:
        return f"& {q(PY)} {q(os.path.join(BRIDGE, 'hook.py'))} deepseek"
    return f"{q(os.path.join(BRIDGE, 'hook.sh'))} deepseek"


def dsh_block():
    path = DSH_HOOKS.replace("\\", "/")
    return (f"\n{DSH_MARK}：用 DeepSeek Harness 自带的 Claude Code hooks 兼容插件，把事件转发给本机 bridge ----\n"
            f"- insert:\n"
            f"    - id: agent-buddy-hooks\n"
            f"      name: '@deepseek-ai/dsh-hooks-claude-code'\n"
            f"      config:\n"
            f"        configPath: {json.dumps(path, ensure_ascii=False)}\n"
            f"        defaultTimeoutMs: 5000\n"
            f"{DSH_MARK} 结束 ----\n")


DSH_PLUGIN = "@deepseek-ai/dsh-hooks-claude-code"
DSH_PROTOCOL = "@deepseek-ai/dsh-hook-protocol"
DSH_DESKTOP_CLI = (os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "DeepSeek Harness", "resources", "runtime", "cli", "bin", "dsh.cmd")
                   if IS_WIN else "/Applications/DeepSeek Harness.app/Contents/Resources/runtime/cli/bin/dsh")


DSH_ALT_APP = "/Applications/DSH Desktop.app"   # 第三方桌面版：同样用 ~/.dsh/profiles/desktop，内核是它自己打包的 dsh


def dsh_alt_version():
    """装了 DSH Desktop 时返回它打包的 dsh 内核版本；没装返回 None。它和官方桌面版可以同时装、同时开，各管各的 profile。"""
    try:
        with open(os.path.join(DSH_ALT_APP, "Contents", "Resources", "dsh-runtime", "node_modules", "@deepseek-ai", "dsh",
                               "package.json"), encoding="utf-8") as f:
            return json.load(f).get("version")
    except (OSError, ValueError, AttributeError):
        return None


DSH_ALT_PROFILE = os.path.join(DSH_HOME, "profiles", "web")      # DSH Desktop 跑的是 web profile，不读 desktop
DSH_BUNDLE = "agent-buddy-dsh-hooks"
DSH_BUNDLE_DIR = os.path.join(DSH_HOME, "agent-buddy-bundle")


def dsh_alt_installed():
    """DSH Desktop 的 web profile 里：组合包装了没有、里面 hooks 插件是什么版本"""
    mods = os.path.join(DSH_ALT_PROFILE, "node_modules")
    have = os.path.exists(os.path.join(mods, DSH_BUNDLE, "package.json"))
    try:
        with open(os.path.join(mods, *DSH_PLUGIN.split("/"), "package.json"), encoding="utf-8") as f:
            return have, json.load(f).get("version")
    except (OSError, ValueError):
        return have, None


def dsh_alt_hooks(on):
    """DSH Desktop：web profile 由它独占管理，命令行装不了插件、也只肯装声明了 dsh.bundle 的组合包，
    改 profile 的 cordis.patch.yml 也会被它重写掉。所以生成一个本地组合包（依赖两个 hooks 插件 + 挂插件的 patch），
    在 DSH Desktop 的插件管理里从本地路径装一次；它自己管理这个组合包，重写配置也不会丢。"""
    hooks = {"hooks": {ev: [{"hooks": [{"type": "command", "command": dsh_hook_cmd(), "timeout": 5}]}] for ev in DSH_EVENTS}}
    want = json.dumps(hooks, indent=2, ensure_ascii=False) + "\n"
    have, plug = dsh_alt_installed()
    spec = "file:" + DSH_BUNDLE_DIR.replace("\\", "/")
    if not on:
        if os.path.exists(DSH_HOOKS):
            os.remove(DSH_HOOKS)
        ok("DSH Desktop hooks 已移除" + (f"；组合包 {DSH_BUNDLE} 在 DSH Desktop 的插件管理里卸载" if have else ""))
        return True
    ver = dsh_alt_version()
    pkg = {"name": DSH_BUNDLE, "version": "1.0.0", "private": True,
           "description": "Agent Buddy：用 Claude Code hooks 兼容插件把 dsh 事件转发给本机 bridge",
           "dsh": {"bundle": {"patch": "./cordis.patch.yml"}},
           "dependencies": {DSH_PLUGIN: ver, DSH_PROTOCOL: ver}}  # 插件依赖的 dsh-hook-protocol 是 peerDependency，要显式带上同版本
    blk = dsh_block()
    files = {"package.json": json.dumps(pkg, indent=2, ensure_ascii=False) + "\n",
             "cordis.patch.yml": blk[blk.index("- insert:"):blk.rindex(DSH_MARK)]}
    os.makedirs(DSH_BUNDLE_DIR, exist_ok=True)
    for name, text in files.items():
        path = os.path.join(DSH_BUNDLE_DIR, name)
        try:
            with open(path, encoding="utf-8") as f:
                same = f.read() == text
        except OSError:
            same = False
        if not same:
            write_atomic(path, text)
    try:
        with open(DSH_HOOKS, encoding="utf-8") as f:
            same = f.read() == want
    except OSError:
        same = False
    if not same:
        write_atomic(DSH_HOOKS, want)
    if have and plug == ver:
        ok(f"DSH Desktop hooks：组合包已装（hooks 插件 {ver}）")
        return True
    why = f"内核已更新到 {ver}，组合包里还是 {plug}" if have else "组合包还没装"
    bad(f"DSH Desktop hooks：{why}。在 DSH Desktop 的插件管理里从本地路径安装（装过的重新装一次）：{spec}")
    return False


def dsh_desktop_version():
    """官方桌面版当前的内核版本。优先读安装目录里随版本更新的 runtime.json；
    ~/.dsh/dsh-runtimes 里那份是旧版本解出来的，桌面版自动更新后不会跟着变。"""
    runtime = os.path.dirname(os.path.dirname(os.path.dirname(DSH_DESKTOP_CLI)))  # .../resources/runtime
    for p in (os.path.join(runtime, "primary-runtime", "runtime.json"),
              os.path.join(DSH_HOME, "dsh-runtimes", "dsh-primary-runtime", "runtime.json")):
        try:
            with open(p, encoding="utf-8") as f:
                return json.load(f)["desktopVersion"]
        except (OSError, ValueError, KeyError):
            pass
    return None


def dsh_plugin_version(pkg):
    try:
        with open(os.path.join(DSH_HOME, "profiles", "desktop", "node_modules", *pkg.split("/"), "package.json"), encoding="utf-8") as f:
            return json.load(f).get("version")
    except (OSError, ValueError):
        return None


def dsh_desktop_plugin():
    """桌面版（Electron）没有打包 hooks 兼容插件（命令行版 dsh 带了）：用桌面版自带的命令行，按桌面版的版本装进它的 profile。
    桌面版自动更新后插件还是旧版本，和新内核对不上就加载不了（事件进不来）：版本不一致时也重装。
    返回 True 表示能用（装好了 / 没有桌面版）；装不上返回 False，这时不能挂插件，否则桌面版启动会报错。"""
    prof = os.path.join(DSH_HOME, "profiles", "desktop")
    if not os.path.isdir(prof):
        return True
    ver = dsh_desktop_version()
    # 插件依赖的 dsh-hook-protocol 是 peerDependency，pnpm 不装、桌面版也没带；不显式装同版本的话，
    # Node 会往上找到 ~/.dsh/profiles/node_modules 里旧版本的那一份，导入失败，插件显示「未运行」
    need = [p for p in (DSH_PLUGIN, DSH_PROTOCOL) if not dsh_plugin_version(p) or (ver and dsh_plugin_version(p) != ver)]
    if not need:
        return True
    if not ver or not os.path.exists(DSH_DESKTOP_CLI):
        bad("DeepSeek Harness 桌面版：找不到它自带的命令行，装不了 hooks 插件")
        return False
    info(f"DeepSeek Harness 桌面版：安装 {'、'.join(need)}@{ver}（桌面版没有自带）")
    r = subprocess.run([DSH_DESKTOP_CLI, "plugin", "--profile", "desktop", "add", *(f"{p}@{ver}" for p in need)], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", creationflags=NO_WINDOW if IS_WIN else 0)
    if r.returncode:
        bad("安装失败：" + (r.stderr or r.stdout).strip().splitlines()[-1][:160])
        return False
    return True


def dsh_patch_files():
    """要挂插件的配置层：每个 profile 自己的 cordis.patch.yml（~/.dsh/profiles/*/）。
    桌面版只读 profile 自己这一层，不读 ~/.dsh/cordis.patch.yml，所以不能只写全局那一份；
    还没有任何 profile（只用 npx 跑过）时退回全局那一份。"""
    root = os.path.join(DSH_HOME, "profiles")
    profs = sorted(d for d in (os.listdir(root) if os.path.isdir(root) else [])
                   if os.path.exists(os.path.join(root, d, "package.json"))
                   and not (dsh_alt_version() and os.path.join(root, d) == DSH_ALT_PROFILE))  # web 归 DSH Desktop，由组合包挂插件
    return [os.path.join(root, d, "cordis.patch.yml") for d in profs] or [DSH_PATCH]


def dsh_patch_edit(path, on):
    """在一个 cordis.patch.yml 末尾加上 / 去掉本项目那一段；返回 (改了没有, 备份文件)。"""
    try:
        with open(path, encoding="utf-8-sig") as f:
            text = f.read()
    except FileNotFoundError:
        text = ""
    new = re.sub(rf"\n?{re.escape(DSH_MARK)}：.*?{re.escape(DSH_MARK)} 结束 ----\n?", "", text, flags=re.S)
    if on:
        new = (new.rstrip("\n") + "\n" if new.strip() else "") + dsh_block()
    if new == text:
        return False, None
    bk = backup(path, "install" if on else "uninstall")
    if new.strip():
        write_atomic(path, new)
    elif os.path.exists(path):
        os.remove(path)
    return True, bk


def deepseek_hooks(on):
    """DeepSeek Harness（dsh）：写一份 Claude Code 格式的 hooks.json，并在每个 profile 的 cordis.patch.yml 末尾挂上
    dsh-hooks-claude-code 插件指向它。没装 dsh 就跳过。前后用标记行包起来，移除时只删这一段。"""
    desktop = os.path.exists(DSH_DESKTOP_CLI)
    if not os.path.isdir(DSH_HOME) and not shutil.which("dsh") and not desktop:
        info("没装 DeepSeek Harness（dsh），跳过")
        return True
    alt_ok = dsh_alt_hooks(on) if dsh_alt_version() else True  # DSH Desktop（web profile）走组合包
    if not desktop and not shutil.which("dsh") and dsh_alt_version():
        return alt_ok                                          # 只有 DSH Desktop，没有官方桌面版 / 命令行版
    return deepseek_hooks_official(on) and alt_ok


def deepseek_hooks_official(on):
    """官方桌面版（desktop profile）和命令行版 dsh：装插件，在每个 profile 的 cordis.patch.yml 挂上插件。"""
    desktop = os.path.exists(DSH_DESKTOP_CLI)
    # 桌面版第一次打开时才建 ~/.dsh/profiles/desktop；没打开过就写不进它读的那一层，写全局的它又不读
    unopened = on and desktop and not os.path.isdir(os.path.join(DSH_HOME, "profiles", "desktop"))
    if unopened and not os.path.isdir(os.path.join(DSH_HOME, "profiles")):
        bad("DeepSeek Harness 桌面版还没打开过：先打开一次，再运行 buddy hooks on")
        return False
    if on and not dsh_desktop_plugin():
        return False
    targets = dsh_patch_files()
    changed, backed = [], False
    for path in targets:
        done, bk = dsh_patch_edit(path, on)
        if done:
            changed.append(path)
            backed = backed or bool(bk)
    if DSH_PATCH not in targets:  # 旧版本写在全局的那一份：去掉，免得命令行版挂两次
        done, bk = dsh_patch_edit(DSH_PATCH, False)
        if done:
            changed.append(DSH_PATCH)
            backed = backed or bool(bk)
    hooks = {"hooks": {ev: [{"hooks": [{"type": "command", "command": dsh_hook_cmd(), "timeout": 5}]}] for ev in DSH_EVENTS}}
    want = json.dumps(hooks, indent=2, ensure_ascii=False) + "\n"
    try:
        with open(DSH_HOOKS, encoding="utf-8") as f:
            have = f.read()
    except FileNotFoundError:
        have = None
    if not changed and (have == want if on else have is None):
        ok("DeepSeek Harness hooks " + ("已是最新" if on else "本来就没有"))
        if unopened:
            bad("DeepSeek Harness 桌面版还没打开过：先打开一次，再运行 buddy hooks on")
        return not unopened
    if on and have != want:
        write_atomic(DSH_HOOKS, want)
    elif not on and have is not None:
        os.remove(DSH_HOOKS)
    where = "、".join(os.path.basename(os.path.dirname(p)) if p != DSH_PATCH else "全局" for p in changed)
    ok(f"DeepSeek Harness hooks 已{'写入' if on else '移除'}" + (f"（{where}" + ("，原文件已备份" if backed else "") + "）" if where else "")
       + ("；完全退出 dsh 再打开生效" if on else ""))
    if unopened:
        bad("DeepSeek Harness 桌面版还没打开过：先打开一次，再运行 buddy hooks on")
    return not unopened


# ---------------- 状态 ----------------
def status():
    out("Agent Buddy 状态")
    pid = running_pid()
    (ok if pid else bad)(f"bridge 进程：{'运行中（pid %s）' % pid if pid else '未运行（buddy start）'}")
    (ok if autostart_enabled() else info)(f"开机自启：{'开' if autostart_enabled() else '关（buddy autostart on）'}")
    st = bridge_status() if pid else None
    if st:
        kind, wifi = st.get("link"), st.get("wifi")
        if kind == "usb":
            ok("连接：USB" + (f"（Wi-Fi 备用，板子 {wifi}）" if wifi else ""))
        elif kind == "wifi":
            ok(f"连接：Wi-Fi（板子 {wifi}）")
        elif kind == "standby":
            info(f"连接：Wi-Fi 待命（板子 {wifi} 正在显示别的电脑；可在板子 设置 → 电脑 里设为首选）")
        elif "link" in st:
            bad("连接：没连上板子（插 USB 线，或确认板子已连 Wi-Fi 并配对过）")
        else:  # 旧 bridge 没有 link 字段
            (ok if st.get("serial") else bad)("串口：" + ("已连接板子" if st.get("serial") else "没连上板子（检查 USB 线）"))
        d = st.get("display") or {}
        info(f"屏幕状态：{d.get('label')} · {d.get('detail')}")
        u = st.get("usage") or {}
        if "h5" in u:
            info(f"Claude 额度：5 小时剩 {100 - u['h5']:.0f}% · 7 天剩 {100 - u.get('d7', 0):.0f}%")
    elif not pid:
        port = find_port()
        (ok if port else bad)(f"板子：{port or '没找到（USB-C 数据线连电脑）'}")
        if port:
            fw = probe(port)
            man = fw_bundle() or {}
            if fw and not str(fw).startswith("error:"):
                newer = ver_tuple(man.get("version")) > ver_tuple(fw)
                (info if newer else ok)(f"板子固件：{fw}" + (f"（firmware/ 有更新的 {man['version']}，buddy flash）" if newer else ""))
            else:
                info("板子固件：识别不出（出厂固件或旧版本，buddy flash 烧录）")
    try:
        with open(CLAUDE_SETTINGS, encoding="utf-8-sig") as f:  # 有的编辑器会存成带 BOM 的 UTF-8
            cs = json.load(f)
        n = sum(1 for ev in CLAUDE_EVENTS for g in (cs.get("hooks") or {}).get(ev, []) for h in g.get("hooks", [])
                if h.get("command") == hook_cmd())
        (ok if n == len(CLAUDE_EVENTS) else bad)(f"Claude Code hooks：{n}/{len(CLAUDE_EVENTS)}" + ("" if n == len(CLAUDE_EVENTS) else "（buddy hooks on）"))
    except (OSError, ValueError):
        bad("Claude Code hooks：没有 ~/.claude/settings.json（buddy hooks on）")
    if os.path.isdir(CODEX_DIR):
        try:
            with open(CODEX_CONFIG, "rb") as f:
                cc = tomllib.load(f)
            n = sum(1 for ev in CODEX_EVENTS for g in (cc.get("hooks") or {}).get(ev, []) for h in g.get("hooks", [])
                    if h.get("command") == hook_cmd("codex"))
            (ok if n == len(CODEX_EVENTS) else bad)(f"Codex hooks：{n}/{len(CODEX_EVENTS)}")
        except (OSError, tomllib.TOMLDecodeError):
            bad("Codex hooks：读不了 ~/.codex/config.toml")
    alt = dsh_alt_version()
    if alt:
        have, plug = dsh_alt_installed()
        if not have:
            bad(f"DSH Desktop hooks：组合包没装（buddy hooks on 后在插件管理里装 file:{DSH_BUNDLE_DIR}）")
        elif plug != alt:
            bad(f"DSH Desktop 内核已更新到 {alt}，组合包里 hooks 插件还是 {plug}（buddy hooks on 后在插件管理里重新装）")
        else:
            (ok if os.path.exists(DSH_HOOKS) else bad)(f"DSH Desktop hooks：" + ("组合包已装，插件 " + alt if os.path.exists(DSH_HOOKS)
                                                                             else "缺 ~/.dsh/agent-buddy-hooks.json（buddy hooks on）"))
    if os.path.exists(DSH_DESKTOP_CLI) or shutil.which("dsh") or (os.path.isdir(DSH_HOME) and not alt):  # 官方桌面版 / 命令行版
        hooked = os.path.exists(DSH_HOOKS)
        for path in dsh_patch_files():
            try:
                with open(path, encoding="utf-8-sig") as f:
                    hooked = hooked and DSH_MARK in f.read()
            except OSError:
                hooked = False
        if os.path.exists(DSH_DESKTOP_CLI) and not os.path.isdir(os.path.join(DSH_HOME, "profiles", "desktop")):
            bad("DeepSeek Harness 桌面版：还没打开过（打开一次后运行 buddy hooks on）")
        elif os.path.exists(DSH_DESKTOP_CLI) and not all(dsh_plugin_version(p) for p in (DSH_PLUGIN, DSH_PROTOCOL)):
            bad("DeepSeek Harness 桌面版：缺 hooks 插件（buddy hooks on）")
        elif os.path.exists(DSH_DESKTOP_CLI) and dsh_desktop_version() and any(
                dsh_plugin_version(p) != dsh_desktop_version() for p in (DSH_PLUGIN, DSH_PROTOCOL)):
            bad(f"DeepSeek Harness 桌面版已更新到 {dsh_desktop_version()}，hooks 插件还是 {dsh_plugin_version(DSH_PLUGIN)}，"
                "加载不了（buddy hooks on 更新，再重启桌面版）")
        (ok if hooked else bad)("DeepSeek Harness hooks：" + ("已注册" if hooked else "没有（buddy hooks on）"))
    if alt or os.path.isdir(DSH_HOME) or shutil.which("dsh") or os.path.exists(DSH_DESKTOP_CLI):
        key = os.environ.get("DEEPSEEK_API_KEY") or os.path.exists(os.path.join(STATE, "deepseek_key"))
        info("DeepSeek 余额 / 用量：从 magpie 读" + ("；没开 magpie 时用 API Key 查余额" if key else "（没开 magpie 时可设 DEEPSEEK_API_KEY 查余额）"))
    try:
        urllib.request.urlopen("http://127.0.0.1:3425/v1/magpie/quotas/cards", timeout=2).read()
        ok("magpie：在线（额度 / 用量 / 路由页有数据）")
    except Exception:  # noqa: BLE001
        info("magpie：没在运行（可选，只影响 magpie 页和 Codex 额度）")
    feats = []
    if host.idle() is None:
        feats.append("键鼠空闲")
    if host.key_count() is None:
        feats.append("打字检测")
    if feats:
        info(f"这台电脑不支持：{'、'.join(feats)}（其余功能正常）")
    if not shutil.which("ffmpeg") and not (IS_MAC and os.path.exists("/opt/homebrew/bin/ffmpeg")):
        info("没装 ffmpeg：录屏不可用（截图可用）")
    info(f"日志：{LOG_FILE}")


# ---------------- 一键配置 ----------------
def setup(a):
    out("Agent Buddy 一键配置" + ("（Windows）" if IS_WIN else "（macOS）" if IS_MAC else ""))
    step(1, "检查 Python 环境")
    try:
        import esptool  # noqa: F401
        import numpy  # noqa: F401
        import PIL  # noqa: F401
        import serial  # noqa: F401
        ok(f"Python {sys.version.split()[0]}，依赖齐全")
    except ImportError as e:
        bad(f"缺少依赖 {e.name}：重新运行项目根目录的 setup")
        return 1

    step(2, "注册 Claude Code / Codex / DeepSeek Harness 的 hooks（修改前自动备份）")
    hooks_ok = claude_hooks(True) & codex_hooks(True) & deepseek_hooks(True)

    step(3, "连接板子")
    if running_pid():
        stop(quiet=True)
        time.sleep(1)
    port = find_port()
    if not port:
        out("  请用 USB-C 数据线把板子连到电脑（等待 60 秒）…")
        port = wait_port(60)
    if not port:
        bad("没找到板子，先跳过烧录；连上后运行 buddy flash")
    else:
        ok(f"找到板子：{port}")
        fw = probe(port)
        man = fw_bundle() or {}
        want = a.flash
        if fw and str(fw).startswith("error:"):
            bad(f"串口打不开（{fw[6:]}）：关掉占用它的程序（Arduino IDE 串口监视器等）")
            fw = None
        if want is None:
            if fw:
                newer = ver_tuple(man.get("version")) > ver_tuple(fw)
                info(f"板子上已经是 Agent Buddy 固件 {fw}" + (f"，有更新的 {man.get('version')}" if newer else "，跳过烧录"))
                want = newer and ask(f"升级到 {man.get('version')}？（板子上的设置会保留）")
            else:
                info("板子上不是 Agent Buddy 固件（新板子 / 出厂固件）")
                want = ask(f"烧录 Agent Buddy 固件 {man.get('version', '')}？")
        if want:
            step("3b", "烧录固件")
            if not flash(port, erase=a.erase):
                return 1
            time.sleep(3)

    step(4, "开机自启")
    want = a.autostart if a.autostart is not None else ask("登录后自动运行 bridge？", True)
    if want:
        autostart(True)
    elif autostart_enabled():
        autostart(False)
    else:
        info("不开机自启：需要时运行 buddy start")

    step(5, "启动并自检")
    if not start():
        return 1
    st = None
    for _ in range(20):
        st = bridge_status()
        if st and st.get("serial"):
            break
        time.sleep(0.5)
    if st and st.get("serial"):
        ok("bridge 已连上板子")
        try:
            http("POST", "/back")
            ok("已发一条测试通知：屏幕上应该弹出「欢迎回来」")
        except Exception:  # noqa: BLE001
            pass
    else:
        bad("bridge 没连上板子：检查 USB 线，或运行 buddy status 查看")

    if not hooks_ok:
        bad("hooks 没注册成功（见第 2 步的提示）：改好配置文件后再运行一次 setup，否则屏幕收不到 Claude Code / Codex / DeepSeek 的状态")
    out("\n完成。接下来：")
    out("  1. 重启已经打开的 Claude Code / Codex / dsh，让 hooks 生效（Codex 提示信任新 hook 时同意）")
    out("  2. 在板子的设置页可以调声音、连 Wi-Fi、标定雷达座位；屏幕颜色或方向不对见 docs/使用手册.md「屏幕冷启动校准」")
    out("  3. 可选：装 magpie（额度 / 用量 / 路由页的数据来源）")
    tool = r"bridge\buddy" if IS_WIN else "bridge/buddy"
    out(f"  常用命令：{tool} status | start | stop | logs | flash")
    return 0


def uninstall():
    stop(quiet=True)
    autostart(False)
    claude_hooks(False)
    codex_hooks(False)
    deepseek_hooks(False)
    ok("已卸载（保留 Python 环境、日志与录屏；删掉整个项目目录即可彻底清除）")


def record(seconds=None, snap=False):
    try:
        http("POST", "/snap" if snap else f"/record?seconds={seconds}")
    except Exception:  # noqa: BLE001
        bad("bridge 未运行")
        return 1
    if not snap:
        out(f"录制中，{seconds} 秒……")
    while True:
        st = json.loads(http("GET", "/record"))
        if st.get("status") in ("done", "error"):
            out(json.dumps(st, ensure_ascii=False))
            return 0 if st["status"] == "done" else 1
        time.sleep(0.3 if snap else 1)


def main():
    global ASSUME_YES
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except (OSError, ValueError):
            pass
    p = argparse.ArgumentParser(prog="buddy", description="Agent Buddy 管理工具")
    p.add_argument("-y", "--yes", action="store_true", help="所有询问都用默认答案")
    sub = p.add_subparsers(dest="cmd")
    s = sub.add_parser("setup", help="一键配置")
    s.add_argument("-y", "--yes", action="store_true", default=argparse.SUPPRESS, help="所有询问都用默认答案")
    s.add_argument("--flash", dest="flash", action="store_true", default=None, help="一定烧录固件")
    s.add_argument("--no-flash", dest="flash", action="store_false", help="不烧录固件")
    s.add_argument("--erase", action="store_true", help="烧录前整片擦除（清空板子上的设置）")
    s.add_argument("--autostart", dest="autostart", action="store_true", default=None)
    s.add_argument("--no-autostart", dest="autostart", action="store_false")
    for name in ("start", "stop", "restart", "status", "build", "uninstall", "snap", "supervise"):
        sub.add_parser(name)
    lg = sub.add_parser("logs")
    lg.add_argument("-f", "--follow", action="store_true")
    lg.add_argument("-n", type=int, default=50)
    au = sub.add_parser("autostart")
    au.add_argument("onoff", choices=["on", "off"])
    hk = sub.add_parser("hooks")
    hk.add_argument("onoff", choices=["on", "off"])
    fl = sub.add_parser("flash")
    fl.add_argument("--build", action="store_true", help="先用 arduino-cli 编译当前代码")
    fl.add_argument("--erase", action="store_true", help="先整片擦除（清空板子上的设置、Wi-Fi）")
    fl.add_argument("--port")
    rc = sub.add_parser("record")
    rc.add_argument("seconds", nargs="?", type=float, default=15)
    pr = sub.add_parser("pair")
    pr.add_argument("--wifi", action="store_true", help="不插 USB，用屏幕验证码配对")
    gm = sub.add_parser("gamma")
    gm.add_argument("value", nargs="?", type=float, default=1.6)
    a = p.parse_args()
    ASSUME_YES = a.yes
    if a.cmd != "supervise":
        migrate_old_name()
    if a.cmd == "setup":
        return setup(a)
    if a.cmd == "start":
        return 0 if start() else 1
    if a.cmd == "stop":
        return 0 if stop() else 1
    if a.cmd == "restart":
        stop(quiet=True)
        return 0 if start() else 1
    if a.cmd == "status":
        status()
        return 0
    if a.cmd == "logs":
        tail(a.n, a.follow)
        return 0
    if a.cmd == "autostart":
        return 0 if autostart(a.onoff == "on") else 1
    if a.cmd == "hooks":
        on = a.onoff == "on"
        return 0 if claude_hooks(on) & codex_hooks(on) & deepseek_hooks(on) else 1
    if a.cmd == "flash":
        return 0 if flash(a.port, a.build, a.erase) else 1
    if a.cmd == "build":
        return 0 if build_fw() else 1
    if a.cmd == "uninstall":
        uninstall()
        return 0
    if a.cmd == "record":
        return record(min(max(a.seconds, 1), 300))
    if a.cmd == "snap":
        return record(snap=True)
    if a.cmd == "gamma":
        try:
            out(http("POST", f"/gamma?g={a.value}"))
        except Exception:  # noqa: BLE001
            bad("bridge 未运行或串口未连接")
            return 1
        return 0
    if a.cmd == "pair":
        if not a.wifi:
            info("插着 USB 启动 bridge 时会自动配对；没有 USB 时用 buddy pair --wifi")
            return 0
        r = subprocess.run([PY, "-c", "import sys, agent_bridge; sys.exit(agent_bridge.wifi_pair_cli())"], cwd=BRIDGE).returncode
        if r == 0 and running_pid():  # 换了密钥：重启 bridge 让它读新的 device.json
            stop(quiet=True)
            start()
        return r
    if a.cmd == "supervise":
        supervise()
        return 0
    p.print_help()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except BrokenPipeError:  # 输出接到 head 之类提前关掉的管道
        os._exit(0)
