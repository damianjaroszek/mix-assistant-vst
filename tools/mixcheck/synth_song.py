# Synthetic 6-stem house song for mixcheck (96 s, 124 BPM): intro 0-32 s, break 32-48, build 48-64, drop 64-96.
# python3 synth_song.py OUT_DIR  -> six stems plus reference.wav (move it out of the stems folder, pass it as --reference).
import numpy as np, wave, sys, os
sr = 48000; bpm = 124.0; beat = 60.0 / bpm; L = 96.0; N = int(sr * L)
t = np.arange(N) / sr
rng = np.random.default_rng(1)
out = sys.argv[1]; os.makedirs(out, exist_ok=True)

def write(name, x):
    x = np.atleast_2d(x)
    if x.shape[0] == 1: x = np.vstack([x, x])
    pcm = (np.clip(x.T, -1, 1) * 32767).astype('<i2')
    with wave.open(os.path.join(out, name + '.wav'), 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())

def env(on, a, d):
    e = np.zeros(N)
    for s in on:
        i = int(s * sr); n = min(N - i, int((a + d * 6) * sr))
        k = np.arange(n) / sr
        e[i:i+n] = np.maximum(e[i:i+n], np.minimum(k / a, 1) * np.exp(-np.maximum(k - a, 0) / d))
    return e

sec = lambda a, b: (t >= a) & (t < b)
drums_on = sec(0, 32) | sec(48, 96)
beats = [b * beat for b in range(int(L / beat)) if drums_on[min(N-1, int(b*beat*sr))]]
# Kick: pitch sweep 150->50 Hz.
kick = np.zeros(N)
for s in beats:
    i = int(s*sr); n = min(N-i, int(0.35*sr)); k = np.arange(n)/sr
    f = 50 + 100*np.exp(-k/0.03); ph = 2*np.pi*np.cumsum(f)/sr
    kick[i:i+n] += np.sin(ph)*np.exp(-k/0.12) + 0.3*rng.standard_normal(n)*np.exp(-k/0.004)
write('01 Kick', 0.9*kick)
# Sub bass: offbeat notes, near-sine.
notes = [55.0, 55.0, 65.41, 49.0]
bass = np.zeros(N); bass_on = sec(16, 32) | sec(64, 96)
for b in range(int(L/beat)):
    s = b*beat + beat/2
    if not bass_on[min(N-1, int(s*sr))]: continue
    f = notes[(b//4) % 4]; i = int(s*sr); n = min(N-i, int(beat/2*sr)); k = np.arange(n)/sr
    bass[i:i+n] += np.sin(2*np.pi*f*k)*np.minimum(k/0.005,1)*np.minimum((beat/2-k)/0.01,1)
write('02 Sub Bass', 0.7*bass)
# Lead vox: saw-ish with vibrato, formant-filtered by a simple resonator sum.
vox_on = sec(32, 48) | sec(64, 96)
mel = [220, 247, 262, 294, 262, 247]
f = np.array([mel[int(x/(2*beat)) % 6] for x in t]) * (1 + 0.01*np.sin(2*np.pi*5*t))
ph = np.cumsum(f)/sr
saw = sum(np.sin(2*np.pi*h*ph)/h for h in range(1, 25))
vox = np.zeros(N)
for fc, g in ((700, 1.0), (1200, 0.6), (2600, 0.3)):
    # crude formant: weight harmonics near fc
    vox += g*sum(np.sin(2*np.pi*h*ph)/h*np.exp(-((h*f-fc)/250)**2) for h in range(1, 25))
vox = (0.3*saw + vox) * vox_on
write('03 Lead Vox', 0.25*vox)
# Lead synth: detuned saws, stereo.
lead_on = sec(48, 96)
fl = np.array([[440, 523, 587, 659][int(x/beat) % 4] for x in t])
def sawp(fr): p = np.cumsum(fr)/sr; return 2*(p % 1) - 1
lead = np.vstack([sawp(fl*1.004)+sawp(fl), sawp(fl*0.996)+sawp(fl)]) * lead_on * env([b*beat/2 for b in range(int(L/beat*2))], 0.003, 0.08)
write('04 Lead Synth', 0.12*lead)
# Pad chords: wide, slow.
pad_on = sec(0, 96)
chord = [220, 261.6, 329.6, 392]
padL = sum(sawp(np.full(N, c*1.003)) for c in chord); padR = sum(sawp(np.full(N, c*0.997)) for c in chord)
# simple one-pole lowpass
def lp(x, fc):
    a = np.exp(-2*np.pi*fc/sr); y = np.empty_like(x); s = 0.0
    for i in range(len(x)): s = (1-a)*x[i] + a*s; y[i] = s
    return y
padL = lp(padL, 1500); padR = lp(padR, 1500)
write('05 Pad Chords', 0.12*np.vstack([padL, padR])*pad_on)
# Hats: offbeat open + 16th closed, high-passed noise.
hat_on = sec(0, 32) | sec(56, 96)
noise = rng.standard_normal(N); hp = noise - lp(noise, 6000)
on16 = [b*beat/4 for b in range(int(L/beat*4)) if hat_on[min(N-1, int(b*beat/4*sr))]]
e = env(on16, 0.001, 0.02) + env([s+beat/2 for s in on16[::4]], 0.001, 0.1)
write('06 Hats', 0.5*hp*e)
# Reference: same song, balanced by hand (kick loudest, hats/lead lower), limited-ish.
ref = np.vstack([0.9*kick, 0.9*kick]) + 0.75*bass + 0.18*vox + 0.07*lead + 0.08*np.vstack([padL, padR])*pad_on + 0.22*hp*e
ref = np.tanh(ref*1.6)/1.6
write('reference', ref)
