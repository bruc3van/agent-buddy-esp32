#!/bin/sh
# Agent Buddy 一键配置（macOS）：装 uv → 建 Python 环境 → 运行 bridge/buddy.py setup
# 参数原样传过去：--yes（全部用默认答案）、--flash / --no-flash、--autostart / --no-autostart、--erase
set -e
cd "$(dirname "$0")"
if ! command -v uv >/dev/null 2>&1; then
  if command -v brew >/dev/null 2>&1; then
    brew install uv
  else
    echo "需要 uv（Python 环境管理工具），从官方脚本安装：https://astral.sh/uv/install.sh"
    curl -LsSf https://astral.sh/uv/install.sh | sh
    PATH="$HOME/.local/bin:$PATH"
  fi
fi
[ -x bridge/.venv/bin/python ] || uv venv -q --python 3.12 bridge/.venv
uv pip install -q --python bridge/.venv/bin/python -r bridge/requirements.txt
chmod +x bridge/*.sh bridge/buddy
exec bridge/.venv/bin/python bridge/buddy.py setup "$@"
