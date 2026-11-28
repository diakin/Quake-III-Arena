"""Проволочная модель K7 (WALKER, 28.11.2026).

python3 walker/tools/k7_wireframe.py [out.png] [папка модели]

Картинка в том же виде, что у черновой модели нового робота (mech_blockout.py): четыре
вида - перспектива, сбоку, спереди, сверху. Берётся настоящая модель K7
(assets/baseq3/models/players/walker: lower.glb, upper.glb, head.glb), собранная как в
игре: корпус на теге tag_torso ног, кабина на теге tag_head корпуса; поза - первый кадр
LEGS_IDLE и TORSO_STAND. Оружия нет (модели орудий отдельные, их ставит игра).

Рисуются рёбра формы, а не все треугольники: края, где соседние грани сходятся под углом
больше FEATURE_ANGLE, края сетки, границы материалов и контур в каждом виде (край, у
которого одна грань смотрит на зрителя, другая от него). Видимость - по буферу глубины:
грани со светлой заливкой цвета группы, видимые рёбра - цветом группы, невидимые - тонким
светло-серым. Группы: ноги, таз, корпус, кабина, гидравлика (цилиндры голеностопа:
узлы knee_actuator_*, материалы chrome и hydraulic_blue).

Единицы - единицы модели (1 = 14 единиц Quake = 2.8 м при 5 единицах Quake на метр),
y вверх, z вперёд, x влево.
"""
import os, sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import k7_hydraulics as glb

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'assets', 'baseq3', 'models', 'players', 'walker')
FEATURE_ANGLE = 35.0
METRES_PER_UNIT = 14 / 5
GROUP_COLOURS = {
    'legs': (60, 110, 40), 'pelvis': (150, 110, 20), 'torso': (40, 80, 150),
    'cockpit': (190, 40, 40), 'hydraulics': (220, 110, 0),
}
HYDRAULIC_MATERIALS = {'chrome', 'hydraulic_blue'}


# ---------------------------------------------------------------- модель

def quat_matrix(quat):
    x_value, y_value, z_value, w_value = quat
    return np.array([
        [1 - 2 * (y_value ** 2 + z_value ** 2), 2 * (x_value * y_value - z_value * w_value), 2 * (x_value * z_value + y_value * w_value)],
        [2 * (x_value * y_value + z_value * w_value), 1 - 2 * (x_value ** 2 + z_value ** 2), 2 * (y_value * z_value - x_value * w_value)],
        [2 * (x_value * z_value - y_value * w_value), 2 * (y_value * z_value + x_value * w_value), 1 - 2 * (x_value ** 2 + y_value ** 2)]])


def world_matrices(document, binary, clip):
    """мировые матрицы узлов в первом кадре анимации clip (или в позе покоя)"""
    local = {}
    for index, node in enumerate(document['nodes']):
        local[index] = {'translation': node.get('translation', [0, 0, 0]), 'rotation': node.get('rotation', [0, 0, 0, 1]),
                        'scale': node.get('scale', [1, 1, 1])}
    for animation in document.get('animations', []):
        if animation['name'] != clip:
            continue
        for channel in animation['channels']:
            values = glb.accessor(document, binary, animation['samplers'][channel['sampler']]['output'])
            local[channel['target']['node']][channel['target']['path']] = values[0].tolist()
    parent = {child: index for index, node in enumerate(document['nodes']) for child in node.get('children', [])}
    cache = {}

    def world(index):
        if index not in cache:
            trs = local[index]
            matrix = np.eye(4)
            matrix[:3, :3] = quat_matrix(trs['rotation']) * np.array(trs['scale'])
            matrix[:3, 3] = trs['translation']
            cache[index] = world(parent[index]) @ matrix if index in parent else matrix
        return cache[index]
    return {index: world(index) for index in range(len(document['nodes']))}, parent


def node_group(document, parent, index, part, material):
    names = [node.get('name', '') for node in document['nodes']]
    if material in HYDRAULIC_MATERIALS or names[index].startswith(('ankle_actuator', 'knee_actuator')):
        return 'hydraulics'
    if part == 'upper':
        return 'torso'
    if part == 'head':
        return 'cockpit'
    current = index
    while current in parent:
        current = parent[current]
        if names[current].startswith('hip_'):
            return 'legs'
    return 'pelvis'


def load_part(part, clip, placement):
    """треугольники части (мировые координаты), их группы и края: (начало, конец, нормаль
    одной грани, нормаль другой или None, группа, ребро формы ли)"""
    document, binary = glb.read_glb(os.path.join(MODEL, f'{part}.glb'))
    worlds, parent = world_matrices(document, binary, clip)
    triangles, groups, edges = [], [], []
    cos_limit = math.cos(math.radians(FEATURE_ANGLE))
    for index, node in enumerate(document['nodes']):
        if 'mesh' not in node:
            continue
        matrix = placement @ worlds[index]
        positions_all, faces_all, materials = [], [], []
        base = 0
        for primitive in document['meshes'][node['mesh']]['primitives']:
            positions = glb.accessor(document, binary, primitive['attributes']['POSITION']).astype(float)
            if 'indices' in primitive:
                faces = glb.accessor(document, binary, primitive['indices']).astype(int).reshape(-1, 3)
            else:
                faces = np.arange(len(positions)).reshape(-1, 3)
            positions = positions @ matrix[:3, :3].T + matrix[:3, 3]
            material = document['materials'][primitive['material']]['name']
            positions_all.append(positions); faces_all.append(faces + base); materials += [material] * len(faces)
            base += len(positions)
        positions = np.concatenate(positions_all); faces = np.concatenate(faces_all)
        # вершины на одном месте - одна (нормали и швы развёртки делят их)
        keys = np.round(positions / 1e-4).astype(np.int64)
        _, weld = np.unique(keys, axis=0, return_inverse=True)
        weld = weld.reshape(-1)
        corners = positions[faces]
        normals = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        lengths = np.linalg.norm(normals, axis=1)
        normals = normals / np.maximum(lengths, 1e-12)[:, None]
        node_groups = [node_group(document, parent, index, part, material) for material in materials]
        edge_faces = {}
        for face_index, face in enumerate(faces):
            if lengths[face_index] < 1e-12:
                continue
            for corner in range(3):
                first, second = weld[face[corner]], weld[face[(corner + 1) % 3]]
                key = (min(first, second), max(first, second))
                edge_faces.setdefault(key, []).append((face_index, face[corner], face[(corner + 1) % 3]))
        for users in edge_faces.values():
            face_a, start, end = users[0]
            if len(users) == 2:
                face_b = users[1][0]
                feature = np.dot(normals[face_a], normals[face_b]) < cos_limit or materials[face_a] != materials[face_b]
                edges.append((positions[start], positions[end], normals[face_a], normals[face_b], node_groups[face_a], feature))
            else:
                edges.append((positions[start], positions[end], normals[face_a], None, node_groups[face_a], True))
        for face_index in range(len(faces)):
            if lengths[face_index] >= 1e-12:
                triangles.append(corners[face_index]); groups.append(node_groups[face_index])
    tags = {document['nodes'][index].get('name'): placement @ worlds[index]
            for index in range(len(document['nodes'])) if document['nodes'][index].get('name', '').startswith('tag_')}
    return triangles, groups, edges, tags


def build():
    lower = load_part('lower', 'LEGS_IDLE', np.eye(4))
    upper = load_part('upper', 'TORSO_STAND', lower[3]['tag_torso'])
    head = load_part('head', None, upper[3]['tag_head'])
    triangles, groups, edges = [], [], []
    for part in (lower, upper, head):
        triangles += part[0]; groups += part[1]; edges += part[2]
    triangles = np.array(triangles)
    floor = triangles[:, :, 1].min()
    triangles[:, :, 1] -= floor
    shift = np.array([0, floor, 0])
    edges = [(start - shift, end - shift, normal_a, normal_b, group, feature) for start, end, normal_a, normal_b, group, feature in edges]
    return triangles, groups, edges


# ---------------------------------------------------------------- картинка

VIEWER = {'front': (0, 0, 1), 'side': (-1, 0, 0), 'top': (0, 1, 0)}


def project_ortho(points, view):
    """(x, y на картинке, глубина: больше - ближе к зрителю)"""
    if view == 'front':      # смотрим против z: левая сторона робота справа на картинке
        return np.stack([points[..., 0], points[..., 1], points[..., 2]], -1)
    if view == 'side':       # с правого бока (из -x): вперёд - вправо
        return np.stack([points[..., 2], points[..., 1], -points[..., 0]], -1)
    if view == 'top':        # сверху: вперёд - вверх
        return np.stack([points[..., 0], points[..., 2], points[..., 1]], -1)
    raise ValueError(view)


def ortho(view):
    def project(points):
        return project_ortho(points, view)
    project.towards_viewer = lambda points: np.broadcast_to(np.array(VIEWER[view], float), np.shape(points))
    return project


def perspective(centre, distance, yaw=35.0, pitch=18.0):
    yaw_rad = math.radians(yaw); pitch_rad = math.radians(pitch)
    rotate_yaw = np.array([[math.cos(yaw_rad), 0, math.sin(yaw_rad)], [0, 1, 0], [-math.sin(yaw_rad), 0, math.cos(yaw_rad)]])
    rotate_pitch = np.array([[1, 0, 0], [0, math.cos(pitch_rad), -math.sin(pitch_rad)], [0, math.sin(pitch_rad), math.cos(pitch_rad)]])
    rotation = rotate_pitch @ rotate_yaw
    eye = np.array(centre) + rotation.T @ np.array([0, 0, distance])

    def project(points):
        camera = (points - np.array(centre)) @ rotation.T
        depth = distance - camera[..., 2]
        return np.stack([camera[..., 0] / depth * distance, camera[..., 1] / depth * distance, -depth], -1)
    project.towards_viewer = lambda points: eye - points
    return project


def rasterize(pixels, depths, width, height):
    """буфер глубины и номер ближайшей грани в каждой точке картинки"""
    depth_buffer = np.full((height, width), -np.inf)
    face_buffer = np.full((height, width), -1, dtype=np.int64)
    for face_index in range(len(pixels)):
        (x0, y0), (x1, y1), (x2, y2) = pixels[face_index]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        left, right = max(int(math.floor(min(x0, x1, x2))), 0), min(int(math.ceil(max(x0, x1, x2))), width - 1)
        top, bottom = max(int(math.floor(min(y0, y1, y2))), 0), min(int(math.ceil(max(y0, y1, y2))), height - 1)
        if left > right or top > bottom:
            continue
        grid_y, grid_x = np.mgrid[top:bottom + 1, left:right + 1] + 0.5
        weight_0 = ((x1 - grid_x) * (y2 - grid_y) - (x2 - grid_x) * (y1 - grid_y)) / area
        weight_1 = ((x2 - grid_x) * (y0 - grid_y) - (x0 - grid_x) * (y2 - grid_y)) / area
        weight_2 = 1 - weight_0 - weight_1
        inside = (weight_0 >= -1e-6) & (weight_1 >= -1e-6) & (weight_2 >= -1e-6)
        if not inside.any():
            continue
        depth = weight_0 * depths[face_index][0] + weight_1 * depths[face_index][1] + weight_2 * depths[face_index][2]
        window = depth_buffer[top:bottom + 1, left:right + 1]
        closer = inside & (depth > window)
        window[closer] = depth[closer]
        face_buffer[top:bottom + 1, left:right + 1][closer] = face_index
    return depth_buffer, face_buffer


def draw_panel(size, title, projector, triangles, groups, edges, ground_lines):
    """грани по буферу глубины (светлая заливка цвета группы, чуть темнее по наклону к
    свету); края формы и контур: видимые - цветом группы, невидимые - светло-серым"""
    width, height = size
    projected = projector(triangles)
    low = projected[..., :2].reshape(-1, 2).min(0); high = projected[..., :2].reshape(-1, 2).max(0)
    margin = 30
    scale = min((width - 2 * margin) / (high[0] - low[0]), (height - 2 * margin - 20) / (high[1] - low[1]))
    offset_x = margin + ((width - 2 * margin) - (high[0] - low[0]) * scale) / 2

    def to_pixels(points):
        return np.stack([offset_x + (points[..., 0] - low[0]) * scale, height - margin - (points[..., 1] - low[1]) * scale], -1)
    pixels = to_pixels(projected)
    depth_buffer, face_buffer = rasterize(pixels, projected[..., 2], width, height)
    # заливка
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    normals /= np.maximum(np.linalg.norm(normals, axis=1), 1e-12)[:, None]
    light = np.array([0.4, 0.8, 0.45]); light /= np.linalg.norm(light)
    shade = 0.75 + 0.25 * np.abs(normals @ light)
    colours = np.array([GROUP_COLOURS[group] for group in groups], float)
    fills = 255 - (255 - colours) * 0.20
    fills = np.clip(fills * shade[:, None] + 255 * 0.02, 0, 255)
    image_array = np.full((height, width, 3), (250, 250, 247), dtype=np.uint8)
    covered = face_buffer >= 0
    image_array[covered] = fills[face_buffer[covered]].astype(np.uint8)
    image = Image.fromarray(image_array)
    draw = ImageDraw.Draw(image)
    for start, end in ground_lines:
        line = to_pixels(projector(np.array([start, end], float)))
        draw.line([tuple(line[0]), tuple(line[1])], fill=(205, 205, 205), width=1)
    # края: формы всегда, контур - где одна грань к зрителю, другая от него
    visible_lines, hidden_lines = [], []
    tolerance = (projected[..., 2].max() - projected[..., 2].min()) * 0.004
    for start, end, normal_a, normal_b, group, feature in edges:
        if not feature:
            middle = (start + end) / 2
            towards = projector.towards_viewer(middle)
            if np.sign(np.dot(normal_a, towards)) == np.sign(np.dot(normal_b, towards)):
                continue
        ends = projector(np.array([start, end]))
        ends_pixels = to_pixels(ends)
        steps = max(2, int(np.linalg.norm(ends_pixels[1] - ends_pixels[0]) / 1.5) + 1)
        share = np.linspace(0, 1, steps)
        points = ends_pixels[0] + (ends_pixels[1] - ends_pixels[0]) * share[:, None]
        depth = ends[0, 2] + (ends[1, 2] - ends[0, 2]) * share
        column = np.clip(points[:, 0].astype(int), 0, width - 1); row = np.clip(points[:, 1].astype(int), 0, height - 1)
        nearest = depth_buffer[row, column]
        for shift_row, shift_column in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            nearest = np.minimum(nearest, depth_buffer[np.clip(row + shift_row, 0, height - 1), np.clip(column + shift_column, 0, width - 1)])
        visible = depth >= nearest - tolerance
        run_start = 0
        for index in range(1, steps + 1):
            if index == steps or visible[index] != visible[run_start]:
                segment = [tuple(points[run_start]), tuple(points[index - 1])]
                (visible_lines if visible[run_start] else hidden_lines).append((segment, GROUP_COLOURS[group]))
                run_start = index
    for segment, _ in hidden_lines:
        draw.line(segment, fill=(212, 212, 212), width=1)
    for segment, colour in visible_lines:
        draw.line(segment, fill=colour, width=1)
    draw.rectangle((0, 0, width - 1, height - 1), outline=(170, 170, 170))
    draw.text((8, 6), title, fill=(30, 30, 30), font=ImageFont.load_default())
    return image


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else 'k7_wireframe.png'
    triangles, groups, edges = build()
    points = triangles.reshape(-1, 3)
    low, high = points.min(0), points.max(0)
    size = high - low
    centre = (low + high) / 2
    width, height = 1600, 1250
    image = Image.new('RGB', (width, height), (250, 250, 247))
    span = max(size) * 0.9
    ground = [((low[0] - span * 0.3, 0, z_value), (high[0] + span * 0.3, 0, z_value)) for z_value in np.linspace(low[2] - span * 0.3, high[2] + span * 0.3, 10)] + \
             [((x_value, 0, low[2] - span * 0.3), (x_value, 0, high[2] + span * 0.3)) for x_value in np.linspace(low[0] - span * 0.3, high[0] + span * 0.3, 9)]
    half_width, half_height = width // 2, (height - 50) // 2
    panels = [
        ((0, 0, half_width, half_height), 'perspective (front-right)', perspective(centre, max(size) * 3.2), ground),
        ((half_width, 0, width, half_height), 'side (from the right)', ortho('side'),
         [((0, 0, low[2] - span * 0.2), (0, 0, high[2] + span * 0.2))]),
        ((0, half_height, half_width, 2 * half_height), 'front', ortho('front'),
         [((low[0] - span * 0.2, 0, 0), (high[0] + span * 0.2, 0, 0))]),
        ((half_width, half_height, width, 2 * half_height), 'top (front is up)', ortho('top'), []),
    ]
    for box, title, projector, lines in panels:
        panel = draw_panel((box[2] - box[0], box[3] - box[1]), title, projector, triangles, groups, edges, lines)
        image.paste(panel, (box[0], box[1]))
    draw = ImageDraw.Draw(image)
    legend_x = 10
    for group, colour in GROUP_COLOURS.items():
        if group not in groups:
            continue
        draw.rectangle((legend_x, height - 35, legend_x + 14, height - 21), fill=colour)
        draw.text((legend_x + 20, height - 34), group, fill=(30, 30, 30))
        legend_x += 120
    metres = size * METRES_PER_UNIT
    draw.text((legend_x + 20, height - 34),
              f'K7, no weapons. size: width {size[0]:.2f}  height {size[1]:.2f}  length {size[2]:.2f} model units '
              f'({metres[0]:.1f} x {metres[1]:.1f} x {metres[2]:.1f} m)', fill=(30, 30, 30))
    image.save(target)
    print(target, f'{len(triangles)} triangles, {len(edges)} edges, size {size.round(2)}')


if __name__ == '__main__':
    main()
