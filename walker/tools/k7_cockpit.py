"""Кабина K7 за тонированным стеклом (WALKER, 28.11.2026).

python3 walker/tools/k7_cockpit.py <head.glb> [out.glb]

Стекло кабины (canopy_glass) чуть прозрачное (bake_walker.py GLASS_OPACITY). Под каждым
из двух стёкол сидела серая металлическая полусфера на 0.035 глубже стекла, сквозь
тонировку виден был только серый металл. Инструмент:

  - выдвигает стёкла вперёд (верхнее на UPPER_SHIFT, нижнее на LOWER_SHIFT), а
    металлические полусферы утапливает на столько же назад, в корпус носа;
  - под каждым стеклом кладёт тёмную обивку (black) - поверхность, повторяющую корпус
    носа чуть впереди него, чтобы сквозь стекло не было видно камуфляжа;
  - в верхней кабине: кресло пилота (спинка, подголовник, боковины, оранжевые ремни) и
    перед ним у нижнего края стекла приборная панель с двумя экранами (light_cyan) и
    огоньками индикаторов (indicator_red, indicator_green);
  - в нижнем окне: панель с экраном и рядом огоньков.

Детали строятся в системе стекла (тот же сдвиг и поворот, без масштаба): x вправо,
y вверх по стеклу, z из стекла наружу. Глубина каждой детали берётся по месту: между
обивкой и стеклом; инструмент проверяет, что ни одна вершина не выходит за стекло.

Новые материалы: black, light_cyan (есть у корпуса), indicator_red, indicator_green
(текстуры рядом с моделью, классы - glow в bake_walker.py). Строки для них нужно иметь
в head_*.skin. Повторно не применяется (узлы cockpit_* уже есть).
"""
import sys, math
import numpy as np

from k7_hydraulics import read_glb, write_glb, add_accessor, Part, box

UPPER_SHIFT = 0.05          # верхнее стекло вперёд, его металлическая полусфера назад
LOWER_SHIFT = 0.04
LINING_GAP = 0.004          # обивка впереди корпуса носа
GLASS_GAP = 0.004           # и не ближе к стеклу
GRID = 16                   # сетка обивки по стороне

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


class Canopy:
    """стекло (эллипсоид с полуосями scale в своей системе) и корпус носа вокруг"""
    def __init__(self, translation, rotation, scale, nose_translation, nose_scale):
        self.translation = np.array(translation, float)
        self.rotation = quat_matrix(rotation)
        self.scale = np.array(scale, float)
        self.nose_translation = np.array(nose_translation, float)
        self.nose_scale = np.array(nose_scale, float)

    def glass_z(self, x_value, y_value):
        inside = (x_value / self.scale[0]) ** 2 + (y_value / self.scale[1]) ** 2
        return self.scale[2] * math.sqrt(max(0.0, 1.0 - inside))

    def nose_z(self, x_value, y_value):
        """наружная поверхность носа на линии (x, y) вдоль z стекла, None - мимо"""
        start = self.translation + self.rotation @ np.array([x_value, y_value, 0.0]) - self.nose_translation
        direction = self.rotation @ np.array([0.0, 0.0, 1.0])
        start = start / self.nose_scale; direction = direction / self.nose_scale
        quad_a = direction @ direction; quad_b = 2 * start @ direction; quad_c = start @ start - 1
        discriminant = quad_b * quad_b - 4 * quad_a * quad_c
        if discriminant < 0:
            return None
        return (-quad_b + math.sqrt(discriminant)) / (2 * quad_a)

    def lining_z(self, x_value, y_value):
        glass = self.glass_z(x_value, y_value) - GLASS_GAP
        nose = self.nose_z(x_value, y_value)
        return glass if nose is None else min(nose + LINING_GAP, glass)

    def inside_glass(self, point):
        return np.sum((np.array(point) / self.scale) ** 2) < 1.0


def lining(part, canopy):
    """тёмная обивка под стеклом: сетка по контуру стекла, z по корпусу носа"""
    positions, faces, index = [], [], {}
    for row in range(GRID + 1):
        for column in range(GRID + 1):
            u_value = -1 + 2 * column / GRID; v_value = -1 + 2 * row / GRID
            if u_value * u_value + v_value * v_value > 1.0:
                # точки за контуром притянуть на контур, чтобы край обивки шёл по стеклу
                length = math.hypot(u_value, v_value)
                u_value, v_value = u_value / length, v_value / length
            x_value = u_value * canopy.scale[0] * 0.995; y_value = v_value * canopy.scale[1] * 0.995
            index[(row, column)] = len(positions)
            positions.append(np.array([x_value, y_value, canopy.lining_z(x_value, y_value)]))
    for row in range(GRID):
        for column in range(GRID):
            corners = [index[(row, column)], index[(row, column + 1)], index[(row + 1, column + 1)], index[(row + 1, column)]]
            faces += [(corners[0], corners[1], corners[2]), (corners[0], corners[2], corners[3])]
    normals = [np.zeros(3) for _ in positions]
    for face in faces:
        corner = [positions[item] for item in face]
        normal = np.cross(corner[1] - corner[0], corner[2] - corner[0])
        if normal[2] < 0:
            normal = -normal
        for item in face:
            normals[item] = normals[item] + normal
    normals = [normal / (np.linalg.norm(normal) or 1) for normal in normals]
    uvs = [(0.5 + point[0] / canopy.scale[0] * 0.5, 0.5 + point[1] / canopy.scale[1] * 0.5) for point in positions]
    part.add(positions, normals, uvs, faces)


def placed_box(parts, material, canopy, centre_x, centre_y, half_x, half_y, depth, lift, problems, name):
    """коробка с центром (x, y): задняя грань на lift впереди обивки, толщина depth"""
    back = max(canopy.lining_z(centre_x + sign_x * half_x, centre_y + sign_y * half_y)
               for sign_x in (-1, 1) for sign_y in (-1, 1)) + lift
    low = (centre_x - half_x, centre_y - half_y, back)
    high = (centre_x + half_x, centre_y + half_y, back + depth)
    for corner_x in (low[0], high[0]):
        for corner_y in (low[1], high[1]):
            if not canopy.inside_glass((corner_x, corner_y, high[2] + GLASS_GAP)):
                problems.append(name)
    box(parts.setdefault(material, Part()), low, high)
    return high[2]


def upper_cockpit(canopy, problems):
    parts = {}
    lining(parts.setdefault('black', Part()), canopy)
    # кресло: спинка, подголовник, боковины, ремни
    placed_box(parts, 'metal_dark', canopy, 0.0, 0.05, 0.072, 0.075, 0.018, 0.012, problems, 'seat back')
    placed_box(parts, 'metal_dark', canopy, 0.0, 0.165, 0.048, 0.03, 0.016, 0.012, problems, 'headrest')
    for side in (-1, 1):
        placed_box(parts, 'metal_dark', canopy, side * 0.083, 0.03, 0.012, 0.07, 0.028, 0.006, problems, 'seat side')
        placed_box(parts, 'orange', canopy, side * 0.03, 0.06, 0.008, 0.06, 0.004, 0.031, problems, 'harness')
    # приборная панель у нижнего края стекла, перед креслом
    top = placed_box(parts, 'metal_dark', canopy, 0.0, -0.12, 0.13, 0.032, 0.016, 0.012, problems, 'console')
    for side in (-1, 1):
        placed_box(parts, 'light_cyan', canopy, side * 0.055, -0.115, 0.038, 0.02, 0.003, top - canopy.lining_z(side * 0.055, -0.115) - 0.001,
                   problems, 'screen')
    for number, x_value in enumerate((-0.11, -0.097, 0.097, 0.11)):
        placed_box(parts, 'indicator_red' if number % 3 == 0 else 'indicator_green', canopy, x_value, -0.135, 0.005, 0.005, 0.003,
                   top - canopy.lining_z(x_value, -0.135) - 0.001, problems, 'indicator')
    return parts


def lower_cockpit(canopy, problems):
    parts = {}
    lining(parts.setdefault('black', Part()), canopy)
    # свободное место у нижнего окна - в его нижней половине (верх уходит в нос)
    top = placed_box(parts, 'metal_dark', canopy, 0.0, -0.075, 0.085, 0.032, 0.012, 0.008, problems, 'panel')
    placed_box(parts, 'light_cyan', canopy, -0.02, -0.075, 0.045, 0.022, 0.003, top - canopy.lining_z(-0.02, -0.075) - 0.001, problems, 'screen')
    for number, y_value in enumerate((-0.057, -0.075, -0.093)):
        placed_box(parts, ('indicator_green', 'indicator_red', 'indicator_green')[number], canopy, 0.06, y_value, 0.006, 0.006, 0.003,
                   top - canopy.lining_z(0.06, y_value) - 0.001, problems, 'indicator')
    return parts


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = read_glb(source)
    nodes = document['nodes']
    names = [node.get('name', '') for node in nodes]
    if any(name.startswith('cockpit') for name in names):
        raise SystemExit('already has cockpit_* nodes')
    head = names.index('head')
    nose = nodes[names.index('nose')]
    material_names = [material['name'] for material in document['materials']]
    for name, material in NEW_MATERIALS.items():
        if name not in material_names:
            document['materials'].append(dict(material, name=name))
            material_names.append(name)
    material_index = {name: index for index, name in enumerate(material_names)}

    # стёкла и металлические полусферы под ними: одинаковый поворот, у стекла z больше
    canopies = [index for index, name in enumerate(names) if name == 'canopy']
    if len(canopies) != 2:
        raise SystemExit(f'expected 2 canopy nodes, found {len(canopies)}')
    canopies.sort(key=lambda index: -nodes[index]['translation'][1])        # верхнее первым
    problems, new_nodes = [], []
    for order, glass_index in enumerate(canopies):
        glass = nodes[glass_index]
        shells = [index for index, node in enumerate(nodes) if index != glass_index and 'mesh' in node
                  and node.get('rotation') == glass.get('rotation')
                  and abs(node['translation'][1] - glass['translation'][1]) < 1e-6]
        if len(shells) != 1:
            raise SystemExit('metal shell under the canopy not found')
        shift = UPPER_SHIFT if order == 0 else LOWER_SHIFT
        glass['translation'] = [glass['translation'][0], glass['translation'][1], glass['translation'][2] + shift]
        shell = nodes[shells[0]]
        shell['translation'] = [shell['translation'][0], shell['translation'][1], shell['translation'][2] - shift]
        canopy = Canopy(glass['translation'], glass['rotation'], glass['scale'], nose['translation'], nose['scale'])
        parts = (upper_cockpit if order == 0 else lower_cockpit)(canopy, problems)
        primitives = []
        for material, part in parts.items():
            primitives.append({
                'attributes': {
                    'POSITION': add_accessor(document, binary, np.array(part.positions, np.float32), 'VEC3', 34962),
                    'NORMAL': add_accessor(document, binary, np.array(part.normals, np.float32), 'VEC3', 34962),
                    'TEXCOORD_0': add_accessor(document, binary, np.array(part.uvs, np.float32), 'VEC2', 34962)},
                'indices': add_accessor(document, binary, np.array(part.indices, np.uint16), 'SCALAR', 34963),
                'material': material_index[material]})
        document['meshes'].append({'name': f'cockpit_{"upper" if order == 0 else "lower"}', 'primitives': primitives})
        new_nodes.append({'name': f'cockpit_{"upper" if order == 0 else "lower"}', 'mesh': len(document['meshes']) - 1,
                          'translation': list(glass['translation']), 'rotation': list(glass['rotation'])})
        print(f'{new_nodes[-1]["name"]}: {sum(len(part.positions) for part in parts.values())} vertices, '
              f'materials {sorted(parts)}')
    if problems:
        raise SystemExit('parts outside the glass: ' + ', '.join(sorted(set(problems))))
    for node in new_nodes:
        nodes.append(node)
        nodes[head].setdefault('children', []).append(len(nodes) - 1)
    write_glb(target, document, binary)
    print(f'{target}: canopies moved out, shells in, 2 cockpits added')


if __name__ == '__main__':
    main()
