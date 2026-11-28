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
step_rng = np.random.default_rng(2611)      # шаги (28.11.2026): свой генератор, остальные звуки прежние


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


def resonator(seconds, frequencies, decay, amplitudes=None, generator=None):
    t = time(seconds)
    result = np.zeros_like(t)
    for index, frequency in enumerate(frequencies):
        amplitude = 1.0 if amplitudes is None else amplitudes[index]
        result += amplitude * np.sin(2 * np.pi * frequency * t + (generator or rng).random() * 6) * np.exp(-t / (decay * (1 - index * 0.08)))
    return result


def pad(samples, seconds):
    result = np.zeros(int(seconds * RATE))
    result[:min(len(samples), len(result))] = samples[:len(result)]
    return result


def mix(*parts):
    length = max(len(part) for part in parts)
    return sum(pad(part, length / RATE) for part in parts)


def reverb(samples, room=0.18, wet=0.25, generator=None):
    impulse_length = int(room * 2 * RATE)
    impulse = (generator or rng).normal(0, 1, impulse_length) * np.exp(-np.arange(impulse_length) / (room * RATE / 3))
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
    """Короткий сухой выстрел (OSA до 28.11.2026, звучал как треск); не сохраняется."""
    seconds = 0.25
    crack = band(noise(seconds), 2500, 10000) * envelope(seconds, 0.0005, 0.012)
    body = band(noise(seconds), 300, 2500) * envelope(seconds, 0.001, 0.05)
    clack = resonator(seconds, [2100 + variant * 60, 3400], 0.012) * envelope(seconds, 0.0, 0.02)
    return reverb(mix(crack * 0.8, body * 0.7, clack * 0.2), 0.12, 0.2)


def plasma_shot():
    """ISKRA: электрический разряд с глухим ударом. Падающий тон 1800-220 Гц («пиу») убран
    28.11.2026; шум берётся тем же одним вызовом, остальные звуки не меняются."""
    seconds = 0.45
    t = time(seconds)
    raw = noise(seconds)
    crackle = band(raw, 2500, 9000) * envelope(seconds, 0.001, 0.04) * 0.45
    body = band(raw, 80, 900, 3) * envelope(seconds, 0.002, 0.12)
    thump = np.sin(2 * np.pi * (55 + 70 * np.exp(-t * 30)) * t) * envelope(seconds, 0.002, 0.15)
    return reverb(mix(crackle, body, thump * 1.2), 0.2, 0.25)


def plasma_hit():
    """попадание ISKRA: хлопок и низкий удар (звенящий тон 1500-600 Гц убран 28.11.2026)"""
    seconds = 0.6
    t = time(seconds)
    raw = noise(seconds)
    pop = band(raw, 100, 3000) * envelope(seconds, 0.001, 0.1)
    thump = np.sin(2 * np.pi * (50 + 60 * np.exp(-t * 20)) * t) * envelope(seconds, 0.002, 0.25)
    return reverb(mix(pop, thump * 1.1), 0.3, 0.3)


def rail_shot():
    """KOPYO: резкий треск разряда и низкий раскат (звенящий падающий тон убран 28.11.2026)"""
    seconds = 1.2
    t = time(seconds)
    raw = noise(seconds)
    crack = band(raw, 1500, 11000) * envelope(seconds, 0.0005, 0.04)
    rumble = band(raw, 40, 400, 3) * envelope(seconds, 0.003, 0.5) * 1.5
    thump = np.sin(2 * np.pi * (60 + 80 * np.exp(-t * 25)) * t) * envelope(seconds, 0.002, 0.2)
    return reverb(mix(crack, rumble, thump * 0.9), 0.45, 0.35)


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
    """Шаг робота (28.11.2026): глухой низкий удар без звона, под ногой хрустит галька и
    шуршит песок, с лёгким скрипом трущихся камешков. metallic > 1 - по металлу: добавляется
    приглушённый металлический отзвук. Случайность - из step_rng; основной генератор
    расходуется как прежним шагом, поэтому остальные звуки не меняются."""
    seconds = 0.9
    old_length = int(seconds * RATE)
    rng.normal(0, 1, old_length)
    rng.random(3)
    rng.normal(0, 1, old_length)
    rng.normal(0, 1, int(0.3 * 2 * RATE))
    t = time(seconds)
    raw = step_rng.normal(0, 1, len(t))
    thud = np.sin(2 * np.pi * (33 + variant * 2 + 22 * np.exp(-t * 18)) * t) * envelope(seconds, 0.006, 0.3)
    impact = band(raw, 30, 280, 3) * envelope(seconds, 0.004, 0.14)
    # хруст гальки: короткие щелчки зёрен, гуще в начале шага
    crunch = np.zeros_like(t)
    grains = 70 + variant * 8
    for _ in range(grains):
        start = int((0.008 + step_rng.exponential(0.06)) * RATE)
        if start >= len(t) - 200:
            continue
        click_length = int(step_rng.uniform(0.002, 0.006) * RATE)
        click = step_rng.normal(0, 1, click_length) * np.exp(-np.arange(click_length) / (click_length / 4))
        crunch[start:start + click_length] += click * step_rng.uniform(0.2, 1.0) * np.exp(-start / RATE / 0.12)
    crunch = band(crunch, 900, 4500)
    # шорох песка
    sand = band(raw, 500, 2200) * np.exp(-((t - 0.06) / 0.07) ** 2)
    # скрип: камешки трутся под весом робота, прерывистые толчки около 160 Гц
    creak = np.zeros_like(t)
    position = 0.07 + variant * 0.01
    while position < 0.2:
        start = int(position * RATE)
        pulse = resonator(0.02, [820 + variant * 40, 1450], 0.004, generator=step_rng)
        creak[start:start + len(pulse)] += pulse[:len(creak) - start] * np.exp(-((position - 0.13) / 0.05) ** 2)
        position += (1 / 160) * step_rng.uniform(0.7, 1.3)
    clank = np.zeros_like(t)
    if metallic > 1:
        clank = np.sin(2 * np.pi * (240 + variant * 15) * t) * np.exp(-t / 0.08) * (metallic - 1)
    body = band(mix(thud * 1.5, impact, clank * 0.3), 20, 1200)
    sound = mix(body, crunch * 0.45, sand * 0.12, creak * 0.1)
    return reverb(sound, 0.25, 0.18, step_rng)


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


def bullet_on_armor(variant):
    """пуля по броне врага (мои попадания, feedback/hit*.wav, с 27.11.2026 вместо писка):
    щелчок и короткий металлический отзвук, без долгого звона - очередь не звенит"""
    seconds = 0.16
    click = band(noise(seconds), 1200, 6000) * envelope(seconds, 0.0003, 0.008)
    knock = band(noise(seconds), 500, 2500) * envelope(seconds, 0.0005, 0.02) * 0.6
    ring = resonator(seconds, [2150 + 140 * variant, 3480 + 160 * variant, 5100], 0.03, [1.0, 0.55, 0.3]) * 0.35
    return mix(click, knock, ring)


def hull_hit(variant):
    """попадание по моему корпусу (звуки боли робота, с 27.11.2026 без тревожных сигналов):
    тот же удар по металлу, ниже и тяжелее - глухой толчок, удар, низкий отзвук"""
    seconds = 0.4
    thump = band(noise(seconds), 50, 260) * envelope(seconds, 0.002, 0.07) * 0.8
    knock = band(noise(seconds), 200, 2200) * envelope(seconds, 0.0008, 0.03)
    ring = resonator(seconds, [190 + 15 * variant, 520 + 30 * variant, 1020 + 50 * variant, 1650], 0.08,
                     [1.0, 0.6, 0.35, 0.18]) * 0.45
    return mix(thump, knock, ring)


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
        shot = cannon_shot(index)
        save(f'weapons/machinegun/machgf{index}b.wav', shot)
        if index == 1:
            # OSA - the shot of the K7 machine gun before the weapon slots (24.11.2026):
            # deep, with the boom below 200 Hz; the short dry 300-10000 Hz shot given to
            # OSA with the slots sounded like a crackle (28.11.2026)
            osa_shot = shot
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
    save('weapons/robot/osa.wav', osa_shot)
    light_gun_shot(1)  # not saved: it keeps the random noise of the sounds below as before
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
    # попадания по корпусу: слышны только у своего робота (cg_event.c, CG_PainEvent)
    save(robot + 'pain25_1.wav', hull_hit(3), 0.65)
    save(robot + 'pain50_1.wav', hull_hit(2), 0.65)
    save(robot + 'pain75_1.wav', hull_hit(1), 0.65)
    save(robot + 'pain100_1.wav', hull_hit(0), 0.65)
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
    save('feedback/hit.wav', bullet_on_armor(1), 0.5)
    save('feedback/hithi.wav', bullet_on_armor(2), 0.5)
    save('feedback/hitlo.wav', bullet_on_armor(0), 0.5)
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
