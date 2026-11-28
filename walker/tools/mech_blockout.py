"""Черновая (проволочная) модель нового шагающего робота (WALKER, 28.11.2026).

python3 walker/tools/mech_blockout.py <out.png>

Компоновка: две ноги с коленом вперёд и бронированной голенью, широкий таз на двух
шарнирах, поворотный погон, над ним узкий корпус: спереди угловатая кабина (короб со
срезанной верхней передней гранью, одно окно на скосе), сзади отсек оборудования.
Оружия нет - оно съёмное. Бокового крыла нет.

Единицы - как у K7 (метры модели), y вверх, z вперёд, x влево (вид спереди: левая
сторона робота справа на картинке). Земля y = 0.

Пока только геометрия для оценки формы: многогранники (короба, клинья, призмы) и
картинка с четырьмя видами проволокой - спереди, сбоку, сверху и в перспективе.
Детали раскрашены по группам: ноги, таз, корпус, кабина, оружие.
"""
import sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont

GROUP_COLOURS = {
    'legs': (60, 110, 40), 'pelvis': (150, 110, 20), 'torso': (40, 80, 150),
    'cockpit': (190, 40, 40), 'weapons': (90, 90, 90), 'hydraulics': (220, 110, 0),
}


class Solid:
    """многогранник: вершины и грани (списки индексов по кругу)"""
    def __init__(self, group, vertices, faces):
        self.group = group
        self.vertices = np.array(vertices, float)
        self.faces = faces

    def edges(self):
        found = set()
        for face in self.faces:
            for index in range(len(face)):
                first, second = face[index], face[(index + 1) % len(face)]
                found.add((min(first, second), max(first, second)))
        return found


SOLIDS = []


def add(group, vertices, faces):
    SOLIDS.append(Solid(group, vertices, faces))


def cuboid(group, low, high):
    x0, y0, z0 = low; x1, y1, z1 = high
    vertices = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    add(group, vertices, [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [3, 2, 6, 7], [0, 3, 7, 4], [1, 2, 6, 5]])


def profile_prism(group, x_low, x_high, profile):
    """призма вдоль x: профиль - многоугольник в плоскости (z, y) по кругу"""
    count = len(profile)
    vertices = [(x_low, y_value, z_value) for z_value, y_value in profile] + \
               [(x_high, y_value, z_value) for z_value, y_value in profile]
    faces = [list(range(count)), list(range(count, 2 * count))]
    faces += [[index, (index + 1) % count, count + (index + 1) % count, count + index] for index in range(count)]
    add(group, vertices, faces)


def beam(group, start, end, width, depth, up=(1.0, 0.0, 0.0)):
    """брус от start до end с сечением width (вдоль up) x depth"""
    start = np.array(start, float); end = np.array(end, float)
    axis = end - start; axis /= np.linalg.norm(axis)
    side = np.array(up, float); side -= axis * (side @ axis); side /= np.linalg.norm(side)
    other = np.cross(axis, side)
    corners = []
    for point in (start, end):
        for sign_side, sign_other in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            corners.append(point + side * sign_side * width / 2 + other * sign_other * depth / 2)
    add(group, corners, [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]])


def cylinder(group, start, end, radius, sides=10):
    start = np.array(start, float); end = np.array(end, float)
    axis = end - start; axis /= np.linalg.norm(axis)
    helper = np.array([0.0, 1.0, 0.0]) if abs(axis[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    side = np.cross(axis, helper); side /= np.linalg.norm(side)
    other = np.cross(axis, side)
    vertices = []
    for point in (start, end):
        for step in range(sides):
            angle = 2 * math.pi * step / sides
            vertices.append(point + radius * (math.cos(angle) * side + math.sin(angle) * other))
    faces = [list(range(sides)), list(range(sides, 2 * sides))]
    faces += [[step, (step + 1) % sides, sides + (step + 1) % sides, sides + step] for step in range(sides)]
    add(group, vertices, faces)


# ---------------------------------------------------------------- модель

HIP_X = 0.62                    # оси ног
ANKLE = (0.36, -0.12)           # (y, z) щиколотки
THIGH_LENGTH = 0.62             # бедро и голень (28.11.2026: короче на четверть, 0.82 и 0.99)
SHIN_LENGTH = 0.74
THIGH_DIRECTION = np.array([-0.887, 0.462])     # от таза к колену (y, z), как было
SHIN_DIRECTION = np.array([-0.864, -0.503])     # от колена к щиколотке
KNEE = tuple(np.array(ANKLE) - SHIN_DIRECTION * SHIN_LENGTH)
HIP_Y = float(KNEE[0] - THIGH_DIRECTION[0] * THIGH_LENGTH)
HIP_Z = float(KNEE[1] - THIGH_DIRECTION[1] * THIGH_LENGTH)
BODY_DROP = 1.95 - HIP_Y        # корпус задан от прежней высоты таза 1.95 и опускается
COCKPIT_HALF = 0.26             # полуширина кабины (была 0.42)
TORSO_HALF = 0.3                # полуширина корпуса (была 0.6)


def leg(side):
    x_value = side * HIP_X
    # бедро: шарнир и брус к колену
    cylinder('legs', (x_value - side * 0.22, HIP_Y, HIP_Z), (x_value + side * 0.2, HIP_Y, HIP_Z), 0.2)
    beam('legs', (x_value, HIP_Y, HIP_Z), (x_value, KNEE[0], KNEE[1]), 0.34, 0.38)
    # колено и голень: брус и бронеплита спереди
    cylinder('legs', (x_value - 0.2, KNEE[0], KNEE[1]), (x_value + 0.2, KNEE[0], KNEE[1]), 0.15)
    beam('legs', (x_value, KNEE[0], KNEE[1]), (x_value, ANKLE[0], ANKLE[1]), 0.26, 0.24)
    beam('legs', (x_value, KNEE[0] - 0.05, KNEE[1] + 0.17), (x_value, ANKLE[0] + 0.32, ANKLE[1] + 0.18), 0.42, 0.08)
    # гидравлика: цилиндр за бедром от таза к голени, шток - тоньше
    barrel_end = ((HIP_Y + KNEE[0]) / 2 - 0.1, (HIP_Z + KNEE[1]) / 2 - 0.2)
    cylinder('hydraulics', (x_value, HIP_Y - 0.12, HIP_Z - 0.3), (x_value, barrel_end[0], barrel_end[1]), 0.07, 8)
    cylinder('hydraulics', (x_value, barrel_end[0], barrel_end[1]), (x_value, KNEE[0] - 0.2, KNEE[1] - 0.14), 0.04, 8)
    # щиколотка и ступня: два передних пальца и задний упор
    cylinder('legs', (x_value - 0.14, ANKLE[0], ANKLE[1]), (x_value + 0.14, ANKLE[0], ANKLE[1]), 0.1)
    for toe in (-1, 1):
        toe_x = x_value + toe * 0.11
        profile_prism('legs', toe_x - 0.08, toe_x + 0.08,
                      [(ANKLE[1] - 0.02, 0.0), (ANKLE[1] + 0.52, 0.0), (ANKLE[1] + 0.55, 0.06), (ANKLE[1] + 0.3, 0.2), (ANKLE[1] - 0.02, 0.24)])
    profile_prism('legs', x_value - 0.1, x_value + 0.1,
                  [(ANKLE[1] - 0.42, 0.0), (ANKLE[1] - 0.02, 0.0), (ANKLE[1] - 0.02, 0.22), (ANKLE[1] - 0.3, 0.1)])


def build():
    for side in (-1, 1):
        leg(side)
    first_body = len(SOLIDS)
    # таз: короб со скосами спереди и сзади снизу, шарниры ног по бокам
    profile_prism('pelvis', -0.44, 0.44, [(-0.38, 1.82), (-0.22, 1.7), (0.24, 1.7), (0.42, 1.86), (0.42, 2.12), (-0.38, 2.12)])
    # погон
    cylinder('pelvis', (0, 2.12, -0.05), (0, 2.3, -0.05), 0.24, 12)
    # корпус (28.11.2026: вместе с кабиной уже - по ширине нижней части кабины), средний отсек
    cuboid('torso', (-TORSO_HALF, 2.3, -0.7), (TORSO_HALF, 3.02, 0.32))
    # отсек оборудования сзади: короб с панелью
    cuboid('torso', (-TORSO_HALF + 0.04, 2.42, -1.02), (TORSO_HALF - 0.04, 2.96, -0.7))
    cuboid('torso', (-TORSO_HALF + 0.1, 2.5, -1.06), (TORSO_HALF - 0.1, 2.88, -1.02))
    # кабина: короб со срезанной верхней передней гранью (как на эскизе) и малым скосом снизу
    profile_prism('cockpit', -COCKPIT_HALF, COCKPIT_HALF, [(0.32, 2.3), (0.82, 2.3), (1.0, 2.4), (1.0, 2.58), (0.56, 2.96), (0.32, 2.96)])
    # одно окно на скосе: трапеция чуть над поверхностью
    slope_low = np.array([1.0, 2.58]); slope_high = np.array([0.56, 2.96])
    normal = np.array([slope_high[1] - slope_low[1], -(slope_high[0] - slope_low[0])]); normal /= np.linalg.norm(normal)
    def on_slope(share, lift):
        point = slope_low + (slope_high - slope_low) * share + normal * lift
        return point[1], point[0]          # y, z
    y_low, z_low = on_slope(0.15, 0.012); y_high, z_high = on_slope(0.8, 0.012)
    low_half, high_half = COCKPIT_HALF - 0.05, COCKPIT_HALF - 0.09
    add('cockpit', [(-low_half, y_low, z_low), (low_half, y_low, z_low), (high_half, y_high, z_high), (-high_half, y_high, z_high)], [[0, 1, 2, 3]])
    # фары и мачта сверху
    for side in (-1, 1):
        cylinder('torso', (side * (TORSO_HALF - 0.1), 3.02, -0.55), (side * (TORSO_HALF - 0.1), 3.1, -0.55), 0.05, 8)
    cylinder('torso', (-(TORSO_HALF - 0.08), 3.02, -0.3), (-(TORSO_HALF - 0.08), 3.45, -0.3), 0.012, 6)
    # оружия нет: оно съёмное, ставится в слоты отдельными моделями
    for solid in SOLIDS[first_body:]:
        solid.vertices[:, 1] -= BODY_DROP


# ---------------------------------------------------------------- картинка

def project_ortho(points, view):
    """(x, y на картинке, глубина: больше - ближе к зрителю)"""
    if view == 'front':      # смотрим против z: левая сторона робота справа на картинке
        return np.stack([points[:, 0], points[:, 1], points[:, 2]], 1)
    if view == 'side':       # смотрим с правого бока (из -x): вперёд - вправо
        return np.stack([points[:, 2], points[:, 1], -points[:, 0]], 1)
    if view == 'top':        # сверху: вперёд - вверх
        return np.stack([points[:, 0], points[:, 2], points[:, 1]], 1)
    raise ValueError(view)


def project_perspective(points, yaw=35.0, pitch=18.0, distance=8.0, centre=(0.0, 1.7, 0.2)):
    yaw_rad = math.radians(yaw); pitch_rad = math.radians(pitch)
    shifted = points - np.array(centre)
    rotate_yaw = np.array([[math.cos(yaw_rad), 0, math.sin(yaw_rad)], [0, 1, 0], [-math.sin(yaw_rad), 0, math.cos(yaw_rad)]])
    rotate_pitch = np.array([[1, 0, 0], [0, math.cos(pitch_rad), -math.sin(pitch_rad)], [0, math.sin(pitch_rad), math.cos(pitch_rad)]])
    camera = shifted @ rotate_yaw.T @ rotate_pitch.T
    depth = distance - camera[:, 2]
    return np.stack([camera[:, 0] / depth * distance, camera[:, 1] / depth * distance, -depth], 1)


def draw_panel(size, title, projector, ground_lines):
    """вид с удалением невидимых линий: грани от дальних к ближним, светлая заливка и
    рёбра цветом группы; невидимые рёбра - тонким серым под всем остальным"""
    width, height = size
    image = Image.new('RGB', size, (250, 250, 247))
    draw = ImageDraw.Draw(image)
    projected_all = np.concatenate([projector(solid.vertices) for solid in SOLIDS])
    low = projected_all[:, :2].min(0); high = projected_all[:, :2].max(0)
    margin = 30
    scale = min((width - 2 * margin) / (high[0] - low[0]), (height - 2 * margin - 20) / (high[1] - low[1]))
    offset_x = margin + ((width - 2 * margin) - (high[0] - low[0]) * scale) / 2
    def to_pixels(points):
        return np.stack([offset_x + (points[:, 0] - low[0]) * scale, height - margin - (points[:, 1] - low[1]) * scale], 1)
    for start, end in ground_lines:
        line = to_pixels(projector(np.array([start, end], float)))
        draw.line([tuple(line[0]), tuple(line[1])], fill=(205, 205, 205), width=1)
    faces = []
    for solid in SOLIDS:
        projected = projector(solid.vertices)
        pixels = to_pixels(projected)
        for first, second in solid.edges():
            draw.line([tuple(pixels[first]), tuple(pixels[second])], fill=(215, 215, 215), width=1)
        for face in solid.faces:
            faces.append((projected[face, 2].mean(), solid.group, [tuple(pixels[index]) for index in face]))
    for _, group, polygon in sorted(faces, key=lambda item: item[0]):
        colour = GROUP_COLOURS[group]
        fill = tuple(int(255 - (255 - channel) * 0.18) for channel in colour)
        draw.polygon(polygon, fill=fill, outline=colour)
    draw.rectangle((0, 0, width - 1, height - 1), outline=(170, 170, 170))
    draw.text((8, 6), title, fill=(30, 30, 30), font=ImageFont.load_default())
    return image


def main():
    build()
    target = sys.argv[1] if len(sys.argv) > 1 else 'mech_blockout.png'
    width, height = 1600, 1250
    image = Image.new('RGB', (width, height), (250, 250, 247))
    ground = [((-1.6, 0, z_value), (1.6, 0, z_value)) for z_value in np.linspace(-1.6, 2.0, 10)] + \
             [((x_value, 0, -1.6), (x_value, 0, 2.0)) for x_value in np.linspace(-1.6, 1.6, 9)]
    floor_line = [((-1.6, 0, 0), (1.6, 0, 0))]
    half_width, half_height = width // 2, (height - 50) // 2
    panels = [
        ((0, 0, half_width, half_height), 'perspective (front-right)', project_perspective, ground),
        ((half_width, 0, width, half_height), 'side (from the right)', lambda points: project_ortho(points, 'side'),
         [((0, 0, -1.6), (0, 0, 2.0))]),
        ((0, half_height, half_width, 2 * half_height), 'front', lambda points: project_ortho(points, 'front'), floor_line),
        ((half_width, half_height, width, 2 * half_height), 'top (front is up)', lambda points: project_ortho(points, 'top'), []),
    ]
    for box, title, projector, lines in panels:
        panel = draw_panel((box[2] - box[0], box[3] - box[1]), title, projector, lines)
        image.paste(panel, (box[0], box[1]))
    draw = ImageDraw.Draw(image)
    legend_x = 10
    used_groups = {solid.group for solid in SOLIDS}
    for group, colour in GROUP_COLOURS.items():
        if group not in used_groups:
            continue
        draw.rectangle((legend_x, height - 35, legend_x + 14, height - 21), fill=colour)
        draw.text((legend_x + 20, height - 34), group, fill=(30, 30, 30))
        legend_x += 130
    all_points = np.concatenate([solid.vertices for solid in SOLIDS])
    size = all_points.max(0) - all_points.min(0)
    draw.text((legend_x + 20, height - 34), f'size: width {size[0]:.2f}  height {size[1]:.2f}  length {size[2]:.2f}', fill=(30, 30, 30))
    image.save(target)
    print(target, f'{len(SOLIDS)} solids, size {size.round(2)}')


if __name__ == '__main__':
    main()
