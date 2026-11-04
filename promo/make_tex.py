"""屏幕录像 → 3D 贴图：统一转 30fps，含邮箱的区域打马赛克。输出 promo/tex/<名>.mp4"""
import glob
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# 片段名: [(x, y, w, h)]，960×544 坐标
MASKS = {
    "buddy_codex": [(676, 312, 258, 38)],
    "magpie_quota": [(36, 168, 370, 48), (36, 396, 370, 48)],
    "magpie_route": [(160, 400, 266, 30), (160, 500, 266, 30)],  # 每条请求下面的账号邮箱
    "settings_conn": [(230, 160, 230, 104), (230, 516, 330, 28)],  # 电脑名、底部露出的 Wi-Fi 名
}
TRIM = {"standby": 5.6}  # 片头还是上一页（切到待机时钟有延迟），从这里开始
# 只留这么长：雷达录到 4 秒多以后板子发帧变慢、画面会顿；扫描正好 4 秒一圈，截 4 秒循环播放也接得上
LENGTH = {"radar": 4.0}


def convert(src, dst, rects, trim=0.0, length=None):
    fc = "[0:v]fps=30"
    last = "base"
    if rects:
        fc += f",split={len(rects) + 1}[base]" + "".join(f"[s{i}]" for i in range(len(rects))) + ";"
        for i, (x, y, w, h) in enumerate(rects):
            fc += (f"[s{i}]crop={w}:{h}:{x}:{y},scale=iw/12:ih/12,scale={w}:{h}:flags=neighbor[p{i}];"
                   f"[{last}][p{i}]overlay={x}:{y}[o{i}];")
            last = f"o{i}"
        fc = fc.rstrip(";")
    else:
        fc += "[base]"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", str(trim), *(["-t", str(length)] if length else []), "-i", src, "-filter_complex", fc, "-map", f"[{last}]",
                    "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", dst], check=True)


if __name__ == "__main__":
    os.makedirs(os.path.join(HERE, "tex"), exist_ok=True)
    for src in sorted(glob.glob(os.path.join(HERE, "screens", "*.mp4"))):
        name = os.path.splitext(os.path.basename(src))[0]
        convert(src, os.path.join(HERE, "tex", name + ".mp4"), MASKS.get(name, []), TRIM.get(name, 0.0),
                LENGTH.get(name))
        print(name)
