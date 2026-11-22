"""Упрощённые модели робота для дальних дистанций (уровни детализации, LOD).

python3 walker/tools/make_lods.py <папка модели в данных игры>

Запускается после bake_walker.py. Для lower.glb, upper.glb и head.glb пишет
<часть>_1.glb, <часть>_2.glb и <часть>_3.glb: те же узлы, анимации, теги и
текстурный атлас, меньше треугольников. Движок (tr_model.c) сам загружает
файлы _1.._3 как уровни 1..3 и выбирает уровень по размеру робота на экране
(R_ComputeLOD; r_lodscale, r_lodbias).

Упрощение — meshoptimizer: на уровне 1 с сохранением формы (simplify), на
дальних — без учёта топологии (simplify_sloppy, швы развёртки не мешают).
Мелкие детали (болты, антенны) на дальних уровнях сводятся к одному
треугольнику. Нужен пакет meshoptimizer (pip install meshoptimizer).
"""
import json, os, struct, sys
import numpy as np
import meshoptimizer

MODEL_DIR = sys.argv[1]
PARTS = ['lower', 'upper', 'head']
# доля треугольников, допустимая ошибка (доля размера детали, как в meshoptimizer), без топологии
LEVELS = {1: (0.5, 0.05, False), 2: (0.25, 0.15, True), 3: (0.1, 0.35, True)}
# на уровнях 2 и 3 детали меньше этой доли размера части сводятся к одному треугольнику
SMALL_PART = {2: 0.03, 3: 0.07}

COMPONENT = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def read_glb(path):
    data = open(path, 'rb').read()
    json_length = struct.unpack('<I', data[12:16])[0]
    document = json.loads(data[20:20 + json_length])
    offset = 20 + json_length
    bin_length = struct.unpack('<I', data[offset:offset + 4])[0]
    return document, data[offset + 8:offset + 8 + bin_length]


def write_glb(path, document, binary):
    json_bytes = json.dumps(document, separators=(',', ':')).encode('utf-8')
    json_bytes += b' ' * (-len(json_bytes) % 4)
    binary = bytes(binary) + b'\0' * (-len(binary) % 4)
    with open(path, 'wb') as file:
        file.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(json_bytes) + 8 + len(binary)))
        file.write(struct.pack('<II', len(json_bytes), 0x4E4F534A) + json_bytes)
        file.write(struct.pack('<II', len(binary), 0x004E4942) + binary)


def accessor(document, binary, index):
    info = document['accessors'][index]
    view = document['bufferViews'][info['bufferView']]
    dtype = np.dtype(COMPONENT[info['componentType']])
    width = WIDTH[info['type']]
    start = view.get('byteOffset', 0) + info.get('byteOffset', 0)
    stride = view.get('byteStride', 0) or dtype.itemsize * width
    rows = np.lib.stride_tricks.as_strided(
        np.frombuffer(binary, dtype=np.uint8, count=stride * (info['count'] - 1) + dtype.itemsize * width, offset=start),
        shape=(info['count'], dtype.itemsize * width), strides=(stride, 1))
    array = np.ascontiguousarray(rows).view(dtype)
    return array.reshape(info['count'], width) if width > 1 else array.reshape(info['count'])


def simplify(positions, indices, ratio, error, sloppy, part_size):
    """новый список индексов (те же вершины)."""
    target = max(3, int(len(indices) * ratio) // 3 * 3)
    destination = np.zeros(len(indices), dtype=np.uint32)
    positions = np.ascontiguousarray(positions, dtype=np.float32)
    source = np.ascontiguousarray(indices, dtype=np.uint32)
    if sloppy:
        count = meshoptimizer.simplify_sloppy(destination, source, positions, target_index_count=target, target_error=error)
    else:
        count = meshoptimizer.simplify(destination, source, positions, target_index_count=target, target_error=error)
    if count < 3:
        return source[:3].copy()        # деталь пропала — один треугольник (пустую поверхность конвертер не примет)
    return destination[:count].copy()


def make_level(part, level):
    document, binary = read_glb(os.path.join(MODEL_DIR, part + '.glb'))
    ratio, error, sloppy = LEVELS[level]
    # размер части (по всем примитивам) — для порога мелких деталей
    all_positions = []
    for mesh in document['meshes']:
        for primitive in mesh['primitives']:
            all_positions.append(accessor(document, binary, primitive['attributes']['POSITION']))
    stacked = np.concatenate(all_positions)
    part_size = float(np.linalg.norm(stacked.max(axis=0) - stacked.min(axis=0)))

    mesh_accessors = set()
    for mesh in document['meshes']:
        for primitive in mesh['primitives']:
            mesh_accessors.update(primitive['attributes'].values())
            mesh_accessors.add(primitive['indices'])
    new_binary = bytearray()
    views, accessors, remap, view_remap = [], [], {}, {}

    def add_view(data, target=None):
        new_binary.extend(b'\0' * (-len(new_binary) % 4))
        view = {'buffer': 0, 'byteOffset': len(new_binary), 'byteLength': len(data)}
        if target:
            view['target'] = target
        new_binary.extend(data)
        views.append(view)
        return len(views) - 1

    for index, info in enumerate(document['accessors']):
        if index in mesh_accessors:
            continue
        if info['bufferView'] not in view_remap:
            view = document['bufferViews'][info['bufferView']]
            start = view.get('byteOffset', 0)
            view_remap[info['bufferView']] = add_view(binary[start:start + view['byteLength']])
            if 'byteStride' in view:
                views[-1]['byteStride'] = view['byteStride']
        copy = dict(info)
        copy['bufferView'] = view_remap[info['bufferView']]
        remap[index] = len(accessors)
        accessors.append(copy)

    def add_accessor(array, kind, target, component=5126):
        array = np.ascontiguousarray(array)
        info = {'bufferView': add_view(array.tobytes(), target), 'componentType': component, 'count': len(array), 'type': kind}
        if kind == 'VEC3':
            info['min'] = array.min(axis=0).tolist()
            info['max'] = array.max(axis=0).tolist()
        accessors.append(info)
        return len(accessors) - 1

    # основной материал (окраска) — для слитых деталей дальних уровней
    names = [material['name'] for material in document['materials']]
    paint = names.index('camo_green') if 'camo_green' in names else 0
    before = after = 0
    for mesh in document['meshes']:
        pieces = []
        for primitive in mesh['primitives']:
            attributes = primitive['attributes']
            pieces.append((primitive['material'],
                           accessor(document, binary, attributes['POSITION']).astype(np.float32),
                           accessor(document, binary, attributes['NORMAL']).astype(np.float32),
                           accessor(document, binary, attributes['TEXCOORD_0']).astype(np.float32),
                           accessor(document, binary, primitive['indices']).astype(np.uint32)))
        if sloppy:
            # дальние уровни: все детали узла в одну поверхность с материалом окраски —
            # упрощение склеивает соседние детали, движок рисует меньше поверхностей
            base = 0; positions = []; normals = []; uvs = []; indices = []
            for material, piece_positions, piece_normals, piece_uvs, piece_indices in pieces:
                positions.append(piece_positions); normals.append(piece_normals); uvs.append(piece_uvs)
                indices.append(piece_indices + base); base += len(piece_positions)
            pieces = [(paint, np.concatenate(positions), np.concatenate(normals), np.concatenate(uvs), np.concatenate(indices))]
        primitives = []
        for material, positions, normals, uvs, indices in pieces:
            size = float(np.linalg.norm(positions.max(axis=0) - positions.min(axis=0)))
            if level in SMALL_PART and size < SMALL_PART[level] * part_size:
                kept = indices[:3].copy()
            else:
                kept = simplify(positions, indices, ratio, error, sloppy, part_size)
            used, compact = np.unique(kept, return_inverse=True)
            compact = compact.reshape(-1)
            before += len(indices) // 3
            after += len(kept) // 3
            index_type = np.uint16 if len(used) < 65536 else np.uint32
            primitives.append({'material': material, 'attributes': {
                'POSITION': add_accessor(positions[used], 'VEC3', 34962),
                'NORMAL': add_accessor(normals[used], 'VEC3', 34962),
                'TEXCOORD_0': add_accessor(uvs[used], 'VEC2', 34962),
            }, 'indices': add_accessor(compact.astype(index_type), 'SCALAR', 34963,
                                       5123 if index_type == np.uint16 else 5125)})
        mesh['primitives'] = primitives

    for animation in document.get('animations', []):
        for sampler in animation['samplers']:
            sampler['input'] = remap[sampler['input']]
            sampler['output'] = remap[sampler['output']]
    for skin in document.get('skins', []):
        if 'inverseBindMatrices' in skin:
            skin['inverseBindMatrices'] = remap[skin['inverseBindMatrices']]
    document['accessors'] = accessors
    document['bufferViews'] = views
    document['buffers'] = [{'byteLength': len(new_binary)}]
    write_glb(os.path.join(MODEL_DIR, f'{part}_{level}.glb'), document, new_binary)
    return before, after


for part in PARTS:
    for level in LEVELS:
        before, after = make_level(part, level)
        print(f'{part}_{level}: {before} -> {after} triangles')
