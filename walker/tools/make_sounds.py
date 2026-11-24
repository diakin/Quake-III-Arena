"""Звуки игры WALKER, синтезированные с нуля (numpy + scipy): лицензионно чистые, свои.

python3 walker/tools/make_sounds.py <папка вывода>

WAV 22050 Гц, 16 бит, моно. Имена — те, что ожидают cgame и меню.
"""
import os, sys, wave
import numpy as np
from scipy import signal

OUT = sys.argv[1]
RATE = 22050
rng = np.random.default_rng(26)


def save(path, samples, gain=0.9):
    samples = np.asarray(samples, dtype=np.float64)
    peak = np.max(np.abs(samples)) or 1.0
    data = (samples / peak * gain * 32767).astype(np.int16)
    full = os.path.join(OUT, 'sound', path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with wave.open(full, 'wb') as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(data.tobytes())


def time(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def noise(seconds):
    return rng.normal(0, 1, int(seconds * RATE))


def band(samples, low, high, order=2):
    nyquist = RATE / 2
    if low <= 0:
        b, a = signal.butter(order, high / nyquist, 'low')
    elif high >= nyquist:
        b, a = signal.butter(order, low / nyquist, 'high')
    else:
        b, a = signal.butter(order, [low / nyquist, high / nyquist], 'band')
    return signal.lfilter(b, a, samples)


def envelope(seconds, attack, decay, curve=4.0):
    t = time(seconds)
    rise = np.clip(t / max(attack, 1e-4), 0, 1)
    fall = np.exp(-np.maximum(t - attack, 0) / decay * curve / 4)
    return rise * fall


def resonator(seconds, frequencies, decay, amplitudes=None):
    t = time(seconds)
    result = np.zeros_like(t)
    for index, frequency in enumerate(frequencies):
        amplitude = 1.0 if amplitudes is None else amplitudes[index]
        result += amplitude * np.sin(2 * np.pi * frequency * t + rng.random() * 6) * np.exp(-t / (decay * (1 - index * 0.08)))
    return result


def pad(samples, seconds):
    result = np.zeros(int(seconds * RATE))
    result[:min(len(samples), len(result))] = samples[:len(result)]
    return result


def mix(*parts):
    length = max(len(part) for part in parts)
    return sum(pad(part, length / RATE) for part in parts)


def reverb(samples, room=0.18, wet=0.25):
    impulse_length = int(room * 2 * RATE)
    impulse = rng.normal(0, 1, impulse_length) * np.exp(-np.arange(impulse_length) / (room * RATE / 3))
    impulse = band(impulse, 200, 6000)
    tail = signal.fftconvolve(samples, impulse)[:len(samples) + impulse_length]
    tail = tail / (np.max(np.abs(tail)) or 1) * np.max(np.abs(samples))
    return mix(samples, tail * wet)


# ---------------------------------------------------------------- оружие
def cannon_shot(variant):
    seconds = 0.55
    crack = band(noise(seconds), 1800, 9000) * envelope(seconds, 0.001, 0.025)
    body = band(noise(seconds), 90, 900) * envelope(seconds, 0.002, 0.12)
    t = time(seconds)
    boom = np.sin(2 * np.pi * (70 + 40 * np.exp(-t * 30) + variant * 3) * t) * envelope(seconds, 0.002, 0.16)
    clack = resonator(seconds, [1450 + variant * 40, 2300, 3700], 0.02) * envelope(seconds, 0.0, 0.03)
    clack = np.roll(clack, int(0.07 * RATE))
    return reverb(mix(crack * 0.7, body * 0.9, boom * 1.2, clack * 0.25), 0.25, 0.3)


def ricochet(variant):
    seconds = 0.45
    t = time(seconds)
    hit = band(noise(seconds), 700, 6000) * envelope(seconds, 0.0005, 0.03)
    sweep = np.sin(2 * np.pi * np.cumsum(2600 - 1700 * t / seconds + variant * 300) / RATE) * envelope(seconds, 0.01, 0.25) * 0.35
    ping = resonator(seconds, [1900 + variant * 250, 3100, 4700], 0.08, [1, 0.5, 0.3]) * 0.4
    return mix(hit, sweep, ping)


def bullet_by():
    seconds = 0.4
    t = time(seconds)
    whoosh = band(noise(seconds), 800, 5000) * np.exp(-((t - 0.15) / 0.07) ** 2)
    return whoosh


def rocket_launch():
    seconds = 1.0
    blast = band(noise(seconds), 60, 2500) * envelope(seconds, 0.005, 0.3)
    hiss = band(noise(seconds), 1500, 8000) * envelope(seconds, 0.05, 0.5) * 0.4
    return reverb(mix(blast, hiss), 0.3, 0.3)


def rocket_fly():
    seconds = 1.5
    roar = band(noise(seconds), 200, 3000)
    t = time(seconds)
    roar *= 0.8 + 0.2 * np.sin(2 * np.pi * 9 * t)
    return roar


def explosion(seconds=1.8, low=40):
    t = time(seconds)
    rumble = band(noise(seconds), low, 700, 3) * envelope(seconds, 0.004, 0.8)
    crack = band(noise(seconds), 1200, 8000) * envelope(seconds, 0.001, 0.05)
    thump = np.sin(2 * np.pi * (55 + 60 * np.exp(-t * 12)) * t) * envelope(seconds, 0.003, 0.35)
    debris = np.zeros_like(t)
    for _ in range(14):
        start = int(rng.uniform(0.15, seconds * 0.8) * RATE)
        piece = resonator(0.12, [rng.uniform(900, 3500)], 0.03) * rng.uniform(0.05, 0.2)
        debris[start:start + len(piece)] += piece[:len(debris) - start]
    return reverb(mix(rumble * 1.3, crack * 0.6, thump * 1.2, debris), 0.6, 0.35)


# ---------------------------------------------------------------- оружие роботов (слоты)
def light_gun_shot(variant):
    """OSA: короткий сухой выстрел лёгкого пулемёта."""
    seconds = 0.25
    crack = band(noise(seconds), 2500, 10000) * envelope(seconds, 0.0005, 0.012)
    body = band(noise(seconds), 300, 2500) * envelope(seconds, 0.001, 0.05)
    clack = resonator(seconds, [2100 + variant * 60, 3400], 0.012) * envelope(seconds, 0.0, 0.02)
    return reverb(mix(crack * 0.8, body * 0.7, clack * 0.2), 0.12, 0.2)


def plasma_shot():
    """ISKRA: электрический хлопок с падающим тоном."""
    seconds = 0.45
    t = time(seconds)
    tone = np.sin(2 * np.pi * np.cumsum(1800 * np.exp(-t * 9) + 220) / RATE) * envelope(seconds, 0.002, 0.18)
    buzz = np.sign(np.sin(2 * np.pi * 95 * t)) * envelope(seconds, 0.002, 0.08) * 0.25
    fizz = band(noise(seconds), 3000, 9000) * envelope(seconds, 0.001, 0.06) * 0.5
    return reverb(mix(tone, buzz, fizz), 0.2, 0.25)


def plasma_hit():
    seconds = 0.6
    t = time(seconds)
    pop = band(noise(seconds), 400, 6000) * envelope(seconds, 0.001, 0.08)
    ring = np.sin(2 * np.pi * (600 + 900 * np.exp(-t * 20)) * t) * envelope(seconds, 0.002, 0.2) * 0.6
    return reverb(mix(pop, ring), 0.3, 0.3)


def rail_shot():
    """KOPYO: резкий треск разряда и звенящий хвост."""
    seconds = 1.2
    t = time(seconds)
    crack = band(noise(seconds), 1500, 11000) * envelope(seconds, 0.0005, 0.04)
    whine = np.sin(2 * np.pi * np.cumsum(3200 - 2600 * np.minimum(t / 0.6, 1)) / RATE) * envelope(seconds, 0.003, 0.5) * 0.35
    thump = np.sin(2 * np.pi * (60 + 80 * np.exp(-t * 25)) * t) * envelope(seconds, 0.002, 0.2)
    return reverb(mix(crack, whine, thump * 0.9), 0.45, 0.35)


def heavy_cannon_shot():
    """TARAN: тяжёлый орудийный выстрел."""
    seconds = 1.6
    t = time(seconds)
    blast = band(noise(seconds), 50, 1500, 3) * envelope(seconds, 0.002, 0.35)
    crack = band(noise(seconds), 1500, 9000) * envelope(seconds, 0.0008, 0.04)
    boom = np.sin(2 * np.pi * (45 + 50 * np.exp(-t * 18)) * t) * envelope(seconds, 0.002, 0.4)
    return reverb(mix(blast * 1.1, crack * 0.6, boom * 1.4), 0.6, 0.35)


def rocket_pod_shot():
    """SHMEL: короткий пуск лёгкой ракеты."""
    seconds = 0.6
    blast = band(noise(seconds), 150, 3000) * envelope(seconds, 0.003, 0.12)
    hiss = band(noise(seconds), 2000, 9000) * envelope(seconds, 0.02, 0.3) * 0.45
    return reverb(mix(blast, hiss), 0.2, 0.25)


# ---------------------------------------------------------------- робот
def stomp(variant, metallic=1.0):
    seconds = 0.9
    t = time(seconds)
    thud = np.sin(2 * np.pi * (38 + variant * 3 + 30 * np.exp(-t * 25)) * t) * envelope(seconds, 0.004, 0.22)
    impact = band(noise(seconds), 60, 600, 3) * envelope(seconds, 0.002, 0.1)
    clank = resonator(seconds, [310 + variant * 20, 780 + variant * 35, 1650], 0.12, [0.6, 0.4, 0.25]) * metallic
    clank *= envelope(seconds, 0.001, 0.2)
    hiss = band(noise(seconds), 2500, 9000) * np.exp(-((t - 0.28 - variant * 0.02) / 0.09) ** 2) * 0.12
    servo = np.sin(2 * np.pi * (180 + 60 * t) * t) * np.exp(-((t - 0.45) / 0.12) ** 2) * 0.06
    return reverb(mix(thud * 1.4, impact * 0.8, clank * 0.35, hiss, servo), 0.3, 0.25)


def bubble(seconds, frequency, rise):
    """пузырёк: короткий тон с быстрым подъёмом частоты (так звучит лопающаяся капля)."""
    t = time(seconds)
    sweep = frequency * (1 + rise * t / seconds)
    return np.sin(2 * np.pi * np.cumsum(sweep) / RATE) * np.exp(-t / (seconds / 4))


def splash(variant, heavy=1.0):
    """всплеск: шлепок по воде, шум брызг и рой капель-пузырьков. Без тонального удара,
    иначе звучит как барабан. heavy — сколько добавить глухого «шлепка» массы робота."""
    local = np.random.default_rng(700 + variant)
    seconds = 0.6
    t = time(seconds)
    # шлепок: широкополосный шум 0-40 мс
    slap = band(noise(seconds), 500, 5000) * np.exp(-t / 0.018) * 1.2
    # толща воды: низкий шум с неровной огибающей (бурление)
    churn = band(noise(seconds), 150, 1400, 3)
    wobble = band(local.normal(0, 1, len(t)), 0, 30)
    wobble = 0.6 + 0.4 * wobble / (np.abs(wobble).max() or 1)
    churn *= np.clip(t / 0.02, 0, 1) * np.exp(-t / 0.1) * wobble * 0.9
    # брызги: высокий шипящий шум, чуть позже шлепка
    spray = band(noise(seconds), 2500, 9500) * np.clip((t - 0.01) / 0.03, 0, 1) * np.exp(-t / 0.14) * 0.45
    # капли падают обратно: пузырьки по всему хвосту
    drops = np.zeros_like(t)
    for _ in range(int(local.integers(28, 40))):
        start = int(local.uniform(0.03, 0.45) ** 1.3 * RATE)
        length = local.uniform(0.012, 0.04)
        piece = bubble(length, local.uniform(700, 2600), local.uniform(0.6, 1.6)) * local.uniform(0.05, 0.25)
        piece = piece * (1 - start / len(t))
        drops[start:start + len(piece)] += piece[:len(drops) - start]
    # глухой шлепок массы робота по дну: шум, а не тон
    thud = band(noise(seconds), 40, 220, 3) * np.exp(-t / 0.07) * 0.5 * heavy
    return reverb(mix(slap, churn, spray, drops, thud), 0.2, 0.15)


def servo(seconds, low, high):
    t = time(seconds)
    frequency = low + (high - low) * np.sin(np.pi * t / seconds) ** 2
    tone = signal.sawtooth(2 * np.pi * np.cumsum(frequency) / RATE) * 0.3 + np.sin(2 * np.pi * np.cumsum(frequency * 2) / RATE) * 0.2
    return band(tone, 100, 4000) * envelope(seconds, 0.05, seconds * 0.8)


def alarm(beeps, frequency=880):
    parts = []
    for index in range(beeps):
        t = time(0.12)
        parts.append(np.sin(2 * np.pi * frequency * t) * envelope(0.12, 0.005, 0.1))
        parts.append(np.zeros(int(0.06 * RATE)))
    return np.concatenate(parts)


def metal_hit(strength):
    seconds = 0.6
    hit = band(noise(seconds), 300, 5000) * envelope(seconds, 0.001, 0.04)
    ring = resonator(seconds, [420, 1130, 2210, 3400], 0.18 * strength, [1, 0.6, 0.4, 0.2])
    return mix(hit * strength, ring * 0.4)


# ---------------------------------------------------------------- интерфейс
def beep(frequency, seconds=0.08, shape='sine'):
    t = time(seconds)
    tone = np.sin(2 * np.pi * frequency * t) if shape == 'sine' else signal.square(2 * np.pi * frequency * t) * 0.4
    return tone * envelope(seconds, 0.003, seconds * 0.6)


def chime(notes, step=0.11, tail=0.35):
    """сигнал из нескольких тонов (награды, смена лидера): без голоса, только тоны."""
    total = step * (len(notes) - 1) + tail
    result = np.zeros(int(total * RATE))
    for index, frequency in enumerate(notes):
        t = time(tail)
        tone = (np.sin(2 * np.pi * frequency * t) + 0.35 * np.sin(2 * np.pi * frequency * 2 * t) +
                0.15 * np.sin(2 * np.pi * frequency * 3 * t)) * np.exp(-t / (tail / 3))
        start = int(index * step * RATE)
        result[start:start + len(tone)] += tone[:len(result) - start]
    return reverb(result, 0.25, 0.25)


def main():
    for index in range(1, 5):
        save(f'weapons/machinegun/machgf{index}b.wav', cannon_shot(index))
        save(f'player/footsteps/mech{index}.wav', stomp(index))
        for alias in ('step', 'boot', 'flesh', 'energy'):
            save(f'player/footsteps/{alias}{index}.wav', stomp(index), 0.8)
        save(f'player/footsteps/metal{index}.wav', stomp(index, 1.6))
        save(f'player/footsteps/clank{index}.wav', stomp(index, 1.8))
        save(f'player/footsteps/splash{index}.wav', splash(index))
    for index in range(1, 4):
        save(f'weapons/machinegun/ric{index}.wav', ricochet(index))
    save('weapons/machinegun/buletby1.wav', bullet_by(), 0.6)
    save('weapons/rocket/rocklf1a.wav', rocket_launch())
    save('weapons/robot/osa.wav', light_gun_shot(1), 0.6)
    save('weapons/robot/shmel.wav', rocket_pod_shot(), 0.8)
    save('weapons/robot/topor.wav', cannon_shot(2), 0.8)
    save('weapons/robot/iskra.wav', plasma_shot(), 0.7)
    save('weapons/robot/taran.wav', heavy_cannon_shot(), 0.95)
    save('weapons/robot/kopyo.wav', rail_shot(), 0.9)
    save('weapons/plasma/plasmx1a.wav', plasma_hit(), 0.8)
    save('weapons/rocket/rockfly.wav', rocket_fly(), 0.6)
    save('weapons/rocket/rocklx1a.wav', explosion(1.6))
    save('weapons/change.wav', mix(metal_hit(0.5)[: int(0.3 * RATE)], servo(0.35, 300, 600)), 0.7)
    save('weapons/noammo.wav', mix(beep(300, 0.06, 'square'), metal_hit(0.2)[: int(0.1 * RATE)]), 0.6)
    save('player/land1.wav', stomp(2, 1.4))
    save('player/talk.wav', beep(1200, 0.1), 0.5)
    save('player/watr_in.wav', splash(3))
    save('player/watr_out.wav', splash(1))
    save('player/watr_un.wav', band(noise(0.8), 100, 800) * envelope(0.8, 0.05, 0.5), 0.6)
    save('player/gibimp1.wav', metal_hit(1.0))
    save('player/gibimp2.wav', metal_hit(0.8))
    save('player/gibimp3.wav', metal_hit(0.6))
    save('player/gibsplt1.wav', explosion(1.2, 60))
    # голос робота: удары по корпусу, тревога, сервоприводы, взрыв
    robot = 'player/walker/'
    save(robot + 'death1.wav', explosion(2.0))
    save(robot + 'death2.wav', mix(explosion(2.0, 50), metal_hit(1.0)))
    save(robot + 'death3.wav', mix(np.concatenate([alarm(2, 660), np.zeros(int(0.1 * RATE))]), np.roll(pad(explosion(1.8), 2.4), int(0.35 * RATE))))
    save(robot + 'pain25_1.wav', mix(metal_hit(1.0), np.roll(pad(alarm(3, 990), 1.0), int(0.15 * RATE))))
    save(robot + 'pain50_1.wav', mix(metal_hit(0.9), np.roll(pad(alarm(2, 880), 1.0), int(0.15 * RATE))))
    save(robot + 'pain75_1.wav', mix(metal_hit(0.8), np.roll(pad(alarm(1, 780), 1.0), int(0.15 * RATE))))
    save(robot + 'pain100_1.wav', metal_hit(0.7))
    save(robot + 'jump1.wav', servo(0.5, 200, 500))
    save(robot + 'fall1.wav', stomp(3, 1.6))
    save(robot + 'falling1.wav', servo(0.8, 400, 200))
    save(robot + 'gasp.wav', band(noise(0.6), 2000, 8000) * envelope(0.6, 0.02, 0.4), 0.5)
    save(robot + 'drown.wav', alarm(4, 500))
    save(robot + 'taunt.wav', mix(servo(0.9, 250, 700), np.roll(pad(beep(600, 0.4, 'square'), 0.9), int(0.3 * RATE))))
    # меню и сигналы
    save('misc/menu1.wav', mix(beep(880), np.roll(pad(beep(1320), 0.2), int(0.05 * RATE))), 0.5)
    save('misc/menu2.wav', beep(1100, 0.05), 0.35)
    save('misc/menu3.wav', mix(beep(660), np.roll(pad(beep(440), 0.2), int(0.05 * RATE))), 0.5)
    save('misc/menu4.wav', beep(160, 0.18, 'square'), 0.5)
    save('misc/silence.wav', np.zeros(int(0.1 * RATE)) + 1e-6, 0.0)
    save('feedback/hit.wav', beep(1500, 0.05), 0.5)
    save('feedback/hithi.wav', beep(1800, 0.05), 0.5)
    save('feedback/hitlo.wav', beep(1200, 0.05), 0.5)
    save('feedback/hit_teammate.wav', beep(300, 0.12, 'square'), 0.5)
    save('feedback/three.wav', beep(700, 0.15), 0.7)
    save('feedback/two.wav', beep(700, 0.15), 0.7)
    save('feedback/one.wav', beep(700, 0.15), 0.7)
    save('feedback/fight.wav', mix(beep(1050, 0.5), servo(0.5, 300, 900) * 0.4), 0.8)
    save('feedback/prepare.wav', alarm(2, 700), 0.6)
    save('feedback/1_minute.wav', alarm(2, 990), 0.6)
    save('feedback/5_minute.wav', alarm(1, 990), 0.6)
    # награды и события счёта
    for name, notes in [('excellent', [660, 880]), ('first_excellent', [660, 880, 1100]),
                        ('impressive', [784, 988, 1175]), ('first_impressive', [784, 988, 1175, 1568]),
                        ('humiliation', [330, 262]), ('first_gauntlet', [330, 262, 196]),
                        ('defense', [523, 659]), ('assist', [587, 740]), ('denied', [440, 349]),
                        ('takenlead', [523, 659, 784]), ('lostlead', [784, 659, 523]), ('tiedlead', [659, 659]),
                        ('1_frag', [880]), ('2_frags', [880, 880]), ('3_frags', [880, 880, 880]),
                        ('redleads', [523, 784]), ('blueleads', [587, 880]), ('teamstied', [659, 659]),
                        ('prepare_team', [440, 554, 659]), ('sudden_death', [880, 660, 880, 660]),
                        ('voc_youwin', [523, 659, 784, 1047]), ('voc_youlose', [440, 392, 330, 262]),
                        ('voc_holyshit', [1047, 1319, 1568]), ('vote_now', [700, 700]),
                        ('vote_passed', [600, 900]), ('vote_failed', [600, 400])]:
        save(f'feedback/{name}.wav', chime(notes), 0.6)
    # фон карты: ветер над водой
    seconds = 8.0
    t = time(seconds)
    wind = band(noise(seconds), 150, 1200) * (0.6 + 0.4 * np.sin(2 * np.pi * t / seconds) ** 2)
    fade = int(0.5 * RATE)
    wind[:fade] *= np.linspace(0, 1, fade) ** 0.5
    wind[-fade:] *= np.linspace(1, 0, fade) ** 0.5
    save('world/wind_loop.wav', wind, 0.5)
    count = sum(len(files) for _, _, files in os.walk(os.path.join(OUT, 'sound')))
    print('sounds:', count)


main()
