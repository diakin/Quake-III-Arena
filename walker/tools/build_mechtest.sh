#!/bin/sh
# Test map wk_mechtest with the blockout robot "mech" (WALKER, 28.11.2026).
#
#   walker/tools/build_mechtest.sh <q3map binary> <bspc binary>
#
# Result: walker/dist/basewalker/wk_mechtest.pk3 - the map (bsp, aas, textures, shader,
# arena, levelshot) and the model models/players/mech with scripts/walker_mech.shader.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS="$ROOT/walker/tools"
Q3MAP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
BSPC=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
STAGE=${WALKER_STAGE:-/tmp/walker_mechtest}

rm -rf "$STAGE"
# q3map finds the game directory by a "quake*" component in the path
mkdir -p "$STAGE/quake3/basewalker" "$STAGE/pk3/maps" "$STAGE/pk3/scripts" "$STAGE/pk3/textures" "$STAGE/pk3/levelshots"

python3 "$TOOLS/make_mech_test.py" "$STAGE/pk3"
python3 "$TOOLS/make_map_mechtest.py" "$STAGE/quake3/basewalker"
cd "$STAGE/quake3/basewalker/maps"
"$Q3MAP" wk_mechtest.map
"$Q3MAP" -vis wk_mechtest.map
"$Q3MAP" -light -skylight 24 48 wk_mechtest.map
"$BSPC" -cfg "$ROOT/walker/maps/aas_walker.cfg" -bsp2aas wk_mechtest.bsp

cp wk_mechtest.bsp wk_mechtest.aas "$STAGE/pk3/maps/"
cp -r ../textures/mechtest "$STAGE/pk3/textures/"
cp ../scripts/mechtest.shader ../scripts/wk_mechtest.arena "$STAGE/pk3/scripts/"
if [ -f "$ROOT/walker/maps/levelshots/wk_mechtest.jpg" ]; then
  cp "$ROOT/walker/maps/levelshots/wk_mechtest.jpg" "$STAGE/pk3/levelshots/"
fi
rm -f "$ROOT/walker/dist/basewalker/wk_mechtest.pk3"
cd "$STAGE/pk3" && zip -q -r -9 "$ROOT/walker/dist/basewalker/wk_mechtest.pk3" .
ls -la "$ROOT/walker/dist/basewalker/wk_mechtest.pk3"
