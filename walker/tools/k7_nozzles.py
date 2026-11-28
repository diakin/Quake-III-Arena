"""Круглые реактивные дюзы K7 вместо решёток (WALKER, 28.11.2026).

python3 walker/tools/k7_nozzles.py <upper.glb> [out.glb]

Сзади на коробе двигателя K7 стояли две тёмные плиты, на каждой по четыре светящиеся
горизонтальные планки (engine_vent), а вокруг - ограждение из пяти алюминиевых трубок,
которое закрывало их сзади. Вместе это было похоже на решётку кондиционера.

Инструмент убирает плиты, планки и ограждение и ставит на их место две круглые дюзы
(узлы nozzle_L, nozzle_R) - тела вращения вокруг оси вдоль корпуса (-z назад):

  - фланец на задней стенке короба (тёмный металл);
  - сужающаяся часть и раструб, расходящийся к срезу (тёмный металл);
  - кольцо по срезу (серый металл);
  - внутренняя стенка раструба и дно у критического сечения (engine_glow): светятся
    и мерцают как раскалённый металл (класс ember в make_material_pass.py).

Материалы - те же, что у остальных деталей, запекание (bake_walker.py) обрабатывает
новые детали как все остальные. Повторно не применяется (узлы nozzle_* уже есть).
"""
import sys, math
import numpy as np

from k7_hydraulics import read_glb, write_glb, add_accessor, Part

NOZZLE_X = (-0.36, 0.36)        # оси дюз поперёк, там же, где были плиты
NOZZLE_Y = 0.05                 # и по высоте
SEGMENTS = 20                   # грани по кругу: каждая вершина хранится во всех кадрах

# профиль (радиус, z) от задней стенки короба (z -1.54) назад
OUTER = [(0.290, -1.540), (0.290, -1.575),      # фланец
         (0.245, -1.575),                       # торец фланца
         (0.215, -1.660),                       # сужение
         (0.258, -1.800)]                       # раструб до среза
LIP = [(0.258, -1.800), (0.268, -1.808), (0.268, -1.830), (0.226, -1.830)]   # кольцо по срезу
INNER = [(0.226, -1.830), (0.196, -1.720), (0.165, -1.630)]                  # внутренняя стенка
THROAT_Z = -1.630                                                            # дно


def lathe(part, centre, profile, inward=False):
    """поверхность вращения вокруг оси z через centre (x, y): у каждого отрезка профиля
    свой пояс с плоской по профилю нормалью (грани чёткие, как у точёной детали)"""
    positions, normals, uvs, faces = [], [], [], []
    total = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(profile, profile[1:])) or 1.0
    walked = 0.0
    for (radius_a, z_a), (radius_b, z_b) in zip(profile, profile[1:]):
        length = math.hypot(radius_b - radius_a, z_b - z_a)
        # нормаль отрезка в плоскости (радиус, z): поворот направления на 90 градусов
        normal_radial, normal_z = (z_b - z_a) / length, -(radius_b - radius_a) / length
        if (normal_radial < 0) != inward and abs(normal_radial) > 1e-6:
            normal_radial, normal_z = -normal_radial, -normal_z
        if abs(normal_radial) <= 1e-6:      # торец: смотрит назад (-z) или вперёд
            normal_z = -1.0 if not inward else 1.0
        base = len(positions)
        for step in range(SEGMENTS + 1):
            angle = 2 * math.pi * step / SEGMENTS
            cosine, sine = math.cos(angle), math.sin(angle)
            for radius, z_value, v_value in ((radius_a, z_a, walked / total), (radius_b, z_b, (walked + length) / total)):
                positions.append((centre[0] + cosine * radius, centre[1] + sine * radius, z_value))
                normals.append((cosine * normal_radial, sine * normal_radial, normal_z))
                uvs.append((step / SEGMENTS, v_value))
        for step in range(SEGMENTS):
            first = base + step * 2
            faces += [(first, first + 2, first + 1), (first + 1, first + 2, first + 3)]
        walked += length
    part.add(positions, normals, uvs, faces)


def disc(part, centre, radius, z_value, normal_z):
    positions = [(centre[0], centre[1], z_value)]; normals = [(0, 0, normal_z)]; uvs = [(0.5, 0.5)]
    faces = []
    for step in range(SEGMENTS):
        angle = 2 * math.pi * step / SEGMENTS
        positions.append((centre[0] + math.cos(angle) * radius, centre[1] + math.sin(angle) * radius, z_value))
        normals.append((0, 0, normal_z)); uvs.append((0.5 + 0.5 * math.cos(angle), 0.5 + 0.5 * math.sin(angle)))
    for step in range(SEGMENTS):
        faces.append((0, 1 + step, 1 + (step + 1) % SEGMENTS))
    part.add(positions, normals, uvs, faces)


def build_nozzle():
    """детали одной дюзы по материалам, ось в начале координат узла"""
    parts = {'metal_dark': Part(), 'metal_gray': Part(), 'engine_glow': Part()}
    lathe(parts['metal_dark'], (0, 0), OUTER)
    lathe(parts['metal_gray'], (0, 0), LIP)
    lathe(parts['engine_glow'], (0, 0), INNER, inward=True)
    disc(parts['engine_glow'], (0, 0), INNER[-1][0], THROAT_Z, -1.0)
    return parts


def is_removed(node):
    """плиты под планками, планки и трубки ограждения сзади короба двигателя"""
    if node.get('name') == 'engine_vent':
        return True
    if 'mesh' not in node:
        return False
    x_value, y_value, z_value = node.get('translation', [0, 0, 0])
    if abs(abs(x_value) - 0.36) < 1e-3 and abs(y_value - 0.05) < 1e-3 and abs(z_value + 1.55) < 1e-3:
        return True                 # тёмная плита под планками
    return False


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = read_glb(source)
    names = [node.get('name', '') for node in document['nodes']]
    if any(name.startswith('nozzle') for name in names):
        raise SystemExit('already has nozzle_* nodes')
    material_index = {material['name']: index for index, material in enumerate(document['materials'])}
    body = names.index('upper_body')

    removed = {index for index, node in enumerate(document['nodes']) if is_removed(node)}
    # ограждение: серые трубки без смещения узла, вершины целиком за коробом (z < -0.8)
    for index, node in enumerate(document['nodes']):
        offset = node.get('translation', [0, 0, 0])
        if 'mesh' not in node or any(abs(value) > 1e-6 for value in offset) or node.get('rotation'):
            continue
        primitives = document['meshes'][node['mesh']]['primitives']
        if any(document['materials'][primitive.get('material', 0)]['name'] != 'metal_gray' for primitive in primitives):
            continue
        if all(document['accessors'][primitive['attributes']['POSITION']]['max'][2] < -0.8 for primitive in primitives):
            removed.add(index)
    vents = sum(1 for index in removed if document['nodes'][index].get('name') == 'engine_vent')
    if vents != 8 or len(removed) != 15:
        raise SystemExit(f'expected 8 vents, 2 plates and 5 railing tubes, found {len(removed)} nodes ({vents} vents)')
    for animation in document.get('animations', []):
        for channel in animation['channels']:
            if channel['target']['node'] in removed:
                raise SystemExit('a removed node is animated')

    parts = build_nozzle()
    primitives = []
    for material, part in parts.items():
        primitives.append({
            'attributes': {
                'POSITION': add_accessor(document, binary, np.array(part.positions, np.float32), 'VEC3', 34962),
                'NORMAL': add_accessor(document, binary, np.array(part.normals, np.float32), 'VEC3', 34962),
                'TEXCOORD_0': add_accessor(document, binary, np.array(part.uvs, np.float32), 'VEC2', 34962)},
            'indices': add_accessor(document, binary, np.array(part.indices, np.uint16), 'SCALAR', 34963),
            'material': material_index[material]})
    document['meshes'].append({'name': 'nozzle', 'primitives': primitives})
    nozzle_mesh = len(document['meshes']) - 1
    new_nodes = [{'name': f'nozzle_{"L" if x_value < 0 else "R"}', 'mesh': nozzle_mesh,
                  'translation': [x_value, NOZZLE_Y, 0.0]} for x_value in NOZZLE_X]

    # убрать узлы, добавить дюзы детьми корпуса, перенумеровать
    keep = [index for index in range(len(document['nodes'])) if index not in removed]
    remap = {old: new for new, old in enumerate(keep)}
    nodes = [document['nodes'][index] for index in keep]
    for node in nodes:
        if 'children' in node:
            node['children'] = [remap[child] for child in node['children'] if child in remap]
    first_new = len(nodes)
    nodes += new_nodes
    nodes[remap[body]]['children'] += [first_new, first_new + 1]
    document['nodes'] = nodes
    for scene in document['scenes']:
        scene['nodes'] = [remap[index] for index in scene['nodes']]
    for animation in document.get('animations', []):
        for channel in animation['channels']:
            channel['target']['node'] = remap[channel['target']['node']]
    used = sorted({node['mesh'] for node in nodes if 'mesh' in node})
    mesh_remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in nodes:
        if 'mesh' in node:
            node['mesh'] = mesh_remap[node['mesh']]
    write_glb(target, document, binary)
    vertices = sum(len(part.positions) for part in parts.values())
    print(f'{target}: {len(removed)} nodes removed (8 vents, 2 plates, 5 railing tubes), '
          f'2 nozzles of {vertices} vertices added')


if __name__ == '__main__':
    main()
