"""Makes the starter sounds for the "MKXAccess Sounds" folder (44.1 kHz, 16-bit mono .wav).
Anyone can replace any of these files with their own sound of the same name."""
import math, struct, sys, os, wave

RATE = 44100

def tone(freq, dur, amp=0.5, decay=6.0, harm=(1.0, 0.35, 0.12), attack=0.004, bright=0.0):
    n = int(RATE * dur); out = []
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / attack) * math.exp(-decay * t)
        v = sum(h * math.sin(2 * math.pi * freq * (k + 1) * t) for k, h in enumerate(harm))
        if bright:  # a little square-ish edge for urgency
            v += bright * (1 if math.sin(2 * math.pi * freq * t) >= 0 else -1) * 0.3
        out.append(amp * env * v / sum(harm))
    return out

def sweep(f0, f1, dur, amp=0.4):
    n = int(RATE * dur); out = []; ph = 0.0
    for i in range(n):
        t = i / RATE; f = f0 + (f1 - f0) * (i / n)
        ph += 2 * math.pi * f / RATE
        env = min(1.0, t / 0.005) * min(1.0, (dur - t) / 0.02)
        out.append(amp * env * (math.sin(ph) + 0.25 * math.sin(2 * ph)) / 1.25)
    return out

def silence(dur): return [0.0] * int(RATE * dur)

def seq(*parts):
    out = []
    for p in parts: out += p
    return out

def write(folder, name, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    if peak > 0.95: samples = [s * 0.95 / peak for s in samples]
    with wave.open(os.path.join(folder, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767)) for s in samples))

def main(folder):
    os.makedirs(folder, exist_ok=True)
    # you: deep, warm "bong" tones; one for 75, two for 50, three faster and harder for 25
    write(folder, "you75", tone(440, 0.45, 0.6, decay=7))
    write(folder, "you50", seq(tone(440, 0.22, 0.6, decay=9), silence(0.03), tone(330, 0.4, 0.6, decay=7)))
    u = lambda: tone(262, 0.13, 0.65, decay=14, bright=0.6)
    write(folder, "you25", seq(u(), silence(0.04), u(), silence(0.04), tone(262, 0.3, 0.65, decay=8, bright=0.6)))
    # opponent: high, bright "ping" tones with the same counts
    write(folder, "opponent75", tone(1320, 0.3, 0.45, decay=12, harm=(1.0, 0.2)))
    write(folder, "opponent50", seq(tone(1320, 0.15, 0.45, decay=16, harm=(1.0, 0.2)), silence(0.03), tone(1175, 0.3, 0.45, decay=12, harm=(1.0, 0.2))))
    o = lambda: tone(1047, 0.1, 0.45, decay=20, harm=(1.0, 0.2))
    write(folder, "opponent25", seq(o(), silence(0.03), o(), silence(0.03), tone(1047, 0.25, 0.45, decay=12, harm=(1.0, 0.2))))
    # X-ray ready: a quick rising four-note power-up, low for you, high for the opponent
    def arp(base, amp, harm):
        notes = [base, base * 5 / 4, base * 3 / 2, base * 2]
        out = []
        for i, f in enumerate(notes):
            out += tone(f, 0.09 if i < 3 else 0.4, amp, decay=10 if i < 3 else 5, harm=harm)
        return out
    write(folder, "xray_you", arp(262, 0.55, (1.0, 0.4, 0.2, 0.1)))
    write(folder, "xray_opponent", arp(784, 0.42, (1.0, 0.25)))
    # walls: a soft knock when a fighter gets close to a wall, a hard double slam in the corner
    import random
    def knock(freq, dur, amp, decay, noise):
        rnd = random.Random(int(freq)); n = int(RATE * dur); out = []; lp = 0.0
        for i in range(n):
            t = i / RATE
            lp += 0.25 * (rnd.uniform(-1, 1) - lp)           # dull noise for the wooden hit
            env = min(1.0, t / 0.002) * math.exp(-decay * t)
            out.append(amp * env * (0.7 * math.sin(2 * math.pi * freq * t * (1 + 0.6 * math.exp(-30 * t))) + noise * lp))
        return out
    # the steady wall sound: exactly one second, whole cycles only, so it loops without a click
    def hum(freq, amp, harm, trem):
        out = []
        for i in range(RATE):
            t = i / RATE
            v = sum(h * math.sin(2 * math.pi * freq * (k + 1) * t) for k, h in enumerate(harm)) / sum(harm)
            out.append(amp * v * (1 - trem + trem * (0.5 + 0.5 * math.sin(2 * math.pi * 6 * t))))
        return out
    write(folder, "wall_loop", hum(110, 0.5, (1.0, 0.5, 0.25, 0.12), 0.35))
    write(folder, "corner_you", seq(knock(110, 0.16, 0.8, 22, 1.8), silence(0.05), knock(90, 0.35, 0.85, 12, 2.0)))
    write(folder, "corner_opponent", seq(knock(330, 0.14, 0.6, 26, 1.2), silence(0.05), knock(280, 0.3, 0.65, 14, 1.4)))
    # menu
    write(folder, "menu_open", sweep(500, 900, 0.12))
    write(folder, "menu_close", sweep(900, 500, 0.12))
    write(folder, "menu_move", tone(1800, 0.03, 0.18, decay=90, harm=(1.0,)))
    write(folder, "menu_edge", tone(180, 0.09, 0.45, decay=35, harm=(1.0, 0.5)))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "MKXAccess Sounds")
