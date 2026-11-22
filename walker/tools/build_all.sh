#!/bin/sh
# Full build of the WALKER game data: base data, sounds, robot model, QVMs and the wk_islands map.
#
#   walker/tools/build_all.sh <dir with lcc tools: cpp rcc q3asm> <q3map binary> <bspc binary>
#
# Result: walker/dist/basewalker/pak0.pk3 (base data, model, QVMs)
#         walker/dist/basewalker/wk_islands.pk3 (map)
# Needs: python3 with numpy, scipy, Pillow, xatlas, embreex, meshoptimizer; zip; fonts DejaVu (and Anton, optional).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS="$ROOT/walker/tools"
LCC=$(cd "$1" && pwd)
Q3MAP=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
BSPC=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
STAGE=${WALKER_STAGE:-/tmp/walker_stage}
DIST="$ROOT/walker/dist/basewalker"

rm -rf "$STAGE"
# q3map finds the game directory by a "quake*" component in the path
mkdir -p "$STAGE/base" "$STAGE/map" "$STAGE/quake3/basewalker" "$DIST"

echo "== base data"
python3 "$TOOLS/make_basedata.py" "$STAGE/base"
python3 "$TOOLS/make_sounds.py" "$STAGE/base"
# steps in water from a recording, if there is one (replaces the synthesized splash1-4)
if [ -f "$ROOT/walker/sounds/footsteps_water.mp3" ]; then
  python3 "$TOOLS/import_splash.py" "$ROOT/walker/sounds/footsteps_water.mp3" "$STAGE/base"
fi
python3 "$TOOLS/make_botfiles.py" "$STAGE/base" "$ROOT/code"
mkdir -p "$STAGE/base/models/players"
cp -r "$ROOT/assets/baseq3/models/players/walker" "$STAGE/base/models/players/"
# robot: unique UV atlas with baked occlusion, wear and dirt for every paint scheme
python3 "$TOOLS/bake_walker.py" "$STAGE/base/models/players/walker"
# simplified robot models for long distances (lower_1..3.glb etc., needs meshoptimizer)
python3 "$TOOLS/make_lods.py" "$STAGE/base/models/players/walker"

echo "== QVM"
cd "$ROOT"
"$TOOLS/build_qvm.sh" "$LCC" "$STAGE/base/vm"
rm -f "$STAGE/base/vm/"*.map

echo "== map wk_islands"
python3 "$TOOLS/make_map_islands.py" "$STAGE/map" "$STAGE/quake3/basewalker"
cd "$STAGE/quake3/basewalker/maps"
"$Q3MAP" wk_islands.map
"$Q3MAP" -vis wk_islands.map
# sun + weak light from the sky dome (48 directions, WALKER patch in q3map/light.c)
"$Q3MAP" -light -skylight 24 48 wk_islands.map
# bot navigation (AAS): robot box, no jumps. Built from a copy of the map with the
# terrain on a 64-unit grid (WALKER_NAV=1): ~2.5 times fewer areas, bot routing is
# much cheaper; the AAS gets the checksum of the real map (BSPC_CHECKSUM_FROM)
rm -rf "$STAGE/nav" && mkdir -p "$STAGE/nav/data" "$STAGE/nav/quake3/basewalker"
WALKER_NAV=1 python3 "$TOOLS/make_map_islands.py" "$STAGE/nav/data" "$STAGE/nav/quake3/basewalker"
(cd "$STAGE/nav/quake3/basewalker/maps" && "$Q3MAP" wk_islands.map)
BSPC_CHECKSUM_FROM="$STAGE/quake3/basewalker/maps/wk_islands.bsp" \
	"$BSPC" -cfg "$ROOT/walker/maps/aas_walker.cfg" -bsp2aas "$STAGE/nav/quake3/basewalker/maps/wk_islands.bsp"
mkdir -p "$STAGE/map/maps" "$STAGE/map/levelshots"
cp wk_islands.bsp "$STAGE/map/maps/"
cp "$STAGE/nav/quake3/basewalker/maps/wk_islands.aas" "$STAGE/map/maps/"
cp "$ROOT/walker/maps/levelshots/wk_islands.jpg" "$STAGE/map/levelshots/"

echo "== pk3"
rm -f "$DIST/pak0.pk3" "$DIST/wk_islands.pk3"
cd "$STAGE/base" && zip -q -r -9 "$DIST/pak0.pk3" .
cd "$STAGE/map" && zip -q -r -9 "$DIST/wk_islands.pk3" .
ls -la "$DIST"
