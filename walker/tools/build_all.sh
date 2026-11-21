#!/bin/sh
# Full build of the WALKER game data: base data, sounds, robot model, QVMs and the wk_islands map.
#
#   walker/tools/build_all.sh <dir with lcc tools: cpp rcc q3asm> <q3map binary>
#
# Result: walker/dist/basewalker/pak0.pk3 (base data, model, QVMs)
#         walker/dist/basewalker/wk_islands.pk3 (map)
# Needs: python3 with numpy, scipy, Pillow; zip; fonts DejaVu (and Anton, optional).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS="$ROOT/walker/tools"
LCC=$(cd "$1" && pwd)
Q3MAP=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
STAGE=${WALKER_STAGE:-/tmp/walker_stage}
DIST="$ROOT/walker/dist/basewalker"

rm -rf "$STAGE"
# q3map finds the game directory by a "quake*" component in the path
mkdir -p "$STAGE/base" "$STAGE/map" "$STAGE/quake3/basewalker" "$DIST"

echo "== base data"
python3 "$TOOLS/make_basedata.py" "$STAGE/base"
python3 "$TOOLS/make_sounds.py" "$STAGE/base"
mkdir -p "$STAGE/base/models/players"
cp -r "$ROOT/assets/baseq3/models/players/walker" "$STAGE/base/models/players/"

echo "== QVM"
cd "$ROOT"
"$TOOLS/build_qvm.sh" "$LCC" "$STAGE/base/vm"
rm -f "$STAGE/base/vm/"*.map

echo "== map wk_islands"
python3 "$TOOLS/make_map_islands.py" "$STAGE/map" "$STAGE/quake3/basewalker"
cd "$STAGE/quake3/basewalker/maps"
"$Q3MAP" wk_islands.map
"$Q3MAP" -vis wk_islands.map
"$Q3MAP" -light wk_islands.map
mkdir -p "$STAGE/map/maps" "$STAGE/map/levelshots"
cp wk_islands.bsp "$STAGE/map/maps/"
cp "$ROOT/walker/maps/levelshots/wk_islands.jpg" "$STAGE/map/levelshots/"

echo "== pk3"
rm -f "$DIST/pak0.pk3" "$DIST/wk_islands.pk3"
cd "$STAGE/base" && zip -q -r -9 "$DIST/pak0.pk3" .
cd "$STAGE/map" && zip -q -r -9 "$DIST/wk_islands.pk3" .
ls -la "$DIST"
