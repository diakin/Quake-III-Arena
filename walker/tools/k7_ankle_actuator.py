"""Один гидроцилиндр голеностопа K7 посередине ноги (WALKER, 28.11.2026).

python3 walker/tools/k7_ankle_actuator.py <lower.glb> [out.glb]

Было (k7_hydraulics.py): по два тонких цилиндра на ногу сзади по углам голени, верхние
проушины на рычагах у оси колена. Стало, по просьбе: один цилиндр на ногу, посередине за
голенью, шток толще:

  - на голени сзади утолщение - короб (камуфляж, как бронеплиты), в нём верхний палец и
    верхний конец корпуса: там подразумевается подвод масла;
  - тёмно-синий корпус выходит из низа короба и в каждом кадре смотрит на нижний палец;
  - полированный шток (материал chrome: класс polished в bake_walker.py, узкий яркий блик)
    висит на нижнем пальце и смотрит на корпус, уходя в него;
  - нижний палец на вилке, стоящей на шпоре пятки (деталь ступни сзади посередине).

Короб стоит ниже места, через которое при сгибе колена проходит низ корпуса бедра: в живых
анимациях кроме приседа бедро опускается за голенью до y = -0.31 (бег), верх короба -0.34.
При приседе (LEGS_WALKCR до -0.70, LEGS_IDLECR до -0.56) и в анимациях гибели бедро
заходит в короб - это допущено.

Пальцы как можно дальше назад (перебор по анимациям): при беге ступня сгибается до -67
градусов, пятка уходит под голень, и шток у нижнего заднего угла голени заходит в неё
самое большее на 0.6 единицы Quake (бег), в приседе на 1.3, в остальных живых анимациях
не касается.

Длины по всем анимациям: шток при самом длинном расстоянии между пальцами в корпусе хотя бы
на ROD_OVERLAP, при самом коротком его верх остаётся в корпусе выше пальца (в коробе).
Старые узлы hydraulic_* (k7_hydraulics.py) и их анимация удаляются; повторный запуск
снова ставит новый цилиндр (узлы ankle_actuator_* тоже удаляются).
"""
import sys, math
import numpy as np
import k7_hydraulics as base

UPPER_PIN = (-0.45, -0.33)          # (y, z) в пространстве колена: в коробе за голенью
LOWER_PIN = (-0.18, -0.48)          # (y, z) в пространстве голеностопа: над задним концом шпоры
HOUSING = ((-0.10, -0.58, -0.49), (0.10, -0.34, -0.15))     # короб на голени (x, y, z)
BARREL_RADIUS = 0.068
BARREL_ABOVE = 0.09                 # корпус выше пальца (в коробе)
BARREL_VISIBLE = 0.28               # корпус ниже короба
ROD_RADIUS = 0.042                  # было 0.03
ROD_OVERLAP = 0.13                  # в живых анимациях
ROD_OVERLAP_DEATH = 0.05
REMOVE_PREFIXES = ('hydraulic_', 'ankle_actuator_')


def strip(document):
    """убрать узлы с REMOVE_PREFIXES, их сетки и каналы анимации"""
    nodes = document['nodes']
    removed = {index for index, node in enumerate(nodes) if node.get('name', '').startswith(REMOVE_PREFIXES)}
    keep = [index for index in range(len(nodes)) if index not in removed]
    remap = {old: new for new, old in enumerate(keep)}
    new_nodes = [nodes[index] for index in keep]
    for node in new_nodes:
        if 'children' in node:
            node['children'] = [remap[child] for child in node['children'] if child in remap]
    document['nodes'] = new_nodes
    for scene in document['scenes']:
        scene['nodes'] = [remap[index] for index in scene['nodes'] if index in remap]
    for animation in document['animations']:
        channels, samplers, sampler_remap = [], [], {}
        for channel in animation['channels']:
            if channel['target']['node'] in removed:
                continue
            old = channel['sampler']
            if old not in sampler_remap:
                sampler_remap[old] = len(samplers)
                samplers.append(animation['samplers'][old])
            channels.append({'sampler': sampler_remap[old],
                             'target': {'node': remap[channel['target']['node']], 'path': channel['target']['path']}})
        animation['channels'], animation['samplers'] = channels, samplers
    used = sorted({node['mesh'] for node in new_nodes if 'mesh' in node})
    mesh_remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in new_nodes:
        if 'mesh' in node:
            node['mesh'] = mesh_remap[node['mesh']]
    return len(removed)


def blue_material(document):
    """тёмно-синий материал корпуса (текстура hydraulic_blue.jpg рядом с моделью)"""
    names = [material['name'] for material in document['materials']]
    if 'hydraulic_blue' in names:
        return names.index('hydraulic_blue')
    document['images'].append({'uri': 'hydraulic_blue.jpg', 'mimeType': 'image/jpeg'})
    document['textures'].append({'source': len(document['images']) - 1})
    document['materials'].append({'name': 'hydraulic_blue', 'pbrMetallicRoughness': {
        'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0.35, 'roughnessFactor': 0.45,
        'baseColorTexture': {'index': len(document['textures']) - 1}}})
    return len(document['materials']) - 1


def build_parts(barrel_below, rod_length):
    parts = {name: {} for name in ('housing', 'barrel', 'rod', 'lower_mount')}

    def get(node, material):
        return parts[node].setdefault(material, base.Part())
    # короб на голени (в пространстве колена, узел в начале координат колена)
    base.box(get('housing', 'camo_green'), HOUSING[0], HOUSING[1])
    # корпус: ось -Y от пальца; верх и палец в коробе, у выхода штока тёмная букса
    base.cylinder(get('barrel', 'hydraulic_blue'), 'y', (0, 0, 0), BARREL_RADIUS, BARREL_ABOVE, -(barrel_below - 0.03), (True, False))
    base.cylinder(get('barrel', 'metal_dark'), 'y', (0, 0, 0), BARREL_RADIUS + 0.006, -(barrel_below - 0.03), -barrel_below)
    base.cylinder(get('barrel', 'metal_dark'), 'x', (0, 0, 0), 0.03, -0.09, 0.09)     # палец (в коробе)
    # шток: проушина, наконечник, полированный шток
    base.cylinder(get('rod', 'metal_dark'), 'x', (0, 0, 0), 0.05, -0.03, 0.03)
    base.cylinder(get('rod', 'metal_dark'), 'y', (0, 0, 0), 0.05, 0.035, 0.11, (False, True))
    # шток круглее (16 граней): блик по карте окружения считается в вершинах, на 10 гранях
    # узкая полоса блика ложилась бы ломаной
    segments, base.SEGMENTS = base.SEGMENTS, 16
    base.cylinder(get('rod', 'chrome'), 'y', (0, 0, 0), ROD_RADIUS, 0.11, rod_length, (False, True))
    base.SEGMENTS = segments
    # нижняя вилка на шпоре пятки (верх шпоры на 0.05 ниже пальца): подошва, две щеки, палец
    base.box(get('lower_mount', 'metal_dark'), (-0.07, -0.065, -0.07), (0.07, -0.04, 0.07))
    for sign in (-1, 1):
        low, high = sorted((sign * 0.034, sign * 0.048))
        base.box(get('lower_mount', 'metal_dark'), (low, -0.05, -0.055), (high, 0.055, 0.055))
    base.cylinder(get('lower_mount', 'metal_gray'), 'x', (0, 0, 0), 0.018, -0.052, 0.052)
    return parts


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = base.read_glb(source)
    removed = strip(document)
    base.UPPER_PIN, base.LOWER_PIN = UPPER_PIN, LOWER_PIN      # углы и длины считает k7_hydraulics
    names = [node.get('name', '') for node in document['nodes']]
    knees = {'L': names.index('knee_L'), 'R': names.index('knee_R')}
    ankles = {'L': names.index('ankle_L'), 'R': names.index('ankle_R')}

    lengths, live_lengths = [], []
    for animation in document['animations']:
        channels = {(channel['target']['node'], channel['target']['path']): channel for channel in animation['channels']}
        for side in 'LR':
            rotation = base.accessor(document, binary, animation['samplers'][channels[(ankles[side], 'rotation')]['sampler']]['output'])
            translation = base.accessor(document, binary, animation['samplers'][channels[(ankles[side], 'translation')]['sampler']]['output'])
            for key in range(len(rotation)):
                length = base.angles(rotation[key], translation[min(key, len(translation) - 1)])[2]
                lengths.append(length)
                if not animation['name'].startswith('BOTH_'):
                    live_lengths.append(length)
    # шток по живым анимациям; в анимациях гибели нога разгибается сильнее, шток выдвигается
    # дальше, но остаётся в корпусе хотя бы на ROD_OVERLAP_DEATH
    shortest, longest = min(lengths), max(live_lengths)
    hidden = UPPER_PIN[0] - HOUSING[0][1]                     # от пальца до низа короба по Y
    barrel_below = hidden + BARREL_VISIBLE
    rod_length = longest - barrel_below + ROD_OVERLAP
    top_above_pin = rod_length - shortest
    print(f'pin distance {shortest:.3f} .. {longest:.3f}, barrel below the pin {barrel_below:.3f}, '
          f'rod {rod_length:.3f}, rod top above the pin at the shortest {top_above_pin:.3f}')
    if top_above_pin > BARREL_ABOVE - 0.02:
        raise SystemExit('rod would come out of the top of the barrel')
    death_overlap = rod_length - (max(lengths) - barrel_below)
    print(f'rod in the barrel at the longest of all animations {death_overlap:.3f}')
    if death_overlap < ROD_OVERLAP_DEATH:
        raise SystemExit('rod would come out of the barrel in a death animation')

    material_index = {material['name']: index for index, material in enumerate(document['materials'])}
    material_index['hydraulic_blue'] = blue_material(document)
    parts = build_parts(barrel_below, rod_length)
    new_nodes, animated = [], []
    for side in 'LR':
        for part_name, parent, origin in (('housing', knees[side], (0.0, 0.0)), ('barrel', knees[side], UPPER_PIN),
                                          ('lower_mount', ankles[side], LOWER_PIN), ('rod', ankles[side], LOWER_PIN)):
            primitives = []
            for material, part in parts[part_name].items():
                primitives.append({
                    'attributes': {
                        'POSITION': base.add_accessor(document, binary, np.array(part.positions, np.float32), 'VEC3', 34962),
                        'NORMAL': base.add_accessor(document, binary, np.array(part.normals, np.float32), 'VEC3', 34962),
                        'TEXCOORD_0': base.add_accessor(document, binary, np.array(part.uvs, np.float32), 'VEC2', 34962)},
                    'indices': base.add_accessor(document, binary, np.array(part.indices, np.uint16), 'SCALAR', 34963),
                    'material': material_index[material]})
            document['meshes'].append({'name': f'ankle_actuator_{part_name}', 'primitives': primitives})
            node = {'name': f'ankle_actuator_{part_name}_{side}', 'mesh': len(document['meshes']) - 1,
                    'translation': [0.0, origin[0], origin[1]]}
            rest = document['nodes'][ankles[side]]
            barrel_angle, rod_angle, _ = base.angles(rest.get('rotation', [0, 0, 0, 1]), rest.get('translation', [0, 0, 0]))
            if part_name in ('barrel', 'rod'):
                node['rotation'] = base.quat_x(barrel_angle if part_name == 'barrel' else rod_angle)
            document['nodes'].append(node)
            document['nodes'][parent].setdefault('children', []).append(len(document['nodes']) - 1)
            if part_name in ('barrel', 'rod'):
                animated.append((side, len(document['nodes']) - 1, part_name))

    for animation in document['animations']:
        channels = {(channel['target']['node'], channel['target']['path']): channel for channel in animation['channels']}
        for side, node_index, part_name in animated:
            rotation_sampler = animation['samplers'][channels[(ankles[side], 'rotation')]['sampler']]
            translation_sampler = animation['samplers'][channels[(ankles[side], 'translation')]['sampler']]
            rotation = base.accessor(document, binary, rotation_sampler['output'])
            translation = base.accessor(document, binary, translation_sampler['output'])
            values = []
            for key in range(len(rotation)):
                barrel_angle, rod_angle, _ = base.angles(rotation[key], translation[min(key, len(translation) - 1)])
                values.append(base.quat_x(barrel_angle if part_name == 'barrel' else rod_angle))
            output = base.add_accessor(document, binary, np.array(values, np.float32), 'VEC4')
            animation['samplers'].append({'input': rotation_sampler['input'], 'output': output,
                                          'interpolation': rotation_sampler.get('interpolation', 'LINEAR')})
            animation['channels'].append({'sampler': len(animation['samplers']) - 1,
                                          'target': {'node': node_index, 'path': 'rotation'}})
    base.write_glb(target, document, binary)
    print(f'{target}: {removed} old actuator nodes removed, {len(animated) * 2} nodes added')


if __name__ == '__main__':
    main()
