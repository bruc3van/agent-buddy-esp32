#!/bin/zsh
# 按顺序渲染 edit.py 用到的全部 3D 镜头（已渲染完的跳过；要重渲某个镜头就删掉 promo/render/<镜头>/.done）
cd "$(dirname "$0")/.."
B=/Applications/Blender.app/Contents/MacOS/Blender
failed=()
for s in opener opener_full assemble back side turn lowfront finale f_buddy_claude f_buddy_codex f_buddy_deepseek f_home_split f_magpie_usage f_magpie_route f_radar f_standby f_welcome_back; do
  [ -f promo/render/$s/.done ] && continue
  echo "== $s $(date +%T)"
  # --python-exit-code：scene.py 抛异常时 Blender 也返回非 0；只有渲染成功才写完成标记，失败的镜头下次重跑
  $B -b --factory-startup --python-exit-code 1 --python promo/scene.py -- $s 2>&1 | grep -E "Error|Traceback"
  rc=${pipestatus[1]}
  if (( rc == 0 )); then touch promo/render/$s/.done; else echo "!! $s 渲染失败（退出码 $rc）"; failed+=($s); fi
done
(( ${#failed} )) && { echo "渲染失败的镜头：${failed[*]}"; exit 1; }
echo ALL DONE $(date +%T)
