"""把一首配乐按节拍延长到成片长度：在节拍网格上找两处听感最像的小节，把中间这段再放一遍，
原曲结尾正好落在片尾；最后统一响度到 -14 LUFS。

bridge/.venv/bin/python promo/music_fit.py promo/music/mixkit-midnight-funk.mp3 [目标秒数] [输出文件名]  → promo/music.wav
曲子来自 Mixkit（Stock Music Free License：可用于网络视频、免署名；不可单独发行、不可登记版权管理系统）。
"""
import os
import subprocess
import sys

import numpy as np

SR = 44100
HERE = os.path.dirname(os.path.abspath(__file__))


def decode(path):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ac', '2', '-ar', str(SR), '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, 2).copy()


def onset_env(mono, hop=441):
    n = len(mono) // hop
    fr = mono[:n * hop].reshape(n, hop)
    spec = np.log1p(np.abs(np.fft.rfft(fr * np.hanning(hop), axis=1)))
    return np.maximum(0, np.diff(spec, axis=0)).sum(1), SR / hop


def beat_grid(mono):
    """精确到 0.05 BPM 的速度 + 相位：让梳状脉冲和起音包络重合得最好"""
    env, fps = onset_env(mono)
    env = env - env.mean()
    t = np.arange(len(env)) / fps
    best = (-1e9, 0, 0)
    for bpm in np.arange(100, 135, 0.05):
        period = 60 / bpm
        for ph in np.linspace(0, period, 24, endpoint=False):
            idx = np.round((np.arange(ph, t[-1], period)) * fps).astype(int)
            s = env[idx].mean()
            if s > best[0]:
                best = (s, bpm, ph)
    return best[1], best[2]


def beat_features(mono, beats):
    """每拍一个特征：24 个对数频带的平均能量"""
    feats = []
    edges = np.geomspace(40, 12000, 25)
    for a, b in zip(beats[:-1], beats[1:]):
        seg = mono[int(a * SR):int(b * SR)]
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        f = np.fft.rfftfreq(len(seg), 1 / SR)
        feats.append([np.log1p(spec[(f >= lo) & (f < hi)].mean()) for lo, hi in zip(edges[:-1], edges[1:])])
    F = np.array(feats)
    return (F - F.mean(0)) / (F.std(0) + 1e-9)


def beats_of(x, ph, period):
    return np.arange(ph, len(x) / SR, period)


def extend(x, beats, period, jump):
    """在 j 拍处跳回 j - jump 拍，把中间这段再放一遍；j 选前后 4 小节听感最像的位置"""
    mono = x.mean(1)
    F = beat_features(mono, beats)
    win = 16
    best = (-1e9, 0)
    for j in range(jump + win, len(F) - win - 32):   # 离结尾至少 8 小节
        i = j - jump
        if beats[i] < 12:   # 不跳回前奏
            continue
        a, b = F[i - win:i + win], F[j - win:j + win]
        s = np.sum(a * b) / (np.linalg.norm(a) * np.linalg.norm(b))
        if s > best[0]:
            best = (s, j)
    s, j = best
    ti, tj = beats[j - jump], beats[j]
    print(f'  在 {tj:.2f}s 处跳回 {ti:.2f}s（{jump // 4} 小节），相似度 {s:.3f}')
    xf = int(0.03 * SR)   # 30ms 交叉淡化，听不出接缝
    a = x[:int(tj * SR) + xf].copy()
    b = x[int(ti * SR):].copy()
    ramp = np.linspace(0, 1, xf)[:, None]
    a[-xf:] = a[-xf:] * (1 - ramp) + b[:xf] * ramp
    return np.concatenate([a, b[xf:]])


def main():
    src = sys.argv[1]
    target = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
    x = decode(src)
    # 去掉开头的静音
    lead = int(np.argmax(np.abs(x).max(1) > 1e-3))
    x = x[lead:]
    mono = x.mean(1)
    dur = len(x) / SR
    bpm, ph = beat_grid(mono)
    period = 60 / bpm
    beats = np.arange(ph, dur, period)
    print(f'{bpm:.2f} BPM，原长 {dur:.1f}s')
    # 分几次延长，每次跳回 12 小节：跳得短，前后更容易找到听感一样的位置
    y = x
    while len(y) / SR < target - 2 * period * 4:
        jump = min(48, int(round((target - len(y) / SR) / period / 4)) * 4)
        y = extend(y, beats_of(y, ph, period), period, jump)
    n = int(target * SR)
    if len(y) > n:  # 多出来的部分从开头扣，保证原曲结尾落在片尾；裁的位置对齐到最近的小节线，进来就在拍子上
        cut = (len(y) - n) / SR
        if cut > 2:
            cut = ph + round((cut - ph) / (4 * period)) * 4 * period
        y = y[int(cut * SR):]
        y = y[:n] if len(y) > n else np.concatenate([y, np.zeros((n - len(y), 2), np.float32)])
        print(f'  从 {cut:.2f}s 开始用')
        fi = int(0.5 * SR)
        y[:fi] *= np.linspace(0, 1, fi)[:, None]
    else:
        y = np.concatenate([np.zeros((n - len(y), 2), np.float32), y])
    tmp = os.path.join(HERE, 'music_raw.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'f32le', '-ar', str(SR), '-ac', '2', '-i', '-', tmp],
                   input=y.astype(np.float32).tobytes(), check=True)
    out = os.path.join(HERE, sys.argv[3] if len(sys.argv) > 3 else 'music.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', tmp, '-af', 'loudnorm=I=-14:TP=-1.5:LRA=11',
                    '-ar', '48000', '-c:a', 'pcm_s16le', out], check=True)
    os.remove(tmp)
    print('→', out, f'{len(y) / SR:.1f}s')


if __name__ == '__main__':
    main()
