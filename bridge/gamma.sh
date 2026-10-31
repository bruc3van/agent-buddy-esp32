#!/bin/sh
# 调屏幕颜色校正：./gamma.sh 1.6（数值越大颜色越深，1 = 关闭校正）
curl -s -X POST "http://127.0.0.1:47823/gamma?g=${1:-1.6}" || echo "bridge 未运行"; echo
