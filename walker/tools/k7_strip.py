"""Убрать с K7 боковое крыло и дюзы (WALKER, 28.11.2026).

python3 walker/tools/k7_strip.py <upper.glb> [out.glb]

  - боковое крыло: плита с надписью K7 на штанге слева от корпуса (узел fin, его
    соседи и штанга с шарниром - всё под одним узлом-группой у корпуса). Слишком похоже
    на робота из War Robots;
  - дюзы nozzle_L, nozzle_R (k7_nozzles.py): K7 не прыгает и не ускоряется, задняя
    стенка короба двигателя остаётся гладкой (рельеф панелей даёт запекание).

Сетки, на которые больше никто не ссылается, удаляются из списка; данные в буфере
остаются (не мешают, загрузчик читает только нужное).
"""
import sys

from k7_hydraulics import read_glb, write_glb


def main():
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else source
    document, binary = read_glb(source)
    nodes = document['nodes']
    names = [node.get('name', '') for node in nodes]
    body = names.index('upper_body')
    parent = {child: index for index, node in enumerate(nodes) for child in node.get('children', [])}

    removed_roots = [index for index, name in enumerate(names) if name.startswith('nozzle')]
    if 'fin' in names:
        group = names.index('fin')
        while parent.get(group) is not None and parent[group] != body:
            group = parent[group]
        if parent.get(group) != body:
            raise SystemExit('the fin is not under upper_body')
        removed_roots.append(group)
    if not removed_roots:
        raise SystemExit('nothing to remove (no fin, no nozzle_* nodes)')

    removed = set()
    pending = list(removed_roots)
    while pending:
        index = pending.pop()
        removed.add(index)
        pending += nodes[index].get('children', [])
    for animation in document.get('animations', []):
        for channel in animation['channels']:
            if channel['target']['node'] in removed:
                raise SystemExit('a removed node is animated')

    keep = [index for index in range(len(nodes)) if index not in removed]
    remap = {old: new for new, old in enumerate(keep)}
    kept = [nodes[index] for index in keep]
    for node in kept:
        if 'children' in node:
            node['children'] = [remap[child] for child in node['children'] if child in remap]
    document['nodes'] = kept
    for scene in document['scenes']:
        scene['nodes'] = [remap[index] for index in scene['nodes']]
    for animation in document.get('animations', []):
        for channel in animation['channels']:
            channel['target']['node'] = remap[channel['target']['node']]
    used = sorted({node['mesh'] for node in kept if 'mesh' in node})
    mesh_remap = {old: new for new, old in enumerate(used)}
    document['meshes'] = [document['meshes'][index] for index in used]
    for node in kept:
        if 'mesh' in node:
            node['mesh'] = mesh_remap[node['mesh']]
    write_glb(target, document, binary)
    print(f'{target}: {len(removed)} nodes removed: ' + ', '.join(sorted(names[index] or f'#{index}' for index in removed)))


if __name__ == '__main__':
    main()
