#!/bin/zsh
# 按顺序渲染 edit.py 用到的全部 3D 镜头（已渲染完的跳过；要重渲某个镜头就删掉 promo/render/<镜头>/.done）
cd "$(dirname "$0")/.."
B=/Applications/Blender.app/Contents/MacOS/Blender
for s in opener opener_full assemble back side turn lowfront finale f_buddy_claude f_buddy_codex f_buddy_deepseek f_home_split f_magpie_usage f_magpie_route f_radar f_standby f_welcome_back; do
  [ -f promo/render/$s/.done ] && continue
  echo "== $s $(date +%T)"
  $B -b --factory-startup --python promo/scene.py -- $s 2>&1 | grep -E "Error|Traceback" ; touch promo/render/$s/.done
done
echo ALL DONE $(date +%T)
