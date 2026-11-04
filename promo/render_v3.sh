#!/bin/zsh
# 修三处瑕疵后重渲全部 3D 镜头并重新出片：
# 屏幕模组正面和玻璃重合导致黑边闪（模组后移）、开窗倒角顶光亮带（黑 PLA 哑光、关光追）、双色定格颜色对齐预览图
cd "$(dirname "$0")/.."
B=/Applications/Blender.app/Contents/MacOS/Blender
failed=()
for s in duo assemble side back opener turn lowfront finale hero f_home_split f_radar f_standby f_welcome_back \
         f_buddy_claude f_buddy_codex f_buddy_deepseek f_magpie_route opener_full f_magpie_usage; do
  [ -f promo/render/$s/.v3 ] && continue
  echo "== $s $(date +%T)"
  # --python-exit-code：scene.py 抛异常时 Blender 也返回非 0；只有渲染成功才写完成标记，失败的镜头下次重跑
  $B -b --factory-startup --python-exit-code 1 --python promo/scene.py -- $s 2>&1 | grep -E "Error|Traceback"
  rc=${pipestatus[1]}
  if (( rc == 0 )); then touch promo/render/$s/.v3; else echo "!! $s 渲染失败（退出码 $rc）"; failed+=($s); fi
done
(( ${#failed} )) && { echo "渲染失败的镜头：${failed[*]}"; exit 1; }
echo RENDER DONE $(date +%T)
bridge/.venv/bin/python - <<'PY'
import sys; sys.path.insert(0, 'promo'); sys.argv = ['x']
import edit
f = edit.seg_3d('hero', wordmark=(1.0, 7.6, '放在桌上的 AI 编程搭子', 805))
f(5.5, 8).convert('RGB').save('promo/cover.png')
PY
for c in diy short full; do bridge/.venv/bin/python promo/edit.py --cut $c 2>&1 | tail -1; done
echo ALL DONE $(date +%T)
