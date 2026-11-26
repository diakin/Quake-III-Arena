"""Карта WALKER «Большие Маяки» (wk_bigbeacons) для захвата маяков (g_gametype 8).

python3 make_map_bigbeacons.py <данные игры> <папка компиляции .../quake*/basewalker>

Игровое поле 4000x4000 единиц = 800x800 м при масштабе игры 1 м = 5 единиц
(ROBOT_UNITS_PER_METRE, game/bg_robots.h): K7 на 40 км/ч проходит его примерно за 70 с.
Устроена как «Маяки» (make_map_beacons.py, всё построение берётся оттуда), но крупнее и
с другим рельефом: маяки по углам и в центре, синие появляются на севере, красные на юге
за длинными укрытиями; между ними пологие холмы высотой в полтора-два робота закрывают
обзор на дальних дистанциях, вокруг центра кольцо бетонных блоков, по бокам и на подходах
к угловым маякам — блоки и разрушенные стены. Расстановка симметрична поворотом на 180°.
"""
import math, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_map_beacons as mb          # он же подключает make_map_islands (sys.argv) и make_props
isl = mb.isl

mb.MAP = 'wk_bigbeacons'
mb.TITLE = 'Big Beacons'
mb.SHADER_FILE = 'walker_bigbeacons'
mb.TERRAIN = 'terrain_bigbeacons'
mb.COLORMAP = 'bigbeacons_colormap'
mb.PLAY = 2000                  # половина игрового поля: 4000 единиц = 800 м
mb.HALF = 2560                  # половина всей карты; кратна шагам сетки 64 и 128
mb.BEACONS = [('a', -1500, 1500), ('b', 1500, 1500), ('c', 0, 0), ('d', -1500, -1500), ('e', 1500, -1500)]
mb.SPAWN_Y = 1800
mb.SPAWN_XS = [-500, -250, 0, 250, 500]
mb.INTERMISSION = ('-1700 -1900 560', '18 48 0')
mb.LEVELSHOT_CONTRAST = 1.2             # холмы на поле до 120: при 3 план белеет
mb.SPEAKERS = [(0, 0), (-1400, 1400), (1400, -1400), (-1400, -1400), (1400, 1400)]

isl.MAP_NAME = mb.MAP
isl.TERRAIN_SHADER = mb.TERRAIN
isl.HALF_X = isl.HALF_Y = mb.HALF
isl.PLAY_X = isl.PLAY_Y = mb.PLAY
# сетка рельефа как у «Маяков»: при 80 на склонах холмов были видны грани (каждый треугольник
# рельефа q3map освещает со своей нормалью, без сглаживания)
isl.COARSE_CELL = 128
isl.CELL = 64

# холмы на поле (x, y, радиус, высота): каждый и его копия, повёрнутая на 180°.
# Робот около 60 единиц: холм 90-110 закрывает его целиком. Холмы широкие и пологие (уклон не
# больше 0,3): на крутых склонах грани треугольников видны по освещению
MOUNDS = [(-850, 650, 520, 105), (950, 850, 420, 85), (0, 1000, 480, 90),
          (-1700, 250, 450, 110), (600, -380, 360, 65), (-1080, -120, 400, 80)]


def coarse_height_map():
    """как у «Маяков»: почти ровное поле, холмы за границей; плюс холмы MOUNDS на поле.
    Под маяками и зонами появления — ровно."""
    half, play, base = mb.HALF, mb.PLAY, mb.BASE_Z
    step = isl.COARSE_CELL
    xs = np.arange(-half, half + 1, step, dtype=float)
    ys = np.arange(-half, half + 1, step, dtype=float)
    grid_x, grid_y = np.meshgrid(xs, ys)
    shape = grid_x.shape
    waves = isl.fft_noise(0, 2.0, 711, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    bumps = isl.fft_noise(0, 1.4, 712, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    height = base + 10 * waves + 3 * bumps
    for x, y, radius, rise in MOUNDS:
        for center_x, center_y in ((x, y), (-x, -y)):
            distance = np.hypot(grid_x - center_x, grid_y - center_y) / radius
            weight = np.clip(1 - distance, 0, 1)
            height += rise * weight * weight * (3 - 2 * weight)
    outside = np.maximum(np.abs(grid_x), np.abs(grid_y)) - play
    rise = np.clip((outside - 80) / (half - play - 140), 0, 1)
    hills = isl.fft_noise(0, 1.6, 713, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    height += (260 + 80 * hills) * rise * rise * (3 - 2 * rise)
    flats = [(x, y, mb.BEACON_RADIUS + 60, 160) for _, x, y in mb.BEACONS]
    flats += [(0, mb.SPAWN_Y, 700, 200), (0, -mb.SPAWN_Y, 700, 200)]
    for center_x, center_y, radius, falloff in flats:
        distance = np.hypot(grid_x - center_x, (grid_y - center_y) * (1.0 if radius < 300 else 3.0))
        weight = np.clip(1 - (distance - radius) / falloff, 0, 1)
        weight = weight * weight * (3 - 2 * weight)
        height = height * (1 - weight) + base * weight
    return xs, ys, height


isl.coarse_height_map = coarse_height_map


def rotated_pair(name, x, y, length, thickness, angle, seed):
    """блок и его копия, повёрнутая на 180° вокруг центра карты."""
    for suffix, (cx, cy), turn in (('1', (x, y), 0), ('2', (-x, -y), 180)):
        direction = math.radians(angle + turn)
        dx, dy = math.cos(direction) * length / 2, math.sin(direction) * length / 2
        mb.block(f'{name}{suffix}', cx - dx, cy - dy, cx + dx, cy + dy, thickness, seed + (0 if suffix == '1' else 50))


def wall_pair(x0, y0, x1, y1, seed):
    for (ax, ay, bx, by), shift in (((x0, y0, x1, y1), 0), ((-x0, -y0, -x1, -y1), 1)):
        base = isl.ground_min(min(ax, bx) - 14, min(ay, by) - 14, max(ax, bx) + 14, max(ay, by) + 14) - 1
        isl.broken_wall(ax, ay, bx, by, base, mb.COVER_HEIGHT, 26, seed + shift)


def layout():
    pads = {}
    for letter, x, y in mb.BEACONS:
        pads[letter] = (x, y, mb.beacon_pad(letter, x, y))
    # длинные укрытия перед зонами появления (с проходами)
    for side, sign in (('n', 1), ('s', -1)):
        seed = 2000 if sign > 0 else 2100
        mb.blocks_along(f'cover_{side}1', -1000 * sign, 1250 * sign, -600 * sign, 1250 * sign, 44, seed)
        mb.blocks_along(f'cover_{side}2', -250 * sign, 1300 * sign, 250 * sign, 1300 * sign, 44, seed + 20)
        mb.blocks_along(f'cover_{side}3', 600 * sign, 1250 * sign, 1000 * sign, 1250 * sign, 44, seed + 40)
    # кольцо вокруг центрального маяка: восемь блоков с проходами
    for index in range(4):
        angle = 22.5 + 45 * index
        radius = 400
        x = radius * math.cos(math.radians(angle))
        y = radius * math.sin(math.radians(angle))
        rotated_pair(f'ring{index}', round(x), round(y), 120, 48, angle + 90, 2200 + index * 7)
    # подходы к угловым маякам: по два блока углом к центру
    for name, (x, y, length, angle, seed) in {
            'corner_a1': (-1180, 1240, 130, 0, 2300), 'corner_a2': (-1240, 1180, 130, 90, 2310),
            'corner_b1': (1180, 1240, 130, 0, 2320), 'corner_b2': (1240, 1180, 130, 90, 2330)}.items():
        rotated_pair(name, x, y, length, 50, angle, seed)
    # между маяками и центром
    for name, (x, y, length, thickness, angle, seed) in {
            'mid_a': (-700, 420, 110, 50, 25, 2400), 'mid_b': (760, 480, 100, 60, -20, 2410),
            'mid_c': (-420, 820, 90, 56, 70, 2420), 'mid_d': (380, 760, 120, 44, 10, 2430),
            'mid_e': (-1250, 700, 100, 64, -35, 2440), 'mid_f': (1300, 650, 110, 50, 40, 2450),
            'mid_g': (1050, -150, 90, 70, 80, 2460), 'mid_h': (-200, -620, 80, 80, 0, 2470)}.items():
        rotated_pair(name, x, y, length, thickness, angle, seed)
    # фланги: разрушенные стены
    wall_pair(-1850, -250, -1850, 120, 71)
    wall_pair(-1350, 200, -1100, 260, 73)
    wall_pair(250, 450, 420, 330, 75)
    return pads


mb.layout = layout

if __name__ == '__main__':
    mb.main()
