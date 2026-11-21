"""Звуки шагов по воде из записи: режет запись на отдельные шаги и пишет splash1-4.wav.

python3 walker/tools/import_splash.py <запись (mp3/wav)> <папка вывода>

Игра проигрывает один файл на каждый шаг робота (событие шага приходит из движения),
поэтому синхронизация с шагами — это правильная нарезка: каждый файл начинается ровно
с удара ноги о воду, без тишины в начале, и содержит один шаг.
Нужен ffmpeg (для mp3). Шаги чуть понижены по высоте — робот тяжелее человека.
"""
import os, subprocess, sys, tempfile, wave
import numpy as np

SOURCE = sys.argv[1]
OUT = sys.argv[2]
RATE = 22050
PITCH = 0.88          # 1.0 — как в записи; меньше — ниже и тяжелее
MAX_STEP = 0.42       # секунды на шаг


def load(path):
    with tempfile.TemporaryDirectory() as folder:
        wav_path = os.path.join(folder, 'source.wav')
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', path, '-vn', '-ac', '1',
                        '-ar', str(RATE), wav_path], check=True)
        with wave.open(wav_path) as file:
            return np.frombuffer(file.readframes(file.getnframes()), dtype=np.int16).astype(np.float64) / 32768


def find_steps(samples):
    """начала шагов: резкий подъём громкости над фоном (журчание воды между шагами).
    Возвращает [(отсчёт начала, насколько шаг громче фона в дБ), ...]."""
    hop = int(0.01 * RATE)
    frames = len(samples) // hop
    level = np.array([np.sqrt(np.mean(samples[index * hop:(index + 1) * hop] ** 2)) for index in range(frames)])
    level_db = 20 * np.log10(level + 1e-9)
    steps = []
    last = -1000
    for index in range(100, frames - 10):
        floor = np.percentile(level_db[index - 100:index], 20)      # фон за последнюю секунду
        rise = level_db[index] - level_db[index - 3]
        if level_db[index] - floor > 14 and rise > 8 and index - last > 35:
            begin = index
            while begin > index - 4 and level_db[begin - 1] - floor > 6:
                begin -= 1
            peak = level_db[index:index + 10].max()
            tail = np.median(level_db[index + 15:index + 40]) if index + 40 < frames else peak
            # отчётливый шаг: высоко над фоном и быстро затихает (не сплошное бултыхание)
            clarity = ( peak - floor ) + ( peak - tail )
            steps.append((begin * hop, clarity))
            last = index
    return steps


def cut_step(samples, start, next_start):
    end = min(start + int(MAX_STEP * RATE), next_start, len(samples))
    piece = samples[start:end].copy()
    fade_in = int(0.003 * RATE)
    piece[:fade_in] *= np.linspace(0, 1, fade_in)
    fade_out = int(0.08 * RATE)
    piece[-fade_out:] *= np.linspace(1, 0, fade_out) ** 2
    return piece


def pitch_down(piece):
    positions = np.arange(0, len(piece) - 1, PITCH)
    return np.interp(positions, np.arange(len(piece)), piece)


def save(path, piece):
    piece = piece / (np.max(np.abs(piece)) or 1) * 0.9
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, 'wb') as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes((piece * 32767).astype(np.int16).tobytes())


def main():
    samples = load(SOURCE)
    steps = find_steps(samples)
    if len(steps) < 4:
        sys.exit('в записи меньше четырёх отчётливых шагов')
    starts = [start for start, _ in steps]
    # четыре самых отчётливых шага
    best = sorted(range(len(steps)), key=lambda index: -steps[index][1])[:4]
    for number, index in enumerate(sorted(best), 1):
        next_start = starts[index + 1] if index + 1 < len(starts) else len(samples)
        piece = cut_step(samples, starts[index], next_start)
        save(os.path.join(OUT, 'sound', 'player', 'footsteps', f'splash{number}.wav'), pitch_down(piece))
        print(f'splash{number}: {starts[index] / RATE:.2f} s, clarity {steps[index][1]:.0f} dB')
    print(f'steps found: {len(steps)}')


main()
