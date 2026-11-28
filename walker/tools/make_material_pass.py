#!/usr/bin/env python3
"""Материалы робота и орудий в один проход (WALKER, 26.11.2026).

python3 walker/tools/make_material_pass.py <папка базовых данных>

Запускается после bake_walker.py и make_robots.py (build_all.sh). Каждая часть робота
(lower, upper, head) рисовалась несколькими шейдерами по классу материала (краска,
металл, стекло, свечение) на одном атласе, у каждого шейдера до трёх слоёв — до восьми
вызовов отрисовки на часть. Здесь для каждой части пишется маска классов
<часть>_mask.tga (r — отражение умножением, g — отражение сложением, b — блик со
светом, a — свечение без освещения; числа берутся из слоёв шейдеров класса) и шейдер
части <часть>_<окраска>_pass с ключом materialImages; шейдеры классов получают ключ
materialPass. Движок (renderer/tr_glsl.c, программа materials) рисует часть одним
вызовом; без GLSL остаются шейдеры классов, как раньше.

Орудия: их материалы (gunmetal, dark, paint, accent, glow_*) — общие почти однотонные
текстуры 128x128 с плоской развёрткой. Они сводятся в один атлас
models/robotweapons/weapons.jpg (клетки по материалу), UV орудий переносятся в клетки
(с обрезкой до [0, 1]: текстуры однотонные, повтор не виден), маска — по клеткам.

Если bake_walker.py записал карту износа <часть>_wear.tga (у K7), маска учитывает и её:
голый металл сколов блестит как металл, грязь и пыль матовые, масло мокрое.
Карта рельефа <часть>_bump.tga (у K7) попадает в шейдер части ключом materialBump.
Дюзы (материалы EMBER) светятся как остальное свечение, но в маске у них g = 1: в
светящемся пикселе каналы r, g, b не нужны (цвет там целиком от свечения), и шейдер
(tr_glsl.c) рисует по этой пометке мерцание раскалённого металла вместо ровного свечения.

Повторный запуск ничего не меняет (шейдеры с materialPass пропускаются).
"""
import json, os, re, struct, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

DATA = sys.argv[1]
ROBOTS = {'walker': 'walker_player.shader', 'strizh': 'strizh_player.shader', 'molot': 'molot_player.shader'}
PARTS = ('lower', 'upper', 'head')
ENV_DEFAULT = 'models/players/walker/env_shine.jpg'
WEAPON_DIR = 'models/robotweapons'
WEAPON_CELL = 128
WEAPON_MARGIN = 6           # texels of a cell the UVs stay away from (bilinear filter, mips)


# ------------------------------------------------------------------ shader text

def parse_shaders(text):
    """name -> (start, end, stages); a stage is a list of lines (lower case tokens kept)."""
    result = {}
    position = 0
    pattern = re.compile(r'^([^\s{}/][^\s{}]*)\s*\n\s*\{', re.M)
    while True:
        match = pattern.search(text, position)
        if not match:
            break
        name = match.group(1)
        depth, index = 0, match.end() - 1
        stages, current = [], None
        line_start = index
        while index < len(text):
            char = text[index]
            if char == '{':
                depth += 1
                if depth == 2:
                    current = []
                    line_start = index + 1
            elif char == '}':
                if depth == 2 and current is not None:
                    current.extend(l.strip() for l in text[line_start:index].splitlines() if l.strip())
                    stages.append(current)
                    current = None
                depth -= 1
                if depth == 0:
                    break
            index += 1
        result[name] = (match.start(), index + 1, stages)
        position = index + 1
    return result


def stage_value(stage, key):
    for line in stage:
        tokens = line.split()
        if tokens and tokens[0].lower() == key.lower():
            return tokens[1:]
    return None


def class_numbers(stages):
    """mask channels (r, g, b, a) and glow wave of a class shader."""
    mul = add = spec = 0.0
    glow = None
    first = stages[0] if stages else []
    rgb = stage_value(first, 'rgbGen')
    if rgb and rgb[0].lower() != 'lightingdiffuse':
        if rgb[0].lower() == 'wave' and len(rgb) >= 6:
            glow = [float(rgb[2]), float(rgb[3]), float(rgb[4]), float(rgb[5])]
        else:
            glow = [1.0, 0.0, 0.0, 0.0]
    env = None
    for stage in stages[1:]:
        if not stage_value(stage, 'tcGen') or stage_value(stage, 'tcGen')[0].lower() != 'environment':
            continue
        env = stage_value(stage, 'map')[0]
        blend = [t.upper() for t in (stage_value(stage, 'blendFunc') or [])]
        const = stage_value(stage, 'rgbGen')
        value = float(const[2]) if const and const[0].lower() == 'const' else 1.0
        specular = stage_value(stage, 'alphaGen')
        if blend == ['GL_DST_COLOR', 'GL_ONE']:
            mul = value
        elif blend == ['GL_SRC_ALPHA', 'GL_ONE'] and specular and specular[0].lower() == 'lightingspecular':
            spec = value
        elif blend == ['GL_ONE', 'GL_ONE']:
            add = value
    channels = (mul, add, spec, 1.0 if glow else 0.0)
    return channels, glow, env


def add_material_pass(text, class_names, pass_name):
    """the line "materialPass <pass>" right after the opening brace of each class shader."""
    shaders = parse_shaders(text)
    for name in sorted(class_names, key=lambda n: -shaders[n][0]):
        start, end, _ = shaders[name]
        block = text[start:end]
        if 'materialPass' in block:
            continue
        brace = block.index('{')
        block = block[:brace + 1] + f'\n\tmaterialPass {pass_name}' + block[brace + 1:]
        text = text[:start] + block + text[end:]
    return text


# ------------------------------------------------------------------ glb

def read_glb(path):
    data = open(path, 'rb').read()
    json_length = struct.unpack('<I', data[12:16])[0]
    document = json.loads(data[20:20 + json_length])
    binary_start = 20 + json_length + 8
    return data, document, binary_start


def accessor_array(data, document, binary_start, index):
    kinds = {5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1'}
    accessor = document['accessors'][index]
    view = document['bufferViews'][accessor['bufferView']]
    width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[accessor['type']]
    offset = binary_start + view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
    array = np.frombuffer(data, dtype=kinds[accessor['componentType']], count=accessor['count'] * width, offset=offset)
    return array.reshape(-1, width), offset


def primitives(document):
    for mesh in document['meshes']:
        for primitive in mesh['primitives']:
            yield primitive


def material_name(document, primitive):
    return document['materials'][primitive['material']].get('name', '')


# ------------------------------------------------------------------ robots

def read_skin(path):
    mapping = {}
    for line in open(path, encoding='ascii'):
        if ',' in line:
            surface, shader = line.strip().split(',', 1)
            if shader:
                mapping[surface] = shader
    return mapping


def rasterize_mask(glb_path, class_of_material, numbers, size):
    data, document, binary_start = read_glb(glb_path)
    classes = sorted(numbers)
    index_image = Image.new('L', (size, size), 0)
    draw = ImageDraw.Draw(index_image)
    for primitive in primitives(document):
        klass = class_of_material.get(material_name(document, primitive))
        if klass is None:
            continue
        value = classes.index(klass) + 1
        uv, _ = accessor_array(data, document, binary_start, primitive['attributes']['TEXCOORD_0'])
        faces, _ = accessor_array(data, document, binary_start, primitive['indices'])
        faces = faces.reshape(-1, 3)
        for face in faces:
            corners = [(float(uv[i][0]) * size, float(uv[i][1]) * size) for i in face]
            draw.polygon(corners, fill=value, outline=value)
    indexes = np.asarray(index_image)
    # the rest of the atlas from the nearest class (seams, mip levels)
    _, (rows, cols) = ndimage.distance_transform_edt(indexes == 0, return_indices=True)
    indexes = indexes[rows, cols]
    mask = np.zeros((size, size, 4), dtype=np.uint8)
    for number, klass in enumerate(classes, 1):
        mask[indexes == number] = [round(channel * 255) for channel in numbers[klass]]
    return mask


# дюзы: раскалённый металл, мерцает как угли (g = 1 в светящемся пикселе маски)
EMBER = {'engine_glow'}
EMBER_CHANNELS = (0.0, 1.0, 0.0, 1.0)

# голый металл сколов блестит как металл K7, масло - как мокрая плёнка
BARE_METAL = (0.40, 0.0, 0.55)      # r (отражение умножением), g, b (блик)
WET = (0.30, 0.0, 0.95)


def apply_wear(mask, wear_path, numbers):
    """карта износа из bake_walker.py: r - голый металл, g - множитель блеска x128, b - масло."""
    if not os.path.exists(wear_path):
        return mask
    size = mask.shape[0]
    wear = np.asarray(Image.open(wear_path).convert('RGB').resize((size, size), Image.BILINEAR)).astype(float)
    bare, gloss, wet = wear[..., 0] / 255, wear[..., 1] / 128, wear[..., 2] / 255
    result = mask.astype(float) / 255
    lit = result[..., 3] < 0.5                       # свечение не трогаем
    for channel in range(3):
        value = result[..., channel] * gloss
        value = np.maximum(value, BARE_METAL[channel] * bare)
        value = np.maximum(value, WET[channel] * wet)
        result[..., channel] = np.where(lit, np.clip(value, 0, 1), result[..., channel])
    return np.round(result * 255).astype(np.uint8)


def robot(name, shader_file):
    folder = os.path.join(DATA, 'models', 'players', name)
    shader_path = os.path.join(DATA, 'scripts', shader_file)
    if not os.path.isdir(folder) or not os.path.exists(shader_path):
        return
    text = open(shader_path, encoding='ascii').read()
    prefix = f'models/players/{name}'
    schemes = sorted(f[len('lower_'):-len('.skin')] for f in os.listdir(folder)
                     if f.startswith('lower_') and f.endswith('.skin'))
    for part in PARTS:
        glb = os.path.join(folder, f'{part}.glb')
        if not os.path.exists(glb):
            continue
        shaders = parse_shaders(text)
        skin = read_skin(os.path.join(folder, f'{part}_default.skin'))
        class_of_material, numbers, glow, env = {}, {}, [1.0, 0.0, 0.0, 0.0], None
        for material, shader in skin.items():
            match = re.match(rf'{prefix}/{part}_(\w+)_default$', shader)
            if not match or shader not in shaders:
                continue
            klass = match.group(1)
            if klass == 'polished':
                continue        # полированные штоки рисуются своим шейдером (bake_walker.py)
            channels, class_glow, class_env = class_numbers(shaders[shader][2])
            class_of_material[material] = klass
            numbers[klass] = channels
            glow = class_glow or glow
            env = env or class_env
        if not class_of_material:
            continue
        for material in EMBER & set(class_of_material):
            if class_of_material[material] == 'glow':
                class_of_material[material] = 'ember'
                numbers['ember'] = EMBER_CHANNELS
        atlas = Image.open(os.path.join(folder, f'{part}_default.jpg'))
        size = max(128, atlas.size[0] // 2)
        mask = rasterize_mask(glb, class_of_material, numbers, size)
        mask = apply_wear(mask, os.path.join(folder, f'{part}_wear.tga'), numbers)
        mask_name = f'{prefix}/{part}_mask.tga'
        Image.fromarray(mask, 'RGBA').save(os.path.join(DATA, mask_name))
        # рельеф K7 (bake_walker.py, write_bump): карта наклонов и её предел в единицах Quake
        bump_line = ''
        range_path = os.path.join(folder, f'{part}_bump.txt')
        if os.path.exists(os.path.join(folder, f'{part}_bump.tga')) and os.path.exists(range_path):
            bump_range = float(open(range_path, encoding='ascii').read().strip())
            bump_line = f'\tmaterialBump {prefix}/{part}_bump.tga {bump_range:g}\n'
            os.remove(range_path)       # в данные игры не идёт
        for scheme in schemes:
            pass_name = f'{prefix}/{part}_{scheme}_pass'
            if pass_name not in shaders:
                text += (f'{pass_name}\n{{\n\tmaterialImages {mask_name} {env or ENV_DEFAULT} '
                         f'{glow[0]:g} {glow[1]:g} {glow[2]:g} {glow[3]:g}\n' + bump_line +
                         f'\t{{\n\t\tmap {prefix}/{part}_{scheme}.jpg\n\t\trgbGen lightingDiffuse\n\t}}\n}}\n')
            classes = [n for n in parse_shaders(text) if re.match(rf'{prefix}/{part}_(paint|metal|glass|glow)_{scheme}$', n)]
            text = add_material_pass(text, classes, pass_name)
        print(f'{name} {part}: mask {size}x{size}, classes {sorted(numbers)}')
    open(shader_path, 'w', encoding='ascii', newline='\n').write(text)


# ------------------------------------------------------------------ weapons

def weapons():
    folder = os.path.join(DATA, WEAPON_DIR)
    shader_path = os.path.join(DATA, 'scripts', 'walker_robots.shader')
    if not os.path.isdir(folder) or not os.path.exists(shader_path):
        return
    text = open(shader_path, encoding='ascii').read()
    shaders = parse_shaders(text)
    materials = sorted(n[len(WEAPON_DIR) + 1:] for n in shaders
                       if n.startswith(WEAPON_DIR + '/') and '/' not in n[len(WEAPON_DIR) + 1:]
                       and not n.endswith('_pass'))
    if any('materialPass' in text[shaders[f'{WEAPON_DIR}/{m}'][0]:shaders[f'{WEAPON_DIR}/{m}'][1]] for m in materials):
        print('weapons: already in one pass')
        return
    columns = 4
    rows = (len(materials) + columns - 1) // columns
    width, height = columns * WEAPON_CELL, max(1, 1 << (rows * WEAPON_CELL - 1).bit_length())
    atlas = Image.new('RGB', (width, height), (0, 0, 0))
    mask = np.zeros((height, width, 4), dtype=np.uint8)
    cells, glow, env = {}, [1.0, 0.0, 0.0, 0.0], None
    for number, material in enumerate(materials):
        stages = shaders[f'{WEAPON_DIR}/{material}'][2]
        image_name = stage_value(stages[0], 'map')[0]
        image = Image.open(os.path.join(DATA, image_name)).convert('RGB').resize((WEAPON_CELL, WEAPON_CELL))
        column, row = number % columns, number // columns
        atlas.paste(image, (column * WEAPON_CELL, row * WEAPON_CELL))
        channels, class_glow, class_env = class_numbers(stages)
        mask[row * WEAPON_CELL:(row + 1) * WEAPON_CELL, column * WEAPON_CELL:(column + 1) * WEAPON_CELL] = \
            [round(channel * 255) for channel in channels]
        glow = class_glow or glow
        env = env or class_env
        cells[material] = (column * WEAPON_CELL, row * WEAPON_CELL)
    atlas_name, mask_name, pass_name = f'{WEAPON_DIR}/weapons.jpg', f'{WEAPON_DIR}/weapons_mask.tga', f'{WEAPON_DIR}/weapons_pass'
    atlas.save(os.path.join(DATA, atlas_name), quality=92)
    Image.fromarray(mask, 'RGBA').save(os.path.join(DATA, mask_name))

    # the UVs of every weapon into the cell of its material
    span = WEAPON_CELL - 2 * WEAPON_MARGIN
    for file in sorted(os.listdir(folder)):
        if not file.endswith('.glb'):
            continue
        path = os.path.join(folder, file)
        data, document, binary_start = read_glb(path)
        data = bytearray(data)
        done = set()
        for primitive in primitives(document):
            material = material_name(document, primitive)
            index = primitive['attributes']['TEXCOORD_0']
            if material not in cells or index in done:
                continue
            done.add(index)
            uv, offset = accessor_array(bytes(data), document, binary_start, index)
            left, top = cells[material]
            moved = np.empty_like(uv)
            moved[:, 0] = (left + WEAPON_MARGIN + np.clip(uv[:, 0], 0, 1) * span) / width
            moved[:, 1] = (top + WEAPON_MARGIN + np.clip(uv[:, 1], 0, 1) * span) / height
            data[offset:offset + moved.nbytes] = moved.astype('<f4').tobytes()
        open(path, 'wb').write(bytes(data))

    # the class shaders draw from the atlas, the pass shader draws them all
    for material in sorted(materials, key=lambda m: -shaders[f'{WEAPON_DIR}/{m}'][0]):
        start, end, stages = shaders[f'{WEAPON_DIR}/{material}']
        old_map = stage_value(stages[0], 'map')[0]
        block = text[start:end].replace(f'map {old_map}', f'map {atlas_name}', 1)
        text = text[:start] + block + text[end:]
    text += (f'\n{pass_name}\n{{\n\tmaterialImages {mask_name} {env or ENV_DEFAULT} '
             f'{glow[0]:g} {glow[1]:g} {glow[2]:g} {glow[3]:g}\n'
             f'\t{{\n\t\tmap {atlas_name}\n\t\trgbGen lightingDiffuse\n\t}}\n}}\n')
    text = add_material_pass(text, [f'{WEAPON_DIR}/{m}' for m in materials], pass_name)
    open(shader_path, 'w', encoding='ascii', newline='\n').write(text)
    print(f'weapons: atlas {width}x{height}, materials {materials}')


for robot_name, robot_shader in ROBOTS.items():
    robot(robot_name, robot_shader)
weapons()
