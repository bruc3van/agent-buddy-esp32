# Agent Buddy 一键配置（Windows）：装 uv → 建 Python 环境 → 运行 bridge\buddy.py setup
# 参数原样传过去：--yes（全部用默认答案）、--flash / --no-flash、--autostart / --no-autostart、--erase
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    Write-Host "需要 uv（Python 环境管理工具），从官方脚本安装：https://astral.sh/uv/install.ps1"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://astral.sh/uv/install.ps1 | iex"
    $env:Path = "$env:USERPROFILE\.local\bin;$env:Path"
    if (-not (Get-Command uv -ErrorAction SilentlyContinue)) { Write-Host "uv 安装失败"; exit 1 }
}
$py = Join-Path $PSScriptRoot "bridge\.venv\Scripts\python.exe"
if (-not (Test-Path $py)) {
    uv venv -q --python 3.12 bridge\.venv
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
uv pip install -q --python $py -r bridge\requirements.txt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $py bridge\buddy.py setup @args
exit $LASTEXITCODE
