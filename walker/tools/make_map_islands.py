"""Карта WALKER «Острова» (wk_islands): мшистые руины на островах в мелкой воде, скалы по краям.

python3 walker/tools/make_map_islands.py <данные игры> <папка компиляции>

<данные игры>      — сюда пишутся текстуры .jpg, небо, шейдеры, scripts/arenas.txt
<папка компиляции> — путь вида .../quake*/basewalker: туда кладутся maps/wk_islands.map,
                     копии текстур в .tga (id q3map под Linux читает только TGA) и шейдеры.
Масштаб: робот 56 единиц ~ 10 м, то есть 1 м ~ 5.6 единицы.
"""
import math, os, shutil, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

DATA = sys.argv[1]
COMPILE = sys.argv[2]
MAP_NAME = 'wk_islands'
rng = np.random.default_rng(1977)
FONT_TITLE = '/usr/share/fonts/truetype/google-fonts/ofl/anton/Anton-Regular.ttf'
if not os.path.exists(FONT_TITLE):
    FONT_TITLE = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'

SUN_AZIMUTH = 140.0      # градусы, как в q3map_sun
SUN_ELEVATION = 48.0

# ================================================================ текстуры

def fft_noise(size, beta, seed, shape=None):
    """Бесшовный шум 1/f^beta, значения примерно в [-1, 1]."""
    local = np.random.default_rng(seed)
    height, width = shape or (size, size)
    white = local.normal(0, 1, (height, width))
    freq_y = np.fft.fftfreq(height)[:, None]
    freq_x = np.fft.fftfreq(width)[None, :]
    radius = np.hypot(freq_x, freq_y)
    radius[0, 0] = 1
    spectrum = np.fft.fft2(white) / radius ** beta
    spectrum[0, 0] = 0
    field = np.real(np.fft.ifft2(spectrum))
    return field / (np.abs(field).max() or 1)


def colorize(field, stops):
    """field в [0,1] -> цвет по опорным точкам [(позиция, (r,g,b)), ...]."""
    positions = [stop[0] for stop in stops]
    result = np.zeros(field.shape + (3,))
    for channel in range(3):
        result[..., channel] = np.interp(field, positions, [stop[1][channel] for stop in stops])
    return result


def normalize(field):
    low, high = np.percentile(field, 1), np.percentile(field, 99)
    return np.clip((field - low) / (high - low + 1e-9), 0, 1)


def shade_relief(height_field, strength=1.0):
    """Псевдо-освещение рельефа текстуры (свет сверху слева)."""
    grad_y, grad_x = np.gradient(height_field)
    light = -(grad_x + grad_y) * strength
    return np.clip(1 + light, 0.6, 1.4)


TEXTURES = {}   # имя -> PIL.Image (RGB или RGBA)


def tex_grass(seed, base_green):
    size = 256
    large = normalize(fft_noise(size, 1.6, seed))
    fine = normalize(fft_noise(size, 0.7, seed + 1))
    field = 0.6 * large + 0.4 * fine
    color = colorize(field, [(0, (48, 58, 30)), (0.45, base_green), (0.75, (110, 120, 60)), (1, (128, 118, 74))])
    tufts = normalize(fft_noise(size, 0.3, seed + 2)) > 0.86
    color[tufts] = color[tufts] * 1.25
    dirt = normalize(fft_noise(size, 1.9, seed + 3)) > 0.82
    color[dirt] = color[dirt] * 0.55 + np.array([70, 58, 40]) * 0.45
    color *= shade_relief(fine * 6, 1.0)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_rock(seed, moss=0.0):
    size = 256
    large = fft_noise(size, 1.8, seed)
    y = np.arange(size)[:, None] / size
    strata = np.sin((y * 9 + large * 1.5) * 2 * np.pi) * 0.3
    cracks = np.abs(fft_noise(size, 1.2, seed + 1)) < 0.04
    field = normalize(large + strata + 0.4 * fft_noise(size, 0.8, seed + 2))
    color = colorize(field, [(0, (58, 54, 50)), (0.4, (98, 92, 84)), (0.75, (132, 126, 114)), (1, (160, 154, 140))])
    color *= shade_relief(field * 8, 1.0)[..., None]
    color[cracks] *= 0.55
    if moss > 0:
        moss_mask = normalize(fft_noise(size, 1.5, seed + 3) + (1 - y) * 0.0)
        moss_amount = np.clip((moss_mask - (1 - moss)) * 3, 0, 1)[..., None]
        color = color * (1 - moss_amount) + np.array([70, 88, 40]) * moss_amount * shade_relief(field * 8)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_shore(seed):
    size = 256
    field = normalize(fft_noise(size, 1.3, seed))
    color = colorize(field, [(0, (70, 66, 52)), (0.5, (104, 96, 72)), (1, (134, 124, 96))])
    pebbles = normalize(fft_noise(size, 0.2, seed + 1)) > 0.8
    color[pebbles] = color[pebbles] * 0.75 + 30
    grass = normalize(fft_noise(size, 1.7, seed + 2)) > 0.75
    color[grass] = color[grass] * 0.6 + np.array([60, 80, 36]) * 0.4
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_seabed(seed):
    size = 256
    field = normalize(fft_noise(size, 1.5, seed))
    x = np.arange(size)[None, :] / size
    ripple = np.sin((x * 6 + field * 0.8) * 2 * np.pi) * 0.12
    color = colorize(np.clip(field + ripple, 0, 1), [(0, (46, 52, 34)), (0.5, (78, 82, 54)), (1, (112, 108, 78))])
    weed = normalize(fft_noise(size, 1.2, seed + 1)) > 0.78
    color[weed] = color[weed] * 0.6 + np.array([40, 70, 30]) * 0.4
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_concrete(seed, tone=1.0, panels=128, moss=0.35, tiles=False):
    size = 256
    stains = normalize(fft_noise(size, 1.6, seed))
    grain = fft_noise(size, 0.5, seed + 1) * 0.08
    base = 150 * tone
    color = np.zeros((size, size, 3))
    color[:] = base
    color *= (0.85 + 0.25 * stains + grain)[..., None]
    color[..., 2] *= 0.97
    y, x = np.mgrid[0:size, 0:size]
    seams = (x % panels < 2) | (y % panels < 2)
    if tiles:
        seams |= (x % 64 < 1) | (y % 64 < 1)
    color[seams] *= 0.62
    # потёки сверху вниз
    streak = normalize(fft_noise(size, 1.0, seed + 2, (size, size)) + np.linspace(-0.4, 0.4, size)[:, None])
    color *= (0.9 + 0.12 * streak)[..., None]
    if moss > 0:
        moss_field = normalize(fft_noise(size, 1.7, seed + 3))
        amount = np.clip((moss_field - (1 - moss)) * 2.5, 0, 0.85)[..., None]
        color = color * (1 - amount) + np.array([62, 84, 38]) * amount
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_metal(seed, base, panel=64, rust=0.2, rivets=True):
    size = 256
    noise = normalize(fft_noise(size, 1.4, seed))
    color = np.zeros((size, size, 3))
    color[:] = base
    color *= (0.85 + 0.25 * noise)[..., None]
    y, x = np.mgrid[0:size, 0:size]
    seams = (x % panel < 2) | (y % (panel * 2) < 2)
    color[seams] *= 0.55
    color[(x % panel == 2) | (y % (panel * 2) == 2)] *= 1.25
    if rivets:
        rivet = ((x % panel - 6) ** 2 + (y % (panel * 2) - 6) ** 2 < 5) | ((x % panel - panel + 6) ** 2 + (y % (panel * 2) - 6) ** 2 < 5)
        color[rivet] *= 1.35
    if rust > 0:
        rust_field = normalize(fft_noise(size, 1.3, seed + 1))
        amount = np.clip((rust_field - (1 - rust)) * 2.5, 0, 0.9)[..., None]
        color = color * (1 - amount) + np.array([120, 66, 34]) * amount * (0.8 + 0.4 * noise[..., None])
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_grate(seed):
    size = 256
    color = np.array(tex_metal(seed, (96, 100, 102), 64, 0.25, False)).astype(float)
    y, x = np.mgrid[0:size, 0:size]
    holes = ((x % 16) > 3) & ((y % 16) > 3)
    color[holes] *= 0.35
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_hazard(seed):
    size = 256
    y, x = np.mgrid[0:size, 0:size]
    stripes = ((x + y) // 32) % 2 == 0
    color = np.zeros((size, size, 3))
    color[stripes] = (200, 160, 40)
    color[~stripes] = (36, 34, 30)
    wear = normalize(fft_noise(size, 1.2, seed))
    color *= (0.75 + 0.3 * wear)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_container(seed, base):
    size = 256
    color = np.array(tex_metal(seed, base, 256, 0.25, False)).astype(float)
    x = np.arange(size)[None, :]
    ribs = np.sin(x / size * 2 * np.pi * 16)
    color *= (0.88 + 0.14 * ribs)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_marked_pad(seed, label, ring_color, letter_size=150):
    """площадка с кругом и буквой (маяки A-E, вертолётные площадки H)."""
    image = tex_concrete(seed, 0.95, 256, 0.15).convert('RGB')
    draw = ImageDraw.Draw(image)
    draw.ellipse((14, 14, 242, 242), outline=ring_color, width=12)
    draw.ellipse((40, 40, 216, 216), outline=ring_color, width=4)
    font = ImageFont.truetype(FONT_TITLE, letter_size)
    draw.text((128, 132), label, font=font, fill=(222, 216, 200), anchor='mm')
    noise = (normalize(fft_noise(256, 1.2, seed + 9)) * 40 - 20)[..., None]
    pixels = np.clip(np.array(image).astype(float) + noise, 0, 255)
    return Image.fromarray(pixels.astype(np.uint8))


def tex_glow(color):
    size = 64
    y, x = np.mgrid[0:size, 0:size] / size - 0.5
    distance = np.hypot(x, y) * 2
    level = np.clip(1.2 - distance * 0.6, 0, 1)
    pixels = np.array(color)[None, None, :] * level[..., None]
    return Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))


def tex_water(seed):
    size = 256
    field = fft_noise(size, 1.2, seed)
    ripple = normalize(np.sin(field * 9) + 0.5 * fft_noise(size, 0.9, seed + 1))
    color = colorize(ripple, [(0, (28, 58, 62)), (0.6, (54, 96, 98)), (0.9, (120, 160, 156)), (1, (190, 214, 206))])
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def make_textures():
    TEXTURES['grass'] = tex_grass(11, (78, 94, 44))
    TEXTURES['grass2'] = tex_grass(12, (64, 82, 40))
    TEXTURES['rock'] = tex_rock(21)
    TEXTURES['rockmoss'] = tex_rock(22, moss=0.55)
    TEXTURES['shore'] = tex_shore(31)
    TEXTURES['seabed'] = tex_seabed(41)
    TEXTURES['concrete'] = tex_concrete(51)
    TEXTURES['concrete_dark'] = tex_concrete(52, 0.7, 64, 0.45)
    TEXTURES['concrete_tiles'] = tex_concrete(53, 0.95, 128, 0.25, tiles=True)
    TEXTURES['metal_panel'] = tex_metal(61, (112, 122, 128), 64, 0.15)
    TEXTURES['metal_rust'] = tex_metal(62, (100, 92, 84), 32, 0.55)
    TEXTURES['metal_grate'] = tex_grate(63)
    TEXTURES['rocket_white'] = tex_metal(64, (196, 198, 194), 128, 0.08)
    TEXTURES['hazard'] = tex_hazard(71)
    TEXTURES['container_red'] = tex_container(81, (138, 50, 36))
    TEXTURES['container_blue'] = tex_container(82, (44, 70, 120))
    TEXTURES['container_orange'] = tex_container(83, (176, 104, 40))
    TEXTURES['helipad'] = tex_marked_pad(91, 'H', (190, 160, 50), 170)
    for index, letter in enumerate('ABCDE'):
        TEXTURES[f'beacon_{letter.lower()}'] = tex_marked_pad(100 + index, letter, (60, 200, 220))
    TEXTURES['glow_cyan'] = tex_glow((120, 240, 255))
    TEXTURES['glow_red'] = tex_glow((255, 70, 50))
    TEXTURES['glow_orange'] = tex_glow((255, 170, 60))
    TEXTURES['water'] = tex_water(111)
    for name, image in TEXTURES.items():
        save_texture(f'textures/walker/{name}', image)


def save_texture(path, image):
    data_path = os.path.join(DATA, path + '.jpg')
    os.makedirs(os.path.dirname(data_path), exist_ok=True)
    image.convert('RGB').save(data_path, quality=90)
    compile_path = os.path.join(COMPILE, path + '.tga')
    os.makedirs(os.path.dirname(compile_path), exist_ok=True)
    image.convert('RGB').save(compile_path)


# ================================================================ небо

def sky_color(direction):
    """цвет неба по направлению (массив [..., 3], нормированный)."""
    elevation = direction[..., 2]
    zenith = np.array([62, 104, 150])
    horizon = np.array([186, 196, 194])
    below = np.array([120, 130, 128])
    up = np.clip(elevation, 0, 1)[..., None] ** 0.55
    color = horizon * (1 - up) + zenith * up
    color = np.where(elevation[..., None] < 0, below + (horizon - below) * np.clip(1 + elevation * 6, 0, 1)[..., None], color)
    # облака: сумма волн по направлению, гуще у горизонта
    cloud = np.zeros(direction.shape[:-1])
    local = np.random.default_rng(5)
    for octave in range(6):
        frequency = 2.5 * 2 ** octave
        for _ in range(3):
            axis = local.normal(0, 1, 3)
            axis /= np.linalg.norm(axis)
            cloud += np.sin((direction @ axis) * frequency + local.random() * 6.28) / (1.6 ** octave)
    cloud = np.clip((cloud - 0.35) * 0.9, 0, 1) * np.clip(elevation * 8, 0, 1) * (1 - 0.5 * np.clip(elevation, 0, 1))
    color = color * (1 - cloud[..., None] * 0.75) + np.array([236, 236, 232]) * cloud[..., None] * 0.75
    # дымка и свечение вокруг солнца
    azimuth, sun_elevation = math.radians(SUN_AZIMUTH), math.radians(SUN_ELEVATION)
    sun = np.array([math.cos(sun_elevation) * math.cos(azimuth), math.cos(sun_elevation) * math.sin(azimuth), math.sin(sun_elevation)])
    closeness = np.clip(direction @ sun, 0, 1)
    color = color + np.array([255, 240, 200])[None, None, :] * (closeness ** 40 * 0.6 + closeness ** 600 * 1.5)[..., None]
    return np.clip(color, 0, 255)


def make_sky():
    size = 256
    axis_map = [(3, -1, 2), (-3, 1, 2), (1, 3, 2), (-1, -3, 2), (-2, -1, 3), (2, -1, -3)]
    texorder = [0, 2, 1, 3, 4, 5]
    suffix = ['rt', 'bk', 'lf', 'ft', 'up', 'dn']
    v_index, u_index = np.mgrid[0:size, 0:size] + 0.5
    s_coord = u_index / size * 2 - 1
    t_coord = 1 - v_index / size * 2
    base = np.stack([s_coord, t_coord, np.ones_like(s_coord)], axis=-1)
    for axis, mapping in enumerate(axis_map):
        direction = np.zeros_like(base)
        for component, source in enumerate(mapping):
            value = base[..., abs(source) - 1]
            direction[..., component] = value if source > 0 else -value
        direction /= np.linalg.norm(direction, axis=-1, keepdims=True)
        image = Image.fromarray(sky_color(direction).astype(np.uint8))
        save_texture(f'env/walker_sky_{suffix[texorder[axis]]}', image)


# ================================================================ брашевая геометрия

DETAIL = 134217728
BRUSHES = []
SHADER_SCALE = {}      # масштаб текстуры по умолчанию для шейдера


def texture_scale(shader):
    name = shader.split('/')[-1]
    if name in ('grass', 'grass2', 'rock', 'rockmoss', 'shore', 'seabed'):
        return 0.5
    if name.startswith('beacon_') or name == 'helipad':
        return 0.75
    if name.startswith('container'):
        return 0.25
    return 0.25


def oriented(points, inside):
    p0, p1, p2 = [np.array(point, dtype=float) for point in points]
    normal = np.cross(p0 - p1, p2 - p1)
    if np.dot(normal, np.array(inside, dtype=float) - p1) > 0:
        return [points[2], points[1], points[0]]
    return points


def fmt(value):
    value = round(float(value), 3)
    return str(int(value)) if value == int(value) else f'{value:g}'


class Brush:
    def __init__(self, detail=True):
        self.faces = []
        self.detail = detail

    def face(self, points, shader, shift=(0, 0), scale=None):
        self.faces.append((points, shader, shift, scale))

    def text(self, inside):
        lines = ['{']
        for points, shader, shift, scale in self.faces:
            ordered = oriented(points, inside)
            scale = scale or texture_scale(shader)
            contents = DETAIL if self.detail else 0
            coords = ' '.join('( ' + ' '.join(fmt(value) for value in point) + ' )' for point in ordered)
            lines.append(f'{coords} {shader} {fmt(shift[0])} {fmt(shift[1])} 0 {scale} {scale} {contents} 0 0')
        lines.append('}')
        return '\n'.join(lines)


CAULK = 'common/caulk'


def tex(name):
    return name if name.startswith('common/') else f'walker/{name}'


def add_prism(polygon, z_bottom, z_top, top, side, bottom=CAULK, detail=True, top_points=None, top_shift=(0, 0), top_scale=None):
    """выпуклая призма: polygon — вершины против часовой стрелки (вид сверху).
    top_points — три точки наклонной верхней плоскости (иначе плоская на z_top)."""
    brush = Brush(detail)
    polygon = [(int(round(x)), int(round(y))) for x, y in polygon]
    if top_points is None:
        top_points = [(polygon[0][0], polygon[0][1], z_top), (polygon[1][0], polygon[1][1], z_top), (polygon[2][0], polygon[2][1], z_top)]
    brush.face(top_points, tex(top), top_shift, top_scale)
    brush.face([(0, 0, z_bottom), (1, 0, z_bottom), (0, 1, z_bottom)], tex(bottom))
    count = len(polygon)
    for index in range(count):
        ax, ay = polygon[index]
        bx, by = polygon[(index + 1) % count]
        brush.face([(ax, ay, z_bottom), (bx, by, z_bottom), (bx, by, z_bottom + 64)], tex(side))
    center_x = sum(point[0] for point in polygon) / count
    center_y = sum(point[1] for point in polygon) / count
    lowest_top = min(point[2] for point in top_points)
    if z_top is not None:
        lowest_top = min(lowest_top, z_top)
    BRUSHES.append((brush, (center_x, center_y, (z_bottom + lowest_top) / 2)))


def add_box(x0, y0, z0, x1, y1, z1, side, top=None, bottom=CAULK, detail=True):
    x0, x1 = sorted((x0, x1)); y0, y1 = sorted((y0, y1))
    add_prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1, top or side, side, bottom, detail)


def polygon_regular(center_x, center_y, radius, sides=8, rotation=None):
    rotation = math.pi / sides if rotation is None else rotation
    return [(center_x + radius * math.cos(rotation + 2 * math.pi * index / sides),
             center_y + radius * math.sin(rotation + 2 * math.pi * index / sides)) for index in range(sides)]


def add_frustum(center_x, center_y, radius_bottom, radius_top, z0, z1, side, top, sides=8, twist=0.0):
    brush = Brush(True)
    bottom_poly = polygon_regular(center_x, center_y, radius_bottom, sides)
    top_poly = polygon_regular(center_x, center_y, radius_top, sides, math.pi / sides + twist)
    bottom_poly = [(round(x), round(y)) for x, y in bottom_poly]
    top_poly = [(round(x), round(y)) for x, y in top_poly]
    brush.face([(0, 0, z1), (1, 0, z1), (0, 1, z1)], tex(top))
    brush.face([(0, 0, z0), (1, 0, z0), (0, 1, z0)], tex(CAULK))
    for index in range(sides):
        following = (index + 1) % sides
        brush.face([(bottom_poly[index][0], bottom_poly[index][1], z0), (bottom_poly[following][0], bottom_poly[following][1], z0),
                    (top_poly[index][0], top_poly[index][1], z1)], tex(side))
    BRUSHES.append((brush, (center_x, center_y, (z0 + z1) / 2)))


def add_ramp(x0, y0, x1, y1, z_low, z_high, rise_toward, side='concrete_dark', top='concrete', z_bottom=-40):
    """пандус: прямоугольник, верх поднимается от z_low к z_high в сторону rise_toward (+x, -x, +y, -y)."""
    x0, x1 = sorted((x0, x1)); y0, y1 = sorted((y0, y1))
    if rise_toward == '+x':
        points = [(x0, y0, z_low), (x1, y0, z_high), (x1, y1, z_high)]
    elif rise_toward == '-x':
        points = [(x1, y0, z_low), (x0, y0, z_high), (x0, y1, z_high)]
    elif rise_toward == '+y':
        points = [(x0, y0, z_low), (x0, y1, z_high), (x1, y1, z_high)]
    else:
        points = [(x0, y1, z_low), (x0, y0, z_high), (x1, y0, z_high)]
    add_prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z_bottom, z_high, top, side, CAULK, True, points)


# ================================================================ рельеф

HALF_X, HALF_Y = 1792, 1344          # граница карты (скалы упираются в оболочку неба)
CELL = 64
PLAY_X, PLAY_Y = 1472, 1024          # невидимые стены по краю игровой зоны
WATER_Z = 0
SEABED_MIN = -20                     # глубже воды нет: робот всегда идёт, а не плывёт
TERRAIN_BOTTOM = -64

ISLANDS = [  # x, y, радиус, амплитуда
    (0, 0, 560, 1.35), (-1060, 250, 400, 1.15), (1060, -250, 420, 1.15), (250, 780, 340, 1.05),
    (-360, -770, 340, 1.05), (-620, -230, 170, 0.9), (620, 380, 200, 0.95), (-1150, -720, 230, 1.0),
    (1150, 760, 230, 1.0), (-320, 660, 150, 0.85), (470, -760, 170, 0.9), (-1300, 700, 200, 0.9), (1300, -700, 200, 0.9),
]
FLATTEN = [  # x, y, радиус плоской зоны, высота, ширина перехода
    (0, 0, 360, 10, 240), (-1060, 250, 150, 18, 160), (1060, -250, 150, 18, 160), (250, 800, 230, 18, 170),
    (-360, -770, 150, 18, 150),
]


def height_map():
    xs = np.arange(-HALF_X, HALF_X + 1, CELL, dtype=float)
    ys = np.arange(-HALF_Y, HALF_Y + 1, CELL, dtype=float)
    grid_x, grid_y = np.meshgrid(xs, ys)
    field = np.zeros_like(grid_x)
    for center_x, center_y, radius, amplitude in ISLANDS:
        field += amplitude * np.exp(-((grid_x - center_x) ** 2 + (grid_y - center_y) ** 2) / (radius * 0.75) ** 2)
    shape = grid_x.shape
    low_noise = fft_noise(0, 1.9, 301, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    fine_noise = fft_noise(0, 1.1, 302, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    raw = -30 + 64 * (field + 0.22 * low_noise) + 6 * fine_noise
    land = raw > 0
    raw[land] = 46 * np.tanh(raw[land] / 46)
    height = np.maximum(raw, SEABED_MIN)
    # скалы по краям
    edge = np.minimum(HALF_X - np.abs(grid_x), HALF_Y - np.abs(grid_y))
    edge_noise = fft_noise(0, 1.7, 303, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    width = 470 + 90 * edge_noise
    closeness = np.clip(1 - edge / width, 0, 1)
    rocky = fft_noise(0, 1.2, 304, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    cliff = 600 * closeness ** 1.6 + 70 * rocky * closeness
    height = np.maximum(height, np.where(closeness > 0, cliff - 22, -999))
    # плоские площадки под постройки
    for center_x, center_y, radius, level, falloff in FLATTEN:
        distance = np.hypot(grid_x - center_x, grid_y - center_y)
        weight = np.clip(1 - (distance - radius) / falloff, 0, 1)
        weight = weight * weight * (3 - 2 * weight)
        height = height * (1 - weight) + level * weight
    return xs, ys, np.round(height / 2) * 2


XS, YS, HEIGHTS = None, None, None


def ground(x, y):
    """высота рельефа в точке (билинейно)."""
    column = (x + HALF_X) / CELL
    row = (y + HALF_Y) / CELL
    col0, row0 = int(math.floor(column)), int(math.floor(row))
    col0 = min(max(col0, 0), len(XS) - 2); row0 = min(max(row0, 0), len(YS) - 2)
    frac_x, frac_y = column - col0, row - row0
    h00, h10 = HEIGHTS[row0, col0], HEIGHTS[row0, col0 + 1]
    h01, h11 = HEIGHTS[row0 + 1, col0], HEIGHTS[row0 + 1, col0 + 1]
    return (h00 * (1 - frac_x) + h10 * frac_x) * (1 - frac_y) + (h01 * (1 - frac_x) + h11 * frac_x) * frac_y


def terrain_shader(points, variety):
    a, b, c = [np.array(point, dtype=float) for point in points]
    normal = np.cross(b - a, c - a)
    normal /= np.linalg.norm(normal)
    mean_height = (a[2] + b[2] + c[2]) / 3
    if abs(normal[2]) < 0.72:
        return 'rock' if mean_height > 60 or variety > 0.3 else 'rockmoss'
    if mean_height < -5:
        return 'seabed'
    if mean_height < 5:
        return 'shore'
    if mean_height > 110:
        return 'rockmoss'
    return 'grass' if variety < 0.55 else 'grass2'


def add_terrain():
    variety_field = normalize(fft_noise(0, 1.5, 305, (len(YS) * 2, len(XS) * 2))[:len(YS), :len(XS)])
    counts = {}
    for row in range(len(YS) - 1):
        for col in range(len(XS) - 1):
            x0, x1 = int(XS[col]), int(XS[col + 1])
            y0, y1 = int(YS[row]), int(YS[row + 1])
            h00, h10 = int(HEIGHTS[row, col]), int(HEIGHTS[row, col + 1])
            h01, h11 = int(HEIGHTS[row + 1, col]), int(HEIGHTS[row + 1, col + 1])
            corner00, corner10 = (x0, y0, h00), (x1, y0, h10)
            corner01, corner11 = (x0, y1, h01), (x1, y1, h11)
            if (row + col) % 2 == 0:
                triangles = [(corner00, corner10, corner11), (corner00, corner11, corner01)]
            else:
                triangles = [(corner00, corner10, corner01), (corner10, corner11, corner01)]
            variety = variety_field[row, col]
            for triangle in triangles:
                shader = terrain_shader(triangle, variety)
                counts[shader] = counts.get(shader, 0) + 1
                polygon = [(point[0], point[1]) for point in triangle]
                area = (polygon[1][0] - polygon[0][0]) * (polygon[2][1] - polygon[0][1]) - (polygon[2][0] - polygon[0][0]) * (polygon[1][1] - polygon[0][1])
                if area < 0:
                    polygon.reverse()
                add_prism(polygon, TERRAIN_BOTTOM, None, shader, CAULK, CAULK, True, list(triangle))
    print('terrain triangles:', counts)


# ================================================================ постройки

BEACONS = {}


def beacon(letter, x, y, base_z):
    """маяк: круглая площадка с буквой, столбик и светящийся фонарь."""
    BEACONS[letter] = (x, y, base_z + 4)
    shift = (128 / 0.75 - x / 0.75, 128 / 0.75 + y / 0.75)
    pad = polygon_regular(x, y, 96, 12)
    add_prism(pad, base_z - 30, base_z + 4, f'beacon_{letter}', 'hazard', CAULK, True, None, shift)
    post_x, post_y = x + 70, y + 70
    add_box(post_x - 6, post_y - 6, base_z + 4, post_x + 6, post_y + 6, base_z + 96, 'metal_panel')
    add_box(post_x - 12, post_y - 12, base_z + 96, post_x + 12, post_y + 12, base_z + 116, 'beacon_light')
    add_box(post_x - 14, post_y - 14, base_z + 116, post_x + 14, post_y + 14, base_z + 120, 'metal_panel')


def helipad(x, y, z_top, ramp_side):
    shift = (128 / 0.75 - x / 0.75, 128 / 0.75 + y / 0.75)
    add_prism(polygon_regular(x, y, 140, 8), z_top - 10, z_top, 'helipad', 'hazard', 'concrete_dark', True, None, shift)
    for offset_x, offset_y in [(-70, -70), (70, -70), (70, 70), (-70, 70)]:
        add_box(x + offset_x - 14, y + offset_y - 14, TERRAIN_BOTTOM + 30, x + offset_x + 14, y + offset_y + 14, z_top - 10, 'concrete_dark')
    for corner in polygon_regular(x, y, 128, 8)[::2]:
        add_box(corner[0] - 5, corner[1] - 5, z_top, corner[0] + 5, corner[1] + 5, z_top + 4, 'glow_orange')
    length = 230
    if ramp_side == '-x':
        add_ramp(x - 130 - length, y - 50, x - 120, y + 50, -24, z_top, '+x')
    elif ramp_side == '+x':
        add_ramp(x + 120, y - 50, x + 130 + length, y + 50, -24, z_top, '-x')
    elif ramp_side == '+y':
        add_ramp(x - 50, y + 120, x + 50, y + 130 + length, -24, z_top, '-y')
    else:
        add_ramp(x - 50, y - 130 - length, x + 50, y - 120, -24, z_top, '+y')


def broken_wall(x0, y0, x1, y1, z_base, height, thickness=16, seed=0, side='concrete'):
    """стена с обломанным верхом: ряд блоков разной высоты, местами провалы."""
    local = np.random.default_rng(seed)
    length = math.hypot(x1 - x0, y1 - y0)
    segments = max(1, int(length // 48))
    horizontal = abs(x1 - x0) >= abs(y1 - y0)
    for index in range(segments):
        if local.random() < 0.18 and 0 < index < segments - 1:
            continue                      # пролом
        start = index / segments
        end = (index + 1) / segments
        top = z_base + height * local.uniform(0.45, 1.0)
        if horizontal:
            sx0, sx1 = x0 + (x1 - x0) * start, x0 + (x1 - x0) * end
            add_box(sx0, y0 - thickness / 2, z_base - 20, sx1, y0 + thickness / 2, top, side, 'concrete_dark')
        else:
            sy0, sy1 = y0 + (y1 - y0) * start, y0 + (y1 - y0) * end
            add_box(x0 - thickness / 2, sy0, z_base - 20, x0 + thickness / 2, sy1, top, side, 'concrete_dark')


def container(x, y, z, along_x=True, color='container_red', stack=1):
    length, width, height = 72, 30, 30
    for level in range(stack):
        z0 = z + level * height
        if along_x:
            add_box(x - length / 2, y - width / 2, z0, x + length / 2, y + width / 2, z0 + height, color, 'metal_rust', 'metal_rust')
        else:
            add_box(x - width / 2, y - length / 2, z0, x + width / 2, y + length / 2, z0 + height, color, 'metal_rust', 'metal_rust')


def platform(x0, y0, x1, y1, z_top, ramps):
    """плита на опорах над водой с пандусами."""
    add_box(x0, y0, z_top - 12, x1, y1, z_top, 'concrete_dark', 'concrete_tiles', 'concrete_dark')
    step_x = (x1 - x0) / 3
    step_y = (y1 - y0) / 2
    for column in range(4):
        for row in range(3):
            pillar_x = x0 + 20 + (x1 - x0 - 40) * column / 3
            pillar_y = y0 + 20 + (y1 - y0 - 40) * row / 2
            add_box(pillar_x - 12, pillar_y - 12, TERRAIN_BOTTOM + 30, pillar_x + 12, pillar_y + 12, z_top - 12, 'concrete_dark')
    for side, center, half_width, length in ramps:
        if side == '-x':
            add_ramp(x0 - length, center - half_width, x0, center + half_width, -24, z_top, '+x')
        elif side == '+x':
            add_ramp(x1, center - half_width, x1 + length, center + half_width, -24, z_top, '-x')
        elif side == '-y':
            add_ramp(center - half_width, y0 - length, center + half_width, y0, -24, z_top, '+y')
        else:
            add_ramp(center - half_width, y1, center + half_width, y1 + length, -24, z_top, '-y')
    # кромка с полосами
    for edge in [(x0, y0, x1, y0 + 4), (x0, y1 - 4, x1, y1)]:
        add_box(edge[0], edge[1], z_top, edge[2], edge[3], z_top + 3, 'hazard')


def central_tower():
    plaza_top = 24
    add_prism(polygon_regular(0, 0, 330, 8), -30, plaza_top, 'concrete_tiles', 'concrete_dark')
    for side, (x0, y0, x1, y1, rise) in {
        'n': (-70, 300, 70, 520, '-y'), 's': (-70, -520, 70, -300, '+y'),
        'e': (300, -70, 520, 70, '-x'), 'w': (-520, -70, -300, 70, '+x')}.items():
        add_ramp(x0, y0, x1, y1, -10, plaza_top, rise)
    # башня
    add_prism(polygon_regular(0, 0, 64, 8), plaza_top, 470, 'metal_panel', 'metal_panel')
    add_frustum(0, 0, 64, 26, 470, 520, 'metal_panel', 'metal_panel')
    add_box(-5, -5, 520, 5, 5, 660, 'metal_rust')
    add_box(-9, -9, 660, 9, 9, 676, 'glow_red')
    for band_z in (120, 300, 430):
        add_prism(polygon_regular(0, 0, 70, 8), band_z, band_z + 10, 'hazard', 'hazard')
    # кольцевая платформа
    segments = 16
    for index in range(segments):
        angle0 = 2 * math.pi * index / segments
        angle1 = 2 * math.pi * (index + 1) / segments
        inner, outer = 62, 210
        polygon = [(inner * math.cos(angle0), inner * math.sin(angle0)), (outer * math.cos(angle0), outer * math.sin(angle0)),
                   (outer * math.cos(angle1), outer * math.sin(angle1)), (inner * math.cos(angle1), inner * math.sin(angle1))]
        add_prism(polygon, 236, 248, 'metal_grate', 'hazard', 'metal_rust')
        rail = [(outer * math.cos(angle0), outer * math.sin(angle0)), ((outer + 8) * math.cos(angle0), (outer + 8) * math.sin(angle0)),
                ((outer + 8) * math.cos(angle1), (outer + 8) * math.sin(angle1)), (outer * math.cos(angle1), outer * math.sin(angle1))]
        add_prism(rail, 248, 262, 'metal_rust', 'metal_rust')
    for angle in range(45, 360, 90):
        radians = math.radians(angle)
        brace_x, brace_y = 180 * math.cos(radians), 180 * math.sin(radians)
        add_box(brace_x - 8, brace_y - 8, plaza_top, brace_x + 8, brace_y + 8, 236, 'metal_rust')
        for light_z in (250,):
            add_box(brace_x - 6, brace_y - 6, 262, brace_x + 6, brace_y + 6, 272, 'glow_cyan')
    # укрытия на площади
    broken_wall(-250, 150, -130, 150, plaza_top, 52, seed=1)
    broken_wall(130, -150, 250, -150, plaza_top, 52, seed=2)
    broken_wall(150, 130, 150, 250, plaza_top, 52, seed=3)
    broken_wall(-150, -250, -150, -130, plaza_top, 52, seed=4)
    beacon('b', -40, -230, plaza_top)


def launch_tower():
    base_x, base_y = 330, 880
    add_box(base_x - 150, base_y - 150, -20, base_x + 150, base_y + 150, 22, 'concrete_dark', 'concrete_tiles')
    add_ramp(base_x - 60, base_y - 300, base_x + 60, base_y - 150, 4, 22, '+y')
    top = 22
    half = 56
    for corner_x, corner_y in [(-half, -half), (half, -half), (half, half), (-half, half)]:
        add_box(base_x + corner_x - 8, base_y + corner_y - 8, top, base_x + corner_x + 8, base_y + corner_y + 8, 600, 'metal_rust')
    for level_z in range(110, 600, 110):
        for x0, y0, x1, y1 in [(-half, -half, half, -half + 8), (-half, half - 8, half, half), (-half, -half, -half + 8, half), (half - 8, -half, half, half)]:
            add_box(base_x + x0, base_y + y0, level_z, base_x + x1, base_y + y1, level_z + 10, 'metal_rust')
    add_box(base_x - half - 20, base_y - half - 20, 600, base_x + half + 20, base_y + half + 20, 612, 'metal_grate', 'metal_grate', 'metal_rust')
    add_box(base_x - 6, base_y - 6, 612, base_x + 6, base_y + 6, 640, 'glow_red')
    # ракета рядом с фермой
    rocket_x, rocket_y = base_x + 120, base_y + 10
    add_prism(polygon_regular(rocket_x, rocket_y, 38, 8), top, 430, 'rocket_white', 'rocket_white')
    add_frustum(rocket_x, rocket_y, 38, 6, 430, 520, 'rocket_white', 'rocket_white')
    for fin_angle in range(0, 360, 90):
        radians = math.radians(fin_angle + 22.5)
        fin_x, fin_y = rocket_x + 44 * math.cos(radians), rocket_y + 44 * math.sin(radians)
        add_box(fin_x - 8, fin_y - 8, top, fin_x + 8, fin_y + 8, top + 70, 'metal_panel')
    # переходной мостик к ракете
    add_box(base_x + half, base_y - 8, 380, rocket_x - 30, base_y + 16, 388, 'metal_grate')
    beacon('d', 120, 700, ground(120, 700))


def rock_stack(x, y, height, radius, seed):
    local = np.random.default_rng(seed)
    base = ground(x, y)
    add_frustum(x, y, radius, radius * local.uniform(0.45, 0.7), -40, base + height, 'rock', 'rockmoss', 7, local.uniform(-0.4, 0.4))
    add_frustum(x + radius * 0.2, y - radius * 0.1, radius * 0.55, radius * 0.25, base + height - 4, base + height + height * 0.25, 'rock', 'rockmoss', 6)


def ruins_west():
    level = 18
    beacon('a', -1060, 250, level)
    broken_wall(-1250, 420, -1010, 420, level, 140, seed=11)
    broken_wall(-1250, 420, -1250, 560, level, 120, seed=12)
    broken_wall(-1010, 420, -1010, 520, level, 100, seed=13)
    broken_wall(-1250, 560, -1080, 560, level, 90, seed=14)
    container(-900, 120, ground(-900, 120), True, 'container_blue', 2)
    container(-930, 60, ground(-930, 60), False, 'container_orange')
    helipad(-760, 660, 30, '-y')


def ruins_east():
    level = 18
    beacon('c', 1060, -250, level)
    broken_wall(1010, -440, 1250, -440, level, 130, seed=21)
    broken_wall(1250, -560, 1250, -440, level, 110, seed=22)
    broken_wall(1010, -560, 1010, -470, level, 100, seed=23)
    container(900, -80, ground(900, -80), True, 'container_red', 2)
    container(950, -20, ground(950, -20), False, 'container_blue')
    container(1180, -60, ground(1180, -60), True, 'container_orange')
    helipad(760, -680, 30, '+y')


def ruins_south():
    beacon('e', -360, -770, 18)
    platform(-760, -520, -440, -330, 40, [('+x', -425, 50, 200), ('-y', -600, 50, 180)])
    broken_wall(-740, -360, -560, -360, 40, 70, seed=31)
    broken_wall(-470, -500, -470, -380, 40, 60, seed=32)
    container(-160, -880, ground(-160, -880), True, 'container_orange', 1)


def ruins_middle():
    platform(430, 80, 760, 260, 36, [('-x', 170, 50, 200), ('+y', 600, 50, 180), ('-y', 640, 50, 180)])
    broken_wall(470, 240, 640, 240, 36, 64, seed=41)
    broken_wall(740, 100, 740, 200, 36, 54, seed=42)
    platform(-760, 60, -520, 200, 32, [('+x', 130, 50, 190), ('-x', 130, 50, 160)])
    broken_wall(-700, 180, -580, 180, 32, 58, seed=43)
    for index, (x, y, height, radius) in enumerate([(-520, 470, 150, 54), (560, -420, 120, 46), (-180, 330, 90, 40),
                                                    (180, -420, 110, 44), (-900, -350, 130, 50), (880, 420, 140, 52),
                                                    (-1250, -250, 100, 44), (1260, 260, 110, 46)]):
        rock_stack(x, y, height, radius, 500 + index)


def add_hull():
    """оболочка неба и невидимые стены по краю игровой зоны."""
    margin = 16
    low, high = TERRAIN_BOTTOM - 64, 1700
    x0, x1, y0, y1 = -HALF_X, HALF_X, -HALF_Y, HALF_Y
    hull = [
        (x0 - margin, y0 - margin, low - margin, x1 + margin, y1 + margin, low, 'common/caulk'),
        (x0 - margin, y0 - margin, high, x1 + margin, y1 + margin, high + margin, 'sky'),
        (x0 - margin, y0 - margin, low, x0, y1 + margin, high, 'sky'),
        (x1, y0 - margin, low, x1 + margin, y1 + margin, high, 'sky'),
        (x0, y0 - margin, low, x1, y0, high, 'sky'),
        (x0, y1, low, x1, y1 + margin, high, 'sky'),
    ]
    for bx0, by0, bz0, bx1, by1, bz1, shader in hull:
        add_box(bx0, by0, bz0, bx1, by1, bz1, shader, shader, shader, detail=False)
    clip = 'common/clip'
    thickness = 32
    for bx0, by0, bx1, by1 in [(-PLAY_X - thickness, -PLAY_Y - thickness, PLAY_X + thickness, -PLAY_Y),
                               (-PLAY_X - thickness, PLAY_Y, PLAY_X + thickness, PLAY_Y + thickness),
                               (-PLAY_X - thickness, -PLAY_Y, -PLAY_X, PLAY_Y), (PLAY_X, -PLAY_Y, PLAY_X + thickness, PLAY_Y)]:
        add_box(bx0, by0, low, bx1, by1, high - 8, clip, clip, clip)


def add_water():
    shader = 'water'
    add_box(-HALF_X, -HALF_Y, TERRAIN_BOTTOM, HALF_X, HALF_Y, WATER_Z, shader, shader, shader)


# ================================================================ сущности и вывод

def spawn_points():
    spots = [(-1340, -620), (-1340, -150), (-1320, 380), (-1250, 880), (1340, 620), (1340, 150), (1320, -380), (1250, -880),
             (-120, 520), (120, -520), (-600, -820), (640, 820), (-850, 820), (850, -830), (-280, -380), (300, 420)]
    entities = []
    for x, y in spots:
        angle = math.degrees(math.atan2(-y, -x))
        z = max(ground(x, y), WATER_Z - 20) + 40
        entities.append({'classname': 'info_player_deathmatch', 'origin': f'{x} {y} {int(z)}', 'angle': str(int(angle))})
    return entities


def write_map():
    worldspawn = {
        'classname': 'worldspawn', 'message': 'Islands', 'ambient': '36', '_color': '0.75 0.82 0.9',
        'gridsize': '128 128 128', '_blocksize': '0 0 0',
    }
    entities = spawn_points()
    entities.append({'classname': 'info_player_intermission', 'origin': '-900 -700 520', 'angles': '18 35 0'})
    # маяки: target_location нужны ботам как точки сбора и игре для названий мест
    for letter, (x, y, z) in BEACONS.items():
        entities.append({'classname': 'target_location', 'origin': f'{x} {y} {int(z) + 40}', 'message': f'Beacon {letter.upper()}'})
    for x, y in [(0, 0), (-1000, 400), (1000, -400), (0, 900), (0, -900)]:
        entities.append({'classname': 'target_speaker', 'origin': f'{x} {y} 120', 'noise': 'sound/world/wind_loop.wav', 'spawnflags': '1'})
    lines = ['// WALKER map wk_islands, generated by walker/tools/make_map_islands.py', '{']
    lines += [f'"{key}" "{value}"' for key, value in worldspawn.items()]
    for brush, inside in BRUSHES:
        lines.append(brush.text(inside))
    lines.append('}')
    for entity in entities:
        lines.append('{')
        lines += [f'"{key}" "{value}"' for key, value in entity.items()]
        lines.append('}')
    path = os.path.join(COMPILE, 'maps', MAP_NAME + '.map')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='ascii', newline='\n') as file:
        file.write('\n'.join(lines) + '\n')
    print('brushes:', len(BRUSHES), '->', path)


SHADERS = f"""// WALKER map shaders: wk_islands
textures/common/caulk
{{
	surfaceparm nodraw
	surfaceparm nolightmap
	surfaceparm nomarks
}}
textures/common/clip
{{
	qer_trans 0.4
	surfaceparm nodraw
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	surfaceparm noimpact
	surfaceparm playerclip
}}
textures/walker/sky
{{
	qer_editorimage env/walker_sky_up.tga
	surfaceparm sky
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm nomarks
	q3map_sun 1 0.95 0.86 160 {SUN_AZIMUTH:g} {SUN_ELEVATION:g}
	q3map_surfacelight 0
	skyparms env/walker_sky - -
}}
textures/walker/water
{{
	qer_editorimage textures/walker/water.tga
	qer_trans 0.5
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm water
	surfaceparm nolightmap
	surfaceparm nomarks
	cull disable
	tessSize 256
	deformVertexes wave 128 sin 0 1.5 0 0.3
	{{
		map textures/walker/water.jpg
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen const 0.62
		rgbGen identity
		tcMod scale 0.5 0.5
		tcMod scroll 0.012 0.02
	}}
	{{
		map textures/walker/water.jpg
		blendFunc GL_ONE GL_ONE
		rgbGen const ( 0.22 0.25 0.25 )
		tcMod scale 0.37 0.37
		tcMod turb 0 0.04 0 0.08
		tcMod scroll -0.02 0.008
	}}
}}
textures/walker/beacon_light
{{
	qer_editorimage textures/walker/glow_cyan.tga
	q3map_lightimage textures/walker/glow_cyan.tga
	q3map_surfacelight 600
	surfaceparm nomarks
	surfaceparm nolightmap
	{{
		map textures/walker/glow_cyan.jpg
		rgbGen wave sin 0.75 0.25 0 0.6
	}}
}}
textures/walker/glow_red
{{
	qer_editorimage textures/walker/glow_red.tga
	q3map_lightimage textures/walker/glow_red.tga
	q3map_surfacelight 300
	surfaceparm nomarks
	surfaceparm nolightmap
	{{
		map textures/walker/glow_red.jpg
		rgbGen wave square 0.4 0.6 0 0.5
	}}
}}
textures/walker/glow_cyan
{{
	qer_editorimage textures/walker/glow_cyan.tga
	q3map_lightimage textures/walker/glow_cyan.tga
	q3map_surfacelight 300
	surfaceparm nomarks
	surfaceparm nolightmap
	{{
		map textures/walker/glow_cyan.jpg
	}}
}}
textures/walker/glow_orange
{{
	qer_editorimage textures/walker/glow_orange.tga
	q3map_lightimage textures/walker/glow_orange.tga
	q3map_surfacelight 200
	surfaceparm nomarks
	surfaceparm nolightmap
	{{
		map textures/walker/glow_orange.jpg
	}}
}}
textures/walker/metal_grate
{{
	qer_editorimage textures/walker/metal_grate.tga
	surfaceparm metalsteps
	{{
		map $lightmap
		rgbGen identity
	}}
	{{
		map textures/walker/metal_grate.jpg
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
	}}
}}
"""


def write_shaders():
    for root in (DATA, COMPILE):
        path = os.path.join(root, 'scripts', 'walker_islands.shader')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='ascii', newline='\n') as file:
            file.write(SHADERS)
    with open(os.path.join(COMPILE, 'scripts', 'shaderlist.txt'), 'w', encoding='ascii', newline='\n') as file:
        file.write('walker_islands\n')
    arena = '{\nmap "wk_islands"\nlongname "Islands"\nfraglimit "20"\ntimelimit "10"\ntype "ffa tourney team"\nbots "Anvil Bastion Cinder Drake Ember"\n}\n'
    with open(os.path.join(DATA, 'scripts', 'arenas.txt'), 'w', encoding='ascii', newline='\n') as file:
        file.write(arena)


def main():
    global XS, YS, HEIGHTS
    make_textures()
    make_sky()
    write_shaders()
    XS, YS, HEIGHTS = height_map()
    print('height range:', HEIGHTS.min(), HEIGHTS.max())
    add_hull()
    add_water()
    add_terrain()
    central_tower()
    launch_tower()
    ruins_west()
    ruins_east()
    ruins_south()
    ruins_middle()
    write_map()
    # карта высот для просмотра
    preview = normalize(HEIGHTS)
    water = HEIGHTS < 0
    image = np.stack([preview * 200 + 30] * 3, axis=-1)
    image[water] = [40, 90, 130]
    Image.fromarray(image[::-1].astype(np.uint8)).resize((len(XS) * 6, len(YS) * 6), Image.NEAREST).save('/tmp/islands_height.png')


main()
