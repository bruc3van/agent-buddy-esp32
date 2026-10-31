#!/bin/sh
# 截图：./snap.sh，输出到项目 recordings/ 目录（PNG）
curl -sf -X POST "http://127.0.0.1:47823/snap" >/dev/null || { echo "bridge 未运行"; exit 1; }
while :; do
  st=$(curl -sf http://127.0.0.1:47823/record) || { echo "bridge 已退出"; exit 1; }
  case "$st" in *'"done"'*|*'"error"'*) echo "$st"; break ;; esac
  sleep 0.3
done
