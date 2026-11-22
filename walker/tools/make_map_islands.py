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

FOG_COLOR = (0.72, 0.76, 0.77)   # как небо у горизонта
FOG_DISTANCE = 12000
SUN_AZIMUTH = 140.0      # градусы, как в q3map_sun
SUN_ELEVATION = 40.0

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


TEX = 512     # размер текстур рельефа и построек


def soft(edge0, edge1, value):
    ratio = np.clip((value - edge0) / (edge1 - edge0), 0, 1)
    return ratio * ratio * (3 - 2 * ratio)


def worley(size, cells, seed, cells_y=None, warp=None):
    """бесшовный клеточный шум: (F1, F2, номер клетки) в пикселях.
    cells_y — число клеток по вертикали (вытянутые клетки), warp — смещение точек выборки (dx, dy)."""
    cells_y = cells_y or cells
    local = np.random.default_rng(seed)
    grid = np.stack(np.meshgrid(np.arange(cells), np.arange(cells_y), indexing='ij'), -1).reshape(-1, 2)
    points = (grid + local.random((len(grid), 2))) * [size / cells, size / cells_y]
    ids = np.arange(len(points))
    tiled, tiled_ids = [], []
    for dx in (-size, 0, size):
        for dy in (-size, 0, size):
            tiled.append(points + [dx, dy]); tiled_ids.append(ids)
    from scipy.spatial import cKDTree
    stretch = np.array([1.0, cells_y / cells])       # расстояния в «клеточной» метрике
    tree = cKDTree(np.concatenate(tiled) * stretch)
    all_ids = np.concatenate(tiled_ids)
    y, x = np.mgrid[0:size, 0:size] + 0.5
    if warp is not None:
        x = (x + warp[0]) % size; y = (y + warp[1]) % size
    distance, index = tree.query(np.stack([x.ravel(), y.ravel()], -1) * stretch, k=2)
    return (distance[:, 0].reshape(size, size), distance[:, 1].reshape(size, size),
            all_ids[index[:, 0]].reshape(size, size))


def strokes(size, count, seed, length, colors, width=1):
    """слой коротких штрихов (травинки) с переносом через край; RGBA."""
    local = np.random.default_rng(seed)
    layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(count):
        x, y = local.random(2) * size
        angle = local.random() * 2 * np.pi
        span = length[0] + local.random() * (length[1] - length[0])
        color = colors[local.integers(len(colors))]
        shade = 0.75 + 0.5 * local.random()
        fill = tuple(int(min(255, channel * shade)) for channel in color) + (int(150 + 100 * local.random()),)
        dx, dy = np.cos(angle) * span, np.sin(angle) * span
        for ox in (-size, 0, size):
            for oy in (-size, 0, size):
                if -span <= x + ox <= size + span and -span <= y + oy <= size + span:
                    draw.line((x + ox, y + oy, x + ox + dx, y + oy + dy), fill=fill, width=width)
    return layer


def to_image(color):
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_grass(seed, base_green):
    size = TEX
    large = normalize(fft_noise(size, 1.8, seed))
    mid = normalize(fft_noise(size, 1.2, seed + 1))
    fine = fft_noise(size, 0.6, seed + 2)
    field = 0.55 * large + 0.45 * mid
    base = np.array(base_green, dtype=float)
    color = colorize(field, [(0, base * 0.55), (0.45, base * 0.85), (0.75, base * 1.05 + [10, 6, -4]), (1, base * 1.1 + [34, 22, 0])])
    image = to_image(color).convert('RGBA')
    palette = [tuple((base * k + add).clip(0, 255).astype(int)) for k, add in
               ((0.7, [0, 0, 0]), (1.0, [0, 0, 0]), (1.15, [8, 6, 0]), (1.0, [40, 30, 2]), (0.85, [24, 12, 4]))]
    image.alpha_composite(strokes(size, 30000, seed + 3, (3, 8), palette))
    color = np.array(image.convert('RGB')).astype(float)
    # проплешины с землёй и камешками
    dirt = soft(0.72, 0.86, normalize(fft_noise(size, 1.7, seed + 4)))[..., None]
    soil = colorize(normalize(fft_noise(size, 0.8, seed + 5)), [(0, (60, 48, 34)), (1, (104, 88, 64))])
    color = color * (1 - dirt * 0.8) + soil * dirt * 0.8
    color *= (0.92 + 0.12 * fine)[..., None]
    color *= shade_relief(fine * 4, 1.0)[..., None] ** 0.5
    return to_image(color)


def tex_rock(seed, moss=0.0):
    """скала: горизонтальные плиты с неровными трещинами, слои, потёки, лишайник."""
    size = TEX
    warp = (fft_noise(size, 1.6, seed + 10) * 26, fft_noise(size, 1.6, seed + 11) * 18)
    f1, f2, cell = worley(size, 4, seed, cells_y=9, warp=warp)
    g1, g2, _ = worley(size, 13, seed + 20, warp=(warp[0] * 0.5, warp[1] * 0.5))
    local = np.random.default_rng(seed + 1)
    cell_tone = local.random(cell.max() + 1)[cell]
    cracks = soft(4.0, 0.8, f2 - f1)
    fine_cracks = soft(2.2, 0.4, g2 - g1) * soft(0.45, 0.7, normalize(fft_noise(size, 1.2, seed + 12)))
    y = np.arange(size)[:, None] / size
    detail = fft_noise(size, 1.0, seed + 2)
    strata = np.sin((y * 22 + fft_noise(size, 1.8, seed + 3) * 1.5) * 2 * np.pi) * 0.5 + 0.5
    height = 0.35 * soft(0, 40, f2 - f1) + 0.25 * cell_tone + 0.25 * detail + 0.15 * strata
    tone = normalize(0.4 * cell_tone + 0.35 * detail + 0.25 * strata)
    color = colorize(tone, [(0, (58, 55, 50)), (0.35, (88, 83, 76)), (0.7, (114, 108, 97)), (1, (136, 129, 115))])
    color *= shade_relief(height * 14, 1.0)[..., None] ** 0.8
    color *= (1 - 0.55 * cracks - 0.3 * fine_cracks)[..., None]
    lichen = soft(0.86, 0.93, normalize(fft_noise(size, 0.6, seed + 4)))[..., None]
    color = color * (1 - lichen * 0.45) + np.array([156, 150, 112]) * lichen * 0.45
    streak = ndimage_blur(fft_noise(size, 0.5, seed + 5), 1.5, 30)
    color *= (0.86 + 0.28 * normalize(streak))[..., None] ** 0.8
    if moss > 0:
        moss_field = normalize(fft_noise(size, 1.6, seed + 6) + 0.3 * (1 - cell_tone) + 0.6 * soft(0, 30, f1) * 0)
        amount = soft(1 - moss, 1 - moss + 0.22, moss_field)[..., None]
        moss_color = colorize(normalize(detail), [(0, (46, 62, 28)), (1, (92, 110, 50))])
        color = color * (1 - amount) + moss_color * amount * shade_relief(height * 12, 0.6)[..., None]
    return to_image(color)


def tex_shore(seed):
    """берег: влажный песок с галькой разного размера и пучками травы."""
    size = TEX
    f1, f2, cell = worley(size, 30, seed)
    local = np.random.default_rng(seed + 1)
    count = cell.max() + 1
    radius = (size / 30) * (0.25 + 0.3 * local.random(count))[cell]
    present = (local.random(count) > 0.45)[cell]
    dome = np.sqrt(np.clip(1 - (f1 / radius) ** 2, 0, 1)) * present
    pebble = soft(0.0, 0.25, dome)
    sand = normalize(fft_noise(size, 1.2, seed + 2))
    color = colorize(sand, [(0, (84, 76, 58)), (0.5, (110, 100, 77)), (1, (132, 120, 94))])
    palette = np.array([[118, 112, 102], [96, 90, 82], [132, 124, 108], [84, 80, 76], [110, 98, 82]])
    pebble_color = palette[local.integers(len(palette), size=count)][cell] * (0.9 + 0.2 * local.random(count))[cell][..., None]
    pebble_color = pebble_color * shade_relief(-dome * 5, 1.0)[..., None]
    color = color * (1 - pebble[..., None]) + pebble_color * pebble[..., None]
    wet = soft(0.5, 0.8, normalize(fft_noise(size, 1.8, seed + 3)))[..., None]
    color *= 1 - wet * 0.22
    image = to_image(color).convert('RGBA')
    image.alpha_composite(strokes(size, 2500, seed + 4, (3, 7), [(70, 88, 40), (96, 104, 52)]))
    return image.convert('RGB')


def tex_seabed(seed):
    size = TEX
    field = normalize(fft_noise(size, 1.5, seed))
    x = np.arange(size)[None, :] / size
    ripple = np.sin((x * 10 + field * 1.2) * 2 * np.pi)
    color = colorize(np.clip(field * 0.8 + ripple * 0.08 + 0.1, 0, 1), [(0, (52, 56, 38)), (0.5, (84, 86, 58)), (1, (116, 112, 82))])
    color *= shade_relief(ripple * 3 + field * 6, 0.8)[..., None]
    f1, _, cell = worley(size, 18, seed + 1)
    local = np.random.default_rng(seed + 2)
    stones = soft(9, 5, f1) * (local.random(cell.max() + 1)[cell] > 0.75)
    color = color * (1 - stones[..., None] * 0.6) + np.array([92, 94, 84]) * stones[..., None] * 0.6
    weed = soft(0.7, 0.85, normalize(fft_noise(size, 1.2, seed + 3)))[..., None]
    color = color * (1 - weed * 0.5) + np.array([40, 66, 32]) * weed * 0.5
    return to_image(color)


def tex_macro(seed):
    """крупные пятна поверх всех текстур (режим умножения x2, 128 — без изменений):
    ломают повторение тайлов, у соседних текстур общий рисунок."""
    size = 512
    tone = fft_noise(size, 2.1, seed)
    hue = fft_noise(size, 2.3, seed + 1)
    detail = fft_noise(size, 1.3, seed + 2)
    level = 128 + 30 * tone + 8 * detail
    color = np.stack([level + 8 * hue, level + 2 * hue, level - 8 * hue], axis=-1)
    return to_image(color)


def tex_mix(first, second, seed):
    """переход между текстурами рельефа: пятна обеих."""
    a = np.array(first).astype(float); b = np.array(second).astype(float)
    mask = soft(0.38, 0.62, normalize(fft_noise(a.shape[0], 1.6, seed) + 0.3 * fft_noise(a.shape[0], 0.9, seed + 1)))[..., None]
    return to_image(a * (1 - mask) + b * mask)


def tex_concrete(seed, tone=1.0, panels=128, moss=0.35, tiles=False, size=TEX):
    panels *= size // 256
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
        seams |= (x % (size // 4) < 2) | (y % (size // 4) < 2)
    color[seams] *= 0.62
    # тёмные потёки вниз от горизонтальных швов
    below = (y % panels).astype(float)
    drip = normalize(ndimage_blur(fft_noise(size, 0.4, seed + 7), 1.2, size * 0.06) + 0.3 * fft_noise(size, 1.5, seed + 8))
    color *= (1 - 0.22 * soft(0.55, 0.9, drip) * np.exp(-below / (panels * 0.45)))[..., None]
    # потёки сверху вниз
    streak = normalize(fft_noise(size, 1.0, seed + 2, (size, size)) + np.linspace(-0.4, 0.4, size)[:, None])
    color *= (0.9 + 0.12 * streak)[..., None]
    if moss > 0:
        moss_field = normalize(fft_noise(size, 1.7, seed + 3))
        amount = np.clip((moss_field - (1 - moss)) * 2.5, 0, 0.85)[..., None]
        color = color * (1 - amount) + np.array([62, 84, 38]) * amount
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_metal(seed, base, panel=64, rust=0.2, rivets=True):
    size = TEX
    panel *= size // 256
    noise = normalize(fft_noise(size, 1.4, seed))
    color = np.zeros((size, size, 3))
    color[:] = base
    color *= (0.85 + 0.25 * noise)[..., None]
    y, x = np.mgrid[0:size, 0:size]
    seams = (x % panel < 2) | (y % (panel * 2) < 2)
    color[seams] *= 0.55
    color[(x % panel == 2) | (y % (panel * 2) == 2)] *= 1.25
    if rivets:
        rivet = ((x % panel - 12) ** 2 + (y % (panel * 2) - 12) ** 2 < 18) | ((x % panel - panel + 12) ** 2 + (y % (panel * 2) - 12) ** 2 < 18)
        color[rivet] *= 1.35
    if rust > 0:
        rust_field = normalize(fft_noise(size, 1.3, seed + 1))
        amount = np.clip((rust_field - (1 - rust)) * 2.5, 0, 0.9)[..., None]
        color = color * (1 - amount) + np.array([120, 66, 34]) * amount * (0.8 + 0.4 * noise[..., None])
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_grate(seed):
    size = TEX
    color = np.array(tex_metal(seed, (96, 100, 102), 64, 0.25, False)).astype(float)
    y, x = np.mgrid[0:size, 0:size]
    holes = ((x % 32) > 6) & ((y % 32) > 6)
    color[holes] *= 0.35
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_hazard(seed):
    size = TEX
    y, x = np.mgrid[0:size, 0:size]
    stripes = ((x + y) // 64) % 2 == 0
    color = np.zeros((size, size, 3))
    color[stripes] = (200, 160, 40)
    color[~stripes] = (36, 34, 30)
    wear = normalize(fft_noise(size, 1.2, seed))
    color *= (0.75 + 0.3 * wear)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_container(seed, base):
    size = TEX
    color = np.array(tex_metal(seed, base, 256, 0.25, False)).astype(float)
    x = np.arange(size)[None, :]
    ribs = np.sin(x / size * 2 * np.pi * 32)
    color *= (0.88 + 0.14 * ribs)[..., None]
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_marked_pad(seed, label, ring_color, letter_size=150):
    """площадка с кругом и буквой (маяки A-E, вертолётные площадки H)."""
    image = tex_concrete(seed, 0.95, 256, 0.15, size=256).convert('RGB')
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


def ripple_field(size, wave_number, spread, seed):
    """бесшовная рябь: волны вытянуты вдоль x (ветер вдоль y), гребни острые."""
    local = np.random.default_rng(seed)
    white = local.normal(0, 1, (size, size)) + 1j * local.normal(0, 1, (size, size))
    freq_y = np.fft.fftfreq(size)[:, None] * size
    freq_x = np.fft.fftfreq(size)[None, :] * size
    band = np.exp(-((np.abs(freq_y) - wave_number) / (wave_number * spread)) ** 2) * np.exp(-(freq_x / (wave_number * 0.45)) ** 2)
    field = np.real(np.fft.ifft2(white * band))
    field = (field - field.mean()) / (field.std() + 1e-9)
    return field


def tex_water_ripple(seed, wave_number):
    """слой ряби для режима modulate x2: 128 — без изменений, гребни светлее, впадины темнее."""
    size = 512
    field = ripple_field(size, wave_number, 0.35, seed) + 0.45 * ripple_field(size, wave_number * 2.1, 0.4, seed + 1)
    field /= field.std()
    field = ndimage_blur(field, 1.5, 1.5)
    field /= field.std()
    crest = np.clip(field, 0, None) ** 1.3
    trough = np.clip(-field, 0, None)
    level = 128 + 42 * crest - 34 * trough        # сила ряби в игре задаёт r_waterRipple
    pixels = np.stack([level * 0.98, level, level * 1.02], axis=-1)
    return Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))


def tex_water_deep(seed):
    """цвет толщи воды: тёмный сине-зелёный с крупными пятнами."""
    size = 256
    field = normalize(fft_noise(size, 1.6, seed))
    color = colorize(field, [(0, (40, 46, 32)), (0.5, (46, 52, 37)), (1, (52, 58, 41))])    # тёмная зеленовато-бурая
    return Image.fromarray(np.clip(color, 0, 255).astype(np.uint8))


def tex_water_env():
    """отражение неба для tcGen environment на ровной воде: строка t = 0.5 - vz / 2,
    где vz — косинус угла между взглядом и вертикалью. У горизонта (t = 0.5) отражение яркое,
    при взгляде сверху (t = 0) слабое — как по формуле Френеля. Нижняя половина — зеркально."""
    size = 128
    t = (np.arange(size) + 0.5) / size
    vz = np.clip(1 - 2 * np.minimum(t, 1 - t), 0, 1)
    fresnel = 0.18 + 0.82 * (1 - vz) ** 1.8          # плавно: без полос между вершинами
    horizon = np.array([132, 150, 162]); zenith = np.array([70, 100, 128])
    sky = horizon[None, :] * (1 - vz[:, None]) ** 0.6 + zenith[None, :] * (1 - (1 - vz[:, None]) ** 0.6)
    column = sky * fresnel[:, None]
    # облака в отражении: слабые полосы вдоль s (меняются при повороте камеры)
    s = np.arange(size) / size
    clouds = 1 + 0.05 * np.sin(s * 2 * np.pi * 3 + 1.3)
    pixels = column[:, None, :] * clouds[None, :, None]
    return Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))


def tex_water_glint(seed):
    """блики неба на гребнях ряби (для сложения): тонкие светлые гребни, остальное чёрное."""
    size = 512
    field = ripple_field(size, 20, 0.35, seed) + 0.5 * ripple_field(size, 41, 0.4, seed + 1)
    field = ndimage_blur(field, 1.0, 1.0)
    field /= field.std()
    level = np.clip(field - 1.2, 0, None) ** 1.4
    level = np.clip(level / (np.percentile(level, 99.7) + 1e-9), 0, 1) * 255
    pixels = np.stack([level * 0.92, level * 0.96, level], axis=-1)
    return Image.fromarray(pixels.astype(np.uint8))


def ndimage_blur(image, sigma_x, sigma_y):
    from scipy import ndimage
    return ndimage.gaussian_filter(image, (sigma_y, sigma_x), mode='wrap')


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
    TEXTURES['water'] = tex_water_deep(111)
    TEXTURES['water_ripple'] = tex_water_ripple(112, 24)
    TEXTURES['water_ripple2'] = tex_water_ripple(114, 13)
    TEXTURES['water_glint'] = tex_water_glint(116)
    for name, image in TEXTURES.items():
        if name in ('grass', 'grass2', 'shore', 'seabed'):
            continue        # только для цветовой карты рельефа
        save_texture(f'textures/walker/{name}', image)


def save_texture(path, image, quality=90):
    data_path = os.path.join(DATA, path + '.jpg')
    os.makedirs(os.path.dirname(data_path), exist_ok=True)
    image.convert('RGB').save(data_path, quality=quality)
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
    if name in ('grass', 'grass2', 'rock', 'rockmoss', 'shore', 'seabed', 'terrain'):
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
COARSE_CELL = 64                     # шаг, на котором строится форма рельефа
CELL = 32                            # шаг сетки треугольников (сглаженная форма): берег плавнее
# WALKER_NAV=1: копия карты для навигации ботов (bspc): рельеф на сетке 64 (вершины лежат на
# той же поверхности), областей AAS в несколько раз меньше — поиск пути ботов намного дешевле
if os.environ.get('WALKER_NAV') == '1':
    CELL = COARSE_CELL
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
    """форма рельефа на сетке COARSE_CELL, затем кубическая интерполяция на сетку CELL."""
    from scipy.interpolate import RectBivariateSpline
    xs, ys, coarse = coarse_height_map()
    fine_xs = np.arange(-HALF_X, HALF_X + 1, CELL, dtype=float)
    fine_ys = np.arange(-HALF_Y, HALF_Y + 1, CELL, dtype=float)
    spline = RectBivariateSpline(ys, xs, coarse, kx=3, ky=3)
    fine = spline(fine_ys, fine_xs)
    fine = np.maximum(fine, SEABED_MIN)
    # пологая отмель у берега: глубина растёт медленно у кромки и быстрее дальше,
    # чтобы полоса мелкой воды, сквозь которую видно дно, была шире
    underwater = fine < 0
    fine[underwater] = SEABED_MIN * (fine[underwater] / SEABED_MIN) ** 1.7
    # шаг высот 1/4 единицы и ни одной вершины ровно на уровне воды: при целых
    # высотах у берега выходили целые клетки на уровне 0 и -1, и кромка воды шла
    # прямыми линиями по сетке
    fine = np.round(fine * 4) / 4
    near_zero = np.abs(fine - WATER_Z) < 0.25
    fine[near_zero] = WATER_Z + np.where(fine[near_zero] >= WATER_Z, 0.25, -0.25)
    return fine_xs, fine_ys, fine


def coarse_height_map():
    xs = np.arange(-HALF_X, HALF_X + 1, COARSE_CELL, dtype=float)
    ys = np.arange(-HALF_Y, HALF_Y + 1, COARSE_CELL, dtype=float)
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
    return xs, ys, height


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


COLORMAP_SIZE = 2048
DETAIL_TILE = 160          # единиц на повтор текстуры мелкой детали


def terrain_shader(points):
    """крутые грани — скала (текстура по граням браша), остальное — общая цветовая карта."""
    a, b, c = [np.array(point, dtype=float) for point in points]
    normal = np.cross(b - a, c - a)
    normal /= np.linalg.norm(normal)
    if abs(normal[2]) < 0.72:
        return 'rock' if (a[2] + b[2] + c[2]) / 3 > 90 else 'rockmoss'
    return 'terrain'


def terrain_height_at(world_x, world_y):
    """высота рельефа ровно по треугольникам сетки (как в add_terrain)."""
    column = np.clip((world_x + HALF_X) / CELL, 0, len(XS) - 1.0001)
    row = np.clip((world_y + HALF_Y) / CELL, 0, len(YS) - 1.0001)
    col0 = np.floor(column).astype(int); row0 = np.floor(row).astype(int)
    fx = column - col0; fy = row - row0
    h00 = HEIGHTS[row0, col0]; h10 = HEIGHTS[row0, col0 + 1]
    h01 = HEIGHTS[row0 + 1, col0]; h11 = HEIGHTS[row0 + 1, col0 + 1]
    even = (row0 + col0) % 2 == 0
    # чётные клетки: диагональ 00-11; нечётные: диагональ 10-01
    even_low = h00 + fx * (h10 - h00) + fy * (h11 - h10)
    even_high = h00 + fx * (h11 - h01) + fy * (h01 - h00)
    odd_low = h00 + fx * (h10 - h00) + fy * (h01 - h00)
    odd_high = h11 + (1 - fx) * (h01 - h11) + (1 - fy) * (h10 - h11)
    return np.where(even, np.where(fx >= fy, even_low, even_high), np.where(fx + fy <= 1, odd_low, odd_high))


def sample_tiled(image, world_x, world_y, tile, pixel):
    """текстура материала в мировых координатах (повтор через tile единиц), заранее уменьшенная
    так, что тексель равен пикселю цветовой карты (pixel единиц): без мерцания и ряби."""
    reduced = max(16, int(round(tile / pixel)))
    array = np.array(image.convert('RGB').resize((reduced, reduced), Image.LANCZOS)).astype(float)
    u = (world_x / tile) % 1.0 * reduced - 0.5
    v = (-world_y / tile) % 1.0 * reduced - 0.5
    x0 = np.floor(u).astype(int); y0 = np.floor(v).astype(int)
    fx = (u - x0)[..., None]; fy = (v - y0)[..., None]
    x0 %= reduced; y0 %= reduced
    x1 = (x0 + 1) % reduced; y1 = (y0 + 1) % reduced
    top = array[y0, x0] * (1 - fx) + array[y0, x1] * fx
    bottom = array[y1, x0] * (1 - fx) + array[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def make_colormap():
    """одна цветовая карта на весь рельеф: материалы смешаны плавно по высоте, крутизне и шуму,
    у кромки воды — тёмная мокрая полоса, под водой — дно темнеет с глубиной.
    Граница материалов не зависит от треугольников сетки."""
    size = COLORMAP_SIZE
    span_x, span_y = 2 * HALF_X, 2 * HALF_Y
    pixel_x = -HALF_X + (np.arange(size) + 0.5) * span_x / size
    pixel_y = HALF_Y - (np.arange(size) + 0.5) * span_y / size
    world_x, world_y = np.meshgrid(pixel_x, pixel_y)
    height = terrain_height_at(world_x, world_y)
    grad_y, grad_x = np.gradient(HEIGHTS, CELL)
    normal_grid = 1 / np.sqrt(1 + grad_x ** 2 + grad_y ** 2)
    from scipy.ndimage import map_coordinates, gaussian_filter
    coords = np.stack([(world_y + HALF_Y) / CELL, (world_x + HALF_X) / CELL])
    normal_z = map_coordinates(normal_grid, coords, order=1, mode='nearest')

    def noise(beta, seed):
        return fft_noise(0, beta, seed, (size, size))

    jitter = noise(1.6, 501)
    variety = normalize(noise(1.8, 502))
    patches = normalize(noise(1.3, 503))
    pixel = span_x / size
    footprint = pixel
    grass = sample_tiled(TEXTURES['grass'], world_x, world_y, 256, footprint)
    grass2 = sample_tiled(TEXTURES['grass2'], world_x + 97, world_y - 41, 256, footprint)
    rockmoss = sample_tiled(TEXTURES['rockmoss'], world_x, world_y, 256, footprint)
    rock = sample_tiled(TEXTURES['rock'], world_x, world_y, 256, footprint)
    sand = sample_tiled(TEXTURES['shore'], world_x, world_y, 256, footprint)
    sand = sand * 0.5 + gaussian_filter(sand, (1.2, 1.2, 0)) * 0.5        # галька мягче
    seabed = sample_tiled(TEXTURES['seabed'], world_x, world_y, 256, footprint)
    seabed = seabed * 0.6 + np.array([92, 84, 66]) * 0.4                  # дно ближе к песку

    def mix(base, other, weight):
        return base * (1 - weight[..., None]) + other * weight[..., None]

    color = mix(grass, grass2, soft(0.4, 0.7, variety))
    # тропинки и проплешины
    color = mix(color, sand * 0.9, 0.55 * soft(0.78, 0.92, patches) * soft(30, 12, np.abs(height - 20)))
    color = mix(color, rockmoss, soft(95, 150, height + 35 * jitter) * soft(0.6, 0.95, normal_z))
    color = mix(color, rockmoss, soft(0.93, 0.82, normal_z))
    color = mix(color, rock, soft(0.84, 0.72, normal_z) * 0.85)
    # берег: песок с галькой, выше по неровной кромке
    shore_top = 5 + 3 * jitter
    color = mix(color, sand, soft(shore_top + 4, shore_top - 2, height))
    # мокрая полоса у воды и дно
    wet = soft(5, -1, height)
    color = color * (1 - 0.32 * wet[..., None]) * np.array([1.0, 1.0, 1.04])
    color = mix(color, seabed, soft(-1, -7, height))
    depth = soft(0, -20, height)
    color = color * (1 - 0.35 * depth[..., None]) + np.array([20, 34, 38]) * 0.35 * depth[..., None]
    # мягкая крупная светотень по рельефу, которую карта освещения сглаживает
    color *= (0.92 + 0.08 * normalize(gaussian_filter(jitter, 3)))[..., None]
    return to_image(color)


SHALLOW_CLEAR = 9.0        # глубина (единиц), на которой дно видно уже в e раз слабее
SHALLOW_MAX = 0.93         # у самого берега сквозь воду видно 90% дна, 10% — отражение
SHALLOW_RANGE = 220.0      # ширина полосы мелководья от берега, единиц (около 40 м)


def make_water_shallow():
    """мелководье у берега. Дно там — настоящий рельеф с картой освещения: зеркальная вода
    не пишет глубину на расстоянии меньше MIRROR_START от берега (water_deep.tga, альфа-тест),
    и рельеф под ней рисуется. Поверх всей воды после рельефа идёт слой water_shore: кадр
    умножается на (1 - полоса) и прибавляется цвет воды, умноженный на полосу. Полоса растёт
    от 0 у кромки до 1 на границе зеркала и спадает за ней — граница зеркала не видна."""
    size = COLORMAP_SIZE
    span_x, span_y = 2 * HALF_X, 2 * HALF_Y
    pixel_x = -HALF_X + (np.arange(size) + 0.5) * span_x / size
    pixel_y = HALF_Y - (np.arange(size) + 0.5) * span_y / size
    world_x, world_y = np.meshgrid(pixel_x, pixel_y)
    height = terrain_height_at(world_x, world_y)
    from scipy.ndimage import distance_transform_edt
    pixel = span_x / size
    shore_distance = distance_transform_edt(height < WATER_Z) * pixel
    # граница зеркала: альфа 255 там, где вода глубже (дальше от берега)
    deep = (shore_distance > MIRROR_START).astype(np.uint8) * 255
    deep_image = Image.fromarray(np.dstack([np.full_like(deep, 255)] * 3 + [deep]), 'RGBA')
    deep_image = deep_image.resize((1024, 1024), Image.NEAREST)
    # полоса цвета воды
    rise = np.clip(shore_distance / MIRROR_START, 0, 1) ** 1.4
    fall = 1 - soft(MIRROR_START, MIRROR_START + BAND_FALL, shore_distance)
    band = np.where(shore_distance <= MIRROR_START, rise, fall)
    band = np.where(height >= WATER_Z, 0, band)
    tone = 0.92 + 0.16 * normalize(fft_noise(0, 1.8, 611, (size, size)))
    color = np.array(SHALLOW_COLOR, dtype=float)[None, None, :] * tone[..., None]
    band_color = color * band[..., None]
    band_mask = Image.fromarray(np.clip(band * 255, 0, 255).astype(np.uint8)).convert('RGB').resize((1024, 1024), Image.LANCZOS)
    return deep_image, band_mask, to_image(band_color)


MIRROR_START = 120.0       # от берега до начала зеркальной воды, единиц (около 20 м)
BAND_FALL = 70.0           # за границей зеркала цвет воды уступает отражению на этой ширине
SHALLOW_COLOR = (44, 50, 35)   # тёмная зеленовато-бурая вода


SEABED_LIGHT = 1.0         # подобрано по скриншоту: суша у кромки и дно у кромки одной яркости


def tex_detail(seed):
    """мелкая деталь для рельефа (умножение x2, среднее 128): зерно, травинки, камешки."""
    size = 512
    fine = fft_noise(size, 0.5, seed)
    mid = fft_noise(size, 1.0, seed + 1)
    level = 128 + 22 * fine + 12 * mid
    image = Image.fromarray(np.clip(level, 0, 255).astype(np.uint8)).convert('RGB').convert('RGBA')
    image.alpha_composite(strokes(size, 9000, seed + 2, (3, 7), [(150, 150, 150), (104, 104, 104), (170, 170, 170)]))
    gray = np.array(image.convert('L')).astype(float)
    gray = 128 + (gray - gray.mean()) * 0.9
    return Image.fromarray(np.clip(gray, 0, 255).astype(np.uint8)).convert('RGB')


def add_terrain():
    """рельеф из треугольных призм; ровные клетки (дно, площадки) сливаются в прямоугольники
    до 16x16 клеток — иначе брашей больше, чем допускает формат BSP."""
    counts = {}
    rows, cols = len(YS) - 1, len(XS) - 1
    flat = np.zeros((rows, cols), dtype=bool)
    for row in range(rows):
        for col in range(cols):
            corners = HEIGHTS[row:row + 2, col:col + 2]
            flat[row, col] = corners.min() == corners.max()
    used = np.zeros_like(flat)
    for row in range(rows):
        for col in range(cols):
            if not flat[row, col] or used[row, col]:
                continue
            level = HEIGHTS[row, col]
            width = 1
            while (col + width < cols and width < 16 and flat[row, col + width] and not used[row, col + width]
                   and HEIGHTS[row, col + width] == level):
                width += 1
            height = 1
            while row + height < rows and height < 16:
                line = slice(col, col + width)
                if (flat[row + height, line].all() and not used[row + height, line].any()
                        and (HEIGHTS[row + height, col:col + width + 1] == level).all()):
                    height += 1
                else:
                    break
            used[row:row + height, col:col + width] = True
            x0, x1 = int(XS[col]), int(XS[col + width])
            y0, y1 = int(YS[row]), int(YS[row + height])
            add_prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], TERRAIN_BOTTOM, float(level), 'terrain', CAULK, CAULK, True)
            counts['terrain (flat)'] = counts.get('terrain (flat)', 0) + 1
    for row in range(rows):
        for col in range(cols):
            if used[row, col]:
                continue
            x0, x1 = int(XS[col]), int(XS[col + 1])
            y0, y1 = int(YS[row]), int(YS[row + 1])
            h00, h10 = float(HEIGHTS[row, col]), float(HEIGHTS[row, col + 1])
            h01, h11 = float(HEIGHTS[row + 1, col]), float(HEIGHTS[row + 1, col + 1])
            corner00, corner10 = (x0, y0, h00), (x1, y0, h10)
            corner01, corner11 = (x0, y1, h01), (x1, y1, h11)
            if (row + col) % 2 == 0:
                triangles = [(corner00, corner10, corner11), (corner00, corner11, corner01)]
            else:
                triangles = [(corner00, corner10, corner01), (corner10, corner11, corner01)]
            for triangle in triangles:
                shader = terrain_shader(triangle)
                counts[shader] = counts.get(shader, 0) + 1
                polygon = [(point[0], point[1]) for point in triangle]
                area = (polygon[1][0] - polygon[0][0]) * (polygon[2][1] - polygon[0][1]) - (polygon[2][0] - polygon[0][0]) * (polygon[1][1] - polygon[0][1])
                if area < 0:
                    polygon.reverse()
                add_prism(polygon, TERRAIN_BOTTOM, None, shader, CAULK, CAULK, True, list(triangle))
    print('terrain brushes:', counts)


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
    # дымка: объём тумана на всю карту, дальние скалы растворяются в цвете горизонта
    add_box(x0, y0, low, x1, y1, high, 'fog', 'fog', 'fog')
    clip = 'common/clip'
    thickness = 32
    for bx0, by0, bx1, by1 in [(-PLAY_X - thickness, -PLAY_Y - thickness, PLAY_X + thickness, -PLAY_Y),
                               (-PLAY_X - thickness, PLAY_Y, PLAY_X + thickness, PLAY_Y + thickness),
                               (-PLAY_X - thickness, -PLAY_Y, -PLAY_X, PLAY_Y), (PLAY_X, -PLAY_Y, PLAY_X + thickness, PLAY_Y)]:
        add_box(bx0, by0, low, bx1, by1, high - 8, clip, clip, clip)


def add_water():
    shader = 'water'
    add_box(-HALF_X, -HALF_Y, TERRAIN_BOTTOM, HALF_X, HALF_Y, WATER_Z, shader, shader, shader)
    # второй, тонкий слой на той же плоскости: рисуется после рельефа (мелководье, рябь)
    add_box(-HALF_X, -HALF_Y, WATER_Z - 2, HALF_X, HALF_Y, WATER_Z, 'water_shore', 'water_shore', 'water_shore')


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
        'classname': 'worldspawn', 'message': 'Islands', 'ambient': '10', '_color': '0.75 0.82 0.9',
        'gridsize': '128 128 128', '_blocksize': '0 0 0',
    }
    entities = spawn_points()
    entities.append({'classname': 'info_player_intermission', 'origin': '-900 -700 520', 'angles': '18 35 0'})
    # маяки: target_location нужны ботам как точки сбора и игре для названий мест
    for letter, (x, y, z) in BEACONS.items():
        entities.append({'classname': 'target_location', 'origin': f'{x} {y} {int(z) + 40}', 'message': f'Beacon {letter.upper()}'})
    # зеркало для воды: точка над водой, в 8 единицах над её плоскостью (отражение рендерит движок)
    deep = np.argwhere(HEIGHTS <= SEABED_MIN)
    centre = np.array([len(YS) / 2, len(XS) / 2])
    row, col = deep[np.argmin(((deep - centre) ** 2).sum(axis=1))]
    entities.append({'classname': 'misc_portal_surface', 'origin': f'{int(XS[col])} {int(YS[row])} {WATER_Z + 8}'})
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
	q3map_sun 1 0.95 0.86 240 {SUN_AZIMUTH:g} {SUN_ELEVATION:g}
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
	// mirror: the engine first draws the scene reflected in the water plane
	// (misc_portal_surface in the map), then these stages go over it
	portal
	tessSize 128
	// depth only where the water is deep: near the shore the mirror leaves the
	// depth buffer alone and the real seabed (terrain with its lightmap) is drawn
	{{
		clampmap textures/walker/water_deep.tga
		blendFunc GL_ZERO GL_ONE
		alphaFunc GE128
		depthWrite
		tcGen vector ( {1.0 / (2 * HALF_X):.8f} 0 0 ) ( 0 {-1.0 / (2 * HALF_Y):.8f} 0 )
		tcMod transform 1 0 0 1 0.5 0.5
	}}
	// water colour over the reflection, like Fresnel: looking down close to the camera
	// one sees into the water, at grazing angles far away the reflection
	{{
		map textures/walker/water.jpg
		blendFunc GL_ONE_MINUS_SRC_ALPHA GL_SRC_ALPHA
		alphaGen portal 1300
		rgbGen identity
		tcMod scale 0.2 0.2
	}}
	{{
		map textures/walker/water.jpg
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen const 0.12
		rgbGen identity
		tcMod scale 0.2 0.2
	}}
	// ripples and glints over the reflection only (the shallows near the shore are
	// drawn over them by the terrain): none close to the camera, full from 600 units
	// on, strength set by r_waterRipple (menu EFFECTS -> Water Ripples)
	{{
		map textures/walker/water_ripple.jpg
		blendFunc GL_DST_COLOR GL_ONE_MINUS_SRC_ALPHA
		rgbGen waterRipple 60 600
		tcMod scale 0.45 0.45
		tcMod scroll 0.004 0.022
	}}
	{{
		map textures/walker/water_ripple2.jpg
		blendFunc GL_DST_COLOR GL_ONE_MINUS_SRC_ALPHA
		rgbGen waterRipple 60 600
		tcMod transform 0.2 0.15 -0.15 0.2 0 0
		tcMod scroll -0.008 0.014
	}}
	{{
		map textures/walker/water_glint.jpg
		blendFunc GL_ONE GL_ONE
		rgbGen waterGlint 100 900 0.35
		tcMod scale 0.45 0.45
		tcMod scroll 0.004 0.022
	}}
}}
textures/walker/water_shore
{{
	qer_editorimage textures/walker/water.tga
	qer_trans 0.5
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm nolightmap
	surfaceparm nomarks
	polygonOffset
	// drawn after the terrain over all the water: near the shore the seabed shows
	// through and turns into water colour towards the mirror
	{{
		clampmap textures/walker/water_band_mask.jpg
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		tcGen vector ( {1.0 / (2 * HALF_X):.8f} 0 0 ) ( 0 {-1.0 / (2 * HALF_Y):.8f} 0 )
		tcMod transform 1 0 0 1 0.5 0.5
	}}
	{{
		clampmap textures/walker/water_band.jpg
		blendFunc GL_ONE GL_ONE
		tcGen vector ( {1.0 / (2 * HALF_X):.8f} 0 0 ) ( 0 {-1.0 / (2 * HALF_Y):.8f} 0 )
		tcMod transform 1 0 0 1 0.5 0.5
	}}
}}
textures/walker/terrain
{{
	qer_editorimage textures/walker/terrain_detail.tga
	{{
		map $lightmap
		rgbGen identity
	}}
	{{
		clampmap textures/walker/terrain_colormap.jpg
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
		tcGen vector ( {1.0 / (2 * HALF_X):.8f} 0 0 ) ( 0 {-1.0 / (2 * HALF_Y):.8f} 0 )
		tcMod transform 1 0 0 1 0.5 0.5
	}}
	{{
		map textures/walker/terrain_detail.jpg
		blendFunc GL_DST_COLOR GL_SRC_COLOR
		tcGen vector ( {1.0 / DETAIL_TILE:.6f} 0 0 ) ( 0 {1.0 / DETAIL_TILE:.6f} 0 )
	}}
}}
textures/walker/fog
{{
	qer_editorimage textures/walker/water.tga
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm fog
	surfaceparm nolightmap
	surfaceparm nomarks
	fogparms ( {FOG_COLOR[0]:.3f} {FOG_COLOR[1]:.3f} {FOG_COLOR[2]:.3f} ) {FOG_DISTANCE}
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


def make_mix_textures():
    save_texture('textures/walker/macro', tex_macro(131))
    colormap = make_colormap()
    save_texture('textures/walker/terrain_colormap', colormap, quality=88)
    deep, band_mask, band_color = make_water_shallow()
    deep.save(os.path.join(DATA, 'textures/walker/water_deep.tga'))
    save_texture('textures/walker/water_band_mask', band_mask, quality=92)
    save_texture('textures/walker/water_band', band_color, quality=90)
    save_texture('textures/walker/terrain_detail', tex_detail(141))


def lightmapped_shaders():
    """явные шейдеры для текстур с картой освещения: освещение, текстура и общий крупный рисунок
    (tcGen vector: проекция в мировых координатах, один рисунок на все поверхности подряд)."""
    defined = {line.strip() for line in SHADERS.splitlines() if line.startswith('textures/')}
    used = sorted({face[1] for brush, _ in BRUSHES for face in brush.faces})
    blocks = []
    for shader in used:
        full = 'textures/' + shader
        if full in defined or not shader.startswith('walker/'):
            continue
        name = shader.split('/')[-1]
        blocks.append(f"""{full}
{{
	qer_editorimage textures/walker/{name}.tga
	{{
		map $lightmap
		rgbGen identity
	}}
	{{
		map textures/walker/{name}.jpg
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
	}}
	{{
		map textures/walker/macro.jpg
		blendFunc GL_DST_COLOR GL_SRC_COLOR
		tcGen vector ( 0.00042 0.00027 0 ) ( -0.00027 0.00042 0.00035 )
	}}
}}""")
    return '\n'.join(blocks) + '\n'


def main():
    global XS, YS, HEIGHTS, SHADERS
    make_textures()
    make_sky()
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
    make_mix_textures()
    SHADERS += lightmapped_shaders()
    write_shaders()
    write_map()
    # карта высот для просмотра
    preview = normalize(HEIGHTS)
    water = HEIGHTS < 0
    image = np.stack([preview * 200 + 30] * 3, axis=-1)
    image[water] = [40, 90, 130]
    Image.fromarray(image[::-1].astype(np.uint8)).resize((len(XS) * 6, len(YS) * 6), Image.NEAREST).save('/tmp/islands_height.png')


main()
