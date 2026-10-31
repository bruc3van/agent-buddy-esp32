#!/bin/sh
# 录制屏幕：./record.sh [秒数，默认 15]，输出到项目 recordings/ 目录（MP4 + GIF）
S=${1:-15}
curl -sf -X POST "http://127.0.0.1:47823/record?seconds=$S" >/dev/null || { echo "bridge 未运行"; exit 1; }
echo "录制中，${S} 秒……"
while :; do
  st=$(curl -sf http://127.0.0.1:47823/record) || { echo "bridge 已退出"; exit 1; }
  case "$st" in *'"done"'*|*'"error"'*) echo "$st"; break ;; esac
  sleep 1
done
