"""Убрать голубую лампу сбоку корпуса K7 (WALKER, 28.11.2026).

python3 walker/tools/k7_side_lamp.py <upper.glb> [out.glb]

В исходной модели на правом боку корпуса у заднего угла стоял светящийся голубой квадрат
(материал light_cyan, узел без имени, 0.05 x 0.12 x 0.12). По просьбе он убран, на его
месте bake_walker.py пишет трафарет "Инв.№ 50-458" (FIXED_DECALS). Узел ищется по
материалу и месту: light_cyan, сбоку (x < -1); повторный запуск ничего не находит.
"""
import sys
import k7_hydraulics as glb


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = glb.read_glb(source)
    materials = [material['name'] for material in document['materials']]
    found = []
    for index, node in enumerate(document['nodes']):
        if 'mesh' not in node:
            continue
        primitives = document['meshes'][node['mesh']]['primitives']
        if all(materials[primitive['material']] == 'light_cyan' for primitive in primitives) \
                and node.get('translation', [0, 0, 0])[0] < -1.0:
            found.append(index)
    if not found:
        raise SystemExit('no side lamp (already removed?)')
    # узел остаётся на месте пустым (без сетки): номера узлов в анимациях не меняются
    for index in found:
        node = document['nodes'][index]
        node.pop('mesh')
        node['name'] = 'removed_side_lamp'
    # сетки, на которые больше никто не ссылается, - убрать (иначе их читают запекание и
    # проход материалов, а запекание переписывает только используемые данные)
    used = sorted({node['mesh'] for node in document['nodes'] if 'mesh' in node})
    remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in document['nodes']:
        if 'mesh' in node:
            node['mesh'] = remap[node['mesh']]
    glb.write_glb(target, document, binary)
    print(f'{target}: side lamp nodes removed: {found}')


if __name__ == '__main__':
    main()
