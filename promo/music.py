"""宣传片配乐：纯代码合成（无版权素材），120 BPM、D 大调、3 分钟，段落和剪辑节点对齐。

bridge/.venv/bin/python promo/music.py  → promo/music_synth.wav（48 kHz 立体声；备用，成片现用 music_fit.py）
"""
import os
import wave

import numpy as np

SR = 48000
BPM = 120
BEAT = 60 / BPM
BAR = 4 * BEAT
LEN = 180.0
N = int(SR * LEN)
rng = np.random.default_rng(7)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


# I – vi – IV – V（D – Bm – G – A），每个和弦 2 小节；pad 用开放排列
CHORDS = [
    (38, [62, 66, 69, 73, 76]),   # Dmaj9
    (35, [59, 62, 66, 69, 73]),   # Bm9
    (31, [59, 62, 67, 71, 74]),   # Gmaj7(9)
    (33, [57, 61, 64, 69, 71]),   # A6/9
]


def chord_at(t):
    return CHORDS[int(t / (2 * BAR)) % 4]


# ---------- 段落：每层的音量包络（秒，0~1），和剪辑对齐 ----------
def env(points):
    """[(t, v)] 折线包络 → 每个采样点的值"""
    ts, vs = zip(*points)
    return np.interp(np.arange(N) / SR, ts, vs)


PAD = env([(0, 0), (6, 0.55), (20, 0.6), (40, 0.5), (60, 0.55), (118, 0.55), (122, 0.8), (130, 0.6),
           (150, 0.75), (172, 0.75), (180, 0)])
PIANO = env([(0, 0.9), (20, 0.7), (60, 0.5), (118, 0.5), (122, 0.9), (130, 0.5), (172, 0.8), (180, 0.8)])
ARP = env([(0, 0), (11, 0), (14, 0.5), (20, 0.7), (118, 0.7), (120, 0), (130, 0), (132, 0.75), (172, 0.6), (176, 0)])
DRUMS = env([(0, 0), (19.9, 0), (20, 1), (118, 1), (118.1, 0), (130, 0), (130.01, 1), (172, 1), (172.1, 0), (180, 0)])
BASS = env([(0, 0), (19.9, 0), (20, 0.9), (118, 0.9), (120, 0), (130, 0), (130.01, 1), (172, 1), (176, 0)])
HATS = env([(0, 0), (40, 0), (40.1, 1), (118, 1), (118.1, 0), (132, 0), (132.1, 1), (170, 1), (170.1, 0), (180, 0)])

t_all = np.arange(N) / SR


def lowpass(x, cutoff, order=2):
    """频域低通（整段静态滤波，numpy 里最快）"""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X /= np.sqrt(1 + (f / cutoff) ** (2 * order))
    return np.fft.irfft(X, len(x))


def highpass(x, cutoff, order=2):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    f[0] = 1e-3
    X /= np.sqrt(1 + (cutoff / f) ** (2 * order))
    return np.fft.irfft(X, len(x))


def place(buf, start, sig, gain=1.0):
    i = int(start * SR)
    if i >= len(buf):
        return
    j = min(len(buf), i + len(sig))
    buf[i:j] += sig[: j - i] * gain


# ---------- 乐器 ----------
def saw(f, t, phase=0.0):
    x = (f * t + phase) % 1.0
    return 2 * x - 1


def pad_layer():
    out = np.zeros(N)
    seg = int(2 * BAR * SR)
    for k in range(0, N, seg):
        t0 = k / SR
        _, notes = chord_at(t0 + 0.01)
        n = min(seg + int(1.5 * SR), N - k)   # 和弦之间留 1.5 秒交叠，换和弦不会断
        t = np.arange(n) / SR
        e = np.minimum(1, t / 1.2) * np.clip((seg / SR + 1.5 - t) / 1.5, 0, 1)
        s = np.zeros(n)
        for m in notes:
            for d in (-0.08, 0.0, 0.07):  # 三层轻微失谐，变宽变暖
                s += saw(hz(m + d), t, rng.random())
        out[k:k + n] += s * e / (len(notes) * 3)
    out = lowpass(out, 1400, 2)
    lfo = 0.85 + 0.15 * np.sin(2 * np.pi * 0.07 * t_all)
    return out * lfo


def tone(f, dur, decay, harmonics=((1, 1.0), (2, 0.5), (3, 0.25), (4, 0.12), (5, 0.06))):
    """钢琴 / 钟琴感的叠加正弦：高次谐波衰减更快"""
    t = np.arange(int(dur * SR)) / SR
    s = np.zeros_like(t)
    for h, a in harmonics:
        s += a * np.sin(2 * np.pi * f * h * t * (1 + 0.0004 * h * h)) * np.exp(-t * decay * (0.6 + 0.4 * h))
    att = np.minimum(1, t / 0.004)
    return s * att


def piano_layer():
    out = np.zeros(N)
    # 主旋律：每 2 小节一句，前奏和尾声稀疏，副歌更密
    motif = [(0, 2), (1.5, 4), (3, 1), (5, 0), (6, 4)]   # (拍, 和弦音序号)
    t0 = 0.0
    while t0 < LEN - 2:
        _, notes = chord_at(t0 + 0.01)
        dense = 60 <= t0 < 118 or 130 <= t0 < 172
        for b, idx in motif if dense or int(t0 / (2 * BAR)) % 2 == 0 else motif[:2]:
            m = notes[idx] + 12
            place(out, t0 + b * BEAT, tone(hz(m), 3.5, 1.4), 0.28)
        t0 += 2 * BAR
    # 结尾一个长音
    place(out, 172.0, sum(tone(hz(m), 8, 0.5) for m in (62, 69, 74, 78)), 0.22)
    return out


def arp_layer():
    out = np.zeros(N)
    step = BEAT / 4
    k = 0
    while k * step < LEN:
        t0 = k * step
        _, notes = chord_at(t0 + 0.001)
        pat = [0, 2, 4, 2, 1, 3, 4, 3]
        m = notes[pat[k % 8]] + (12 if k % 16 >= 8 else 0)
        place(out, t0, tone(hz(m), 0.6, 9, ((1, 1), (2, 0.3), (3, 0.15))), 0.09 * (1.0 if k % 4 == 0 else 0.7))
        k += 1
    return lowpass(out, 5000)


def kick():
    t = np.arange(int(0.45 * SR)) / SR
    f = 45 + 85 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * np.exp(-t * 7) + 0.3 * np.sin(ph * 2) * np.exp(-t * 30)


def snare():
    t = np.arange(int(0.35 * SR)) / SR
    noise = highpass(rng.standard_normal(len(t)), 1200) * np.exp(-t * 18)
    body = np.sin(2 * np.pi * 190 * t) * np.exp(-t * 25)
    return 0.5 * noise + 0.5 * body


def hat():
    t = np.arange(int(0.08 * SR)) / SR
    return highpass(rng.standard_normal(len(t)), 7000, 4) * np.exp(-t * 60)


def drums():
    k, s, h = kick(), snare(), hat()
    kb, sb, hb = np.zeros(N), np.zeros(N), np.zeros(N)
    b = 0
    while b * BEAT < LEN:
        t0 = b * BEAT
        place(kb, t0, k, 0.9)
        if b % 4 in (1, 3):
            place(sb, t0, s, 0.35)
        place(hb, t0 + BEAT / 2, h, 0.22)
        place(hb, t0, h, 0.1)
        b += 1
    return kb, sb, hb


def bass_layer():
    out = np.zeros(N)
    step = BEAT / 2
    k = 0
    while k * step < LEN:
        t0 = k * step
        root, _ = chord_at(t0 + 0.001)
        t = np.arange(int(step * 0.95 * SR)) / SR
        f = hz(root)
        s = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(2 * np.pi * 2 * f * t)
        e = np.minimum(1, t / 0.01) * np.exp(-t * 3)
        place(out, t0, s * e, 0.45 if k % 2 == 0 else 0.3)
        k += 1
    return lowpass(out, 400)


def riser(t_end, dur):
    """转场前的白噪声上扬"""
    n = int(dur * SR)
    t = np.arange(n) / SR
    x = rng.standard_normal(n)
    x = highpass(x, 600) * (t / dur) ** 2.5
    return t_end - dur, x * 0.12


def reverb(x, seconds=3.2, seed=1):
    r = np.random.default_rng(seed)
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = r.standard_normal(n) * np.exp(-t * 6.9 / seconds)
    ir = lowpass(ir, 6000)
    ir[: int(0.02 * SR)] *= np.linspace(0, 1, int(0.02 * SR))
    ir /= np.sqrt(np.sum(ir ** 2))
    L = len(x) + n
    nfft = 1 << (L - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(ir, nfft), nfft)[: len(x)]
    return y


def main():
    pad = pad_layer() * PAD
    piano = piano_layer() * PIANO
    arp = arp_layer() * ARP
    kb, sb, hb = drums()
    bass = bass_layer() * BASS
    # 侧链：鼓点压一下 pad 和贝斯，律动更清楚
    duck = np.ones(N)
    b = 0
    while b * BEAT < LEN:
        i = int(b * BEAT * SR)
        n = min(int(0.3 * SR), N - i)
        duck[i:i + n] -= 0.35 * np.exp(-np.arange(n) / SR * 12) * DRUMS[i]
        b += 1
    pad *= duck
    bass *= duck
    fx = np.zeros(N)
    for te, d in ((20, 4), (60, 3), (130, 6), (150, 3)):
        s, x = riser(te, d)
        place(fx, s, x)
    # 冲击：段落开头一记低频 + 混响
    for te in (20, 60, 130, 150):
        place(fx, te, kick() * 0.8)

    dry_l = pad * 0.9 + piano * 0.8 + arp * 1.1 + bass + kb * DRUMS + sb * DRUMS + hb * HATS * 1.1 + fx
    dry_r = pad * 0.9 + piano * 0.7 + arp * 0.9 + bass + kb * DRUMS + sb * DRUMS * 0.9 + hb * HATS * 0.9 + fx
    send = pad * 0.5 + piano * 0.9 + arp * 0.7 + sb * DRUMS * 0.4 + fx
    # 琶音左右摆一点
    pan = 0.5 + 0.25 * np.sin(2 * np.pi * t_all / (2 * BAR))
    dry_l += arp * (pan - 0.5)
    dry_r -= arp * (pan - 0.5)
    left = dry_l + 0.45 * reverb(send, seed=1)
    right = dry_r + 0.45 * reverb(send, seed=2)
    mix = np.stack([left, right], 1)
    mix = highpass(mix[:, 0], 28)[:, None] * [1, 0] + highpass(mix[:, 1], 28)[:, None] * [0, 1]
    mix /= np.max(np.abs(mix)) + 1e-9
    mix = np.tanh(mix * 1.6) / np.tanh(1.6)        # 软限幅，听感更饱满
    fade = np.ones(N)
    fade[-int(3 * SR):] = np.linspace(1, 0, int(3 * SR)) ** 1.5
    mix *= fade[:, None] * 0.89
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'music_synth.wav')  # 成片现用 music_fit.py 生成的 music.wav
    with wave.open(out, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((mix * 32767).astype('<i2').tobytes())
    print(out)


if __name__ == '__main__':
    main()
