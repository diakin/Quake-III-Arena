"""Гидроцилиндры голеностопа K7 (WALKER, 27.11.2026).

python3 walker/tools/k7_hydraulics.py <lower.glb> [out.glb]

Раньше за голенью K7 висели два хромированных стержня с оранжевым кольцом и тёмной
заглушкой: закреплены только на голени, концы ни к чему не присоединены. Инструмент
убирает их и ставит на каждую ногу по два гидроцилиндра привода ступни, сзади по углам
голени (поперёк +-ROD_X):

  - верхняя проушина на вилке на конце рычага, который сидит на торце оси колена
    снаружи оранжевых колец и жёстко связан с голенью;
  - нижняя проушина на вилке на краю ступни у пятки;
  - корпус цилиндра (крышка, труба, оранжевая букса на выходе штока) висит на верхнем
    пальце и в каждом кадре смотрит на нижний;
  - хромированный шток висит на нижнем пальце и смотрит на верхний, уходя в корпус.

Голень и ступня поворачиваются только вокруг поперечной оси X (анимации), поэтому корпус
и шток - узлы с поворотом вокруг X: в каждом ключе анимации (ключи стоят на кадрах,
15 кадров в секунду) угол считается по положению пальцев, при сгибе голеностопа шток
выдвигается из корпуса и уходит в него. Длины корпуса и штока подобраны по всем
анимациям: шток всегда в корпусе не меньше чем на ROD_OVERLAP_MIN и не упирается в
крышку.

Почему по углам, а не в середине за голенью, как старые стержни: низ корпуса бедра
(коробка шириной +-0.17) закрывает колено целиком и уходит на 0.25 назад от оси; при
сгибе колена (присед, бег) он проходит через место за голенью у колена - цилиндр там
пересекался бы с бедром в любой позе с приседом. Снаружи ширины бедра и его боковой
пластины (до 0.245) место свободно во всех анимациях (проверено по выпуклым оболочкам
деталей бедра во всех кадрах).

Повторно не применяется (узлы hydraulic_* уже есть). Материалы - те же, что у остальных
деталей: запекание (bake_walker.py) обрабатывает новые детали как все остальные, шток -
как хром гидравлики (блестит, с него течёт масло).
"""
import sys, json, struct, math
import numpy as np

COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

ROD_X = (0.30, -0.30)               # два цилиндра на ногу, поперёк: снаружи бедра (+-0.245)
KNEE_BOSS_X = 0.256                 # рычаг верхней вилки: от торца оси колена (кольца до 0.24)
UPPER_PIN = (-0.15, -0.29)          # (y, z) в пространстве колена: сзади голени у колена
LOWER_PIN = (-0.16, -0.33)          # (y, z) в пространстве голеностопа: над пяткой
BARREL_LENGTH = 0.52                # от верхнего пальца до конца буксы
ROD_TOP_GAP = 0.09                  # при самом коротком цилиндре верх штока ниже пальца на столько
ROD_OVERLAP_MIN = 0.15              # при самом длинном шток в корпусе хотя бы на столько
SEGMENTS = 10                       # грани цилиндров: каждая вершина хранится во всех 184 кадрах


# ---------------------------------------------------------------- glb

def read_glb(path):
    data = open(path, 'rb').read()
    json_length = struct.unpack('<I', data[12:16])[0]
    document = json.loads(data[20:20 + json_length])
    offset = 20 + json_length
    binary_length = struct.unpack('<I', data[offset:offset + 4])[0]
    return document, bytearray(data[offset + 8:offset + 8 + binary_length])


def accessor(document, binary, index):
    info = document['accessors'][index]; view = document['bufferViews'][info['bufferView']]
    dtype = np.dtype(COMPONENT[info['componentType']]); width = WIDTH[info['type']]
    start = view.get('byteOffset', 0) + info.get('byteOffset', 0)
    stride = view.get('byteStride', 0) or dtype.itemsize * width
    count = info['count']
    raw = np.frombuffer(bytes(binary), dtype=np.uint8, count=stride * (count - 1) + dtype.itemsize * width, offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(count, dtype.itemsize * width), strides=(stride, 1))
    values = np.ascontiguousarray(rows).view(dtype)
    return values.reshape(count, width) if width > 1 else values.reshape(count)


def add_accessor(document, binary, values, kind, target=None):
    values = np.ascontiguousarray(values)
    while len(binary) % 4:
        binary.append(0)
    offset = len(binary)
    binary.extend(values.tobytes())
    view = {'buffer': 0, 'byteOffset': offset, 'byteLength': values.nbytes}
    if target:
        view['target'] = target
    document['bufferViews'].append(view)
    component = {np.dtype(np.float32): 5126, np.dtype(np.uint16): 5123, np.dtype(np.uint32): 5125}[values.dtype]
    info = {'bufferView': len(document['bufferViews']) - 1, 'componentType': component,
            'count': len(values), 'type': kind}
    if kind == 'VEC3' and target == 34962:
        info['min'] = values.min(axis=0).tolist(); info['max'] = values.max(axis=0).tolist()
    if kind == 'SCALAR' and values.dtype == np.float32:
        info['min'] = [float(values.min())]; info['max'] = [float(values.max())]
    document['accessors'].append(info)
    return len(document['accessors']) - 1


def write_glb(path, document, binary):
    while len(binary) % 4:
        binary.append(0)
    document['buffers'] = [{'byteLength': len(binary)}]
    text = json.dumps(document, separators=(',', ':')).encode('utf-8')
    text += b' ' * ((4 - len(text) % 4) % 4)
    with open(path, 'wb') as file:
        file.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        file.write(struct.pack('<II', len(text), 0x4E4F534A)); file.write(text)
        file.write(struct.pack('<II', len(binary), 0x004E4942)); file.write(bytes(binary))


# ---------------------------------------------------------------- geometry

class Part:
    """вершины одного материала: положение, нормаль, uv (0..1 на деталь, как у остальных)"""
    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []

    def add(self, positions, normals, uvs, faces):
        # грань наружу (против часовой стрелки снаружи, как в glTF): по нормалям её вершин
        oriented = []
        for face in faces:
            corners = [np.array(positions[item], float) for item in face]
            geometric = np.cross(corners[1] - corners[0], corners[2] - corners[0])
            outward = sum(np.array(normals[item], float) for item in face)
            oriented.append(face if np.dot(geometric, outward) >= 0 else (face[0], face[2], face[1]))
        faces = oriented
        base = len(self.positions)
        self.positions += [tuple(map(float, item)) for item in positions]
        self.normals += [tuple(map(float, item)) for item in normals]
        self.uvs += [tuple(map(float, item)) for item in uvs]
        self.indices += [base + int(item) for face in faces for item in face]


def cylinder(part, axis, centre, radius, start, end, caps=(True, True)):
    """цилиндр вдоль оси 'x' или 'y' от start до end (по оси, от centre); caps - крышки на
    концах start и end (спрятанные в соседних деталях не нужны)"""
    centre = np.array(centre, float)
    def point(along, angle, distance):
        if axis == 'y':
            return centre + np.array([math.cos(angle) * distance, along, math.sin(angle) * distance])
        return centre + np.array([along, math.cos(angle) * distance, math.sin(angle) * distance])
    def direction(angle):
        if axis == 'y':
            return np.array([math.cos(angle), 0, math.sin(angle)])
        return np.array([0, math.cos(angle), math.sin(angle)])
    axis_vector = np.array([0, 1, 0]) if axis == 'y' else np.array([1, 0, 0])
    positions, normals, uvs, faces = [], [], [], []
    for step in range(SEGMENTS + 1):
        angle = 2 * math.pi * step / SEGMENTS
        for along, v_value in ((start, 0.0), (end, 1.0)):
            positions.append(point(along, angle, radius)); normals.append(direction(angle)); uvs.append((step / SEGMENTS, v_value))
    for step in range(SEGMENTS):
        first = step * 2
        faces += [(first, first + 2, first + 1), (first + 1, first + 2, first + 3)]
    for along, sign, wanted in ((start, -1.0 if end > start else 1.0, caps[0]), (end, 1.0 if end > start else -1.0, caps[1])):
        if not wanted:
            continue
        base = len(positions)
        positions.append(point(along, 0, 0)); normals.append(axis_vector * sign); uvs.append((0.5, 0.5))
        for step in range(SEGMENTS):
            angle = 2 * math.pi * step / SEGMENTS
            positions.append(point(along, angle, radius)); normals.append(axis_vector * sign)
            uvs.append((0.5 + 0.5 * math.cos(angle), 0.5 + 0.5 * math.sin(angle)))
        for step in range(SEGMENTS):
            following = base + 1 + (step + 1) % SEGMENTS
            faces.append((base, base + 1 + step, following))
    part.add(positions, normals, uvs, faces)


def box(part, low, high):
    low = np.array(low, float); high = np.array(high, float)
    positions, normals, uvs, faces = [], [], [], []
    for axis in range(3):
        for sign in (-1, 1):
            normal = np.zeros(3); normal[axis] = sign
            others = [item for item in range(3) if item != axis]
            base = len(positions)
            for corner_u, corner_v in ((0, 0), (1, 0), (1, 1), (0, 1)):
                point = np.where(np.arange(3) == axis, high if sign > 0 else low, 0.0)
                point[others[0]] = (low, high)[corner_u][others[0]]
                point[others[1]] = (low, high)[corner_v][others[1]]
                positions.append(point); normals.append(normal); uvs.append((corner_u, corner_v))
            faces += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    part.add(positions, normals, uvs, faces)


# ---------------------------------------------------------------- the actuators

def quat_x(angle):
    return [math.sin(angle / 2), 0.0, 0.0, math.cos(angle / 2)]


def rotate_x(quaternion, vector):
    """поворот вектора кватернионом (только вокруг X - другие у голеностопа не встречаются)"""
    qx, qy, qz, qw = quaternion
    if abs(qy) > 1e-5 or abs(qz) > 1e-5:
        raise SystemExit('ankle rotation not about X: actuators would not stay in their plane')
    angle = 2 * math.atan2(qx, qw)
    y_value, z_value = vector
    return (y_value * math.cos(angle) - z_value * math.sin(angle), y_value * math.sin(angle) + z_value * math.cos(angle))


def pins(ankle_rotation, ankle_translation):
    """верхний палец в пространстве колена и голеностопа, нижний - в обоих; (y, z)"""
    lower_in_knee = rotate_x(ankle_rotation, LOWER_PIN)
    lower_in_knee = (lower_in_knee[0] + ankle_translation[1], lower_in_knee[1] + ankle_translation[2])
    relative = (UPPER_PIN[0] - ankle_translation[1], UPPER_PIN[1] - ankle_translation[2])
    inverse = [-ankle_rotation[0], 0.0, 0.0, ankle_rotation[3]]
    upper_in_ankle = rotate_x(inverse, relative)
    return lower_in_knee, upper_in_ankle


def angles(ankle_rotation, ankle_translation):
    """поворот корпуса (ось -Y на нижний палец) и штока (ось +Y на верхний) и длина"""
    lower_in_knee, upper_in_ankle = pins(ankle_rotation, ankle_translation)
    down = (lower_in_knee[0] - UPPER_PIN[0], lower_in_knee[1] - UPPER_PIN[1])
    up = (upper_in_ankle[0] - LOWER_PIN[0], upper_in_ankle[1] - LOWER_PIN[1])
    barrel = math.atan2(-down[1], -down[0])        # R_x(a)(0,-1,0) = (0, -cos a, -sin a)
    rod = math.atan2(up[1], up[0])                 # R_x(a)(0,1,0) = (0, cos a, sin a)
    return barrel, rod, math.hypot(*down)


def oriented_box(part, start, end, half_width, x_low, x_high):
    """плоская пластина в плоскости YZ от точки start до end (y, z), ширина 2*half_width,
    по X от x_low до x_high (концы скруглены не будут - под ними бобышки)"""
    start = np.array(start, float); end = np.array(end, float)
    along = end - start; along /= np.linalg.norm(along)
    across = np.array([-along[1], along[0]])
    corners2 = [start - across * half_width, end - across * half_width, end + across * half_width, start + across * half_width]
    positions, normals, uvs, faces = [], [], [], []
    def vertex(x_value, corner):
        return np.array([x_value, corner[0], corner[1]])
    for x_value, normal_x in ((x_low, -1.0), (x_high, 1.0)):
        base = len(positions)
        for index, corner in enumerate(corners2):
            positions.append(vertex(x_value, corner)); normals.append((normal_x, 0, 0)); uvs.append((index in (1, 2), index in (2, 3)))
        faces += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    for index in range(4):
        first, second = corners2[index], corners2[(index + 1) % 4]
        edge = second - first
        normal2 = np.array([edge[1], -edge[0]]); normal2 /= np.linalg.norm(normal2)
        centre = sum(corners2) / 4
        if np.dot(normal2, (first + second) / 2 - centre) < 0:
            normal2 = -normal2
        base = len(positions)
        for corner, x_value in ((first, x_low), (second, x_low), (second, x_high), (first, x_high)):
            positions.append(vertex(x_value, corner)); normals.append((0, normal2[0], normal2[1])); uvs.append((0, 0))
        faces += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    part.add(positions, normals, uvs, faces)


def build_parts(rod_length, outward):
    """детали одного цилиндра в четырёх узлах: корпус (на верхнем пальце, ось -Y), шток (на
    нижнем пальце, ось +Y), верхняя и нижняя вилки (неподвижны). outward: +1 - цилиндр на +X
    от середины ноги, -1 - на -X. {узел: {материал: Part}}"""
    parts = {name: {} for name in ('barrel', 'rod', 'upper_mount', 'lower_mount')}
    def get(node, material):
        return parts[node].setdefault(material, Part())
    # корпус: проушина, шейка, крышка, труба, букса
    cylinder(get('barrel', 'metal_dark'), 'x', (0, 0, 0), 0.058, -0.03, 0.03)
    box(get('barrel', 'metal_dark'), (-0.026, -0.105, -0.036), (0.026, -0.035, 0.036))
    cylinder(get('barrel', 'metal_dark'), 'y', (0, 0, 0), 0.066, -0.095, -0.15)
    cylinder(get('barrel', 'metal_gray'), 'y', (0, 0, 0), 0.058, -0.15, -(BARREL_LENGTH - 0.06), (False, False))
    cylinder(get('barrel', 'orange'), 'y', (0, 0, 0), 0.064, -(BARREL_LENGTH - 0.06), -BARREL_LENGTH)
    # штуцер шланга на крышке
    cylinder(get('barrel', 'metal_dark'), 'x', (0, -0.125, 0.0), 0.018, 0.06, 0.088, (False, True))
    # шток: проушина, наконечник, хромированный шток
    cylinder(get('rod', 'metal_dark'), 'x', (0, 0, 0), 0.048, -0.026, 0.026)
    cylinder(get('rod', 'metal_dark'), 'y', (0, 0, 0), 0.04, 0.035, 0.105, (False, True))
    cylinder(get('rod', 'chrome'), 'y', (0, 0, 0), 0.03, 0.105, rod_length, (False, False))
    # верхняя вилка: две щеки вокруг пальца, палец; внутренняя щека продолжается рычагом до
    # оси колена (бобышка на торце оси) - всё в координатах от пальца
    for sign in (-1, 1):
        box(get('upper_mount', 'metal_dark'), (0.034 if sign > 0 else -0.048, -0.06, -0.06),
            (0.048 if sign > 0 else -0.034, 0.06, 0.06))
    cylinder(get('upper_mount', 'metal_gray'), 'x', (0, 0, 0), 0.018, -0.058, 0.058)
    pivot = (-UPPER_PIN[0], -UPPER_PIN[1])            # ось колена от пальца
    inner_low, inner_high = sorted((-outward * 0.034, -outward * 0.048))
    oriented_box(get('upper_mount', 'metal_dark'), (0, 0), pivot, 0.045, inner_low, inner_high)
    # бобышка от 0.244 (не на одной грани с кольцом 0.24) до внутренней щеки
    boss_low, boss_high = sorted((outward * (KNEE_BOSS_X - abs(ROD_X[0])) - outward * 0.012, -outward * 0.034))
    cylinder(get('upper_mount', 'orange'), 'x', (0, pivot[0], pivot[1]), 0.07, boss_low, boss_high)
    # нижняя вилка: подошва на краю ступни (внутрь к середине ступни), две щеки, палец
    base_low, base_high = sorted((-outward * 0.11, outward * 0.05))
    box(get('lower_mount', 'metal_dark'), (base_low, -0.09, -0.065), (base_high, -0.068, 0.065))
    for sign in (-1, 1):
        box(get('lower_mount', 'metal_dark'), (0.030 if sign > 0 else -0.043, -0.075, -0.05),
            (0.043 if sign > 0 else -0.030, 0.05, 0.05))
    cylinder(get('lower_mount', 'metal_gray'), 'x', (0, 0, 0), 0.016, -0.05, 0.05)
    return parts


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = read_glb(source)
    names = [node.get('name', '') for node in document['nodes']]
    if any(name.startswith('hydraulic') for name in names):
        raise SystemExit('already has hydraulic_* nodes')
    material_index = {material['name']: index for index, material in enumerate(document['materials'])}
    knees = {'L': names.index('knee_L'), 'R': names.index('knee_R')}
    ankles = {'L': names.index('ankle_L'), 'R': names.index('ankle_R')}

    # длины по всем анимациям
    lengths = []
    for animation in document['animations']:
        channels = {(channel['target']['node'], channel['target']['path']): channel for channel in animation['channels']}
        for side in 'LR':
            rotation = accessor(document, binary, animation['samplers'][channels[(ankles[side], 'rotation')]['sampler']]['output'])
            translation = accessor(document, binary, animation['samplers'][channels[(ankles[side], 'translation')]['sampler']]['output'])
            for key in range(len(rotation)):
                lengths.append(angles(rotation[key], translation[min(key, len(translation) - 1)])[2])
    rest = document['nodes'][ankles['L']]
    lengths.append(angles(rest.get('rotation', [0, 0, 0, 1]), rest.get('translation', [0, 0, 0]))[2])
    shortest, longest = min(lengths), max(lengths)
    rod_length = shortest - ROD_TOP_GAP
    overlap = rod_length - (longest - BARREL_LENGTH)
    print(f'pin distance {shortest:.3f} .. {longest:.3f}, barrel {BARREL_LENGTH}, rod {rod_length:.3f}, '
          f'rod in barrel at the longest {overlap:.3f}')
    if overlap < ROD_OVERLAP_MIN:
        raise SystemExit('stroke too long for the barrel')

    # старые штоки: дети колена сзади голени (хром, оранжевое кольцо, заглушка)
    removed = set()
    for side in 'LR':
        for child in document['nodes'][knees[side]]['children']:
            node = document['nodes'][child]
            if 'mesh' not in node or 'translation' not in node:
                continue
            x_value, y_value, z_value = node['translation']
            if abs(abs(x_value) - 0.1) < 1e-3 and abs(z_value + 0.22) < 1e-3:
                removed.add(child)
    if len(removed) != 12:
        raise SystemExit(f'expected 12 old rod nodes, found {len(removed)}')

    # новые узлы и сетки
    new_nodes = []          # (родитель-сустав, узел)
    animated = []           # (сторона, индекс в new_nodes, 'barrel'|'rod')
    for side in 'LR':
        for x_value in ROD_X:
            parts = build_parts(rod_length, 1 if x_value > 0 else -1)
            for part_name, parent, origin in (('upper_mount', knees[side], UPPER_PIN), ('barrel', knees[side], UPPER_PIN),
                                              ('lower_mount', ankles[side], LOWER_PIN), ('rod', ankles[side], LOWER_PIN)):
                primitives = []
                for material, part in parts[part_name].items():
                    primitives.append({
                        'attributes': {
                            'POSITION': add_accessor(document, binary, np.array(part.positions, np.float32), 'VEC3', 34962),
                            'NORMAL': add_accessor(document, binary, np.array(part.normals, np.float32), 'VEC3', 34962),
                            'TEXCOORD_0': add_accessor(document, binary, np.array(part.uvs, np.float32), 'VEC2', 34962)},
                        'indices': add_accessor(document, binary, np.array(part.indices, np.uint16), 'SCALAR', 34963),
                        'material': material_index[material]})
                document['meshes'].append({'name': f'hydraulic_{part_name}', 'primitives': primitives})
                node = {'name': f'hydraulic_{part_name}_{side}{"o" if x_value * (1 if side == "L" else -1) > 0 else "i"}',
                        'mesh': len(document['meshes']) - 1, 'translation': [x_value, origin[0], origin[1]]}
                new_nodes.append((parent, node))
                if part_name in ('barrel', 'rod'):
                    animated.append((side, len(new_nodes) - 1, part_name))

    # поза покоя
    for side, position, part_name in animated:
        rest = document['nodes'][ankles[side]]
        barrel, rod, _ = angles(rest.get('rotation', [0, 0, 0, 1]), rest.get('translation', [0, 0, 0]))
        new_nodes[position][1]['rotation'] = quat_x(barrel if part_name == 'barrel' else rod)

    # убрать старые штоки и вставить новые узлы, перенумеровать
    old_count = len(document['nodes'])
    keep = [index for index in range(old_count) if index not in removed]
    remap = {old: new for new, old in enumerate(keep)}
    nodes = [document['nodes'][index] for index in keep]
    first_new = len(nodes)
    for offset, (parent, node) in enumerate(new_nodes):
        nodes.append(node)
    for node in nodes[:first_new]:
        if 'children' in node:
            node['children'] = [remap[child] for child in node['children'] if child in remap]
    for offset, (parent, node) in enumerate(new_nodes):
        nodes[remap[parent]].setdefault('children', []).append(first_new + offset)
    document['nodes'] = nodes
    for scene in document['scenes']:
        scene['nodes'] = [remap[index] for index in scene['nodes']]
    for animation in document['animations']:
        for channel in animation['channels']:
            channel['target']['node'] = remap[channel['target']['node']]
    # сетки старых штоков больше никому не нужны
    used = sorted({node['mesh'] for node in nodes if 'mesh' in node})
    mesh_remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in nodes:
        if 'mesh' in node:
            node['mesh'] = mesh_remap[node['mesh']]

    # анимация корпуса и штока: в каждом ключе голеностопа
    ankle_new = {side: remap[ankles[side]] for side in 'LR'}
    for animation in document['animations']:
        channels = {(channel['target']['node'], channel['target']['path']): channel for channel in animation['channels']}
        for side, position, part_name in animated:
            rotation_sampler = animation['samplers'][channels[(ankle_new[side], 'rotation')]['sampler']]
            translation_sampler = animation['samplers'][channels[(ankle_new[side], 'translation')]['sampler']]
            rotation = accessor(document, binary, rotation_sampler['output'])
            translation = accessor(document, binary, translation_sampler['output'])
            values = []
            for key in range(len(rotation)):
                barrel, rod, _ = angles(rotation[key], translation[min(key, len(translation) - 1)])
                values.append(quat_x(barrel if part_name == 'barrel' else rod))
            output = add_accessor(document, binary, np.array(values, np.float32), 'VEC4')
            animation['samplers'].append({'input': rotation_sampler['input'], 'output': output,
                                          'interpolation': rotation_sampler.get('interpolation', 'LINEAR')})
            animation['channels'].append({'sampler': len(animation['samplers']) - 1,
                                          'target': {'node': first_new + position, 'path': 'rotation'}})
    write_glb(target, document, binary)
    print(f'{target}: {len(removed)} old rod nodes removed, {len(new_nodes)} actuator nodes added')


if __name__ == '__main__':
    main()
