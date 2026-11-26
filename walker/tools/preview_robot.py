"""Просмотр запечённого робота без игры (WALKER).

python3 walker/tools/preview_robot.py <папка модели> <окраска> <out.png> <поворот> <наклон> [приближение] [высота центра]

Рисует lower/upper/head.glb в позе покоя с атласами <часть>_<окраска>.jpg программным
растеризатором: рассеянный свет и блик по маске классов <часть>_mask.tga. Нужен, чтобы
смотреть износ, надписи и масло крупно с любой стороны (камера игры смотрит сверху сзади).
Пример: preview_robot.py /tmp/stage/base/models/players/walker default k7.png 25 15 2.4 0.5
"""
import sys, os, json, struct, math
import numpy as np
from PIL import Image

COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def read_glb(path):
    data = open(path, 'rb').read()
    jl = struct.unpack('<I', data[12:16])[0]
    doc = json.loads(data[20:20 + jl])
    off = 20 + jl
    bl = struct.unpack('<I', data[off:off + 4])[0]
    return doc, data[off + 8:off + 8 + bl]


def acc(doc, binary, index):
    info = doc['accessors'][index]; view = doc['bufferViews'][info['bufferView']]
    dt = np.dtype(COMPONENT[info['componentType']]); w = WIDTH[info['type']]
    start = view.get('byteOffset', 0) + info.get('byteOffset', 0)
    stride = view.get('byteStride', 0) or dt.itemsize * w
    n = info['count']
    raw = np.frombuffer(binary, dtype=np.uint8, count=stride * (n - 1) + dt.itemsize * w, offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(n, dt.itemsize * w), strides=(stride, 1))
    a = np.ascontiguousarray(rows).view(dt)
    return a.reshape(n, w) if w > 1 else a.reshape(n)


def quat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def node_m(node):
    if 'matrix' in node:
        return np.array(node['matrix'], float).reshape(4, 4).T
    m = np.eye(4)
    m[:3, :3] = quat(node.get('rotation', [0, 0, 0, 1])) * np.array(node.get('scale', [1, 1, 1]))[None, :]
    m[:3, 3] = node.get('translation', [0, 0, 0])
    return m


def load_part(path):
    doc, binary = read_glb(path)
    mats = {}
    def walk(i, parent):
        m = parent @ node_m(doc['nodes'][i]); mats[i] = m
        for c in doc['nodes'][i].get('children', []):
            walk(c, m)
    for r in doc['scenes'][doc.get('scene', 0)]['nodes']:
        walk(r, np.eye(4))
    P, N, U, F = [], [], [], []
    base = 0
    for i, node in enumerate(doc['nodes']):
        if 'mesh' not in node:
            continue
        m = mats[i]; nm = np.linalg.inv(m[:3, :3]).T
        for prim in doc['meshes'][node['mesh']]['primitives']:
            a = prim['attributes']
            p = acc(doc, binary, a['POSITION']).astype(float); n = acc(doc, binary, a['NORMAL']).astype(float)
            uv = acc(doc, binary, a['TEXCOORD_0']).astype(float)
            idx = acc(doc, binary, prim['indices']).astype(np.int64).reshape(-1, 3) if 'indices' in prim else np.arange(len(p)).reshape(-1, 3)
            P.append(p @ m[:3, :3].T + m[:3, 3]); nn = n @ nm.T; N.append(nn / (np.linalg.norm(nn, axis=1, keepdims=True) + 1e-9))
            U.append(uv); F.append(idx + base); base += len(p)
    return np.concatenate(P), np.concatenate(N), np.concatenate(U), np.concatenate(F)


def main():
    folder, scheme, out = sys.argv[1], sys.argv[2], sys.argv[3]
    yaw, pitch = float(sys.argv[4]), float(sys.argv[5])
    zoom = float(sys.argv[6]) if len(sys.argv) > 6 else 1.0
    target_y = float(sys.argv[7]) if len(sys.argv) > 7 else None
    W = H = 900
    parts = []
    for name in ('lower', 'upper', 'head'):
        P, N, U, F = load_part(os.path.join(folder, name + '.glb'))
        tex = np.asarray(Image.open(os.path.join(folder, f'{name}_{scheme}.jpg')).convert('RGB')).astype(float)
        mpath = os.path.join(folder, f'{name}_mask.tga')
        mask = np.asarray(Image.open(mpath).convert('RGBA')).astype(float) / 255 if os.path.exists(mpath) else None
        parts.append((P, N, U, F, tex, mask))
    allp = np.concatenate([p[0] for p in parts])
    center = (allp.min(0) + allp.max(0)) / 2
    if target_y is not None:
        center[1] = target_y
    radius = np.linalg.norm(allp.max(0) - allp.min(0)) / 2 / zoom
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    eye_dir = np.array([sy * cp, sp, cy * cp])          # from center to eye
    forward = -eye_dir
    right = np.cross(forward, [0, 1, 0]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    eye = center + eye_dir * radius * 3
    light = np.array([0.4, 0.8, 0.45]); light /= np.linalg.norm(light)
    zbuf = np.full((H, W), np.inf); img = np.zeros((H, W, 3)) + np.array([70, 95, 110])
    focal = W / 2 / math.tan(math.radians(17))
    for P, N, U, F, tex, mask in parts:
        rel = P - eye
        x = rel @ right; y = rel @ up; z = rel @ forward
        sx = W / 2 + focal * x / z; syy = H / 2 - focal * y / z
        for face in F:
            xs, ys, zs = sx[face], syy[face], z[face]
            if np.any(zs <= 0.01):
                continue
            x0, x1, x2 = xs; y0, y1, y2 = ys
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9:
                continue
            lx, hx = int(max(0, math.floor(xs.min()))), int(min(W, math.ceil(xs.max())))
            ly, hy = int(max(0, math.floor(ys.min()))), int(min(H, math.ceil(ys.max())))
            if hx <= lx or hy <= ly:
                continue
            gy, gx = np.mgrid[ly:hy, lx:hx] + 0.5
            w1 = ((gx - x0) * (y2 - y0) - (x2 - x0) * (gy - y0)) / area
            w2 = ((x1 - x0) * (gy - y0) - (gx - x0) * (y1 - y0)) / area
            w0 = 1 - w1 - w2
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            iz = w0 / zs[0] + w1 / zs[1] + w2 / zs[2]
            depth = 1 / iz
            rows = gy[inside].astype(int); cols = gx[inside].astype(int)
            d = depth[inside]
            closer = d < zbuf[rows, cols]
            if not closer.any():
                continue
            rows, cols, d = rows[closer], cols[closer], d[closer]
            b0 = (w0[inside][closer] / zs[0]) * d; b1 = (w1[inside][closer] / zs[1]) * d; b2 = (w2[inside][closer] / zs[2]) * d
            uv = b0[:, None] * U[face[0]] + b1[:, None] * U[face[1]] + b2[:, None] * U[face[2]]
            n = b0[:, None] * N[face[0]] + b1[:, None] * N[face[1]] + b2[:, None] * N[face[2]]
            n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
            th, tw = tex.shape[:2]
            tu = np.clip((uv[:, 0] % 1) * tw, 0, tw - 1).astype(int); tv = np.clip((uv[:, 1] % 1) * th, 0, th - 1).astype(int)
            base = tex[tv, tu]
            diff = np.clip(n @ light, 0, 1)
            col = base * (0.42 + 0.75 * diff)[:, None]
            if mask is not None:
                mh, mw = mask.shape[:2]
                m = mask[np.clip((uv[:, 1] % 1) * mh, 0, mh - 1).astype(int), np.clip((uv[:, 0] % 1) * mw, 0, mw - 1).astype(int)]
                view = -forward
                refl = 2 * (n @ light)[:, None] * n - light
                spec = np.clip(refl @ view, 0, 1) ** 16
                envish = 0.5 + 0.5 * n[:, 1]
                col = col * (1 + m[:, 0:1] * envish[:, None] * 0.6) + 255 * (m[:, 1:2] * 0.2 * envish[:, None] + m[:, 2:3] * spec[:, None] * 0.9)
                col = np.where(m[:, 3:4] > 0.5, base, col)
            zbuf[rows, cols] = d
            img[rows, cols] = np.clip(col, 0, 255)
    Image.fromarray(img.astype(np.uint8)).save(out)


main()
