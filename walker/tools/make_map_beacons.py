"""Карта WALKER «Маяки» (wk_beacons) для захвата маяков (g_gametype 8).

python3 make_map_beacons.py <данные игры> <папка компиляции .../quake*/basewalker>

Игровое поле 2000x2000: пять маяков (четыре по углам, один в центре), команды появляются у
северного (синие) и южного (красные) краёв за длинными бетонными укрытиями, вокруг центра —
укрытия поменьше: целые блоки с выбоинами и две разрушенные стены. Край поля — невидимая
граница (игра показывает её мерцающей стеной вблизи, cgame/cg_beacons.c); за ней земля
продолжается пологими холмами до оболочки неба. Воды нет.

Берёт всё общее (текстуры, рельеф, тени, модели, шейдеры) из make_map_islands.py и
make_props.py; расстановка — симметрия поворота на 180° вокруг центра.
"""
import math, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_map_islands as isl          # читает sys.argv: данные, папка компиляции
import make_props as props

MAP = 'wk_beacons'
PLAY = 1000                     # половина игрового поля (граница)
HALF = 1600                     # половина всей карты (до оболочки неба)
BASE_Z = 24                     # уровень земли на поле
BEACON_RADIUS = 150
COVER_HEIGHT = 130              # укрытия в полный рост (робот около 90)

isl.MAP_NAME = MAP
isl.TERRAIN_SHADER = 'terrain_beacons'
isl.HALF_X = isl.HALF_Y = HALF
isl.PLAY_X = isl.PLAY_Y = PLAY
isl.COARSE_CELL = 128
isl.CELL = 64

BEACONS = [('a', -780, 800), ('b', 780, 800), ('c', 0, 0), ('d', -780, -800), ('e', 780, -800)]
SPAWN_Y = 890
SPAWN_XS = [-340, -170, 0, 170, 340]


def rotated(x, y):
    return -x, -y


# ================================================================ рельеф

def coarse_height_map():
    """поле почти ровное (пологие волны ±8), под маяками и зонами появления — ровно; за
    границей земля поднимается пологими холмами к краю карты."""
    step = isl.COARSE_CELL
    xs = np.arange(-HALF, HALF + 1, step, dtype=float)
    ys = np.arange(-HALF, HALF + 1, step, dtype=float)
    grid_x, grid_y = np.meshgrid(xs, ys)
    shape = grid_x.shape
    waves = isl.fft_noise(0, 2.0, 701, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    bumps = isl.fft_noise(0, 1.4, 702, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    height = BASE_Z + 8 * waves + 3 * bumps
    # холмы за границей: по расстоянию от края поля
    outside = np.maximum(np.abs(grid_x), np.abs(grid_y)) - PLAY
    rise = np.clip((outside - 80) / (HALF - PLAY - 140), 0, 1)
    hills = isl.fft_noise(0, 1.6, 703, (shape[0] * 2, shape[1] * 2))[:shape[0], :shape[1]]
    height += (230 + 70 * hills) * rise * rise * (3 - 2 * rise)
    # ровные площадки: маяки, зоны появления
    flats = [(x, y, BEACON_RADIUS + 50, 140) for _, x, y in BEACONS]
    flats += [(0, SPAWN_Y, 420, 160), (0, -SPAWN_Y, 420, 160)]
    for center_x, center_y, radius, falloff in flats:
        distance = np.hypot(grid_x - center_x, (grid_y - center_y) * (1.0 if radius < 300 else 2.2))
        weight = np.clip(1 - (distance - radius) / falloff, 0, 1)
        weight = weight * weight * (3 - 2 * weight)
        height = height * (1 - weight) + BASE_Z * weight
    return xs, ys, height


isl.coarse_height_map = coarse_height_map


# ================================================================ постройки

def beacon_pad(letter, x, y):
    """круг маяка: бетонная плита вровень с землёй, кольцо и буква (текстура beacon_<буква>)."""
    pad = isl.polygon_regular(x, y, BEACON_RADIUS, 24)
    bottom = isl.ground_min(x - BEACON_RADIUS, y - BEACON_RADIUS, x + BEACON_RADIUS, y + BEACON_RADIUS) - 8
    scale = 2 * BEACON_RADIUS / 228          # кольцо текстуры — 228 точек из 256
    shift = (128 - x / scale, 128 + y / scale)
    isl.add_prism(pad, bottom, BASE_Z + 3, f'beacon_{letter}', 'hazard', isl.CAULK, True, None, shift, scale)
    return BASE_Z + 3


def blocks_along(name, x0, y0, x1, y1, thickness, seed, segment=96, gap=8):
    """длинное укрытие из нескольких блоков подряд."""
    length = math.hypot(x1 - x0, y1 - y0)
    count = max(1, int(round((length + gap) / (segment + gap))))
    piece = (length - gap * (count - 1)) / count
    for index in range(count):
        start = index * (piece + gap) / length
        end = (index * (piece + gap) + piece) / length
        block(f'{name}_{index}', x0 + (x1 - x0) * start, y0 + (y1 - y0) * start,
              x0 + (x1 - x0) * end, y0 + (y1 - y0) * end, thickness, seed + index)


def block(name, x0, y0, x1, y1, thickness, seed):
    """бетонный блок от (x0, y0) до (x1, y1), стоит на самой низкой точке земли под ним."""
    margin = thickness / 2 + 2
    base = isl.ground_min(min(x0, x1) - margin, min(y0, y1) - margin, max(x0, x1) + margin, max(y0, y1) + margin) - 1
    mesh = props.concrete_block((x0, y0), (x1, y1), base, COVER_HEIGHT, thickness, seed)
    props.add_lod_prop(name, mesh, mesh.far)
    # коллизия на 2 ед. внутри видимой поверхности
    along = np.array([x1 - x0, y1 - y0]) / math.hypot(x1 - x0, y1 - y0)
    across = np.array([-along[1], along[0]])
    inner = thickness / 2 - 2
    corners = [np.array([x0, y0]) + across * inner, np.array([x1, y1]) + across * inner,
               np.array([x1, y1]) - across * inner, np.array([x0, y0]) - across * inner]
    corners = [tuple(point) for point in corners]
    area = sum(corners[i][0] * corners[(i + 1) % 4][1] - corners[(i + 1) % 4][0] * corners[i][1] for i in range(4))
    if area < 0:
        corners.reverse()
    isl.add_prism(corners, base - 20, base + COVER_HEIGHT - 4, 'clip_solid', 'clip_solid', 'clip_solid')


def pair(function, *arguments_list):
    """постройка и её копия, повёрнутая на 180° вокруг центра."""
    for arguments in arguments_list:
        function(*arguments)


def layout():
    pads = {}
    for letter, x, y in BEACONS:
        pads[letter] = (x, y, beacon_pad(letter, x, y))
    # длинные укрытия перед зонами появления
    for side, sign in (('n', 1), ('s', -1)):
        blocks_along(f'cover_{side}1', -700 * sign, 560 * sign, -300 * sign, 560 * sign, 40, 1000 if sign > 0 else 1100)
        blocks_along(f'cover_{side}2', -100 * sign, 560 * sign, 300 * sign, 560 * sign, 40, 1200 if sign > 0 else 1300)
    # укрытия вокруг центра (пары с поворотом)
    for name, (x, y, length, thickness, angle, seed) in {
            'mid_a': (400, 330, 110, 44, 20, 1400), 'mid_c': (-360, 290, 90, 56, -15, 1500),
            'mid_e': (170, 300, 70, 70, 0, 1600)}.items():
        for suffix, (cx, cy), turn in (('1', (x, y), 0), ('2', rotated(x, y), 180)):
            direction = math.radians(angle + turn)
            dx, dy = math.cos(direction) * length / 2, math.sin(direction) * length / 2
            block(f'{name}{suffix}', cx - dx, cy - dy, cx + dx, cy + dy, thickness, seed + (0 if suffix == '1' else 50))
    # две разрушенные стены — с изломами
    for (x0, y0, x1, y1), seed in (((-620, -20, -620, 150), 61), ((620, 20, 620, -150), 62)):
        base = isl.ground_min(min(x0, x1) - 14, min(y0, y1) - 14, max(x0, x1) + 14, max(y0, y1) + 14) - 1
        isl.broken_wall(x0, y0, x1, y1, base, COVER_HEIGHT, 26, seed)
    return pads


# ================================================================ вывод

TERRAIN_SHADER_TEXT = """textures/walker/terrain_beacons
{{
	qer_editorimage textures/walker/terrain_detail.tga
	{{
		map $lightmap
		rgbGen identity
	}}
	{{
		clampmap textures/walker/beacons_colormap.jpg
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
		tcGen vector ( {sx:.8f} 0 0 ) ( 0 {sy:.8f} 0 )
		tcMod transform 1 0 0 1 0.5 0.5
	}}
	{{
		map textures/walker/terrain_detail.jpg
		blendFunc GL_DST_COLOR GL_SRC_COLOR
		tcGen vector ( {detail:.6f} 0 0 ) ( 0 {detail:.6f} 0 )
	}}
}}
"""


def write_shaders(text):
    for root in (isl.DATA, isl.COMPILE):
        path = os.path.join(root, 'scripts', 'walker_beacons.shader')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='ascii', newline='\n') as file:
            file.write(text)
    with open(os.path.join(isl.COMPILE, 'scripts', 'shaderlist.txt'), 'w', encoding='ascii', newline='\n') as file:
        file.write('walker_beacons\n')
    arena = ('{\nmap "wk_beacons"\nlongname "Beacons"\nfraglimit "0"\ntimelimit "10"\ntype "beacons team ffa"\n'
             'bots "Anvil Bastion Cinder Drake Ember"\n}\n')
    with open(os.path.join(isl.DATA, 'scripts', 'wk_beacons.arena'), 'w', encoding='ascii', newline='\n') as file:
        file.write(arena)


def write_map(pads):
    worldspawn = {
        'classname': 'worldspawn', 'message': 'Beacons', 'ambient': '10', '_color': '0.75 0.82 0.9',
        'gridsize': '128 128 128', '_blocksize': '0 0 0', 'barrier': str(PLAY),
    }
    entities = []
    # маяки игры раньше target_location: те же точки названы для ботов и сообщений
    for letter, (x, y, z) in pads.items():
        entities.append({'classname': 'team_beacon', 'origin': f'{x} {y} {int(z)}', 'radius': str(BEACON_RADIUS),
                         'letter': letter.upper()})
    for letter, (x, y, z) in pads.items():
        entities.append({'classname': 'target_location', 'origin': f'{x} {y} {int(z) + 40}', 'message': f'Beacon {letter.upper()}'})
    # появление: синие на севере, красные на юге, лицом к центру
    for team, sign in (('blue', 1), ('red', -1)):
        for x in SPAWN_XS:
            sx, sy = x * sign, SPAWN_Y * sign
            z = int(isl.ground(sx, sy)) + 40
            angle = 270 if sign > 0 else 90
            for classname in (f'team_CTF_{team}spawn', f'team_CTF_{team}player'):
                entities.append({'classname': classname, 'origin': f'{sx} {sy} {z}', 'angle': str(angle)})
            entities.append({'classname': 'info_player_deathmatch', 'origin': f'{sx} {sy} {z}', 'angle': str(angle)})
    entities.append({'classname': 'info_player_intermission', 'origin': f'-900 -1150 420', 'angles': '20 50 0'})
    for x, y in [(0, 0), (-700, 700), (700, -700)]:
        entities.append({'classname': 'target_speaker', 'origin': f'{x} {y} 140', 'noise': 'sound/world/wind_loop.wav', 'spawnflags': '1'})
    prop_entities, _ = props.write_props(isl.COMPILE)
    entities += prop_entities
    lines = ['// WALKER map wk_beacons, generated by walker/tools/make_map_beacons.py', '{']
    lines += [f'"{key}" "{value}"' for key, value in worldspawn.items()]
    for brush, inside in isl.BRUSHES:
        lines.append(brush.text(inside))
    lines.append('}')
    for entity in entities:
        lines.append('{')
        lines += [f'"{key}" "{value}"' for key, value in entity.items()]
        lines.append('}')
    path = os.path.join(isl.COMPILE, 'maps', MAP + '.map')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='ascii', newline='\n') as file:
        file.write('\n'.join(lines) + '\n')
    print('brushes:', len(isl.BRUSHES), '->', path)


def levelshot():
    """картинка карты для меню: план сверху (рельеф, маяки, укрытия)."""
    size = 512
    image = np.zeros((size, size, 3))
    coords = (np.arange(size) + 0.5) / size * 2 * PLAY - PLAY
    world_x, world_y = np.meshgrid(coords, -coords)
    height = isl.terrain_height_at(world_x, world_y)
    image[...] = np.array([70, 92, 52]) + (height - BASE_Z)[..., None] * 3
    for _, x, y in BEACONS:
        mask = np.hypot(world_x - x, world_y - y) < BEACON_RADIUS
        image[mask] = image[mask] * 0.4 + np.array([230, 230, 230]) * 0.6
    path = os.path.join(isl.DATA, 'levelshots', MAP + '.jpg')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray(np.clip(image, 0, 255).astype(np.uint8)).save(path, quality=90)


def main():
    isl.make_textures()
    isl.make_sky()
    isl.XS, isl.YS, isl.HEIGHTS = isl.height_map()
    print('height range:', isl.HEIGHTS.min(), isl.HEIGHTS.max())
    isl.add_hull()
    isl.add_terrain()
    pads = layout()
    isl.save_texture('textures/walker/macro', isl.tex_macro(131))
    isl.save_texture('textures/walker/beacons_colormap', isl.make_colormap(), quality=88)
    isl.save_texture('textures/walker/terrain_detail', isl.tex_detail(141))
    text = isl.SHADERS.replace('// WALKER map shaders: wk_islands', '// WALKER map shaders: wk_beacons')
    text += TERRAIN_SHADER_TEXT.format(sx=1.0 / (2 * HALF), sy=-1.0 / (2 * HALF), detail=1.0 / isl.DETAIL_TILE)
    text += isl.lightmapped_shaders() + isl.vertex_lit_shaders()
    text = isl.mark_no_sun_shadow(text)
    counts = props.write_casters(os.path.join(isl.DATA, 'maps', MAP + '.casters'))
    print('shadow casters (both, simple only, full only):', counts)
    write_shaders(text)
    write_map(pads)
    levelshot()


if __name__ == '__main__':
    main()
