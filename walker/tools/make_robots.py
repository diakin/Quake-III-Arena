#!/usr/bin/env python3
"""
Программные модели WALKER: роботы STRIZH (лёгкий разведчик) и MOLOT (тяжёлый штурмовик),
шесть видов оружия для слотов роботов.

python3 walker/tools/make_robots.py <папка базовых данных>

Роботы: в <папке>/models/players/<робот>/ пишутся lower.glb, upper.glb, head.glb (жёсткие
узлы с анимацией; движок переводит их в MD3, renderer/tr_model_glb.c), animation.cfg (кадры
как у K7), текстуры материалов окрасок default/red/blue, файлы .skin и значки. Единицы как у
K7 (1 единица glTF = 14 единиц Quake, asset.extras.q3_scale 14), поэтому потом их можно
запечь тем же bake_walker.py (тени в углах, потёртости, грязь) и упростить make_lods.py.

Слоты оружия: теги tag_weapon1.. в upper.glb (у STRIZH: 1 — средний справа, 2 — лёгкий
слева; у MOLOT: 1 — тяжёлый на правом плече). Оружие: models/robotweapons/<id>.glb, начало
координат — точка крепления, ствол по +X, тег tag_flash — дульный срез; шейдеры материалов
оружия и роботов в scripts/walker_robots.shader.

Модель строится в координатах Quake (X вперёд, Y влево, Z вверх, начало — точка игрока,
земля на Z = -24) и переводится в glTF (+X влево, +Y вверх, +Z вперёд) при записи.

Ноги — две кости с обратной кинематикой: ступня в опоре идёт назад ровно с той скоростью,
с какой движется робот (темп ног в cgame = скорость / cg_strideSpeed 110), в переносе —
поднимается и выносится вперёд.
"""
import json, math, os, struct, sys, zlib
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.spatial import ConvexHull

OUT = sys.argv[1]
GROUND = -24.0
STRIDE_SPEED = 110.0          # cg_strideSpeed: при этой скорости анимация идёт в своём темпе
SCHEMES = ('default', 'red', 'blue')
ROBOT_SCALE = 14.0            # единиц Quake в единице glTF у роботов (как у K7)

DEATHS = [('BOTH_DEATH1', 15), ('BOTH_DEAD1', 1), ('BOTH_DEATH2', 15), ('BOTH_DEAD2', 1),
          ('BOTH_DEATH3', 15), ('BOTH_DEAD3', 1)]
TORSO = [('TORSO_GESTURE', 20), ('TORSO_ATTACK', 6), ('TORSO_ATTACK2', 6), ('TORSO_DROP', 5),
         ('TORSO_RAISE', 5), ('TORSO_STAND', 15), ('TORSO_STAND2', 1)]
LEGS = [('LEGS_WALKCR', 15, 16), ('LEGS_WALK', 15, 18), ('LEGS_RUN', 10, 18), ('LEGS_BACK', 15, 18),
        ('LEGS_SWIM', 15, 15), ('LEGS_JUMP', 8, 15), ('LEGS_LAND', 6, 15), ('LEGS_JUMPB', 8, 15),
        ('LEGS_LANDB', 6, 15), ('LEGS_IDLE', 15, 15), ('LEGS_IDLECR', 15, 15), ('LEGS_TURN', 8, 15)]
GLB_FPS = 15


# ================================================================ геометрия

class Part:
    """сетка одного узла: треугольники по материалам, в координатах узла (Quake)."""
    def __init__(self):
        self.groups = {}

    def add(self, material, positions, normals, uvs, faces):
        group = self.groups.setdefault(material, ([], [], [], []))
        base = sum(len(chunk) for chunk in group[0])
        group[0].append(np.asarray(positions, dtype=np.float32))
        group[1].append(np.asarray(normals, dtype=np.float32))
        group[2].append(np.asarray(uvs, dtype=np.float32))
        group[3].append(np.asarray(faces, dtype=np.int64) + base)

    def arrays(self, material):
        positions, normals, uvs, faces = self.groups[material]
        return (np.concatenate(positions), np.concatenate(normals), np.concatenate(uvs), np.concatenate(faces))

    def triangles(self):
        return sum(len(self.arrays(material)[3]) for material in self.groups)


def planar_uv(points, normal, scale=1 / 32.0):
    axis = int(np.argmax(np.abs(normal)))
    keep = [index for index in range(3) if index != axis]
    return np.asarray(points)[:, keep] * scale


def hull(part, material, points):
    """выпуклое тело по точкам, грани плоские."""
    points = np.asarray(points, dtype=float)
    shape = ConvexHull(points)
    for simplex, equation in zip(shape.simplices, shape.equations):
        triangle = points[simplex]
        normal = equation[:3]
        if np.dot(np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0]), normal) < 0:
            triangle = triangle[[0, 2, 1]]
        part.add(material, triangle, [normal] * 3, planar_uv(triangle, normal), [(0, 1, 2)])


def bevel_points(center, size, bevel=0.0, taper=(1.0, 1.0), shift=(0.0, 0.0)):
    """точки бруса с фаской: верх сужен в taper раз и сдвинут на shift."""
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    points = []
    for sz in (-1, 1):
        scale_x = 1.0 if sz < 0 else taper[0]
        scale_y = 1.0 if sz < 0 else taper[1]
        ox = 0.0 if sz < 0 else shift[0]
        oy = 0.0 if sz < 0 else shift[1]
        for sx in (-1, 1):
            for sy in (-1, 1):
                x = cx + ox + sx * hx * scale_x
                y = cy + oy + sy * hy * scale_y
                z = cz + sz * hz
                if bevel <= 0:
                    points.append((x, y, z))
                    continue
                # угол заменяют три точки: фаска по каждому ребру
                points.append((x - sx * bevel, y, z))
                points.append((x, y - sy * bevel, z))
                points.append((x, y, z - sz * bevel))
    return points


def box(part, material, center, size, bevel=0.0, taper=(1.0, 1.0), shift=(0.0, 0.0)):
    hull(part, material, bevel_points(center, size, min(bevel, min(size) * 0.45), taper, shift))


def cylinder(part, material, start, end, radius, sides=10, radius_end=None, caps=True):
    start, end = np.asarray(start, dtype=float), np.asarray(end, dtype=float)
    radius_end = radius if radius_end is None else radius_end
    axis = end - start
    length = np.linalg.norm(axis)
    axis /= length
    helper = np.array([0, 0, 1.0]) if abs(axis[2]) < 0.9 else np.array([1.0, 0, 0])
    side_u = np.cross(axis, helper); side_u /= np.linalg.norm(side_u)
    side_v = np.cross(axis, side_u)
    positions, normals, uvs, faces = [], [], [], []
    for index in range(sides + 1):
        angle = 2 * math.pi * index / sides
        direction = side_u * math.cos(angle) + side_v * math.sin(angle)
        normal = direction + axis * (radius - radius_end) / length
        normal /= np.linalg.norm(normal)
        positions += [start + direction * radius, end + direction * radius_end]
        normals += [normal, normal]
        uvs += [(index / sides, 0), (index / sides, length / 32.0)]
    for index in range(sides):
        a, b, c, d = 2 * index, 2 * index + 1, 2 * index + 3, 2 * index + 2
        faces += [(a, d, c), (a, c, b)]
    points = np.asarray(positions)
    normal = np.cross(points[faces[0][1]] - points[faces[0][0]], points[faces[0][2]] - points[faces[0][0]])
    if np.dot(normal, normals[faces[0][0]]) < 0:
        faces = [(face[0], face[2], face[1]) for face in faces]
    part.add(material, positions, normals, uvs, faces)
    if caps:
        for center, cap_radius, sign in ((start, radius, -1), (end, radius_end, 1)):
            ring = [center + (side_u * math.cos(2 * math.pi * k / sides) + side_v * math.sin(2 * math.pi * k / sides)) * cap_radius
                    for k in range(sides)]
            cap_normal = axis * sign
            cap_faces = [(0, k, k + 1) for k in range(1, sides - 1)]
            ring_points = np.asarray(ring)
            normal = np.cross(ring_points[1] - ring_points[0], ring_points[2] - ring_points[0])
            if np.dot(normal, cap_normal) < 0:
                cap_faces = [(face[0], face[2], face[1]) for face in cap_faces]
            part.add(material, ring, [cap_normal] * sides, planar_uv(ring, cap_normal), cap_faces)


def side(points, sign):
    """точки, заданные для правой стороны (y < 0); sign 1 — отражение налево."""
    return [(x, -y, z) for x, y, z in points] if sign > 0 else list(points)


# ================================================================ узлы и анимация

def quat_axis(axis, angle):
    axis = np.asarray(axis, dtype=float); axis /= np.linalg.norm(axis)
    half = angle / 2
    return np.array([math.cos(half), *(axis * math.sin(half))])      # w, x, y, z


def quat_mul(a, b):
    w1, x1, y1, z1 = a; w2, x2, y2, z2 = b
    return np.array([w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2])


IDENTITY = np.array([1.0, 0, 0, 0])


def pitch(angle):          # вокруг Y (влево): положительный угол — нос вниз
    return quat_axis((0, 1, 0), angle)


def roll(angle):
    return quat_axis((1, 0, 0), angle)


def yaw(angle):
    return quat_axis((0, 0, 1), angle)


class Node:
    def __init__(self, name, translation=(0, 0, 0), part=None, parent=None):
        self.name = name
        self.translation = np.asarray(translation, dtype=float)
        self.rotation = IDENTITY.copy()
        self.part = part
        self.children = []
        if parent:
            parent.children.append(self)
        self.keys = {}        # анимация -> список (translation, rotation)


def collect(root):
    nodes = []
    def walk(node):
        nodes.append(node)
        for child in node.children:
            walk(child)
    walk(root)
    return nodes


# ================================================================ glTF

def q2g(vector, scale=1.0):
    return [float(vector[1]) / scale, float(vector[2]) / scale, float(vector[0]) / scale]


def q2g_quat(quat):
    w, x, y, z = quat
    return [float(y), float(z), float(x), float(w)]


class GlbWriter:
    def __init__(self):
        self.binary = bytearray()
        self.views, self.accessors = [], []

    def blob(self, data, target=None):
        while len(self.binary) % 4:
            self.binary.append(0)
        view = {'buffer': 0, 'byteOffset': len(self.binary), 'byteLength': len(data)}
        if target:
            view['target'] = target
        self.binary += data
        self.views.append(view)
        return len(self.views) - 1

    def accessor(self, array, kind, component, target=None, bounds=False):
        array = np.ascontiguousarray(array)
        view = self.blob(array.tobytes(), target)
        accessor = {'bufferView': view, 'componentType': component, 'count': int(array.shape[0]), 'type': kind}
        if bounds:
            flat = array.reshape(array.shape[0], -1)
            accessor['min'] = [float(v) for v in flat.min(axis=0)]
            accessor['max'] = [float(v) for v in flat.max(axis=0)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def write_glb(path, root, animations, scale=1.0, shader_dir=None):
    """root — узел-корень; animations — [(имя, число кадров)] в порядке кадров; координаты делятся на scale.
    shader_dir: имя шейдера материала = <shader_dir>/<материал> (для моделей без .skin)."""
    writer = GlbWriter()
    nodes = collect(root)
    index_of = {id(node): position for position, node in enumerate(nodes)}
    materials = {}
    meshes = []
    gltf_nodes = []
    for node in nodes:
        entry = {'name': node.name, 'translation': q2g(node.translation, scale), 'rotation': q2g_quat(node.rotation)}
        if node.children:
            entry['children'] = [index_of[id(child)] for child in node.children]
        if node.part and node.part.groups:
            primitives = []
            for material in node.part.groups:
                positions, normals, uvs, faces = node.part.arrays(material)
                gpos = (positions[:, [1, 2, 0]] / scale).astype(np.float32)
                gnor = normals[:, [1, 2, 0]].astype(np.float32)
                if material not in materials:
                    materials[material] = len(materials)
                index_type = np.uint16 if len(positions) < 65535 else np.uint32
                primitives.append({
                    'attributes': {
                        'POSITION': writer.accessor(gpos, 'VEC3', 5126, 34962, bounds=True),
                        'NORMAL': writer.accessor(gnor, 'VEC3', 5126, 34962),
                        'TEXCOORD_0': writer.accessor(uvs.astype(np.float32), 'VEC2', 5126, 34962)},
                    'indices': writer.accessor(faces.reshape(-1).astype(index_type), 'SCALAR',
                                               5123 if index_type == np.uint16 else 5125, 34963),
                    'material': materials[material]})
            meshes.append({'name': node.name, 'primitives': primitives})
            entry['mesh'] = len(meshes) - 1
        gltf_nodes.append(entry)

    gltf_animations = []
    start = 0.0
    for name, frames in animations:
        times = np.array([start + frame / GLB_FPS for frame in range(frames)], dtype=np.float32)
        channels, samplers = [], []
        time_accessor = writer.accessor(times, 'SCALAR', 5126, bounds=True)
        for node in nodes:
            keys = node.keys.get(name)
            if not keys:
                continue
            translations = np.array([q2g(key[0], scale) for key in keys], dtype=np.float32)
            rotations = np.array([q2g_quat(key[1]) for key in keys], dtype=np.float32)
            samplers.append({'input': time_accessor, 'output': writer.accessor(translations, 'VEC3', 5126), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': index_of[id(node)], 'path': 'translation'}})
            samplers.append({'input': time_accessor, 'output': writer.accessor(rotations, 'VEC4', 5126), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': index_of[id(node)], 'path': 'rotation'}})
        gltf_animations.append({'name': name, 'extras': {'fps': GLB_FPS}, 'channels': channels, 'samplers': samplers})
        start += frames / GLB_FPS + 1.0

    material_list = []
    for name, _ in sorted(materials.items(), key=lambda item: item[1]):
        entry = {'name': name}
        if shader_dir:
            entry['extras'] = {'q3_shader': f'{shader_dir}/{name}'}
        material_list.append(entry)
    document = {
        'asset': {'version': '2.0', 'generator': 'WALKER make_robots.py', 'extras': {'q3_scale': scale}},
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': gltf_nodes, 'meshes': meshes, 'materials': material_list,
        'accessors': writer.accessors, 'bufferViews': writer.views,
        'buffers': [{'byteLength': len(writer.binary)}],
    }
    if gltf_animations:
        document['animations'] = gltf_animations
    text = json.dumps(document, separators=(',', ':')).encode()
    text += b' ' * ((4 - len(text) % 4) % 4)
    while len(writer.binary) % 4:
        writer.binary.append(0)
    total = 12 + 8 + len(text) + 8 + len(writer.binary)
    with open(path, 'wb') as handle:
        handle.write(struct.pack('<III', 0x46546C67, 2, total))
        handle.write(struct.pack('<II', len(text), 0x4E4F534A)); handle.write(text)
        handle.write(struct.pack('<II', len(writer.binary), 0x004E4942)); handle.write(bytes(writer.binary))
    return sorted(materials, key=lambda name: materials[name])


# ================================================================ ноги

def leg_angles(hip, ankle, thigh, shin, knee_forward):
    """углы бедра и голени (вокруг Y) от вертикали вниз; 2 кости в плоскости XZ."""
    dx, dz = ankle[0] - hip[0], ankle[2] - hip[2]
    distance = min(math.hypot(dx, dz), thigh + shin - 0.01)
    base = math.atan2(-dx, -dz)
    cos_inner = (thigh ** 2 + distance ** 2 - shin ** 2) / (2 * thigh * distance)
    inner = math.acos(max(-1.0, min(1.0, cos_inner)))
    cos_knee = (thigh ** 2 + shin ** 2 - distance ** 2) / (2 * thigh * shin)
    knee = math.pi - math.acos(max(-1.0, min(1.0, cos_knee)))
    if knee_forward:
        return base - inner, base - inner + knee
    return base + inner, base + inner - knee


class Legs:
    def __init__(self, spec):
        self.spec = spec
        self.root = Node('lower_root')
        self.pelvis = Node('pelvis', (0, 0, spec['hip_z']), spec['pelvis_part'], self.root)
        self.tag_torso = Node('tag_torso', (0, 0, spec['torso_z']), None, self.pelvis)
        self.legs = []
        for name, sign in (('l', 1), ('r', -1)):
            hip = Node(f'hip_{name}', (0, sign * spec['hip_y'], 0), spec['thigh_part'](sign), self.pelvis)
            knee = Node(f'knee_{name}', (0, 0, -spec['thigh']), spec['shin_part'](sign), hip)
            ankle = Node(f'ankle_{name}', (0, 0, -spec['shin']), spec['foot_part'](sign), knee)
            self.legs.append((hip, knee, ankle, sign))

    def pose(self, pelvis_offset, pelvis_rotation, feet, foot_pitch=(0.0, 0.0)):
        spec = self.spec
        keys = {id(self.pelvis): (np.array([pelvis_offset[0], pelvis_offset[1], spec['hip_z'] + pelvis_offset[2]]), pelvis_rotation)}
        for (hip, knee, ankle, sign), (foot_x, foot_z), toe in zip(self.legs, feet, foot_pitch):
            hip_point = (pelvis_offset[0], 0, spec['hip_z'] + pelvis_offset[2])
            thigh_angle, shin_angle = leg_angles(hip_point, (foot_x, 0, foot_z), spec['thigh'], spec['shin'], spec['knee_forward'])
            keys[id(hip)] = (hip.translation, pitch(thigh_angle))
            keys[id(knee)] = (knee.translation, pitch(shin_angle - thigh_angle))
            keys[id(ankle)] = (ankle.translation, pitch(-shin_angle + toe))
        return keys

    def record(self, name, poses):
        nodes = collect(self.root)
        for node in nodes:
            node.keys[name] = []
        for keys in poses:
            for node in nodes:
                node.keys[name].append(keys.get(id(node), (node.translation, node.rotation)))


def gait(phase, travel, lift, ankle_z, duty=0.55):
    """ступня в цикле шага: опора (назад на travel), перенос (вперёд с подъёмом)."""
    phase %= 1.0
    if phase < duty:
        t = phase / duty
        return travel / 2 - travel * t, ankle_z, 0.0
    t = (phase - duty) / (1 - duty)
    eased = 0.5 - 0.5 * math.cos(math.pi * t)
    return -travel / 2 + travel * eased, ankle_z + lift * math.sin(math.pi * t), -0.25 * math.sin(math.pi * t)


def legs_animations(legs):
    spec = legs.spec
    ankle_z = GROUND + spec['ankle_height']
    poses_of = {}

    def cycle(frames, fps, direction=1, crouch=0.0, lift=None):
        travel = STRIDE_SPEED * (frames / fps) * 0.55        # тело за цикл проходит speed*T, опора 55 %
        lift = spec['lift'] if lift is None else lift
        poses = []
        for frame in range(frames):
            phase = frame / frames
            bob = -spec['bob'] * (0.5 + 0.5 * math.cos(4 * math.pi * phase)) - crouch
            feet, toes = [], []
            for offset in (0.0, 0.5):
                x, z, toe = gait(phase + offset, travel, lift, ankle_z)
                feet.append((direction * x, z))
                toes.append(toe * direction)
            sway = roll(spec['sway'] * math.sin(2 * math.pi * phase))
            poses.append(legs.pose((0, 0, bob), sway, feet, toes))
        return poses

    def still(frames, crouch=0.0, breathe=0.0):
        poses = []
        for frame in range(frames):
            bob = -crouch - breathe * (0.5 - 0.5 * math.cos(2 * math.pi * frame / frames))
            poses.append(legs.pose((0, 0, bob), IDENTITY, [(1.5, ankle_z), (-1.5, ankle_z)]))
        return poses

    def death(frames, forward, side_tilt):
        poses = []
        for frame in range(frames):
            t = min(1.0, frame / max(1, frames - 1))
            eased = 1 - (1 - t) ** 2
            drop = spec['hip_z'] - (GROUND + spec['ankle_height'] + spec['thigh'] * 0.45)
            rotation = quat_mul(pitch(forward * 0.5 * eased), roll(side_tilt * 0.45 * eased))
            spread = 4 + 6 * eased
            poses.append(legs.pose((0, 0, -drop * eased), rotation, [(spread * 0.6, ankle_z), (-spread, ankle_z)]))
        return poses

    poses_of['BOTH_DEATH1'] = death(15, 1, 0)
    poses_of['BOTH_DEAD1'] = poses_of['BOTH_DEATH1'][-1:]
    poses_of['BOTH_DEATH2'] = death(15, -1, 0)
    poses_of['BOTH_DEAD2'] = poses_of['BOTH_DEATH2'][-1:]
    poses_of['BOTH_DEATH3'] = death(15, 0.3, 1)
    poses_of['BOTH_DEAD3'] = poses_of['BOTH_DEATH3'][-1:]
    poses_of['LEGS_WALKCR'] = cycle(15, 16, crouch=spec['crouch'])
    poses_of['LEGS_WALK'] = cycle(15, 18)
    poses_of['LEGS_RUN'] = cycle(10, 18)
    poses_of['LEGS_BACK'] = cycle(15, 18, direction=-1)
    poses_of['LEGS_SWIM'] = cycle(15, 15)
    jump = [still(1, crouch=spec['crouch'] * (1 - frame / 7))[0] for frame in range(8)]
    land = [still(1, crouch=spec['crouch'] * math.sin(math.pi * frame / 5))[0] for frame in range(6)]
    poses_of['LEGS_JUMP'] = jump
    poses_of['LEGS_LAND'] = land
    poses_of['LEGS_JUMPB'] = jump
    poses_of['LEGS_LANDB'] = land
    poses_of['LEGS_IDLE'] = still(15, breathe=0.6)
    poses_of['LEGS_IDLECR'] = still(15, crouch=spec['crouch'], breathe=0.4)
    turn = []
    for frame in range(8):
        phase = frame / 8
        feet = []
        for offset in (0.0, 0.5):
            local = (phase + offset) % 1.0
            lift = spec['lift'] * 0.6 * max(0.0, math.sin(2 * math.pi * local))
            feet.append((1.5 if offset == 0 else -1.5, ankle_z + lift))
        turn.append(legs.pose((0, 0, 0), IDENTITY, feet))
    poses_of['LEGS_TURN'] = turn

    order = [name for name, _ in DEATHS] + [name for name, _, _ in LEGS]
    for name in order:
        legs.record(name, poses_of[name])
    return [(name, len(poses_of[name])) for name in order]


def torso_animations(root, body):
    """корпус: стойка, отдача, жест, гибель (наклон и оседание)."""
    poses_of = {}
    rest = body.translation.copy()

    def pose(offset=(0, 0, 0), rotation=IDENTITY):
        return {id(body): (rest + np.asarray(offset, dtype=float), rotation)}

    def death(frames, forward, side_tilt):
        return [pose((0, 0, -3 * min(1, f / 10)), quat_mul(pitch(forward * 0.35 * min(1, f / 12)), roll(side_tilt * 0.3 * min(1, f / 12))))
                for f in range(frames)]

    poses_of['BOTH_DEATH1'] = death(15, 1, 0)
    poses_of['BOTH_DEAD1'] = poses_of['BOTH_DEATH1'][-1:]
    poses_of['BOTH_DEATH2'] = death(15, -1, 0)
    poses_of['BOTH_DEAD2'] = poses_of['BOTH_DEATH2'][-1:]
    poses_of['BOTH_DEATH3'] = death(15, 0.4, 1)
    poses_of['BOTH_DEAD3'] = poses_of['BOTH_DEATH3'][-1:]
    poses_of['TORSO_GESTURE'] = [pose(rotation=yaw(0.35 * math.sin(2 * math.pi * f / 20))) for f in range(20)]
    poses_of['TORSO_ATTACK'] = [pose((-1.2 * math.exp(-f * 0.9), 0, 0), pitch(-0.02 * math.exp(-f * 0.9))) for f in range(6)]
    poses_of['TORSO_ATTACK2'] = poses_of['TORSO_ATTACK']
    poses_of['TORSO_DROP'] = [pose(rotation=pitch(0.15 * f / 4)) for f in range(5)]
    poses_of['TORSO_RAISE'] = [pose(rotation=pitch(0.15 * (1 - f / 4))) for f in range(5)]
    poses_of['TORSO_STAND'] = [pose((0, 0, 0.4 * math.sin(2 * math.pi * f / 15))) for f in range(15)]
    poses_of['TORSO_STAND2'] = [pose()]
    order = [name for name, _ in DEATHS] + [name for name, _ in TORSO]
    nodes = collect(root)
    for name in order:
        for node in nodes:
            node.keys[name] = [frame.get(id(node), (node.translation, node.rotation)) for frame in poses_of[name]]
    return [(name, len(poses_of[name])) for name in order]


# ================================================================ STRIZH

def strizh():
    """лёгкий разведчик: клиновидный корпус, длинные ноги коленом назад, трёхпалые ступни;
    слот 1 (средний) на пилоне справа, слот 2 (лёгкий) слева."""

    def thigh(sign):
        part = Part()
        cylinder(part, 'metal', (0, -3.4 * sign, 0), (0, 3.6 * sign, 0), 3.4, 12)
        cylinder(part, 'dark', (0, -4.2 * sign, 0), (0, -3.4 * sign, 0), 2.2, 10)
        # бедро: бронеплита, сужается к колену
        box(part, 'paint', (0.6, 0.4 * sign, -8.5), (6.5, 4.2, 15), bevel=1.0, taper=(1.25, 1.1))
        box(part, 'accent', (2.7, 0.4 * sign, -6), (1.4, 3.4, 8), bevel=0.4)
        box(part, 'dark', (-2.6, 0.4 * sign, -9), (1.6, 3.0, 10), bevel=0.3)
        cylinder(part, 'metal', (-2.4, 0.4 * sign, -3), (-3.0, 0.4 * sign, -14), 0.8, 8)       # гидроцилиндр
        return part

    def shin(sign):
        part = Part()
        cylinder(part, 'metal', (0, -2.8, 0), (0, 2.8, 0), 2.6, 10)
        box(part, 'metal', (0, 0, -8.5), (3.2, 2.8, 16), bevel=0.6, taper=(0.85, 0.9))
        # бронепластина голени (колено назад: пластина смотрит назад-вверх)
        hull(part, 'paint', [(1.4, -2.0, -1.5), (1.4, 2.0, -1.5), (3.2, -1.8, -6), (3.2, 1.8, -6),
                             (2.4, -1.6, -14), (2.4, 1.6, -14), (1.0, -1.7, -13), (1.0, 1.7, -13),
                             (0.6, -1.9, -2), (0.6, 1.9, -2)])
        box(part, 'accent', (2.6, 0, -9.5), (1.0, 2.8, 2.2), bevel=0.3)
        cylinder(part, 'dark', (-2.0, 0, -2), (-1.6, 0, -13.5), 0.6, 6)
        cylinder(part, 'metal', (-2.0, 0, -6), (-1.8, 0, -11), 0.95, 8)
        return part

    def foot(sign):
        part = Part()
        cylinder(part, 'metal', (0, -2.2, 0), (0, 2.2, 0), 2.0, 10)
        box(part, 'dark', (0.5, 0, -1.3), (5, 4.6, 2.6), bevel=0.6)
        # три пальца: средний вперёд, боковые разведены; шпора назад
        for angle, length in ((0.0, 9.5), (0.45, 7.5), (-0.45, 7.5)):
            direction = np.array([math.cos(angle), math.sin(angle), 0])
            across = np.array([-direction[1], direction[0], 0])
            base = np.array([2.0, 0, -1.6])
            tip = base + direction * length
            points = []
            for along, half, top in ((0, 1.3, 1.0), (0.7, 1.0, 0.6), (1.0, 0.35, -0.6)):
                center = base + (tip - base) * along
                for s in (-1, 1):
                    points.append(tuple(center + across * half * s + np.array([0, 0, top])))
                    points.append(tuple(center + across * half * s + np.array([0, 0, -1.5])))
            hull(part, 'metal' if angle == 0 else 'paint', points)
        hull(part, 'metal', [(-1, -0.9, -0.4), (-1, 0.9, -0.4), (-1, -0.9, -2.9), (-1, 0.9, -2.9), (-5.5, 0, -2.9), (-5.5, 0, -2.2)])
        return part

    pelvis = Part()
    box(pelvis, 'metal', (0, 0, 0), (9, 13, 5.5), bevel=1.2, taper=(0.9, 0.9))
    box(pelvis, 'dark', (0, 0, 3.4), (7, 8, 1.6), bevel=0.4)
    cylinder(pelvis, 'dark', (0, 0, 2), (0, 0, 4.5), 3.2, 12)
    box(pelvis, 'paint', (3.6, 0, -0.5), (2.5, 10, 4), bevel=0.8)
    spec = dict(hip_z=2.0, hip_y=8.5, torso_z=4.0, thigh=18.0, shin=17.0, ankle_height=3.2, knee_forward=False,
                lift=7.0, bob=1.2, sway=0.03, crouch=5.0,
                pelvis_part=pelvis, thigh_part=thigh, shin_part=shin, foot_part=foot)

    upper_root = Node('upper_root')
    body = Part()
    # корпус-клин: широкая корма, нос сходится к кабине
    box(body, 'paint', (-1, 0, 8), (20, 15, 11), bevel=2.0, taper=(0.82, 0.8), shift=(1.0, 0))
    hull(body, 'paint', [(6, -6, 3.5), (6, 6, 3.5), (6, -5.5, 12.5), (6, 5.5, 12.5),
                         (14, -3, 5.5), (14, 3, 5.5), (13, -2.5, 10), (13, 2.5, 10)])
    hull(body, 'glass', [(7.5, -3.6, 12.3), (7.5, 3.6, 12.3), (12.6, -2.3, 10.2), (12.6, 2.3, 10.2),
                         (8.5, -3.2, 13.2), (8.5, 3.2, 13.2)])
    box(body, 'accent', (10.5, 0, 4.4), (5, 6.5, 1.6), bevel=0.4)
    box(body, 'dark', (0, 0, 2.6), (15, 10, 2.4), bevel=0.6)
    # боковые воздухозаборники с решётками
    for sign in (1, -1):
        hull(body, 'accent', side([(-3, -7.2, 5), (5, -6.5, 5.2), (-3, -7.2, 11), (4, -6.6, 10.5),
                                   (-2, -9.0, 6), (-2, -9.0, 10)], sign))
        for slat in range(4):
            box(body, 'dark', (-2.2 + slat * 1.1, sign * 8.6, 8), (0.5, 0.6, 3.4))
    # хребет, двигательный блок, сопла
    box(body, 'metal', (-4, 0, 14.0), (12, 3.6, 2.2), bevel=0.7)
    box(body, 'metal', (-11.5, 0, 8.5), (5, 11, 8.5), bevel=1.2)
    for sign in (1, -1):
        cylinder(body, 'dark', (-13.5, sign * 3, 8), (-15.5, sign * 3, 8), 2.2, 12, radius_end=2.6)
        cylinder(body, 'glow', (-15.4, sign * 3, 8), (-15.6, sign * 3, 8), 1.7, 12)
    # кили и антенна
    for sign in (1, -1):
        hull(body, 'paint', side([(-12, -3.2, 13.5), (-6, -3.2, 13.5), (-12, -4.6, 20.5), (-10, -4.4, 20.5),
                                  (-12, -2.6, 13.5), (-6, -2.6, 13.5)], sign))
    cylinder(body, 'dark', (-7, 5.5, 14), (-7.5, 5.5, 27), 0.35, 5, caps=False)
    box(body, 'glow', (-7.5, 5.5, 27.2), (0.7, 0.7, 0.7))
    # пилоны оружия: справа средний (слот 1), слева лёгкий (слот 2), над ними бронещитки
    box(body, 'metal', (1.5, -9.6, 8.5), (8, 3.4, 5), bevel=0.8)
    cylinder(body, 'dark', (1.5, -8, 8.5), (1.5, -12.6, 8.5), 2.2, 10)
    hull(body, 'paint', [(-3, -8.2, 11), (7, -8.2, 11), (-2, -12.4, 13), (5, -12.4, 13), (-3, -8.2, 13.6), (6, -8.2, 13.6)])
    box(body, 'metal', (1.5, 9.4, 8.5), (7, 3.0, 4.2), bevel=0.7)
    cylinder(body, 'dark', (1.5, 8, 8.5), (1.5, 11.8, 8.5), 1.9, 10)
    hull(body, 'paint', [(-2, 8.2, 11), (6, 8.2, 11), (-1, 11.6, 12.6), (4, 11.6, 12.6), (-2, 8.2, 13.2), (5, 8.2, 13.2)])
    body_node = Node('torso', (0, 0, 0), body, upper_root)
    Node('tag_head', (4.5, 0, 14.4), None, body_node)
    Node('tag_weapon1', (1.5, -13.0, 8.5), None, body_node)
    Node('tag_weapon2', (1.5, 12.2, 8.5), None, body_node)
    Node('tag_weapon', (1.5, -13.0, 8.5), None, body_node)
    Node('tag_torso', (0, 0, 0), None, upper_root)

    head_root = Node('head_root')
    head = Part()
    hull(head, 'paint', [(-3, -3.4, 0), (-3, 3.4, 0), (4, -2.6, 0.5), (4, 2.6, 0.5), (-2.5, -3, 4.2), (-2.5, 3, 4.2),
                         (2.5, -2.2, 4.0), (2.5, 2.2, 4.0), (5.2, -1.6, 2.2), (5.2, 1.6, 2.2)])
    hull(head, 'glass', [(4.1, -2.0, 1.4), (4.1, 2.0, 1.4), (5.25, -1.4, 2.0), (5.25, 1.4, 2.0), (3.6, -1.8, 3.4), (3.6, 1.8, 3.4)])
    box(head, 'glow', (5.0, 0, 2.2), (0.4, 2.2, 0.5))
    for sign in (1, -1):
        cylinder(head, 'dark', (1, sign * 3.2, 2.2), (1, sign * 4.4, 2.2), 1.2, 8)
        cylinder(head, 'glow', (1, sign * 4.4, 2.2), (1, sign * 4.6, 2.2), 0.7, 8)
    Node('head', (0, 0, 0), head, head_root)
    Node('tag_head', (0, 0, 0), None, head_root)
    palette = {'default': ((198, 202, 204), (236, 118, 30)),
               'red': ((196, 48, 38), (238, 206, 86)),
               'blue': ((44, 94, 198), (238, 206, 86))}
    return 'strizh', spec, upper_root, body_node, head_root, palette


# ================================================================ MOLOT

def molot():
    """тяжёлый штурмовик: широкий корпус со слоями брони, наплечники, короткие толстые ноги
    коленом вперёд, широкие ступни; тяжёлый слот на правом плече."""

    def thigh(sign):
        part = Part()
        cylinder(part, 'metal', (0, -4.8 * sign, 0), (0, 5.2 * sign, 0), 5.0, 14)
        cylinder(part, 'dark', (0, -5.8 * sign, 0), (0, -4.8 * sign, 0), 3.2, 12)
        box(part, 'paint', (0, 0.6 * sign, -7.2), (10, 8, 13), bevel=1.6, taper=(0.9, 0.95))
        box(part, 'accent', (4.6, 0.6 * sign, -6.5), (1.4, 6, 7), bevel=0.4)
        box(part, 'dark', (-4.4, 0.6 * sign, -7), (2, 6.4, 9), bevel=0.4)
        return part

    def shin(sign):
        part = Part()
        cylinder(part, 'metal', (0, -4.2, 0), (0, 4.2, 0), 4.2, 12)
        # наколенник
        hull(part, 'paint', [(3, -4.4, 3), (3, 4.4, 3), (6.2, -3.8, 0), (6.2, 3.8, 0), (5.2, -3.6, -5), (5.2, 3.6, -5),
                             (1, -4.2, -3), (1, 4.2, -3)])
        box(part, 'metal', (0.4, 0, -8), (8, 7.4, 13), bevel=1.2, taper=(0.85, 0.85))
        hull(part, 'paint', [(4.0, -3.4, -4), (4.0, 3.4, -4), (5.0, -3.0, -12.5), (5.0, 3.0, -12.5),
                             (3.0, -3.4, -13.5), (3.0, 3.4, -13.5), (3.0, -3.6, -4.5), (3.0, 3.6, -4.5)])
        box(part, 'accent', (4.9, 0, -9), (0.8, 4.5, 1.6), bevel=0.2)
        for offset in (-1, 1):
            cylinder(part, 'dark', (-3.6, offset * 2.5, -2), (-3.4, offset * 2.5, -13), 0.9, 8)
        return part

    def foot(sign):
        part = Part()
        cylinder(part, 'metal', (0, -3.2, 0), (0, 3.2, 0), 3.0, 12)
        box(part, 'metal', (1.0, 0, -1.8), (10, 10, 3.6), bevel=1.0, taper=(0.85, 0.9))
        # четыре пальца: два вперёд, два назад
        for direction, offset in ((1, -2.8), (1, 2.8), (-1, -2.6), (-1, 2.6)):
            start_x = 5.5 if direction > 0 else -3.5
            tip_x = start_x + direction * 6.0
            hull(part, 'paint' if direction > 0 else 'dark',
                 [(start_x, offset - 1.9, -0.6), (start_x, offset + 1.9, -0.6), (start_x, offset - 1.9, -3.6), (start_x, offset + 1.9, -3.6),
                  (tip_x, offset - 1.2, -2.2), (tip_x, offset + 1.2, -2.2), (tip_x, offset - 1.4, -3.6), (tip_x, offset + 1.4, -3.6)])
        return part

    pelvis = Part()
    box(pelvis, 'metal', (0, 0, 0), (13, 21, 7), bevel=1.6, taper=(0.9, 0.95))
    box(pelvis, 'dark', (0, 0, 4.2), (9, 12, 2), bevel=0.5)
    cylinder(pelvis, 'dark', (0, 0, 3), (0, 0, 6), 5, 14)
    for sign in (1, -1):
        box(pelvis, 'paint', (5.5, sign * 6, -2), (3, 8, 7), bevel=0.8, taper=(0.8, 1))
    spec = dict(hip_z=-4.0, hip_y=13.0, torso_z=5.0, thigh=15.0, shin=15.0, ankle_height=3.6, knee_forward=True,
                lift=5.0, bob=1.6, sway=0.045, crouch=4.0,
                pelvis_part=pelvis, thigh_part=thigh, shin_part=shin, foot_part=foot)

    upper_root = Node('upper_root')
    body = Part()
    box(body, 'paint', (-1, 0, 10), (24, 26, 16), bevel=2.6, taper=(0.88, 0.86))
    # лобовая плита и слои брони
    hull(body, 'paint', [(10, -12, 3), (10, 12, 3), (16, -10.5, 6), (16, 10.5, 6), (13.5, -10, 15), (13.5, 10, 15),
                         (10, -11, 17), (10, 11, 17)])
    box(body, 'accent', (15.4, 0, 8.2), (1.4, 18, 1.8), bevel=0.4)
    hull(body, 'glass', [(13.7, -6, 12.6), (13.7, 6, 12.6), (14.6, -5.5, 11.6), (14.6, 5.5, 11.6), (13.2, -5.6, 13.6), (13.2, 5.6, 13.6)])
    box(body, 'dark', (0, 0, 2.4), (18, 18, 3.4), bevel=0.8)
    for layer in range(3):
        box(body, 'paint', (2 - layer * 6, 0, 18.4), (5.5, 18 - layer * 2, 1.6), bevel=0.5)
    # наплечники
    for sign in (1, -1):
        box(body, 'paint', (0, sign * 16.5, 12.5), (17, 7.5, 11), bevel=2.0, taper=(0.9, 0.85))
        box(body, 'accent', (8.2, sign * 16.5, 12.5), (1.4, 6.8, 9), bevel=0.4)
        box(body, 'dark', (0, sign * 20.3, 11), (12, 0.8, 6), bevel=0.2)
    # левый щит (у MOLOT нет левого оружия)
    hull(body, 'paint', [(-6, 20.6, 2), (10, 20.6, 2), (-6, 20.6, 17), (8, 20.6, 15),
                         (-5, 23.0, 3), (9, 23.0, 3), (-5, 23.0, 15.5), (7, 23.0, 14)])
    box(body, 'accent', (2, 23.1, 9), (12, 0.4, 2), bevel=0.1)
    # тяжёлый слот: поворотное основание на правом плече
    cylinder(body, 'metal', (0, -16, 18), (0, -16, 20.5), 5.2, 16)
    cylinder(body, 'dark', (0, -16, 20.5), (0, -16, 21.5), 4.2, 16)
    # ранец, выхлопные трубы, решётки
    box(body, 'metal', (-15, 0, 11), (8, 20, 14), bevel=1.5)
    for sign in (1, -1):
        cylinder(body, 'dark', (-17.5, sign * 6, 14), (-17.5, sign * 6, 25), 2.2, 12)
        cylinder(body, 'glow', (-17.5, sign * 6, 25), (-17.5, sign * 6, 25.3), 1.6, 12)
    for vent in range(5):
        box(body, 'dark', (-19.1, -7 + vent * 3.5, 9), (0.6, 2.2, 7))
    body_node = Node('torso', (0, 0, 0), body, upper_root)
    Node('tag_head', (6, 5, 18.5), None, body_node)
    Node('tag_weapon1', (0, -16, 21.5), None, body_node)
    Node('tag_weapon', (0, -16, 21.5), None, body_node)
    Node('tag_torso', (0, 0, 0), None, upper_root)

    head_root = Node('head_root')
    head = Part()
    box(head, 'paint', (0, 0, 2.4), (9, 8, 4.8), bevel=1.2, taper=(0.85, 0.85))
    hull(head, 'glass', [(4.4, -3, 2.2), (4.4, 3, 2.2), (4.9, -2.6, 3.0), (4.9, 2.6, 3.0), (4.3, -2.8, 3.6), (4.3, 2.8, 3.6)])
    box(head, 'glow', (4.9, 0, 2.9), (0.3, 3.4, 0.5))
    cylinder(head, 'dark', (-2, -2.5, 4.8), (-2, -2.5, 9), 0.3, 5, caps=False)
    Node('head', (0, 0, 0), head, head_root)
    Node('tag_head', (0, 0, 0), None, head_root)
    palette = {'default': ((96, 104, 74), (214, 172, 58)),
               'red': ((178, 46, 36), (232, 200, 92)),
               'blue': ((40, 82, 172), (232, 200, 92))}
    return 'molot', spec, upper_root, body_node, head_root, palette


# ================================================================ оружие

def weapon_osa():
    """OSA, лёгкий пулемёт: короб, кожух с кольцами, ствол, магазин сбоку."""
    part = Part()
    box(part, 'gunmetal', (3, 0, 0), (7, 4, 4), bevel=0.7)
    box(part, 'paint', (2.5, 0, 2.2), (5, 3, 0.8), bevel=0.3)
    cylinder(part, 'dark', (6.5, 0, 0.3), (14, 0, 0.3), 1.4, 10)
    for ring in range(4):
        cylinder(part, 'gunmetal', (7.5 + ring * 1.7, 0, 0.3), (8.2 + ring * 1.7, 0, 0.3), 1.6, 10)
    cylinder(part, 'gunmetal', (14, 0, 0.3), (16, 0, 0.3), 1.0, 8, radius_end=1.2)
    box(part, 'accent', (2.5, -2.6, -0.5), (3.5, 1.4, 3.2), bevel=0.3)
    box(part, 'dark', (-0.8, 0, 0), (1.6, 3, 3), bevel=0.3)
    return part, 16.2


def weapon_shmel():
    """SHMEL, лёгкие ракеты: короб с шестью трубами 3x2."""
    part = Part()
    box(part, 'paint', (4.5, 0, 0), (10, 7, 5.2), bevel=0.8)
    box(part, 'gunmetal', (-0.8, 0, 0), (1.8, 4.5, 3.6), bevel=0.4)
    for column in (-2.0, 0, 2.0):
        for row in (-1.1, 1.1):
            cylinder(part, 'dark', (8.6, column, row), (9.7, column, row), 0.85, 8)
            cylinder(part, 'glow_red', (9.5, column, row), (9.6, column, row), 0.5, 8)
    box(part, 'accent', (4.5, 0, 2.7), (6, 4, 0.3))
    return part, 9.8


def weapon_topor():
    """TOPOR, средняя автопушка: короб, длинный ствол с дульным тормозом, барабан."""
    part = Part()
    box(part, 'gunmetal', (4, 0, 0), (10, 5.5, 5.5), bevel=1.0)
    box(part, 'paint', (3, 0, 3.0), (7, 4, 1), bevel=0.4)
    cylinder(part, 'gunmetal', (9, 0, 0.5), (11, 0, 0.5), 2.2, 12)
    cylinder(part, 'dark', (11, 0, 0.5), (20, 0, 0.5), 1.3, 12)
    box(part, 'gunmetal', (20.8, 0, 0.5), (2.2, 3.0, 2.6), bevel=0.4)
    cylinder(part, 'paint', (2, -3, -1), (2, -5.5, -1), 2.6, 14)
    box(part, 'accent', (6.5, 2.9, 0), (3, 0.4, 2), bevel=0.1)
    box(part, 'dark', (-1.4, 0, 0), (1.8, 4, 4), bevel=0.4)
    return part, 22.0


def weapon_iskra():
    """ISKRA, средняя плазма: кожух, светящиеся кольца ускорителя, конденсаторы."""
    part = Part()
    box(part, 'paint', (3, 0, 0), (8, 5.5, 5), bevel=1.2, taper=(0.9, 0.9))
    cylinder(part, 'dark', (7, 0, 0.3), (18, 0, 0.3), 1.5, 12)
    for ring in range(4):
        x = 9 + ring * 2.4
        cylinder(part, 'gunmetal', (x, 0, 0.3), (x + 1.0, 0, 0.3), 2.5, 14)
        cylinder(part, 'glow_cyan', (x + 1.0, 0, 0.3), (x + 1.3, 0, 0.3), 2.2, 14)
    cylinder(part, 'gunmetal', (18, 0, 0.3), (19.5, 0, 0.3), 1.9, 12, radius_end=1.3)
    for offset in (-1, 1):
        cylinder(part, 'glow_cyan', (1, offset * 3.2, 1.6), (6, offset * 3.2, 1.6), 0.8, 8)
    box(part, 'dark', (-1.2, 0, 0), (1.6, 4, 3.6), bevel=0.4)
    return part, 19.6


def weapon_taran():
    """TARAN, тяжёлое орудие: казённик, толстый ствол, дульный тормоз, противооткатники."""
    part = Part()
    box(part, 'paint', (5, 0, 0), (14, 9, 8), bevel=1.6, taper=(0.9, 0.9))
    box(part, 'accent', (5, 0, 4.2), (8, 6, 0.6), bevel=0.2)
    cylinder(part, 'gunmetal', (12, 0, 0.5), (15, 0, 0.5), 3.4, 16)
    cylinder(part, 'dark', (15, 0, 0.5), (30, 0, 0.5), 2.2, 16, radius_end=2.0)
    for offset in (-1, 1):
        cylinder(part, 'gunmetal', (11, offset * 2.8, -2.2), (22, offset * 2.8, -2.2), 0.8, 8)
    box(part, 'gunmetal', (31.5, 0, 0.5), (3.4, 5.2, 4.0), bevel=0.6)
    for offset in (-1, 1):
        box(part, 'dark', (31.5, offset * 2.7, 0.5), (2.2, 0.6, 2.4))
    box(part, 'dark', (-2.6, 0, 0), (2.4, 6, 5), bevel=0.5)
    return part, 33.4


def weapon_kopyo():
    """KOPYO, тяжёлый рельсотрон: две направляющие с разрядом между ними, катушки, накопитель."""
    part = Part()
    box(part, 'paint', (2.5, 0, 0), (9, 8, 7), bevel=1.4)
    box(part, 'gunmetal', (-2.6, 0, 0), (2.2, 5.5, 5), bevel=0.5)
    for offset in (-1, 1):
        box(part, 'gunmetal', (19, offset * 1.9, 0.5), (26, 1.2, 3.4), bevel=0.3)
        box(part, 'dark', (19, offset * 2.6, 0.5), (24, 0.4, 2.2))
    box(part, 'glow_cyan', (19, 0, 0.5), (25, 0.5, 1.4))
    for coil in range(5):
        x = 9 + coil * 5
        box(part, 'paint', (x, 0, 0.5), (1.6, 6.6, 5.6), bevel=0.4)
        box(part, 'glow_cyan', (x + 0.85, 0, 0.5), (0.2, 5.4, 4.4))
    box(part, 'accent', (2.5, 4.1, 0), (5, 0.3, 3))
    return part, 32.6


WEAPONS = [('osa', weapon_osa), ('shmel', weapon_shmel), ('topor', weapon_topor),
           ('iskra', weapon_iskra), ('taran', weapon_taran), ('kopyo', weapon_kopyo)]


# ================================================================ текстуры, оболочки

def noise_texture(size, seed, base, spread):
    rng = np.random.default_rng(seed)
    image = np.zeros((size, size))
    for scale in (4, 16, 64):
        small = rng.normal(0, 1, (scale, scale))
        layer = np.array(Image.fromarray(((small + 3) * 40).clip(0, 255).astype(np.uint8)).resize((size, size), Image.BICUBIC)) / 255.0
        image += layer * spread / scale ** 0.3
    image = image - image.mean()
    return (np.asarray(base, dtype=float)[None, None, :] * (1.0 + image[..., None])).clip(0, 255)


def plain_texture(color, seed, size=128, spread=0.18):
    return Image.fromarray(noise_texture(size, seed, color, spread).astype(np.uint8))


def gun_texture(color, seed, size=128, lines=True):
    """оружие не запекается: панели и кромки нарисованы в текстуре."""
    image = plain_texture(color, seed, size, 0.2)
    if lines:
        draw = ImageDraw.Draw(image)
        dark = tuple(int(c * 0.55) for c in color)
        light = tuple(min(255, int(c * 1.25)) for c in color)
        for step in range(0, size, size // 4):
            draw.line([(0, step), (size, step)], fill=dark, width=1)
            draw.line([(0, step + 1), (size, step + 1)], fill=light, width=1)
        rng = np.random.default_rng(seed)
        for _ in range(18):
            x, y = rng.integers(2, size - 2, 2)
            draw.point((int(x), int(y)), fill=light)
    return image.filter(ImageFilter.GaussianBlur(0.4))


def icon_image(letter, color):
    image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle([4, 4, 60, 60], 8, fill=tuple(color) + (230,), outline=(20, 20, 20, 255), width=3)
    try:
        font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 40)
    except OSError:
        font = ImageFont.load_default()
    draw.text((32, 33), letter, fill=(255, 255, 255, 255), font=font, anchor='mm')
    return image


ROBOT_MATERIAL_COLORS = {'metal': (98, 102, 106), 'dark': (40, 40, 42), 'glow': (120, 230, 255), 'glass': (26, 42, 56)}


def write_robot_materials(name, folder, palette, part_materials):
    """текстуры материалов по окраскам (их съест bake_walker.py), шейдеры для незапечённой модели и .skin."""
    shaders = []
    for scheme in SCHEMES:
        color, accent = palette[scheme]
        colors = dict(ROBOT_MATERIAL_COLORS, paint=color, accent=accent)
        for material, value in colors.items():
            seed = zlib.crc32(f'{name}{scheme}{material}'.encode()) % 1000
            plain_texture(value, seed, 128, 0.04 if material in ('glow', 'glass') else 0.16).save(
                os.path.join(folder, f'{material}_{scheme}.jpg'), quality=90)
            shader = f'models/players/{name}/{material}_{scheme}'
            image = f'{shader}.jpg'
            if material == 'glow':
                body = f'\t{{\n\t\tmap {image}\n\t\trgbGen identity\n\t}}\n'
            else:
                body = f'\t{{\n\t\tmap {image}\n\t\trgbGen lightingDiffuse\n\t}}\n'
            shaders.append(f'{shader}\n{{\n{body}}}\n')
        for part, (materials, tags) in part_materials.items():
            lines = [f'{material},models/players/{name}/{material}_{scheme}' for material in materials] + [f'{tag},' for tag in tags]
            with open(os.path.join(folder, f'{part}_{scheme}.skin'), 'w') as handle:
                handle.write('\n'.join(lines) + '\n')
        icon_image(name[0].upper(), color).save(os.path.join(folder, f'icon_{scheme}.tga'))
    return shaders


WEAPON_MATERIALS = {'gunmetal': (84, 88, 92), 'dark': (36, 36, 38), 'paint': (104, 110, 96), 'accent': (214, 168, 48),
                    'glow_cyan': (130, 235, 255), 'glow_red': (255, 70, 40)}


def write_weapon_materials(folder):
    shaders = []
    for material, color in WEAPON_MATERIALS.items():
        glowing = material.startswith('glow')
        gun_texture(color, zlib.crc32(material.encode()) % 1000, 128, not glowing).save(os.path.join(folder, f'{material}.jpg'), quality=90)
        shader = f'models/robotweapons/{material}'
        if glowing:
            body = f'\t{{\n\t\tmap {shader}.jpg\n\t\trgbGen wave sin 0.9 0.1 0 0.5\n\t}}\n'
        else:
            shine = 0.55 if material in ('gunmetal', 'dark') else 0.35
            body = (f'\t{{\n\t\tmap {shader}.jpg\n\t\trgbGen lightingDiffuse\n\t}}\n'
                    f'\t{{\n\t\tmap models/players/walker/env_shine.jpg\n\t\ttcGen environment\n'
                    f'\t\tblendFunc GL_SRC_ALPHA GL_ONE\n\t\trgbGen const ( {shine:.2f} {shine:.2f} {shine:.2f} )\n'
                    f'\t\talphaGen lightingSpecular\n\t}}\n')
        shaders.append(f'{shader}\n{{\n{body}}}\n')
    return shaders


def animation_cfg(name):
    lines = [f'// animation config file for the {name} robot (generated by make_robots.py)',
             '// first frame, num frames, looping frames, frames per second', '', 'sex n', 'footsteps mech',
             'headoffset 0 0 0', '']
    frame = 0
    for title, frames in DEATHS:
        lines.append(f'{frame}\t{frames}\t0\t15\t\t// {title}'); frame += frames
    fps_torso = {'TORSO_DROP': 20, 'TORSO_RAISE': 20}
    loops = {'TORSO_STAND': 15}
    for title, frames in TORSO:
        lines.append(f'{frame}\t{frames}\t{loops.get(title, 0)}\t{fps_torso.get(title, 15)}\t\t// {title}'); frame += frames
    looping = {'LEGS_WALKCR', 'LEGS_WALK', 'LEGS_RUN', 'LEGS_BACK', 'LEGS_SWIM', 'LEGS_IDLE', 'LEGS_IDLECR', 'LEGS_TURN'}
    for title, frames, fps in LEGS:
        lines.append(f'{frame}\t{frames}\t{frames if title in looping else 0}\t{fps}\t\t// {title}'); frame += frames
    return '\n'.join(lines) + '\n'


def build_robot(robot):
    name, spec, upper_root, body, head_root, palette = robot()
    folder = os.path.join(OUT, 'models', 'players', name)
    os.makedirs(folder, exist_ok=True)
    legs = Legs(spec)
    lower_animations = legs_animations(legs)
    lower_materials = write_glb(os.path.join(folder, 'lower.glb'), legs.root, lower_animations, ROBOT_SCALE)
    upper_animations = torso_animations(upper_root, body)
    upper_materials = write_glb(os.path.join(folder, 'upper.glb'), upper_root, upper_animations, ROBOT_SCALE)
    head_materials = write_glb(os.path.join(folder, 'head.glb'), head_root, [], ROBOT_SCALE)
    with open(os.path.join(folder, 'animation.cfg'), 'w') as handle:
        handle.write(animation_cfg(name))
    upper_tags = [node.name for node in collect(upper_root) if node.name.startswith('tag_')]
    shaders = write_robot_materials(name, folder, palette, {
        'lower': (lower_materials, ['tag_torso']),
        'upper': (upper_materials, upper_tags),
        'head': (head_materials, ['tag_head'])})
    triangles = sum(node.part.triangles() for root in (legs.root, upper_root, head_root) for node in collect(root) if node.part)
    print(f'{name}: {triangles} triangles, frames {sum(f for _, f in lower_animations)}/{sum(f for _, f in upper_animations)}')
    return shaders


def build_weapons():
    folder = os.path.join(OUT, 'models', 'robotweapons')
    os.makedirs(folder, exist_ok=True)
    for name, make in WEAPONS:
        part, muzzle = make()
        root = Node(f'{name}_root')
        Node(name, (0, 0, 0), part, root)
        Node('tag_flash', (muzzle, 0, 0.4), None, root)
        write_glb(os.path.join(folder, f'{name}.glb'), root, [], 1.0, 'models/robotweapons')
        print(f'weapon {name}: {part.triangles()} triangles, muzzle at {muzzle}')
    return write_weapon_materials(folder)


def main():
    shaders = ['// WALKER robots STRIZH and MOLOT (before baking) and the weapons of the robot slots,',
               '// generated by walker/tools/make_robots.py\n']
    for robot in (strizh, molot):
        shaders += build_robot(robot)
    shaders += build_weapons()
    os.makedirs(os.path.join(OUT, 'scripts'), exist_ok=True)
    with open(os.path.join(OUT, 'scripts', 'walker_robots.shader'), 'w') as handle:
        handle.write('\n'.join(shaders))


main()
