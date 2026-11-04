#!/bin/zsh
# 灯光跟着相机转（背景光斑不再扫动）、雷达贴图截成均匀的 4 秒：重渲受影响的镜头并重新出片
cd "$(dirname "$0")/.."
B=/Applications/Blender.app/Contents/MacOS/Blender
for s in turn opener opener_full hero lowfront side back assemble f_radar f_magpie_route; do
  [ -f promo/render/$s/.v5 ] && continue
  echo "== $s $(date +%T)"
  $B -b --factory-startup --python promo/scene.py -- $s 2>&1 | grep -E "Error|Traceback" ; touch promo/render/$s/.v5
done
echo RENDER DONE $(date +%T)
bridge/.venv/bin/python - <<'PY'
import sys; sys.path.insert(0, 'promo'); sys.argv = ['x']
import edit
f = edit.seg_3d('hero', wordmark=(1.0, 7.6, '放在桌上的 AI 编程搭子', 805))
f(5.5, 8).convert('RGB').save('promo/cover.png')
PY
for c in diy short full; do bridge/.venv/bin/python promo/edit.py --cut $c 2>&1 | tail -1; done
echo ALL DONE $(date +%T)
