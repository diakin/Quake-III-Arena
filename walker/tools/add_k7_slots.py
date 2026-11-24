#!/usr/bin/env python3
"""
Теги слотов оружия у K7: tag_weapon1 (правый лёгкий) и tag_weapon2 (левый лёгкий) снаружи
боковых коробов корпуса, в upper.glb исходной модели (assets/baseq3/models/players/walker).

python3 walker/tools/add_k7_slots.py <upper.glb>

Теги — дочерние узлы upper_body (того же узла, что двигает корпус в анимациях), поэтому
оружие на них качается вместе с корпусом. Повторный запуск ничего не меняет.
Точки заданы в координатах Quake относительно tag_torso (X вперёд, Y влево, Z вверх);
модель K7 в единицах glTF, 1 единица = 14 единиц Quake.
"""
import json, struct, sys
import numpy as np

SLOTS = {'tag_weapon1': (-6.0, -19.5, 0.5), 'tag_weapon2': (-6.0, 19.5, 0.5)}
SCALE = 14.0

path = sys.argv[1]
data = open(path, 'rb').read()
json_length = struct.unpack('<I', data[12:16])[0]
document = json.loads(data[20:20 + json_length])
binary_chunk = data[20 + json_length:]
nodes = document['nodes']
if any(node.get('name') in SLOTS for node in nodes):
    print('K7 slots: already there')
    sys.exit(0)


def quat_matrix(quat):
    x, y, z, w = quat
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def local_matrix(node):
    matrix = np.eye(4)
    if 'matrix' in node:
        return np.array(node['matrix']).reshape(4, 4).T
    matrix[:3, :3] = quat_matrix(node.get('rotation', [0, 0, 0, 1])) * np.array(node.get('scale', [1, 1, 1]))
    matrix[:3, 3] = node.get('translation', [0, 0, 0])
    return matrix


parent = {}
for index, node in enumerate(nodes):
    for child in node.get('children', []):
        parent[child] = index


def world(index):
    matrix = local_matrix(nodes[index])
    while index in parent:
        index = parent[index]
        matrix = local_matrix(nodes[index]) @ matrix
    return matrix


body = next(index for index, node in enumerate(nodes) if node.get('name') == 'upper_body')
torso_tag = next(index for index, node in enumerate(nodes) if node.get('name') == 'tag_torso')
origin = world(torso_tag)[:3, 3]
inverse = np.linalg.inv(world(body))
for name, (forward, left, up) in SLOTS.items():
    point = origin + np.array([left, up, forward]) / SCALE           # glTF: +X влево, +Y вверх, +Z вперёд
    local = inverse @ np.append(point, 1.0)
    nodes.append({'name': name, 'translation': [float(v) for v in local[:3]]})
    nodes[body].setdefault('children', []).append(len(nodes) - 1)
text = json.dumps(document, separators=(',', ':')).encode()
text += b' ' * ((4 - len(text) % 4) % 4)
with open(path, 'wb') as handle:
    handle.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + len(binary_chunk)))
    handle.write(struct.pack('<II', len(text), 0x4E4F534A))
    handle.write(text)
    handle.write(binary_chunk)
print('K7 slots added:', ', '.join(SLOTS))
