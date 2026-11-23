"""Детальная геометрия карты: модели (MD3), которые q3map встраивает в карту (misc_model).

Подключается из make_map_islands.py. Каждая постройка строится как сетка треугольников
с материалами (текстуры карты), записывается в <COMPILE>/models/walker_props/<имя>.md3, а в
карту добавляется сущность misc_model. q3map превращает её в поверхности карты с освещением
по вершинам (поэтому большие грани разбиваются на ячейки до MAX_EDGE единиц) и тенями.
Коллизия у моделей своя не считается: make_map_islands.py ставит рядом невидимые браши
(textures/walker/clip_solid).

Что здесь есть:
  rock_cluster      валуны из искажённых шумом икосфер, с пластами; верх обрастает мхом
  stones            россыпь мелких камней
  ruined_wall       стена с оконными проёмами, рваным верхом, арматурой и обломками у подножия
  shipping_container контейнер с гофрой, дверями с запорами, рамой и угловыми фитингами
  beam, ladder, railing, pipe, lattice_mast, x_braces — детали ферм, башен и платформ
"""
import math, os, struct
import numpy as np

MAX_EDGE = 28.0                 # грани крупнее разбиваются: освещение считается по вершинам
SURFACE_VERTS = 900             # не больше вершин в одной поверхности (буфер движка 1000)
PROPS = []                      # (имя, начало координат, Mesh)
TILE = {'rock': 256, 'rockmoss': 256, 'container_red': 128, 'container_blue': 128,
        'container_orange': 128}  # единиц на повтор текстуры; остальные 128


# ================================================================ сетка

class Mesh:
    """треугольники по материалам: вершины, нормали; UV ставятся при записи (по проекции)."""

    def __init__(self):
        self.parts = {}         # материал -> [positions (n,3), normals (n,3), faces (m,3)]

    def _part(self, material):
        if material not in self.parts:
            self.parts[material] = [[], [], [], 0]
        return self.parts[material]

    def add(self, material, positions, normals, faces):
        part = self._part(material)
        positions = np.asarray(positions, dtype=float)
        part[0].append(positions)
        part[1].append(np.asarray(normals, dtype=float))
        part[2].append(np.asarray(faces, dtype=np.int64) + part[3])
        part[3] += len(positions)

    def extend(self, other):
        for material, (positions, normals, faces, _) in other.parts.items():
            if positions:
                self.add(material, np.concatenate(positions), np.concatenate(normals), np.concatenate(faces) - 0)
        return self

    def arrays(self, material):
        positions, normals, faces, _ = self.parts[material]
        return np.concatenate(positions), np.concatenate(normals), np.concatenate(faces)

    def bounds(self):
        points = np.concatenate([np.concatenate(part[0]) for part in self.parts.values() if part[0]])
        return points.min(axis=0), points.max(axis=0)

    def triangle_count(self):
        return sum(sum(len(faces) for faces in part[2]) for part in self.parts.values())


# ================================================================ формы для теней

# Тени построек и моделей от солнца рисует игра (теневые объёмы, renderer/tr_sunshadow.c) по
# выпуклым формам; q3map их солнцем не учитывает (q3map_nosunshadow). Уровни: CASTER_BOTH —
# в простых и полных тенях, CASTER_SIMPLE — только в простых (крупные оболочки),
# CASTER_FULL — только в полных (тонкие элементы, отдельные колонны стен).
CASTER_BOTH, CASTER_SIMPLE, CASTER_FULL = 0, 1, 2
CASTERS = []
_caster_pause = [0]


def caster(points, level=CASTER_BOTH):
    """выпуклая форма для тени: вершины, оболочка строится при записи; мелочь не нужна."""
    if _caster_pause[0]:
        return
    points = np.asarray(points, dtype=float).reshape(-1, 3)
    extent = points.max(axis=0) - points.min(axis=0)
    if extent.max() < 6:
        return
    CASTERS.append((level, points))


def thin_level(thickness):
    """тонкие детали (перила, раскосы, ступени) — только в полных тенях."""
    return CASTER_BOTH if thickness >= 4 else CASTER_FULL


def quad(mesh, material, p0, p1, p2, p3, max_edge=MAX_EDGE):
    """плоский четырёхугольник p0-p1-p2-p3 (против часовой стрелки снаружи), сетка ячеек."""
    p0, p1, p2, p3 = [np.asarray(point, dtype=float) for point in (p0, p1, p2, p3)]
    normal = np.cross(p1 - p0, p3 - p0)
    length = np.linalg.norm(normal)
    if length < 1e-9:
        normal = np.cross(p2 - p1, p3 - p1)
        length = np.linalg.norm(normal)
        if length < 1e-9:
            return
    normal /= length
    columns = max(1, int(math.ceil(max(np.linalg.norm(p1 - p0), np.linalg.norm(p2 - p3)) / max_edge)))
    rows = max(1, int(math.ceil(max(np.linalg.norm(p3 - p0), np.linalg.norm(p2 - p1)) / max_edge)))
    u = np.linspace(0, 1, columns + 1)
    v = np.linspace(0, 1, rows + 1)
    grid_u, grid_v = np.meshgrid(u, v)
    grid_u = grid_u.reshape(-1, 1); grid_v = grid_v.reshape(-1, 1)
    positions = (p0 * (1 - grid_u) * (1 - grid_v) + p1 * grid_u * (1 - grid_v)
                 + p2 * grid_u * grid_v + p3 * (1 - grid_u) * grid_v)
    faces = []
    for row in range(rows):
        for column in range(columns):
            a = row * (columns + 1) + column
            b = a + 1
            c = a + columns + 2
            d = a + columns + 1
            faces += [(a, b, c), (a, c, d)]
    mesh.add(material, positions, np.repeat(normal[None, :], len(positions), axis=0), faces)


def triangle(mesh, material, p0, p1, p2):
    p0, p1, p2 = [np.asarray(point, dtype=float) for point in (p0, p1, p2)]
    normal = np.cross(p1 - p0, p2 - p0)
    if np.linalg.norm(normal) < 1e-9:
        return
    normal /= np.linalg.norm(normal)
    mesh.add(material, [p0, p1, p2], [normal] * 3, [(0, 1, 2)])


def box(mesh, material, x0, y0, z0, x1, y1, z1, top=None, bottom=False, max_edge=MAX_EDGE):
    """прямоугольный блок; top — материал верха; низ по умолчанию не рисуется (стоит на земле)."""
    x0, x1 = sorted((x0, x1)); y0, y1 = sorted((y0, y1)); z0, z1 = sorted((z0, z1))
    top = top or material
    caster([(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)], thin_level(min(x1 - x0, y1 - y0, z1 - z0)))
    quad(mesh, top, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), max_edge)
    if bottom:
        quad(mesh, material, (x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0), max_edge)
    quad(mesh, material, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), max_edge)
    quad(mesh, material, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1), max_edge)
    quad(mesh, material, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), max_edge)
    quad(mesh, material, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1), max_edge)


def oriented_box(mesh, material, center, axis_x, axis_y, axis_z, half, bottom=True, max_edge=MAX_EDGE):
    """блок с произвольными осями (единичные векторы) и полуразмерами half."""
    center = np.asarray(center, dtype=float)
    axes = [np.asarray(axis, dtype=float) * size for axis, size in zip((axis_x, axis_y, axis_z), half)]
    corner = lambda sx, sy, sz: center + sx * axes[0] + sy * axes[1] + sz * axes[2]
    caster([corner(sx, sy, sz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)], thin_level(2 * min(half)))
    quad(mesh, material, corner(-1, -1, 1), corner(1, -1, 1), corner(1, 1, 1), corner(-1, 1, 1), max_edge)
    if bottom:
        quad(mesh, material, corner(-1, 1, -1), corner(1, 1, -1), corner(1, -1, -1), corner(-1, -1, -1), max_edge)
    quad(mesh, material, corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), corner(-1, -1, 1), max_edge)
    quad(mesh, material, corner(1, 1, -1), corner(-1, 1, -1), corner(-1, 1, 1), corner(1, 1, 1), max_edge)
    quad(mesh, material, corner(-1, 1, -1), corner(-1, -1, -1), corner(-1, -1, 1), corner(-1, 1, 1), max_edge)
    quad(mesh, material, corner(1, -1, -1), corner(1, 1, -1), corner(1, 1, 1), corner(1, -1, 1), max_edge)


def beam(mesh, material, start, end, width, height=None, up=(0, 0, 1)):
    """брус квадратного (или прямоугольного) сечения между двумя точками."""
    start = np.asarray(start, dtype=float); end = np.asarray(end, dtype=float)
    direction = end - start
    length = np.linalg.norm(direction)
    if length < 1e-6:
        return
    axis_x = direction / length
    up = np.asarray(up, dtype=float)
    if abs(np.dot(up, axis_x)) > 0.95:
        up = np.array([1.0, 0, 0]) if abs(axis_x[0]) < 0.9 else np.array([0, 1.0, 0])
    axis_y = np.cross(up, axis_x); axis_y /= np.linalg.norm(axis_y)
    axis_z = np.cross(axis_x, axis_y)
    height = height or width
    oriented_box(mesh, material, (start + end) / 2, axis_x, axis_y, axis_z, (length / 2, width / 2, height / 2), True, 64)


def cylinder(mesh, material, start, end, radius, sides=8, caps=True, radius_end=None):
    """цилиндр (или усечённый конус) между двумя точками, гладкие боковые нормали."""
    start = np.asarray(start, dtype=float); end = np.asarray(end, dtype=float)
    radius_end = radius if radius_end is None else radius_end
    axis = end - start
    length = np.linalg.norm(axis)
    axis /= length
    helper = np.array([0, 0, 1.0]) if abs(axis[2]) < 0.9 else np.array([1.0, 0, 0])
    side_u = np.cross(axis, helper); side_u /= np.linalg.norm(side_u)
    side_v = np.cross(axis, side_u)
    segments = max(1, int(math.ceil(length / 64)))
    angles = np.linspace(0, 2 * math.pi, sides + 1)
    positions, normals, faces = [], [], []
    for row in range(segments + 1):
        fraction = row / segments
        center = start + (end - start) * fraction
        current = radius + (radius_end - radius) * fraction
        for angle in angles:
            offset = math.cos(angle) * side_u + math.sin(angle) * side_v
            positions.append(center + offset * current)
            slope = (radius - radius_end) / length
            normal = offset + axis * slope
            normals.append(normal / np.linalg.norm(normal))
    for row in range(segments):
        for column in range(sides):
            a = row * (sides + 1) + column
            b = a + 1
            c = a + sides + 2
            d = a + sides + 1
            faces += [(a, b, c), (a, c, d)]
    mesh.add(material, positions, normals, faces)
    caster(positions, thin_level(2 * min(radius, radius_end)))
    if caps:
        for center, current, sign in ((end, radius_end, 1), (start, radius, -1)):
            ring = [center + (math.cos(angle) * side_u + math.sin(angle) * side_v) * current for angle in angles[:-1]]
            cap_faces = [(0, index, index + 1) if sign > 0 else (0, index + 1, index) for index in range(1, sides - 1)]
            mesh.add(material, ring, [axis * sign] * len(ring), cap_faces)


# ================================================================ шум

PERMUTATION = np.random.default_rng(91).permutation(256)
PERMUTATION = np.concatenate([PERMUTATION, PERMUTATION])
LATTICE = np.random.default_rng(92).random(256)


def value_noise(points):
    points = np.asarray(points, dtype=float)
    cell = np.floor(points).astype(np.int64)
    frac = points - cell
    fade = frac * frac * (3 - 2 * frac)
    cell &= 255
    result = 0.0
    for corner in range(8):
        offset = np.array([(corner >> 0) & 1, (corner >> 1) & 1, (corner >> 2) & 1])
        index = PERMUTATION[PERMUTATION[PERMUTATION[(cell[:, 0] + offset[0]) & 255] + ((cell[:, 1] + offset[1]) & 255)] + ((cell[:, 2] + offset[2]) & 255)]
        weight = np.prod(np.where(offset[None, :] == 1, fade, 1 - fade), axis=1)
        result = result + weight * LATTICE[index]
    return result


def fbm(points, octaves=4):
    total, amplitude, norm = 0.0, 1.0, 0.0
    for octave in range(octaves):
        total = total + amplitude * value_noise(points * (2 ** octave) + octave * 13.7)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


# ================================================================ камни

def icosphere(level):
    golden = (1 + 5 ** 0.5) / 2
    vertices = [(-1, golden, 0), (1, golden, 0), (-1, -golden, 0), (1, -golden, 0), (0, -1, golden), (0, 1, golden),
                (0, -1, -golden), (0, 1, -golden), (golden, 0, -1), (golden, 0, 1), (-golden, 0, -1), (-golden, 0, 1)]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
             (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
             (8, 6, 7), (9, 8, 1)]
    vertices = [np.array(vertex, dtype=float) / np.linalg.norm(vertex) for vertex in vertices]
    for _ in range(level):
        cache = {}
        def middle(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                point = vertices[a] + vertices[b]
                vertices.append(point / np.linalg.norm(point))
                cache[key] = len(vertices) - 1
            return cache[key]
        refined = []
        for a, b, c in faces:
            ab, bc, ca = middle(a, b), middle(b, c), middle(c, a)
            refined += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = refined
    return np.array(vertices), np.array(faces)


def vertex_normals(positions, faces):
    normals = np.zeros_like(positions)
    face_normals = np.cross(positions[faces[:, 1]] - positions[faces[:, 0]], positions[faces[:, 2]] - positions[faces[:, 0]])
    for corner in range(3):
        np.add.at(normals, faces[:, corner], face_normals)
    return normals / (np.linalg.norm(normals, axis=1, keepdims=True) + 1e-12)


def boulder(mesh, center, radius_x, radius_y, height, seed, sink, level=3, roughness=0.32):
    """один валун: икосфера, вытянутая по осям, с крупными сколами, пластами и мелкой шероховатостью.
    Нижняя часть уходит в землю на sink единиц (там вершины прижимаются)."""
    local = np.random.default_rng(seed)
    unit, faces = icosphere(level)
    offset = local.random(3) * 50
    big = fbm(unit * 1.3 + offset, 3) - 0.5
    small = fbm(unit * 4.5 + offset * 2, 3) - 0.5
    # сколы: несколько плоскостей срезают объём — грани скалы становятся плоскими
    scale = np.ones(len(unit))
    for _ in range(local.integers(9, 15)):
        normal = local.normal(size=3); normal[2] *= 0.6; normal /= np.linalg.norm(normal)
        level_cut = local.uniform(0.55, 0.82)
        projection = unit @ normal
        scale = np.minimum(scale, np.where(projection > level_cut, level_cut / np.maximum(projection, 1e-6), 1.0))
    strata = 0.035 * np.round(np.sin(unit[:, 2] * local.uniform(7, 11) + big * 4) * 1.5) / 1.5
    radial = scale * (1 + roughness * 0.6 * big + 0.05 * small + strata)
    positions = unit * radial[:, None] * np.array([radius_x, radius_y, height / 2])
    yaw = local.uniform(0, 2 * math.pi)
    rotation = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
    positions = positions @ rotation.T
    positions[:, 2] += height / 2 - sink
    # подошва: всё ниже земли сплющивается
    positions[:, 2] = np.maximum(positions[:, 2], -sink - 4 + 0.15 * (positions[:, 2] + sink))
    positions += np.asarray(center, dtype=float)
    caster(positions)
    # плоское затенение граней: скол читается как скол, а не как гладкий бок
    face_normals = np.cross(positions[faces[:, 1]] - positions[faces[:, 0]], positions[faces[:, 2]] - positions[faces[:, 0]])
    face_normals /= np.linalg.norm(face_normals, axis=1, keepdims=True) + 1e-12
    smooth = vertex_normals(positions, faces)
    mossy = (face_normals[:, 2] > 0.62) & (fbm(positions[faces].mean(axis=1) / 40.0, 2) > 0.45)
    for material, selected in (('rock', ~mossy), ('rockmoss', mossy)):
        if np.any(selected):
            chosen = faces[selected]
            corner_positions = positions[chosen].reshape(-1, 3)
            # нормаль: среднее плоской и гладкой — грани видны, но без резких полос
            corner_normals = (np.repeat(face_normals[selected], 3, axis=0) * 0.7 + smooth[chosen].reshape(-1, 3) * 0.3)
            corner_normals /= np.linalg.norm(corner_normals, axis=1, keepdims=True)
            mesh.add(material, corner_positions, corner_normals, np.arange(len(corner_positions)).reshape(-1, 3))


def rock_cluster(center, height, radius, seed):
    """скальный выход: главный валун и 2-4 поменьше вокруг. Возвращает сетку и
    (радиус, высоту) для невидимого браша коллизии главного валуна."""
    local = np.random.default_rng(seed)
    mesh = Mesh()
    boulder(mesh, center, radius * local.uniform(0.9, 1.1), radius * local.uniform(0.75, 0.95), height, seed, 18)
    for index in range(local.integers(2, 5)):
        angle = local.uniform(0, 2 * math.pi)
        distance = radius * local.uniform(0.8, 1.3)
        size = radius * local.uniform(0.3, 0.55)
        spot = np.asarray(center, dtype=float) + [math.cos(angle) * distance, math.sin(angle) * distance, 0]
        boulder(mesh, spot, size, size * local.uniform(0.7, 1.0), size * local.uniform(0.9, 1.6), seed * 7 + index, size * 0.3, 2)
    return mesh


def stones(points, seed, material='rock'):
    """россыпь мелких камней: points — список (x, y, z, радиус)."""
    local = np.random.default_rng(seed)
    mesh = Mesh()
    for index, (x, y, z, radius) in enumerate(points):
        boulder(mesh, (x, y, z), radius * local.uniform(0.8, 1.2), radius * local.uniform(0.6, 1.0),
                radius * local.uniform(0.7, 1.3), seed * 101 + index, radius * 0.2, 1, 0.25)
    return mesh


# ================================================================ скруглённые формы

def _signed_distance(inside, step):
    """расстояние до границы в единицах: снаружи положительное, внутри отрицательное."""
    from scipy.ndimage import distance_transform_edt
    return (distance_transform_edt(~inside) - distance_transform_edt(inside)) * step


def _split_long_edges(positions, normals, faces, max_edge):
    """делит рёбра длиннее max_edge пополам (в обоих треугольниках ребра, без Т-стыков):
    освещение моделей считается по вершинам, длинным треугольникам нужны промежуточные."""
    positions = [np.asarray(point, dtype=float) for point in positions]
    normals = [np.asarray(normal, dtype=float) for normal in normals]
    faces = [list(face) for face in faces]
    while True:
        edges = {}
        for index, face in enumerate(faces):
            for corner in range(3):
                key = tuple(sorted((face[corner], face[(corner + 1) % 3])))
                edges.setdefault(key, []).append(index)
        longest = {}
        for (first, second), owners in edges.items():
            length = np.linalg.norm(positions[first] - positions[second])
            if length > max_edge:
                for owner in owners:
                    if owner not in longest or length > longest[owner][0]:
                        longest[owner] = (length, (first, second))
        if not longest:
            break
        # каждое выбранное ребро делится один раз; треугольник — по своему самому длинному
        chosen = {edge for _, edge in longest.values()}
        midpoints = {}
        for first, second in chosen:
            midpoints[(first, second)] = len(positions)
            positions.append((positions[first] + positions[second]) / 2)
            normal = normals[first] + normals[second]
            normals.append(normal / (np.linalg.norm(normal) + 1e-9))
        result = []
        for face in faces:
            for corner in range(3):
                key = tuple(sorted((face[corner], face[(corner + 1) % 3])))
                if key in midpoints:
                    a, b, c = face[corner], face[(corner + 1) % 3], face[(corner + 2) % 3]
                    middle = midpoints[key]
                    result += [[a, middle, c], [middle, b, c]]
                    break
            else:
                result.append(face)
        faces = result
    return np.array(positions), np.array(normals), np.array(faces)


def rounded_surface(inside, step, radius_convex, radius_concave, error=0.15, max_edge=24, displacement=None):
    """поверхность тела, заданного вокселями inside (шаг step), со скруглёнными рёбрами и углами:
    выпуклые — радиус radius_convex (морфологическое открытие поля расстояний), вогнутые
    (углы проёмов, надломы) — radius_concave (закрытие). Marching cubes, затем упрощение
    плоских участков (meshoptimizer, допуск error) и деление длинных рёбер. displacement
    (единицы, та же сетка) прибавляется к полю после скругления — неровности излома.
    Возвращает вершины (в единицах от угла сетки), нормали, треугольники против часовой
    стрелки снаружи."""
    from scipy.ndimage import gaussian_filter
    from skimage.measure import marching_cubes
    import meshoptimizer
    distance = _signed_distance(inside, step)
    distance = _signed_distance(distance <= -radius_convex, step) - radius_convex
    if radius_concave > 0:
        distance = _signed_distance(distance <= radius_concave, step) + radius_concave
    if displacement is not None:
        distance = distance + displacement          # бугристый излом
    # мелкие оторванные куски (тонкая перемычка над проёмом после скругления) — убрать
    from scipy.ndimage import label
    pieces, count = label(distance < 0)
    if count > 1:
        sizes = np.bincount(pieces.ravel())
        small = np.nonzero(sizes < max(400, sizes[1:].max() * 0.02))[0]
        small = small[small > 0]
        distance[np.isin(pieces, small)] = step
    distance = gaussian_filter(distance, 0.7)
    positions, faces, _, _ = marching_cubes(distance, 0.0, spacing=(step, step, step))
    destination = np.zeros(faces.size, dtype=np.uint32)
    count = meshoptimizer.simplify(destination, np.ascontiguousarray(faces.reshape(-1), dtype=np.uint32),
                                   np.ascontiguousarray(positions, dtype=np.float32), target_index_count=0,
                                   target_error=error, options=meshoptimizer.SIMPLIFY_ERROR_ABSOLUTE)
    faces = destination[:count].reshape(-1, 3).astype(np.int64)
    used = np.unique(faces)
    remap = np.full(len(positions), -1)
    remap[used] = np.arange(len(used))
    positions, faces = positions[used], remap[faces]
    # нормали — по градиенту поля (гладкие на скруглениях)
    gradient = np.stack(np.gradient(distance), axis=-1)
    cell = np.clip(np.round(positions / step).astype(int), 0, np.array(distance.shape) - 1)
    normals = gradient[cell[:, 0], cell[:, 1], cell[:, 2]]
    normals /= np.linalg.norm(normals, axis=1, keepdims=True) + 1e-9
    # обход: нормаль треугольника наружу, как градиент
    face_normals = np.cross(positions[faces[:, 1]] - positions[faces[:, 0]], positions[faces[:, 2]] - positions[faces[:, 0]])
    if np.sum(face_normals * normals[faces].mean(axis=1)) < 0:
        faces = faces[:, ::-1]
    return _split_long_edges(positions, normals, faces, max_edge)


# ================================================================ разрушенная стена

def chunk(mesh, material, center, size, seed):
    """обломок бетона: искажённый блок."""
    local = np.random.default_rng(seed)
    yaw, tilt = local.uniform(0, math.pi), local.uniform(-0.5, 0.5)
    axis_x = np.array([math.cos(yaw), math.sin(yaw), tilt * 0.3]); axis_x /= np.linalg.norm(axis_x)
    axis_z = np.array([0, tilt * 0.5, 1.0]); axis_z -= axis_x * np.dot(axis_z, axis_x); axis_z /= np.linalg.norm(axis_z)
    axis_y = np.cross(axis_z, axis_x)
    half = size * local.uniform(0.5, 1.0, 3) * np.array([1, 0.8, 0.5])
    oriented_box(mesh, material, center, axis_x, axis_y, axis_z, half, False, 64)


WALL_EDGE_RADIUS = 1.5       # скругление рёбер опалубки (несколько полос вместо острого ребра)
WALL_CORNER_RADIUS = 1.5     # скругление внутренних углов (проёмы, надломы)
BREAK_DEPTH = 7.0            # излом: зона от рваного верха вниз, где поверхность бугристая
BREAK_ROUGHNESS = 1.6        # неровность излома (единицы, среднеквадратично)
REBAR_SHARE = 0.4            # доля мест, где из излома выходит стержень арматуры
WALL_MESH_ERROR = 0.3        # допуск упрощения сетки стены (единицы)


def ruined_wall(start, end, base_z, height, thickness, seed, windows=True, material='concrete'):
    """стена из колонн шириной ~12 единиц: верх рваный (ломаная линия), окна — пропуски в колоннах
    между подоконником и перемычкой.
    Возвращает сетку и список невидимых блоков коллизии (вдоль стены, по низу излома)."""
    local = np.random.default_rng(seed)
    start = np.array([start[0], start[1], 0.0]); end = np.array([end[0], end[1], 0.0])
    along = end - start
    length = np.linalg.norm(along)
    along /= length
    across = np.array([-along[1], along[0], 0.0])
    columns = max(2, int(round(length / 12)))
    # профиль излома: плавная волна + резкие провалы
    u = np.linspace(0, 1, columns + 1)
    profile = (0.72 + 0.22 * np.sin(u * local.uniform(5, 9) + local.uniform(0, 6))
               + 0.18 * (local.random(columns + 1) - 0.5))
    for _ in range(local.integers(0, 3)):
        middle = local.uniform(0.15, 0.85); width = local.uniform(0.06, 0.16)
        profile -= local.uniform(0.25, 0.55) * np.clip(1 - np.abs(u - middle) / width, 0, 1)
    profile[0] = max(profile[0], 0.75); profile[-1] = max(profile[-1], 0.6)   # углы держатся
    tops = base_z + height * np.clip(profile, 0.12, 1.0)
    bottom = base_z - 24
    # окна: полосы колонн
    openings = []
    if windows and height > 70:
        sill, lintel = base_z + height * 0.35, base_z + height * 0.35 + min(46, height * 0.4)
        position = local.uniform(1.5, 3.0)
        while position + 4 < columns - 1.5:
            span = int(local.integers(3, 5))
            openings.append((int(position), int(position) + span, sill, lintel))
            position += span + local.uniform(3, 6)
    mesh = Mesh()
    half = thickness / 2
    runs = {}               # простые тени: части колонн по 4 подряд в одну оболочку
    for column in range(columns):
        u0, u1 = column * length / columns, (column + 1) * length / columns
        top0, top1 = tops[column], tops[column + 1]
        lower_top = None
        for first, last, sill, lintel in openings:
            if first <= column < last:
                lower_top = sill
                upper_bottom = lintel
        pieces = [(bottom, None)] if lower_top is None else [(bottom, lower_top), (upper_bottom, None)]
        for piece_bottom, piece_top in pieces:
            if piece_top is None and min(top0, top1) <= piece_bottom + 2:
                continue        # верх обрушился ниже перемычки
            corner = lambda uu, side, zz: start + along * uu + across * side * half + np.array([0, 0, zz])
            t0 = top0 if piece_top is None else piece_top
            t1 = top1 if piece_top is None else piece_top
            corners = [corner(u0, side, piece_bottom) for side in (-1, 1)] + [corner(u1, side, piece_bottom) for side in (-1, 1)] \
                + [corner(u0, side, t0) for side in (-1, 1)] + [corner(u1, side, t1) for side in (-1, 1)]
            caster(corners, CASTER_FULL)
            runs.setdefault((column // 4, piece_bottom > bottom, piece_top is None), []).extend(corners)
    for corners in runs.values():
        caster(corners, CASTER_SIMPLE)
    # поверхность: тело стены (рваный верх, проёмы) в вокселях со скруглёнными рёбрами
    step, pad = 1.0, 6.0
    us = np.arange(-pad, length + pad + step, step)
    vs = np.arange(-half - pad, half + pad + step, step)
    zs = np.arange(bottom - pad, tops.max() + pad + step, step)
    grid_u, grid_v, grid_z = np.meshgrid(us, vs, zs, indexing='ij')
    top_at = np.interp(grid_u, np.linspace(0, length, columns + 1), tops)
    inside = (grid_u >= 0) & (grid_u <= length) & (np.abs(grid_v) <= half) & (grid_z >= bottom) & (grid_z <= top_at)
    for first, last, sill, lintel in openings:
        u0, u1 = first * length / columns, last * length / columns
        inside &= ~((grid_u >= u0) & (grid_u <= u1) & (grid_z >= sill) & (grid_z <= lintel))
    # сколы на рёбрах торцов и проёмов: вогнутые выбоины с шершавым дном
    chips = []
    for _ in range(int(local.integers(2, 5))):
        if openings and local.random() < 0.5:
            first, last, sill, lintel = openings[int(local.integers(0, len(openings)))]
            chip_u = (first if local.random() < 0.5 else last) * length / columns
            chip_z = local.uniform(sill, min(lintel, float(np.interp(chip_u, np.linspace(0, length, columns + 1), tops))))
        else:
            chip_u = 0.0 if local.random() < 0.5 else length
            chip_z = local.uniform(bottom + 30, float(np.interp(chip_u, np.linspace(0, length, columns + 1), tops)) - 8)
        chips.append((chip_u, half * (1 if local.random() < 0.5 else -1), chip_z, local.uniform(4, 8)))
    chip_weight = np.zeros(inside.shape)
    for chip_u, chip_v, chip_z, radius in chips:
        reach = np.sqrt((grid_u - chip_u) ** 2 + ((grid_v - chip_v) * 1.3) ** 2 + ((grid_z - chip_z) * 0.8) ** 2)
        inside &= ~(reach < radius)
        chip_weight = np.maximum(chip_weight, np.clip(1 - (reach - radius) / 3, 0, 1))
    # излом: верх стены (до BREAK_DEPTH вниз от рваного профиля) и дно сколов
    broken_weight = np.maximum(np.clip(1 - (top_at - grid_z) / BREAK_DEPTH, 0, 1), chip_weight)
    noise_points = np.stack([grid_u, grid_v, grid_z], axis=-1).reshape(-1, 3) + seed * 37.0
    roughness = (fbm(noise_points / 4.5, 3) * 0.7 + value_noise(noise_points / 1.8) * 0.3).reshape(inside.shape)
    roughness = (roughness - roughness.mean()) / (roughness.std() + 1e-9)
    displacement = BREAK_ROUGHNESS * roughness * broken_weight
    local_points, local_normals, faces = rounded_surface(inside, step, WALL_EDGE_RADIUS, WALL_CORNER_RADIUS,
                                                          error=WALL_MESH_ERROR, displacement=displacement)
    local_points += np.array([us[0], vs[0], zs[0]])
    points = start + np.outer(local_points[:, 0], along) + np.outer(local_points[:, 1], across) + np.outer(local_points[:, 2], [0, 0, 1])
    normals = np.outer(local_normals[:, 0], along) + np.outer(local_normals[:, 1], across) + np.outer(local_normals[:, 2], [0, 0, 1])
    # излом и сколы — текстура скола (цемент со щебнем), остальное — гладкий бетон опалубки
    centre = local_points[faces].mean(axis=1)
    cell = np.clip(np.round((centre - np.array([us[0], vs[0], zs[0]])) / step).astype(int), 0, np.array(inside.shape) - 1)
    broken = broken_weight[cell[:, 0], cell[:, 1], cell[:, 2]] > 0.35
    for part_material, selected in ((material, ~broken), ('concrete_broken', broken)):
        if np.any(selected):
            chosen = faces[selected]
            used = np.unique(chosen)
            remap = np.full(len(points), -1)
            remap[used] = np.arange(len(used))
            mesh.add(part_material, points[used], normals[used], remap[chosen])
    # арматура: вертикальные стержни у обеих граней внутри стены, видны только концы,
    # выходящие из излома на несколько единиц (часть загнута)
    for bar_u in np.arange(7, length - 4, 14):
        top_here = float(np.interp(bar_u, np.linspace(0, length, columns + 1), tops))
        in_opening = any(first * length / columns - 2 < bar_u < last * length / columns + 2 for first, last, _, _ in openings)
        lintel_here = max([lintel for first, last, _, lintel in openings if first * length / columns - 2 < bar_u < last * length / columns + 2], default=bottom)
        if in_opening and top_here < lintel_here + 6:
            continue                # над проёмом нет бетона — торчало бы из подоконника
        for bar_v in (-(half - 3.5), half - 3.5):
            if local.random() > REBAR_SHARE:
                continue
            base = start + along * bar_u + across * bar_v
            low = base + np.array([0, 0, top_here - 10])
            tip = base + np.array([0, 0, top_here + local.uniform(3, 7)])
            beam(mesh, 'metal_rust', low, tip, 1.1)
            if local.random() < 0.35:
                bend = along * local.uniform(-1, 1) + across * local.uniform(-0.6, 0.6) + np.array([0, 0, local.uniform(0.2, 0.8)])
                beam(mesh, 'metal_rust', tip, tip + bend / (np.linalg.norm(bend) + 1e-9) * local.uniform(3, 6), 1.1)
    # коллизия: отрезки по 48 единиц высотой до нижней точки излома на отрезке
    clips = []
    segments = max(1, int(round(length / 48)))
    for index in range(segments):
        first = int(index * columns / segments); last = int((index + 1) * columns / segments)
        top = float(tops[first:last + 1].min()) - 2
        if top < base_z + 8:
            continue
        clips.append((start + along * (index * length / segments), start + along * ((index + 1) * length / segments), top))
    return mesh, clips


# ================================================================ контейнер

def shipping_container(center, length, width, height, along_x, color, seed):
    """морской контейнер: гофрированные стенки, торцевые двери с запорными штангами,
    рама и угловые фитинги. Центр подошвы — center."""
    local = np.random.default_rng(seed)
    mesh = Mesh()
    cx, cy, cz = center
    half_length, half_width = length / 2, width / 2
    if along_x:
        caster([(cx + a, cy + b, cz + z) for a in (-half_length, half_length) for b in (-half_width, half_width) for z in (0, height)])
    else:
        caster([(cx + b, cy + a, cz + z) for a in (-half_length, half_length) for b in (-half_width, half_width) for z in (0, height)])
    _caster_pause[0] += 1
    def point(a, b, z):        # a — вдоль контейнера, b — поперёк
        return (cx + a, cy + b, cz + z) if along_x else (cx - b, cy + a, cz + z)
    rib = 3.0                   # шаг гофры
    depth = 0.9
    # длинные стенки: трапециевидная гофра
    for side in (-1, 1):
        steps = int(length / rib)
        for index in range(steps):
            a0 = -half_length + index * rib
            a1 = a0 + rib
            offset = side * (half_width - (depth if index % 2 else 0))
            p = [point(a0, offset, 2), point(a1, offset, 2), point(a1, offset, height - 2), point(a0, offset, height - 2)]
            if side < 0:
                quad(mesh, color, p[0], p[1], p[2], p[3], 64)
            else:
                quad(mesh, color, p[1], p[0], p[3], p[2], 64)
            # перемычка между гребнем и впадиной
            offset_next = side * (half_width - (depth if (index + 1) % 2 else 0))
            q = [point(a1, offset, 2), point(a1, offset_next, 2), point(a1, offset_next, height - 2), point(a1, offset, height - 2)]
            quad(mesh, color, q[0], q[1], q[2], q[3], 64)      # тонкая перемычка: обе стороны
            quad(mesh, color, q[1], q[0], q[3], q[2], 64)
    # крыша и торцы
    quad(mesh, color, point(-half_length, -half_width, height), point(half_length, -half_width, height),
         point(half_length, half_width, height), point(-half_length, half_width, height), 64)
    sign = 1 if along_x else 1
    quad(mesh, color, point(-half_length, half_width, 0), point(-half_length, -half_width, 0),
         point(-half_length, -half_width, height), point(-half_length, half_width, height), 64)
    quad(mesh, color, point(half_length, -half_width, 0), point(half_length, half_width, 0),
         point(half_length, half_width, height), point(half_length, -half_width, height), 64)
    # двери: две створки (рамка), четыре запорные штанги с ручками
    for b0, b1 in ((-half_width + 1, -0.3), (0.3, half_width - 1)):
        beam(mesh, 'metal_rust', point(half_length + 0.5, b0, 2), point(half_length + 0.5, b1, 2), 1.2)
        beam(mesh, 'metal_rust', point(half_length + 0.5, b0, height - 2), point(half_length + 0.5, b1, height - 2), 1.2)
    for b in (-half_width * 0.62, -half_width * 0.22, half_width * 0.22, half_width * 0.62):
        cylinder(mesh, 'metal_rust', point(half_length + 1.2, b, 2), point(half_length + 1.2, b, height - 2), 0.55, 6, False)
        beam(mesh, 'metal_rust', point(half_length + 1.6, b, height * 0.45), point(half_length + 1.6, b + 3, height * 0.45), 0.8)
    # рама: нижние и верхние продольные балки, угловые стойки, фитинги
    for b in (-half_width, half_width):
        for z in (1, height - 1):
            beam(mesh, 'metal_rust', point(-half_length, b, z), point(half_length, b, z), 2.2)
    for a in (-half_length, half_length):
        for b in (-half_width, half_width):
            beam(mesh, 'metal_rust', point(a, b, 0), point(a, b, height), 2.4)
            for z in (1.2, height - 1.2):
                box(mesh, 'metal_panel', *_corner_box(point(a, b, z), 1.8), bottom=True, max_edge=64)
    _caster_pause[0] -= 1
    return mesh


def _corner_box(center, half):
    x, y, z = center
    return x - half, y - half, z - half, x + half, y + half, z + half


# ================================================================ металлоконструкции

def ladder(mesh, base, top_z, facing, width=10, material='metal_rust'):
    """вертикальная лестница: две тетивы и ступени через 10 единиц. facing — единичный вектор
    от стены наружу (лестница стоит на 3 единицы от стены)."""
    base = np.asarray(base, dtype=float)
    facing = np.asarray(facing, dtype=float)
    side = np.array([-facing[1], facing[0], 0.0])
    out = facing * 3
    for sign in (-1, 1):
        beam(mesh, material, base + out + side * sign * width / 2, np.array([0, 0, top_z - base[2]]) + base + out + side * sign * width / 2, 1.4)
    z = base[2] + 8
    while z < top_z - 2:
        beam(mesh, material, np.array([base[0], base[1], z]) + out - side * width / 2,
             np.array([base[0], base[1], z]) + out + side * width / 2, 0.9)
        z += 10


def railing(mesh, points, base_z, height=22, post_step=24, material='metal_rust', closed=False):
    """перила вдоль ломаной: стойки, поручень, средняя перекладина, бортик."""
    points = [np.asarray((x, y), dtype=float) for x, y in points]
    pairs = list(zip(points, points[1:] + ([points[0]] if closed else [])))
    for first, second in pairs:
        length = np.linalg.norm(second - first)
        posts = max(1, int(round(length / post_step)))
        for index in range(posts + 1):
            spot = first + (second - first) * index / posts
            beam(mesh, material, (*spot, base_z), (*spot, base_z + height), 1.6)
        beam(mesh, material, (*first, base_z + height), (*second, base_z + height), 1.8)
        beam(mesh, material, (*first, base_z + height * 0.5), (*second, base_z + height * 0.5), 1.0)
        beam(mesh, 'hazard', (*first, base_z + 2), (*second, base_z + 2), 0.8, 4)


def x_braces(mesh, corners, z0, z1, width=3, material='metal_rust'):
    """крестовые раскосы между соседними стойками (corners — по кругу) на ярусе z0..z1."""
    for index in range(len(corners)):
        first = np.asarray(corners[index], dtype=float)
        second = np.asarray(corners[(index + 1) % len(corners)], dtype=float)
        beam(mesh, material, (*first, z0), (*second, z1), width)
        beam(mesh, material, (*second, z0), (*first, z1), width)


def lattice_mast(mesh, center, z0, z1, base_half, top_half, material='metal_rust'):
    """решётчатая мачта: четыре сходящиеся стойки, пояса и раскосы."""
    cx, cy = center
    levels = max(2, int((z1 - z0) / 24))
    corners = lambda half: [(cx - half, cy - half), (cx + half, cy - half), (cx + half, cy + half), (cx - half, cy + half)]
    previous = None
    for level in range(levels + 1):
        fraction = level / levels
        z = z0 + (z1 - z0) * fraction
        current = corners(base_half + (top_half - base_half) * fraction)
        for index in range(4):
            first, second = current[index], current[(index + 1) % 4]
            beam(mesh, material, (*first, z), (*second, z), 1.4)
        if previous is not None:
            previous_z, previous_corners = previous
            for index in range(4):
                beam(mesh, material, (*previous_corners[index], previous_z), (*current[index], z), 2.0)
                beam(mesh, material, (*previous_corners[index], previous_z), (*current[(index + 1) % 4], z), 1.0)
        previous = (z, current)


def pipe_run(mesh, start, end, radius, material='metal_panel', bracket_step=80, wall_side=None):
    cylinder(mesh, material, start, end, radius, 8, True)
    start = np.asarray(start, dtype=float); end = np.asarray(end, dtype=float)
    length = np.linalg.norm(end - start)
    if wall_side is None:
        return
    for index in range(1, int(length / bracket_step) + 1):
        spot = start + (end - start) * index * bracket_step / length
        beam(mesh, 'metal_rust', spot, spot + np.asarray(wall_side) * (radius + 4), 1.6, radius * 2.4)


# ================================================================ запись MD3

def encode_normal(normal):
    x, y, z = normal
    if z >= 0.9999:
        return 0
    if z <= -0.9999:
        return 128
    lat = int(round(math.atan2(y, x) / (math.pi / 128))) & 255
    lng = int(round(math.acos(max(-1.0, min(1.0, z))) / (math.pi / 128))) & 255
    return (lat << 8) | lng


def project_uv(positions, normals, tile, origin):
    """проекция на ось, ближайшую к нормали; мировые координаты — швы между моделями и с
    брашами совпадают по рисунку."""
    world = positions + origin
    axis = np.argmax(np.abs(normals), axis=1)
    s = np.where(axis == 0, world[:, 1], world[:, 0])
    t = np.where(axis == 2, -world[:, 1], -world[:, 2])
    return np.stack([s / tile, t / tile], axis=1)


def split_surfaces(positions, normals, faces):
    """режет поверхность на куски не больше SURFACE_VERTS вершин (по порядку граней)."""
    chunks = []
    current, mapping = [], {}
    for face in faces:
        new = [vertex for vertex in face if vertex not in mapping]
        if len(mapping) + len(new) > SURFACE_VERTS:
            chunks.append((current, mapping)); current, mapping = [], {}
        for vertex in face:
            if vertex not in mapping:
                mapping[vertex] = len(mapping)
        current.append([mapping[vertex] for vertex in face])
    if current:
        chunks.append((current, mapping))
    result = []
    for chunk_faces, chunk_mapping in chunks:
        order = np.empty(len(chunk_mapping), dtype=np.int64)
        for old, new in chunk_mapping.items():
            order[new] = old
        result.append((positions[order], normals[order], np.array(chunk_faces)))
    return result


def write_md3(path, mesh, origin, shader_prefix):
    surfaces = []
    for material in sorted(mesh.parts):
        if not mesh.parts[material][0]:
            continue
        positions, normals, faces = mesh.arrays(material)
        tile = TILE.get(material, 128)
        for chunk_positions, chunk_normals, chunk_faces in split_surfaces(positions, normals, faces):
            uv = project_uv(chunk_positions, chunk_normals, tile, origin)
            surfaces.append((material, chunk_positions, chunk_normals, uv, chunk_faces))
    mins, maxs = mesh.bounds()
    assert np.all(np.abs(np.concatenate([mins, maxs])) < 511), f'{path}: model too large ({mins}, {maxs})'
    header_size, frame_size = 108, 56
    surface_blobs = []
    for index, (material, positions, normals, uv, faces) in enumerate(surfaces):
        shader = f'{shader_prefix}{material}'.encode('ascii')
        surface_header = 108
        # сетки строятся против часовой стрелки вокруг внешней нормали, а в Quake лицевая
        # сторона — по часовой: порядок вершин в треугольнике разворачивается
        triangles = struct.pack(f'<{faces.size}i', *faces[:, ::-1].reshape(-1).tolist())
        shaders = struct.pack('<64si', shader, 0)
        st = struct.pack(f'<{uv.size}f', *uv.reshape(-1).tolist())
        xyz = b''.join(struct.pack('<hhhH', *[int(round(value * 64)) for value in position], encode_normal(normal))
                       for position, normal in zip(positions, normals))
        ofs_triangles = surface_header
        ofs_shaders = ofs_triangles + len(triangles)
        ofs_st = ofs_shaders + len(shaders)
        ofs_xyz = ofs_st + len(st)
        ofs_end = ofs_xyz + len(xyz)
        head = struct.pack('<4s64siiiiiiiiii', b'IDP3', f'surface{index}'.encode('ascii'), 0, 1, 1, len(positions), len(faces),
                           ofs_triangles, ofs_shaders, ofs_st, ofs_xyz, ofs_end)
        surface_blobs.append(head + triangles + shaders + st + xyz)
    radius = float(np.linalg.norm(np.maximum(np.abs(mins), np.abs(maxs))))
    frame = struct.pack('<3f3f3ff16s', *mins.tolist(), *maxs.tolist(), 0, 0, 0, radius, b'frame0')
    ofs_frames = header_size
    ofs_tags = ofs_frames + frame_size
    ofs_surfaces = ofs_tags
    ofs_end = ofs_surfaces + sum(len(blob) for blob in surface_blobs)
    header = struct.pack('<4si64siiiiiiiii', b'IDP3', 15, os.path.basename(path).encode('ascii'), 0, 1, 0, len(surface_blobs), 0,
                         ofs_frames, ofs_tags, ofs_surfaces, ofs_end)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as file:
        file.write(header + frame + b''.join(surface_blobs))
    return sum(len(faces) for _, _, _, _, faces in surfaces)


def write_casters(path):
    """формы для теней: выпуклые оболочки (scipy) — вершины и треугольники, обход против
    часовой стрелки снаружи. Строки: "<уровень> <вершин> <треугольников>", вершины, треугольники."""
    from scipy.spatial import ConvexHull
    lines = ['// shadow casters for renderer/tr_sunshadow.c: level (0 both, 1 simple, 2 full) verts tris']
    counts = [0, 0, 0]
    for level, points in CASTERS:
        points = np.unique(np.round(points, 1), axis=0)
        try:
            hull = ConvexHull(points)
        except Exception:
            continue            # плоская или вырожденная форма
        used = np.unique(hull.simplices)
        remap = {old: new for new, old in enumerate(used)}
        triangles = []
        for simplex, equation in zip(hull.simplices, hull.equations):
            a, b, c = (points[index] for index in simplex)
            if np.dot(np.cross(b - a, c - a), equation[:3]) < 0:
                simplex = simplex[[0, 2, 1]]
            triangles.append([remap[index] for index in simplex])
        lines.append(f'{level} {len(used)} {len(triangles)}')
        lines.append(' '.join(f'{x:.1f} {y:.1f} {z:.1f}' for x, y, z in points[used]))
        lines.append(' '.join(f'{a} {b} {c}' for a, b, c in triangles))
        counts[level] += 1
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='ascii', newline='\n') as file:
        file.write('\n'.join(lines) + '\n')
    return counts


def add_prop(name, mesh):
    """модель в мировых координатах: начало — центр её границ (MD3 хранит до ±511 единиц)."""
    if not mesh.parts:
        return
    mins, maxs = mesh.bounds()
    if np.any(maxs - mins > 1000):
        raise ValueError(f'prop {name} is too large: {maxs - mins}')
    origin = np.round((mins + maxs) / 2)
    for part in mesh.parts.values():
        part[0] = [positions - origin for positions in part[0]]
    PROPS.append((name, origin, mesh))


def write_props(compile_dir, shader_prefix='textures/walker/v_'):
    """пишет все модели, возвращает сущности misc_model и множество материалов."""
    entities, materials, total = [], set(), 0
    for name, origin, mesh in PROPS:
        relative = f'models/walker_props/{name}.md3'
        total += write_md3(os.path.join(compile_dir, relative), mesh, origin, shader_prefix)
        materials.update(mesh.parts)
        entities.append({'classname': 'misc_model', 'model': relative,
                         'origin': f'{origin[0]:g} {origin[1]:g} {origin[2]:g}'})
    print(f'props: {len(PROPS)} models, {total} triangles, materials: {sorted(materials)}')
    return entities, materials
