"""Кабина K7, утопленная в корпус носа, под небольшим стеклянным колпаком (WALKER, 28.11.2026).

python3 walker/tools/k7_cockpit.py <head.glb без кабины> [out.glb]

Стекло кабины (canopy_glass) - эллипсоид, наполовину сидящий в эллипсоиде носа; под ним
сидела серая металлическая полусфера, сквозь тонировку был виден только металл. Инструмент:

  - утапливает оба стекла в нос так, чтобы снаружи оставался невысокий колпак
    (UPPER_DOME, LOWER_DOME - высота колпака над носом по оси стекла);
  - убирает серые полусферы и вырезает в носу отверстие под каждым стеклом: нос
    строится заново более мелкой сеткой, треугольники внутри эллипсоида стекла
    убираются (на краю отверстия - после дробления, чтобы край был ровным);
  - внутреннюю часть эллипсоида стекла превращает в стенки кабины (black, нормали
    внутрь): сквозь колпак видно углубление в корпусе;
  - по краю отверстия ставит металлическую окантовку (трубка по линии пересечения
    стекла и носа);
  - в верхней кабине: кресло пилота (спинка, сиденье, подголовник, боковины, оранжевые
    ремни) и у нижнего края проёма приборная панель с двумя экранами (light_cyan) и
    огоньками (indicator_red, indicator_green); в нижней - панель с экраном и огоньками;
  - стекло строится более мелкой сеткой, чтобы блик от карты окружения был точкой.

Детали кабины строятся в системе стекла (его сдвиг и поворот, без масштаба): x вправо,
y вверх по стеклу, z из стекла наружу; инструмент проверяет, что все они внутри
углубления. Новые материалы: black, light_cyan, indicator_red, indicator_green (текстуры
рядом с моделью, строки в head_*.skin, огоньки - класс glow в bake_walker.py).
"""
import sys, math
import numpy as np

from k7_hydraulics import read_glb, write_glb, add_accessor, Part, box

UPPER_DOME = 0.05           # высота колпака над носом
LOWER_DOME = 0.045
WALL_SCALE = 0.99           # стенки кабины чуть внутри стекла
RIM_RADIUS = 0.013          # окантовка проёма
NOSE_RINGS, NOSE_SEGMENTS = 28, 44
GLASS_RINGS, GLASS_SEGMENTS = 24, 36
WALL_RINGS, WALL_SEGMENTS = 14, 24
SPLIT_DEPTH = 3             # дробление треугольников на краю отверстия

NEW_MATERIALS = {
    'black': {'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0.3, 'roughnessFactor': 0.7}},
    'light_cyan': {'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0, 'roughnessFactor': 0.3},
                   'emissiveFactor': [0.2, 0.8, 1.0]},
    'indicator_red': {'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0, 'roughnessFactor': 0.3},
                      'emissiveFactor': [1.0, 0.1, 0.05]},
    'indicator_green': {'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0, 'roughnessFactor': 0.3},
                        'emissiveFactor': [0.2, 1.0, 0.3]},
}


def quat_matrix(quaternion):
    x, y, z, w = quaternion
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


class Ellipsoid:
    def __init__(self, translation, scale, rotation=(0, 0, 0, 1)):
        self.translation = np.array(translation, float)
        self.scale = np.array(scale, float)
        self.rotation = quat_matrix(rotation)

    def value(self, point):
        """< 1 внутри"""
        local = self.rotation.T @ (np.asarray(point, float) - self.translation)
        return float(np.sum((local / self.scale) ** 2))

    def to_head(self, unit_point, factor=1.0):
        return self.translation + self.rotation @ (np.asarray(unit_point, float) * self.scale * factor)


# ---------------------------------------------------------------- сферы с вырезом

def unit_sphere(rings, segments):
    """единичная сфера: вершины (положение, uv) и треугольники; полюса по оси z"""
    vertices, faces = [], []
    for ring in range(rings + 1):
        theta = math.pi * ring / rings
        for segment in range(segments + 1):
            phi = 2 * math.pi * segment / segments
            vertices.append((np.array([math.sin(theta) * math.cos(phi), math.sin(theta) * math.sin(phi), math.cos(theta)]),
                             (segment / segments, ring / rings)))
    for ring in range(rings):
        for segment in range(segments):
            first = ring * (segments + 1) + segment
            second = first + segments + 1
            if ring > 0:
                faces.append((first, second, first + 1))
            if ring < rings - 1:
                faces.append((first + 1, second, second + 1))
    return vertices, faces


def carve(part, vertices, faces, keep, inward=False, depth=SPLIT_DEPTH):
    """треугольники сферы, для которых keep(точка на сфере) истинно; на границе - дробление"""
    positions, normals, uvs, out_faces, shared = [], [], [], [], {}

    def emit(corners):
        face = []
        for point, uv in corners:
            key = (tuple(np.round(point, 5)), round(uv[0], 5), round(uv[1], 5))
            if key not in shared:
                shared[key] = len(positions)
                unit = point / np.linalg.norm(point)
                positions.append(point); normals.append(-unit if inward else unit); uvs.append(uv)
            face.append(shared[key])
        out_faces.append(tuple(face))

    def centre_of(corners):
        return sum(point for point, _ in corners) / 3

    def walk(corners, level):
        flags = [keep(point) for point, _ in corners]
        if all(flags):
            emit(corners); return
        if not any(flags) and not keep(centre_of(corners)):
            return
        if level >= depth:
            if keep(centre_of(corners)):
                emit(corners)
            return
        middle = []
        for first, second in ((0, 1), (1, 2), (2, 0)):
            # на прямой стороне, не на сфере: у соседнего недроблёного треугольника та же
            # сторона прямая, иначе между ними щель
            point = ( corners[first][0] + corners[second][0] ) / 2
            middle.append((point, ((corners[first][1][0] + corners[second][1][0]) / 2, (corners[first][1][1] + corners[second][1][1]) / 2)))
        for child in ((corners[0], middle[0], middle[2]), (middle[0], corners[1], middle[1]),
                      (middle[2], middle[1], corners[2]), (middle[0], middle[1], middle[2])):
            walk(child, level + 1)

    for face in faces:
        walk([vertices[item] for item in face], 0)
    part.add(positions, normals, uvs, out_faces)


# ---------------------------------------------------------------- окантовка и детали

class Hull:
    """корпус головы: нос и подбородок (точка внутри, если внутри любого)"""
    def __init__(self, parts):
        self.parts = parts

    def value(self, point):
        return min(part.value(point) for part in self.parts)


def rim(part, canopy, nose):
    """трубка по линии пересечения стекла и носа; в системе стекла без масштаба"""
    points = []
    count = 48
    for step in range(count):
        phi = 2 * math.pi * step / count
        low, high = 0.0, math.pi          # от полюса (снаружи носа) к противоположному
        for _ in range(40):
            middle = (low + high) / 2
            unit = np.array([math.sin(middle) * math.cos(phi), math.sin(middle) * math.sin(phi), math.cos(middle)])
            if nose.value(canopy.to_head(unit)) > 1.0:
                low = middle
            else:
                high = middle
        unit = np.array([math.sin(low) * math.cos(phi), math.sin(low) * math.sin(phi), math.cos(low)])
        points.append(unit * canopy.scale)
    positions, normals, uvs, faces = [], [], [], []
    around = 8
    for index, point in enumerate(points):
        tangent = points[(index + 1) % count] - points[index - 1]
        tangent /= np.linalg.norm(tangent)
        outward = point / canopy.scale ** 2
        outward -= tangent * (outward @ tangent); outward /= np.linalg.norm(outward)
        binormal = np.cross(tangent, outward)
        for step in range(around + 1):
            angle = 2 * math.pi * step / around
            direction = math.cos(angle) * outward + math.sin(angle) * binormal
            positions.append(point + direction * RIM_RADIUS); normals.append(direction)
            uvs.append((index / count, step / around))
    for index in range(count):
        following = (index + 1) % count
        for step in range(around):
            first = index * (around + 1) + step; second = following * (around + 1) + step
            faces += [(first, second, first + 1), (first + 1, second, second + 1)]
    part.add(positions, normals, uvs, faces)


def cabin_box(parts, material, canopy, problems, name, low, high):
    for corner_x in (low[0], high[0]):
        for corner_y in (low[1], high[1]):
            for corner_z in (low[2], high[2]):
                if np.sum((np.array([corner_x, corner_y, corner_z]) / (canopy.scale * WALL_SCALE)) ** 2) >= 0.97:
                    problems.append(name)
    box(parts.setdefault(material, Part()), low, high)


def upper_cabin(canopy, problems):
    parts = {}
    # кресло у задней стенки, лицом к проёму
    cabin_box(parts, 'metal_dark', canopy, problems, 'seat back', (-0.07, -0.05, -0.105), (0.07, 0.11, -0.085))
    cabin_box(parts, 'metal_dark', canopy, problems, 'headrest', (-0.045, 0.115, -0.1), (0.045, 0.17, -0.083))
    cabin_box(parts, 'metal_dark', canopy, problems, 'seat', (-0.07, -0.075, -0.1), (0.07, -0.05, 0.0))
    for side in (-1, 1):
        cabin_box(parts, 'metal_dark', canopy, problems, 'seat side',
                  (side * 0.085 - 0.012, -0.07, -0.1), (side * 0.085 + 0.012, 0.09, -0.04))
        cabin_box(parts, 'orange', canopy, problems, 'harness',
                  (side * 0.03 - 0.008, -0.04, -0.085), (side * 0.03 + 0.008, 0.1, -0.079))
    # приборная панель у нижнего края проёма
    cabin_box(parts, 'metal_dark', canopy, problems, 'console', (-0.11, -0.14, 0.03), (0.11, -0.095, 0.075))
    for side in (-1, 1):
        cabin_box(parts, 'light_cyan', canopy, problems, 'screen',
                  (side * 0.045 - 0.032, -0.13, 0.075), (side * 0.045 + 0.032, -0.104, 0.078))
    for number, x_value in enumerate((-0.098, -0.088, 0.088, 0.098)):
        cabin_box(parts, 'indicator_red' if number % 3 == 0 else 'indicator_green', canopy, problems, 'indicator',
                  (x_value - 0.004, -0.122, 0.075), (x_value + 0.004, -0.114, 0.078))
    return parts


def lower_cabin(canopy, problems):
    parts = {}
    cabin_box(parts, 'metal_dark', canopy, problems, 'panel', (-0.085, -0.05, -0.07), (0.085, 0.05, -0.055))
    cabin_box(parts, 'light_cyan', canopy, problems, 'screen', (-0.065, -0.03, -0.055), (0.025, 0.03, -0.052))
    for number, y_value in enumerate((0.022, 0.0, -0.022)):
        cabin_box(parts, ('indicator_green', 'indicator_red', 'indicator_green')[number], canopy, problems, 'indicator',
                  (0.05, y_value - 0.006, -0.055), (0.062, y_value + 0.006, -0.052))
    return parts


# ---------------------------------------------------------------- glb

def add_mesh(document, binary, material_index, name, parts):
    primitives = []
    for material, part in parts.items():
        if not part.positions:
            continue
        primitives.append({
            'attributes': {
                'POSITION': add_accessor(document, binary, np.array(part.positions, np.float32), 'VEC3', 34962),
                'NORMAL': add_accessor(document, binary, np.array(part.normals, np.float32), 'VEC3', 34962),
                'TEXCOORD_0': add_accessor(document, binary, np.array(part.uvs, np.float32), 'VEC2', 34962)},
            'indices': add_accessor(document, binary, np.array(part.indices, np.uint16 if len(part.positions) < 65536 else np.uint32),
                                    'SCALAR', 34963),
            'material': material_index[material]})
    document['meshes'].append({'name': name, 'primitives': primitives})
    return len(document['meshes']) - 1


def dome_shift(canopy_node, nose, dome):
    """сдвиг стекла вдоль z головы, при котором колпак выступает над носом на dome"""
    def height(shift):
        translation = np.array(canopy_node['translation'], float) + np.array([0, 0, shift])
        canopy = Ellipsoid(translation, canopy_node['scale'], canopy_node['rotation'])
        low, high = 0.0, 2.0            # доля полуоси от полюса внутрь, где начинается нос
        for _ in range(40):
            middle = (low + high) / 2
            if nose.value(canopy.to_head((0, 0, 1 - middle))) > 1.0:
                low = middle
            else:
                high = middle
        return low * canopy.scale[2]
    low, high = -0.3, 0.3
    for _ in range(40):
        middle = (low + high) / 2
        if height(middle) > dome:
            high = middle
        else:
            low = middle
    return low


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = read_glb(source)
    nodes = document['nodes']
    names = [node.get('name', '') for node in nodes]
    if any(name.startswith('cockpit') for name in names):
        raise SystemExit('already has cockpit_* nodes')
    head = names.index('head')
    nose_node = nodes[names.index('nose')]
    chin_node = nodes[names.index('chin')]
    shell_nodes = [nose_node, chin_node]
    nose = Hull([Ellipsoid(node['translation'], node['scale'], node.get('rotation', (0, 0, 0, 1))) for node in shell_nodes])
    material_names = [material['name'] for material in document['materials']]
    for name, material in NEW_MATERIALS.items():
        if name not in material_names:
            document['materials'].append(dict(material, name=name))
            material_names.append(name)
    material_index = {name: index for index, name in enumerate(material_names)}

    glass_indices = sorted([index for index, name in enumerate(names) if name == 'canopy'],
                           key=lambda index: -nodes[index]['translation'][1])          # верхнее первым
    if len(glass_indices) != 2:
        raise SystemExit('expected 2 canopy nodes')
    shells = []
    for glass_index in glass_indices:
        glass = nodes[glass_index]
        found = [index for index, node in enumerate(nodes) if index != glass_index and 'mesh' in node
                 and node.get('rotation') == glass.get('rotation') and abs(node['translation'][1] - glass['translation'][1]) < 1e-6]
        if len(found) != 1:
            raise SystemExit('metal shell under the canopy not found')
        shells.append(found[0])

    problems, new_nodes, canopies = [], [], []
    for order, glass_index in enumerate(glass_indices):
        glass = nodes[glass_index]
        shift = dome_shift(glass, nose, UPPER_DOME if order == 0 else LOWER_DOME)
        glass['translation'] = [glass['translation'][0], glass['translation'][1], glass['translation'][2] + shift]
        canopy = Ellipsoid(glass['translation'], glass['scale'], glass['rotation'])
        canopies.append(canopy)
        label = 'upper' if order == 0 else 'lower'
        # стекло мелкой сеткой: только то, что над носом (с запасом)
        vertices, faces = unit_sphere(GLASS_RINGS, GLASS_SEGMENTS)
        glass_part = Part()
        carve(glass_part, vertices, faces, lambda point, canopy=canopy: nose.value(canopy.to_head(point)) > 0.97, depth=0)
        glass['mesh'] = add_mesh(document, binary, material_index, f'canopy_{label}', {'canopy_glass': glass_part})
        # стенки кабины: часть эллипсоида внутри носа, нормали внутрь
        vertices, faces = unit_sphere(WALL_RINGS, WALL_SEGMENTS)
        wall_part = Part()
        carve(wall_part, vertices, faces, lambda point, canopy=canopy: nose.value(canopy.to_head(point, WALL_SCALE)) < 1.0, inward=True)
        new_nodes.append({'name': f'cockpit_{label}_wall',
                          'mesh': add_mesh(document, binary, material_index, f'cockpit_{label}_wall', {'black': wall_part}),
                          'translation': list(glass['translation']), 'rotation': list(glass['rotation']),
                          'scale': [value * WALL_SCALE for value in glass['scale']]})
        # окантовка и детали: в системе стекла без масштаба
        parts = (upper_cabin if order == 0 else lower_cabin)(canopy, problems)
        rim(parts.setdefault('metal_gray', Part()), canopy, nose)
        new_nodes.append({'name': f'cockpit_{label}', 'mesh': add_mesh(document, binary, material_index, f'cockpit_{label}', parts),
                          'translation': list(glass['translation']), 'rotation': list(glass['rotation'])})
        print(f'{label}: glass moved by {shift:+.3f}, glass {len(glass_part.positions)}, wall {len(wall_part.positions)}, '
              f'cabin {sum(len(part.positions) for part in parts.values())} vertices')
    if problems:
        raise SystemExit('cabin parts outside the recess: ' + ', '.join(sorted(set(problems))))

    # нос и подбородок заново, с отверстиями под стёклами
    for node, shell in zip(shell_nodes, nose.parts):
        material = document['materials'][document['meshes'][node['mesh']]['primitives'][0]['material']]['name']
        vertices, faces = unit_sphere(NOSE_RINGS, NOSE_SEGMENTS)
        shell_part = Part()
        carve(shell_part, vertices, faces, lambda point, shell=shell: all(canopy.value(shell.to_head(point)) > 1.0 for canopy in canopies))
        node['mesh'] = add_mesh(document, binary, material_index, node['name'], {material: shell_part})
        print(f'{node["name"]}: {len(shell_part.positions)} vertices')

    # мелкие детали головы, вошедшие в углубления: укоротить с ближнего к стеклу конца
    # вдоль длинной оси, а если остаётся меньше трети - убрать
    removed = set(shells)
    for index, node in enumerate(nodes):
        if index in removed or 'mesh' not in node or node in shell_nodes or node.get('name', '').startswith(('canopy', 'cockpit')):
            continue
        accessor_info = document['accessors'][document['meshes'][node['mesh']]['primitives'][0]['attributes']['POSITION']]
        low = np.array(accessor_info['min']); high = np.array(accessor_info['max'])
        transform = Ellipsoid(node.get('translation', (0, 0, 0)), node.get('scale', (1, 1, 1)), node.get('rotation', (0, 0, 0, 1)))
        def intrudes(box_low, box_high):
            samples = [transform.to_head((x_value, y_value, z_value)) for x_value in np.linspace(box_low[0], box_high[0], 5)
                       for y_value in np.linspace(box_low[1], box_high[1], 5) for z_value in np.linspace(box_low[2], box_high[2], 9)]
            return any(canopy.value(point) < 1.1 for canopy in canopies for point in samples)
        if not intrudes(low, high):
            continue
        axis = int(np.argmax(high - low))
        best = None
        for keep_high in (False, True):          # какой конец остаётся
            lowest, highest = 0.0, 1.0           # доля длины, которая остаётся
            for _ in range(30):
                middle = (lowest + highest) / 2
                box_low, box_high = low.copy(), high.copy()
                if keep_high:
                    box_low[axis] = high[axis] - (high[axis] - low[axis]) * middle
                else:
                    box_high[axis] = low[axis] + (high[axis] - low[axis]) * middle
                if intrudes(box_low, box_high):
                    highest = middle
                else:
                    lowest = middle
            if best is None or lowest > best[0]:
                best = (lowest, keep_high)
        share, keep_high = best
        if share < 0.33:
            removed.add(index)
            print(f'node {index} ({node.get("name", "")}): removed, it went into the recess')
            continue
        # укоротить узлом: масштаб по оси и сдвиг, чтобы оставшийся конец не сдвинулся
        scale = list(node.get('scale', (1, 1, 1))); scale[axis] *= share; node['scale'] = scale
        fixed_end = high[axis] if keep_high else low[axis]
        local_shift = np.zeros(3); local_shift[axis] = fixed_end * (1 - share) * transform.scale[axis]
        node['translation'] = list(np.array(node.get('translation', (0, 0, 0)), float) + transform.rotation @ local_shift)
        print(f'node {index} ({node.get("name", "")}): shortened to {share:.2f} of its length')

    # серые полусферы и вошедшие в углубления детали убрать, новые узлы - детьми головы
    shells = sorted(removed)
    keep = [index for index in range(len(nodes)) if index not in shells]
    remap = {old: new for new, old in enumerate(keep)}
    kept = [nodes[index] for index in keep]
    for node in kept:
        if 'children' in node:
            node['children'] = [remap[child] for child in node['children'] if child in remap]
    for node in new_nodes:
        kept.append(node)
        kept[remap[head]].setdefault('children', []).append(len(kept) - 1)
    document['nodes'] = kept
    for scene in document['scenes']:
        scene['nodes'] = [remap[index] for index in scene['nodes']]
    for animation in document.get('animations', []):
        for channel in animation['channels']:
            if channel['target']['node'] in shells:
                raise SystemExit('a removed shell is animated')
            channel['target']['node'] = remap[channel['target']['node']]
    used = sorted({node['mesh'] for node in kept if 'mesh' in node})
    mesh_remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in kept:
        if 'mesh' in node:
            node['mesh'] = mesh_remap[node['mesh']]
    write_glb(target, document, binary)
    print(f'{target}: done')


if __name__ == '__main__':
    main()
