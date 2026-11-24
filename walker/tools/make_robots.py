#!/usr/bin/env python3
"""
Программные модели роботов WALKER: STRIZH (лёгкий разведчик) и MOLOT (тяжёлый штурмовик).

python3 walker/tools/make_robots.py <папка базовых данных>

В <папке>/models/players/<робот>/ пишутся lower.glb, upper.glb, head.glb (жёсткие узлы с
анимацией, движок переводит их в MD3, renderer/tr_model_glb.c), animation.cfg (те же кадры,
что у K7), текстуры окрасок default/red/blue, файлы .skin и значки; шейдеры — в
scripts/walker_robots.shader.

Модель строится в координатах Quake (X вперёд, Y влево, Z вверх, начало — точка игрока, земля
на Z = -24) и переводится в glTF (+X влево, +Y вверх, +Z вперёд) при записи.

Ноги — две кости с обратной кинематикой: ступня в опоре идёт назад ровно с той скоростью, с
какой движется робот (длина шага подогнана под cgame: темп ног = скорость / cg_strideSpeed 110),
в переносе — поднимается и выносится вперёд.
"""
import json, math, os, struct, sys, zlib
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
GROUND = -24.0
STRIDE_SPEED = 110.0          # cg_strideSpeed: при этой скорости анимация идёт в своём темпе
SCHEMES = ('default', 'red', 'blue')

# кадры как в animation.cfg K7: имя, число кадров, кадров в секунду (из cfg)
DEATHS = [('BOTH_DEATH1', 15), ('BOTH_DEAD1', 1), ('BOTH_DEATH2', 15), ('BOTH_DEAD2', 1),
          ('BOTH_DEATH3', 15), ('BOTH_DEAD3', 1)]
TORSO = [('TORSO_GESTURE', 20), ('TORSO_ATTACK', 6), ('TORSO_ATTACK2', 6), ('TORSO_DROP', 5),
         ('TORSO_RAISE', 5), ('TORSO_STAND', 15), ('TORSO_STAND2', 1)]
LEGS = [('LEGS_WALKCR', 15, 16), ('LEGS_WALK', 15, 18), ('LEGS_RUN', 10, 18), ('LEGS_BACK', 15, 18),
        ('LEGS_SWIM', 15, 15), ('LEGS_JUMP', 8, 15), ('LEGS_LAND', 6, 15), ('LEGS_JUMPB', 8, 15),
        ('LEGS_LANDB', 6, 15), ('LEGS_IDLE', 15, 15), ('LEGS_IDLECR', 15, 15), ('LEGS_TURN', 8, 15)]
GLB_FPS = 15                  # кадры в glb пишутся с этой частотой (номера кадров важнее времени)


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


def planar_uv(points, normal, scale=1 / 32.0):
    axis = int(np.argmax(np.abs(normal)))
    keep = [index for index in range(3) if index != axis]
    return np.asarray(points)[:, keep] * scale


def quad(part, material, corners, inside):
    """четырёхугольник, лицом наружу от точки inside."""
    corners = np.asarray(corners, dtype=float)
    normal = np.cross(corners[1] - corners[0], corners[2] - corners[0])
    if np.dot(normal, corners.mean(axis=0) - inside) < 0:
        corners = corners[::-1]
        normal = -normal
    normal /= np.linalg.norm(normal) + 1e-9
    part.add(material, corners, [normal] * 4, planar_uv(corners, normal), [(0, 1, 2), (0, 2, 3)])


def hull8(part, material, corners):
    """тело из 8 углов: низ (4, по кругу), верх (4, в том же порядке)."""
    corners = np.asarray(corners, dtype=float)
    inside = corners.mean(axis=0)
    bottom, top = corners[:4], corners[4:]
    quad(part, material, bottom, inside)
    quad(part, material, top, inside)
    for index in range(4):
        following = (index + 1) % 4
        quad(part, material, [bottom[index], bottom[following], top[following], top[index]], inside)


def box(part, material, center, size, taper=(1.0, 1.0), shift=(0.0, 0.0)):
    """брус: size — (длина по X, ширина по Y, высота по Z); верх сужен в taper раз и сдвинут на shift."""
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    bottom = [(cx - hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz - hz), (cx + hx, cy + hy, cz - hz), (cx - hx, cy + hy, cz - hz)]
    tx, ty = hx * taper[0], hy * taper[1]
    sx, sy = cx + shift[0], cy + shift[1]
    top = [(sx - tx, sy - ty, cz + hz), (sx + tx, sy - ty, cz + hz), (sx + tx, sy + ty, cz + hz), (sx - tx, sy + ty, cz + hz)]
    hull8(part, material, bottom + top)


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
        slope = (radius - radius_end) / length
        normal = direction + axis * slope
        normal /= np.linalg.norm(normal)
        positions += [start + direction * radius, end + direction * radius_end]
        normals += [normal, normal]
        uvs += [(index / sides, 0), (index / sides, length / 32.0)]
    for index in range(sides):
        a, b, c, d = 2 * index, 2 * index + 1, 2 * index + 3, 2 * index + 2
        faces += [(a, d, c), (a, c, b)]
    # наружу: проверка по первой грани
    p = np.asarray(positions)
    normal = np.cross(p[faces[0][1]] - p[faces[0][0]], p[faces[0][2]] - p[faces[0][0]])
    if np.dot(normal, normals[faces[0][0]]) < 0:
        faces = [(f[0], f[2], f[1]) for f in faces]
    part.add(material, positions, normals, uvs, faces)
    if caps:
        for center, r, sign in ((start, radius, -1), (end, radius_end, 1)):
            ring = [center + (side_u * math.cos(2 * math.pi * k / sides) + side_v * math.sin(2 * math.pi * k / sides)) * r
                    for k in range(sides)]
            cap_normal = axis * sign
            cap_faces = [(0, k, k + 1) for k in range(1, sides - 1)]
            p = np.asarray(ring)
            normal = np.cross(p[1] - p[0], p[2] - p[0])
            if np.dot(normal, cap_normal) < 0:
                cap_faces = [(f[0], f[2], f[1]) for f in cap_faces]
            part.add(material, ring, [cap_normal] * sides, planar_uv(ring, cap_normal), cap_faces)


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


def pitch(angle):          # вокруг Y (влево): положительный угол — нос вниз, вектор вниз уходит назад
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
        self.parent = parent
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

def q2g(vector):
    return [float(vector[1]), float(vector[2]), float(vector[0])]


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


def write_glb(path, root, animations, materials_order):
    """root — узел-корень; animations — [(имя, число кадров)] в порядке кадров."""
    writer = GlbWriter()
    nodes = collect(root)
    index_of = {id(node): position for position, node in enumerate(nodes)}
    materials = {}
    meshes = []
    gltf_nodes = []
    for node in nodes:
        entry = {'name': node.name, 'translation': q2g(node.translation), 'rotation': q2g_quat(node.rotation)}
        if node.children:
            entry['children'] = [index_of[id(child)] for child in node.children]
        if node.part and node.part.groups:
            primitives = []
            for material in node.part.groups:
                positions, normals, uvs, faces = node.part.arrays(material)
                gpos = positions[:, [1, 2, 0]].astype(np.float32)
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
            translations = np.array([q2g(key[0]) for key in keys], dtype=np.float32)
            rotations = np.array([q2g_quat(key[1]) for key in keys], dtype=np.float32)
            samplers.append({'input': time_accessor, 'output': writer.accessor(translations, 'VEC3', 5126), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': index_of[id(node)], 'path': 'translation'}})
            samplers.append({'input': time_accessor, 'output': writer.accessor(rotations, 'VEC4', 5126), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': index_of[id(node)], 'path': 'rotation'}})
        gltf_animations.append({'name': name, 'extras': {'fps': GLB_FPS}, 'channels': channels, 'samplers': samplers})
        start += frames / GLB_FPS + 1.0          # промежуток между анимациями

    document = {
        'asset': {'version': '2.0', 'generator': 'WALKER make_robots.py', 'extras': {'q3_scale': 1.0}},
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': gltf_nodes, 'meshes': meshes,
        'materials': [{'name': name} for name, _ in sorted(materials.items(), key=lambda item: item[1])],
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
    base = math.atan2(-dx, -dz)                   # угол, переводящий «вниз» в направление на ступню
    cos_inner = (thigh ** 2 + distance ** 2 - shin ** 2) / (2 * thigh * distance)
    inner = math.acos(max(-1.0, min(1.0, cos_inner)))
    cos_knee = (thigh ** 2 + shin ** 2 - distance ** 2) / (2 * thigh * shin)
    knee = math.pi - math.acos(max(-1.0, min(1.0, cos_knee)))
    if knee_forward:            # колено вперёд: бедро отклоняется вперёд (отрицательный угол)
        return base - inner, base - inner + knee
    return base + inner, base + inner - knee


class Legs:
    def __init__(self, spec):
        self.spec = spec
        self.root = Node('lower_root')
        self.pelvis = Node('pelvis', (0, 0, spec['hip_z']), spec['pelvis_part'], self.root)
        self.tag_torso = Node('tag_torso', (0, 0, spec['torso_z']), None, self.pelvis)
        self.legs = []
        for side, sign in (('l', 1), ('r', -1)):
            hip = Node(f'hip_{side}', (0, sign * spec['hip_y'], 0), spec['thigh_part'](sign), self.pelvis)
            knee = Node(f'knee_{side}', (0, 0, -spec['thigh']), spec['shin_part'](sign), hip)
            ankle = Node(f'ankle_{side}', (0, 0, -spec['shin']), spec['foot_part'](sign), knee)
            self.legs.append((hip, knee, ankle, sign))

    def pose(self, pelvis_offset, pelvis_rotation, feet, foot_pitch=(0.0, 0.0)):
        """feet — точки голеностопа (x, z) левой и правой ноги относительно корня."""
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
        travel = STRIDE_SPEED * (frames / fps) * 0.55                  # тело за цикл проходит speed*T, опора 55 %
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

    def death(frames, forward, side):
        poses = []
        for frame in range(frames):
            t = min(1.0, frame / max(1, frames - 1))
            eased = 1 - (1 - t) ** 2
            drop = spec['hip_z'] - (GROUND + spec['ankle_height'] + spec['thigh'] * 0.45)
            rotation = quat_mul(pitch(forward * 0.5 * eased), roll(side * 0.45 * eased))
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
    # поворот на месте: шаги без продвижения
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


def torso_animations(root, body, extra_nodes):
    """корпус: стойка, отдача, жест, гибель (наклон и оседание)."""
    poses_of = {}
    rest = body.translation.copy()

    def pose(offset=(0, 0, 0), rotation=IDENTITY):
        return {id(body): (rest + np.asarray(offset, dtype=float), rotation)}

    def death(frames, forward, side):
        return [pose((0, 0, -3 * min(1, f / 10)), quat_mul(pitch(forward * 0.35 * min(1, f / 12)), roll(side * 0.3 * min(1, f / 12))))
                for f in range(frames)]

    poses_of['BOTH_DEATH1'] = death(15, 1, 0)
    poses_of['BOTH_DEAD1'] = poses_of['BOTH_DEATH1'][-1:]
    poses_of['BOTH_DEATH2'] = death(15, -1, 0)
    poses_of['BOTH_DEAD2'] = poses_of['BOTH_DEATH2'][-1:]
    poses_of['BOTH_DEATH3'] = death(15, 0.4, 1)
    poses_of['BOTH_DEAD3'] = poses_of['BOTH_DEATH3'][-1:]
    poses_of['TORSO_GESTURE'] = [pose(rotation=yaw(0.35 * math.sin(2 * math.pi * f / 20))) for f in range(20)]
    poses_of['TORSO_ATTACK'] = [pose((-1.6 * math.exp(-f * 0.9), 0, 0), pitch(-0.03 * math.exp(-f * 0.9))) for f in range(6)]
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


# ================================================================ роботы

def strizh():
    """лёгкий разведчик: узкий корпус, ноги с коленом назад, два скорострельных пулемёта."""
    def thigh(sign):
        part = Part()
        box(part, 'paint', (0, sign * 1.5, -8), (6, 4, 18), taper=(0.8, 0.9))
        cylinder(part, 'metal', (0, sign * -2.5, 0), (0, sign * 3.5, 0), 3.6, 10)
        return part

    def shin(sign):
        part = Part()
        box(part, 'paint', (0, 0, -8.5), (4.5, 3.5, 17), taper=(0.7, 0.8))
        cylinder(part, 'metal', (0, -2.6, 0), (0, 2.6, 0), 2.8, 8)
        cylinder(part, 'dark', (-2.5, 0, -3), (-2.5, 0, -13), 0.9, 6)
        return part

    def foot(sign):
        part = Part()
        box(part, 'metal', (2.5, 0, -1.5), (13, 6.5, 3), taper=(0.8, 0.85))
        box(part, 'paint', (8, 0, -1.8), (3, 5.5, 2.2))
        cylinder(part, 'metal', (0, -2, 0), (0, 2, 0), 2.0, 8)
        return part

    pelvis = Part()
    box(pelvis, 'metal', (0, 0, 0), (10, 15, 6), taper=(0.9, 0.9))
    box(pelvis, 'dark', (0, 0, 3.5), (7, 9, 2))
    spec = dict(hip_z=2.0, hip_y=8.5, torso_z=4.0, thigh=18.0, shin=17.0, ankle_height=3.0, knee_forward=False,
                lift=7.0, bob=1.2, sway=0.03, crouch=5.0,
                pelvis_part=pelvis, thigh_part=thigh, shin_part=shin, foot_part=foot)

    upper_root = Node('upper_root')
    body_part = Part()
    box(body_part, 'paint', (1, 0, 9), (20, 16, 12), taper=(0.75, 0.8), shift=(1.5, 0))
    box(body_part, 'metal', (-7, 0, 8), (6, 12, 9))
    box(body_part, 'glow', (-10.2, 0, 9), (0.6, 8, 2))
    box(body_part, 'dark', (2, 0, 2.5), (12, 10, 3))
    for sign in (1, -1):
        box(body_part, 'metal', (0, sign * 10, 9), (7, 4, 6))
        box(body_part, 'paint', (2, sign * 13, 9), (14, 5, 7), taper=(0.85, 0.9))
        for barrel in (-1.6, 1.6):
            cylinder(body_part, 'dark', (8, sign * 13 + barrel, 9.5), (22, sign * 13 + barrel, 9.5), 1.0, 8)
        cylinder(body_part, 'metal', (6, sign * 13, 9.5), (9, sign * 13, 9.5), 2.8, 8)
    cylinder(body_part, 'dark', (-6, 5, 15), (-6, 5, 24), 0.5, 5, caps=False)
    body = Node('torso', (0, 0, 0), body_part, upper_root)
    Node('tag_head', (5, 0, 15), None, body)
    Node('tag_weapon', (22, -13, 9.5), None, body)
    Node('tag_torso', (0, 0, 0), None, upper_root)

    head_root = Node('head_root')
    head_part = Part()
    box(head_part, 'paint', (0, 0, 2.5), (9, 8, 5), taper=(0.7, 0.75), shift=(0.5, 0))
    box(head_part, 'glass', (4, 0, 2.6), (1.5, 6.4, 2.2))
    box(head_part, 'glow', (4.9, 0, 2.6), (0.3, 4, 0.8))
    Node('head', (0, 0, 0), head_part, head_root)
    Node('tag_head', (0, 0, 0), None, head_root)
    palette = {'default': ((196, 200, 202), (232, 116, 32)), 'red': ((200, 52, 40), (240, 210, 80)), 'blue': ((44, 92, 196), (240, 210, 80))}
    return 'strizh', spec, upper_root, body, head_root, palette


def molot():
    """тяжёлый штурмовик: широкий бронированный корпус, короткие толстые ноги, пушка."""
    def thigh(sign):
        part = Part()
        box(part, 'paint', (0, sign * 1, -7), (10, 8, 15), taper=(0.85, 0.9))
        cylinder(part, 'metal', (0, sign * -4.5, 0), (0, sign * 5, 0), 5.2, 12)
        box(part, 'dark', (-4, sign * 1, -7), (2, 6, 10))
        return part

    def shin(sign):
        part = Part()
        box(part, 'paint', (0.5, 0, -7.5), (9, 8, 15), taper=(0.8, 0.85))
        cylinder(part, 'metal', (0, -4, 0), (0, 4, 0), 4.2, 10)
        box(part, 'metal', (4.5, 0, -6), (2, 6, 8))
        return part

    def foot(sign):
        part = Part()
        box(part, 'metal', (2, 0, -1.8), (17, 11, 3.6), taper=(0.85, 0.9))
        box(part, 'dark', (9.5, 0, -2.2), (3, 9, 2.4))
        cylinder(part, 'metal', (0, -3.5, 0), (0, 3.5, 0), 3.0, 10)
        return part

    pelvis = Part()
    box(pelvis, 'metal', (0, 0, 0), (14, 24, 8), taper=(0.9, 0.95))
    box(pelvis, 'dark', (0, 0, 4.5), (10, 14, 2))
    spec = dict(hip_z=-4.0, hip_y=13.0, torso_z=5.0, thigh=15.0, shin=15.0, ankle_height=3.6, knee_forward=True,
                lift=5.0, bob=1.6, sway=0.045, crouch=4.0,
                pelvis_part=pelvis, thigh_part=thigh, shin_part=shin, foot_part=foot)

    upper_root = Node('upper_root')
    body_part = Part()
    box(body_part, 'paint', (0, 0, 10), (26, 30, 16), taper=(0.85, 0.85))
    box(body_part, 'metal', (-10, 0, 9), (8, 22, 12))
    box(body_part, 'paint', (12, 0, 7), (6, 24, 10), taper=(0.6, 0.95), shift=(-1, 0))
    box(body_part, 'dark', (0, 0, 2), (18, 20, 4))
    for sign in (1, -1):
        box(body_part, 'paint', (0, sign * 17, 12), (20, 4, 14), taper=(0.9, 0.8))
    for vent in (-6, -2, 2, 6):
        box(body_part, 'dark', (-14.2, vent, 11), (0.6, 2.4, 6))
    # пушка на правом плече
    box(body_part, 'metal', (-1, -12, 20), (14, 8, 7))
    cylinder(body_part, 'metal', (5, -12, 20.5), (11, -12, 20.5), 3.6, 12)
    cylinder(body_part, 'dark', (11, -12, 20.5), (38, -12, 20.5), 2.1, 12)
    cylinder(body_part, 'metal', (35, -12, 20.5), (40, -12, 20.5), 2.8, 12)
    body = Node('torso', (0, 0, 0), body_part, upper_root)
    Node('tag_head', (6, 4, 18), None, body)
    Node('tag_weapon', (40, -12, 20.5), None, body)
    Node('tag_torso', (0, 0, 0), None, upper_root)

    head_root = Node('head_root')
    head_part = Part()
    box(head_part, 'paint', (0, 0, 2.5), (11, 10, 5), taper=(0.8, 0.85))
    box(head_part, 'glass', (5.2, 0, 3), (1.2, 7.5, 1.6))
    box(head_part, 'glow', (5.9, 0, 3), (0.3, 5, 0.6))
    Node('head', (0, 0, 0), head_part, head_root)
    Node('tag_head', (0, 0, 0), None, head_root)
    palette = {'default': ((92, 100, 70), (210, 170, 60)), 'red': ((176, 46, 36), (230, 200, 90)), 'blue': ((40, 80, 170), (230, 200, 90))}
    return 'molot', spec, upper_root, body, head_root, palette


# ================================================================ текстуры, оболочки

def noise_texture(size, seed, base, spread):
    rng = np.random.default_rng(seed)
    image = np.zeros((size, size, 3))
    for scale in (4, 16, 64):
        small = rng.normal(0, 1, (scale, scale, 1))
        image += np.array(Image.fromarray(((small[..., 0] + 3) * 40).clip(0, 255).astype(np.uint8)).resize((size, size), Image.BICUBIC))[..., None] / 255.0 * (spread / scale ** 0.3)
    image = np.asarray(base, dtype=float) * (0.85 + image - image.mean() + 0.15)
    return image.clip(0, 255)


def paint_texture(color, stripe, seed, size=256):
    """окраска: пластины со швами, полоса, потёртости."""
    image = noise_texture(size, seed, color, 0.25)
    pixels = Image.fromarray(image.astype(np.uint8))
    draw = ImageDraw.Draw(pixels)
    step = size // 4
    for index in range(1, 4):
        draw.line([(0, index * step), (size, index * step)], fill=tuple(int(c * 0.55) for c in color), width=2)
        draw.line([(index * step, 0), (index * step, size)], fill=tuple(int(c * 0.6) for c in color), width=1)
    draw.rectangle([0, size * 0.42, size, size * 0.5], fill=stripe)
    rng = np.random.default_rng(seed + 7)
    for _ in range(40):
        x, y = rng.integers(0, size, 2)
        r = rng.integers(2, 6)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=(118, 118, 112))
    return pixels.filter(ImageFilter.GaussianBlur(0.6))


def icon_image(letter, color):
    image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle([4, 4, 60, 60], 8, fill=color + (230,), outline=(20, 20, 20, 255), width=3)
    try:
        font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 40)
    except OSError:
        font = ImageFont.load_default()
    draw.text((32, 33), letter, fill=(255, 255, 255, 255), font=font, anchor='mm')
    return image


SHINE = {'paint': 0.35, 'metal': 0.55}


def write_materials(name, folder, palette, part_materials):
    """текстуры, шейдеры и .skin окрасок."""
    texture_names = {}
    for scheme in SCHEMES:
        color, stripe = palette[scheme]
        paint_texture(color, stripe, zlib.crc32((name + scheme).encode()) % 1000).save(os.path.join(folder, f'paint_{scheme}.jpg'), quality=88)
    noise = Image.fromarray(noise_texture(128, 3, (92, 96, 100), 0.3).astype(np.uint8))
    noise.save(os.path.join(folder, 'metal.jpg'), quality=88)
    Image.fromarray(noise_texture(64, 5, (34, 34, 36), 0.2).astype(np.uint8)).save(os.path.join(folder, 'dark.jpg'), quality=88)
    Image.new('RGB', (16, 16), (120, 230, 255)).save(os.path.join(folder, 'glow.jpg'), quality=90)
    Image.new('RGB', (16, 16), (24, 40, 52)).save(os.path.join(folder, 'glass.jpg'), quality=90)

    shaders = []
    for scheme in SCHEMES:
        for material in ('paint', 'metal', 'dark', 'glow', 'glass'):
            image = f'models/players/{name}/' + (f'paint_{scheme}.jpg' if material == 'paint' else f'{material}.jpg')
            shader = f'models/players/{name}/{material}_{scheme}'
            if material == 'glow':
                body = f'\t{{\n\t\tmap {image}\n\t\trgbGen identity\n\t}}\n'
            elif material in SHINE:
                body = (f'\t{{\n\t\tmap {image}\n\t\trgbGen lightingDiffuse\n\t}}\n'
                        f'\t{{\n\t\tmap models/players/walker/env_shine.jpg\n\t\ttcGen environment\n'
                        f'\t\tblendFunc GL_SRC_ALPHA GL_ONE\n\t\trgbGen const ( {SHINE[material]:.2f} {SHINE[material]:.2f} {SHINE[material]:.2f} )\n'
                        f'\t\talphaGen lightingSpecular\n\t}}\n')
            else:
                body = f'\t{{\n\t\tmap {image}\n\t\trgbGen lightingDiffuse\n\t}}\n'
            shaders.append(f'{shader}\n{{\n{body}}}\n')
        for part, (materials, tags) in part_materials.items():
            lines = [f'{material},models/players/{name}/{material}_{scheme}' for material in materials] + [f'{tag},' for tag in tags]
            with open(os.path.join(folder, f'{part}_{scheme}.skin'), 'w') as handle:
                handle.write('\n'.join(lines) + '\n')
        icon_image(name[0].upper(), palette[scheme][0]).save(os.path.join(folder, f'icon_{scheme}.tga'))
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


def build(robot):
    name, spec, upper_root, body, head_root, palette = robot()
    folder = os.path.join(OUT, 'models', 'players', name)
    os.makedirs(folder, exist_ok=True)
    legs = Legs(spec)
    lower_animations = legs_animations(legs)
    lower_materials = write_glb(os.path.join(folder, 'lower.glb'), legs.root, lower_animations, None)
    upper_animations = torso_animations(upper_root, body, [])
    upper_materials = write_glb(os.path.join(folder, 'upper.glb'), upper_root, upper_animations, None)
    head_materials = write_glb(os.path.join(folder, 'head.glb'), head_root, [], None)
    with open(os.path.join(folder, 'animation.cfg'), 'w') as handle:
        handle.write(animation_cfg(name))
    shaders = write_materials(name, folder, palette, {
        'lower': (lower_materials, ['tag_torso']),
        'upper': (upper_materials, ['tag_head', 'tag_weapon', 'tag_torso']),
        'head': (head_materials, ['tag_head'])})
    print(f'{name}: lower {sum(len(node.part.arrays(m)[3]) for node in collect(legs.root) if node.part for m in node.part.groups)} tris, '
          f'frames {sum(f for _, f in lower_animations)}/{sum(f for _, f in upper_animations)}')
    return shaders


def main():
    shaders = ['// WALKER robots STRIZH and MOLOT, generated by walker/tools/make_robots.py\n']
    for robot in (strizh, molot):
        shaders += build(robot)
    os.makedirs(os.path.join(OUT, 'scripts'), exist_ok=True)
    with open(os.path.join(OUT, 'scripts', 'walker_robots.shader'), 'w') as handle:
        handle.write('\n'.join(shaders))


main()
