"""Тестовая карта для черновой модели робота (WALKER, 28.11.2026).

python3 walker/tools/make_map_mechtest.py <папка .../quake*/basewalker>

Карта wk_mechtest «Mech test»: ровная площадка 2048 x 2048 единиц в стенах, на ней
ступени разной высоты, два пандуса на площадки, колонны, низкая стена, два прохода разной
ширины. Вид - как у черновой модели: однотонные светлые поверхности, тёмная линия по сетке
64 единицы (текстура 128 пикселей с рамкой, масштаб 0.5), у каждого вида препятствий свой
цвет. Солнце и свет неба считает q3map.

Пишет maps/wk_mechtest.map, textures/mechtest/*.tga, scripts/mechtest.shader, shaderlist.txt
и scripts/wk_mechtest.arena (карта видна в START SERVER). Дальше:

    q3map wk_mechtest.map; q3map -vis wk_mechtest.map; q3map -light -skylight 24 48 wk_mechtest.map
    bspc -cfg walker/maps/aas_walker.cfg -bsp2aas wk_mechtest.bsp      (навигация ботов)
"""
import os, sys
import numpy as np
from PIL import Image, ImageDraw

NAME = 'wk_mechtest'
HALF = 1024                 # половина стороны площадки
WALL_HEIGHT = 192
SKY_TOP = 1024

# цвет линии (заливка - тот же оттенок, светлее), как группы на картинке черновой модели
SURFACES = {
    'floor': (120, 140, 110),
    'wall': (90, 110, 160),
    'step': (170, 130, 50),
    'ramp': (180, 70, 60),
    'column': (110, 110, 110),
}


def texture(colour, size=128, line=3):
    fill = tuple(int(255 - (255 - channel) * 0.22) for channel in colour)
    image = Image.new('RGB', (size, size), fill)
    ImageDraw.Draw(image).rectangle((0, 0, size - 1, size - 1), outline=colour, width=line)
    return image


# ---------------------------------------------------------------- браши

class Brush:
    def __init__(self, material):
        self.material = material
        self.planes = []        # (три точки, материал)

    def plane(self, normal, point, material=None):
        """плоскость с внешней нормалью normal через point: q3map берёт нормаль как
        (p0 - p1) x (p2 - p1)"""
        normal = np.asarray(normal, float); normal /= np.linalg.norm(normal)
        helper = np.array([0.0, 0.0, 1.0]) if abs(normal[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
        along_u = np.cross(helper, normal); along_u /= np.linalg.norm(along_u)
        along_v = np.cross(normal, along_u)
        point = np.asarray(point, float)
        self.planes.append(((point + along_u * 64, point, point + along_v * 64), material or self.material))

    def text(self):
        lines = ['{']
        for points, material in self.planes:
            coords = ' '.join('( ' + ' '.join(f'{value:.3f}'.rstrip('0').rstrip('.') for value in point) + ' )' for point in points)
            lines.append(f'{coords} mechtest/{material} 0 0 0 0.5 0.5 0 0 0')
        lines.append('}')
        return '\n'.join(lines)


BRUSHES = []


def box(material, low, high, top=None):
    brush = Brush(material)
    x0, y0, z0 = low; x1, y1, z1 = high
    brush.plane((-1, 0, 0), (x0, y0, z0)); brush.plane((1, 0, 0), (x1, y1, z1))
    brush.plane((0, -1, 0), (x0, y0, z0)); brush.plane((0, 1, 0), (x1, y1, z1))
    brush.plane((0, 0, -1), (x0, y0, z0)); brush.plane((0, 0, 1), (x1, y1, z1), top)
    BRUSHES.append(brush)


def ramp(material, x0, y0, x1, y1, height, rising):
    """клин на полу: rising '+x' - подъём к x1 (там вертикальная стенка высотой height)"""
    brush = Brush(material)
    brush.plane((0, 0, -1), (x0, y0, 0))
    brush.plane((0, -1, 0), (x0, y0, 0)); brush.plane((0, 1, 0), (x1, y1, 0))
    brush.plane((-1, 0, 0), (x0, y0, 0)); brush.plane((1, 0, 0), (x1, y1, 0))
    if rising == '+x':
        normal = (-height, 0, x1 - x0); point = (x0, y0, 0)
    elif rising == '-x':
        normal = (height, 0, x1 - x0); point = (x1, y0, 0)
    elif rising == '+y':
        normal = (0, -height, y1 - y0); point = (x0, y0, 0)
    else:
        normal = (0, height, y1 - y0); point = (x0, y1, 0)
    brush.plane(normal, point)
    BRUSHES.append(brush)


def build():
    # пол и стены по краю
    box('floor', (-HALF - 64, -HALF - 64, -32), (HALF + 64, HALF + 64, 0))
    box('wall', (-HALF - 64, -HALF - 64, 0), (-HALF, HALF + 64, WALL_HEIGHT))
    box('wall', (HALF, -HALF - 64, 0), (HALF + 64, HALF + 64, WALL_HEIGHT))
    box('wall', (-HALF, -HALF - 64, 0), (HALF, -HALF, WALL_HEIGHT))
    box('wall', (-HALF, HALF, 0), (HALF, HALF + 64, WALL_HEIGHT))
    # небо: колпак над стенами
    box('sky', (-HALF - 128, -HALF - 128, -64), (-HALF - 64, HALF + 128, SKY_TOP))
    box('sky', (HALF + 64, -HALF - 128, -64), (HALF + 128, HALF + 128, SKY_TOP))
    box('sky', (-HALF - 64, -HALF - 128, -64), (HALF + 64, -HALF - 64, SKY_TOP))
    box('sky', (-HALF - 64, HALF + 64, -64), (HALF + 64, HALF + 128, SKY_TOP))
    box('sky', (-HALF - 128, -HALF - 128, SKY_TOP), (HALF + 128, HALF + 128, SKY_TOP + 64))
    box('sky', (-HALF - 128, -HALF - 128, -128), (HALF + 128, HALF + 128, -64))
    # ступени разной высоты (на запад): 8, 16, 24, 32, 48 единиц
    for index, height in enumerate((8, 16, 24, 32, 48)):
        x0 = -800 + index * 160
        box('step', (x0, 500, 0), (x0 + 128, 628, height))
    # пандус 15° на площадку 64 и пандус 30° на площадку 96
    ramp('ramp', 200, -700, 440, -572, 64, '+x')
    box('ramp', (440, -700, 0), (696, -444, 64))
    ramp('ramp', 200, -380, 366, -252, 96, '+x')
    box('ramp', (366, -380, 0), (622, -124, 96))
    # колонны
    for x_value, y_value in ((-300, -300), (-300, 0), (-600, -300), (-600, 0)):
        box('column', (x_value - 32, y_value - 32, 0), (x_value + 32, y_value + 32, 160))
    # низкая стена (из-за неё видно корпус) и два прохода: 96 и 160 единиц
    box('wall', (-200, 250, 0), (300, 282, 40))
    box('wall', (400, 300, 0), (432, 700, 128))
    box('wall', (528, 300, 0), (560, 700, 128))
    box('wall', (720, 300, 0), (752, 700, 128))


def entities():
    spawns = [((-800, -800, 40), 45), ((800, 800, 40), 225), ((-800, 800, 40), 315), ((800, -800, 40), 135),
              ((0, -850, 40), 90), ((0, 850, 40), 270)]
    lines = []
    for (x_value, y_value, z_value), angle in spawns:
        lines.append(f'{{\n"classname" "info_player_deathmatch"\n"origin" "{x_value} {y_value} {z_value}"\n"angle" "{angle}"\n}}')
    lines.append(f'{{\n"classname" "info_player_intermission"\n"origin" "-700 -700 400"\n"angles" "30 45 0"\n}}')
    return '\n'.join(lines)


def main():
    base = sys.argv[1]
    for folder in ('maps', 'textures/mechtest', 'scripts'):
        os.makedirs(os.path.join(base, folder), exist_ok=True)
    for name, colour in SURFACES.items():
        texture(colour).save(os.path.join(base, 'textures', 'mechtest', f'{name}.tga'))
    Image.new('RGB', (64, 64), (176, 196, 214)).save(os.path.join(base, 'textures', 'mechtest', 'sky.tga'))
    with open(os.path.join(base, 'scripts', 'mechtest.shader'), 'w') as handle:
        handle.write('// WALKER test map wk_mechtest, generated by walker/tools/make_map_mechtest.py\n\n'
                     'textures/mechtest/sky\n{\n\tqer_editorimage textures/mechtest/sky.tga\n'
                     '\tsurfaceparm sky\n\tsurfaceparm noimpact\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n'
                     '\tq3map_sun 1 0.96 0.88 230 40 55\n\tq3map_surfacelight 40\n\tskyparms - 512 -\n'
                     '\t{\n\t\tmap textures/mechtest/sky.tga\n\t\trgbGen identity\n\t}\n}\n')
    # q3map читает шейдеры по списку scripts/shaderlist.txt
    with open(os.path.join(base, 'scripts', 'shaderlist.txt'), 'w') as handle:
        handle.write('mechtest\n')
    with open(os.path.join(base, 'scripts', f'{NAME}.arena'), 'w') as handle:
        handle.write(f'{{\nmap "{NAME}"\nlongname "Mech test"\nfraglimit 0\ntype "ffa tourney team"\n}}\n')
    build()
    with open(os.path.join(base, 'maps', f'{NAME}.map'), 'w') as handle:
        handle.write('{\n"classname" "worldspawn"\n"message" "Mech test"\n"_minlight" "40"\n')
        handle.write('\n'.join(brush.text() for brush in BRUSHES))
        handle.write('\n}\n' + entities() + '\n')
    print(f'{NAME}: {len(BRUSHES)} brushes')


if __name__ == '__main__':
    main()
