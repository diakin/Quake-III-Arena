#!/bin/sh
# Сборка игровых модулей (QVM) из code/game, code/cgame, code/q3_ui компилятором lcc и q3asm.
# Использование: walker/tools/build_qvm.sh <папка инструментов lcc> <папка вывода vm>
#   инструменты: cpp, rcc (из lcc/, цель bytecode) и q3asm (из q3asm/)
set -e
TOOLS=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CODE=$ROOT/code
BUILD=${BUILD:-/tmp/qvmbuild}
mkdir -p "$OUT"

compile() {   # модуль, файлы...
  module=$1; shift
  mkdir -p "$BUILD/$module"
  for source in "$@"; do
    name=$(basename "$source" .c)
    "$TOOLS/cpp" -D__STDC__=1 -D__LCC__ -DQ3_VM -I"$CODE/cgame" -I"$CODE/game" -I"$CODE/q3_ui" "$CODE/$source" "$BUILD/$module/$name.i"
    "$TOOLS/rcc" -target=bytecode -g "$BUILD/$module/$name.i" "$BUILD/$module/$name.asm"
  done
}
assemble() {  # модуль, имя qvm, файл syscalls.asm, файлы...
  module=$1; output=$2; syscalls=$3; shift 3
  list=$BUILD/$module.q3asm
  echo "-o \"$OUT/$output\"" > "$list"
  for source in "$@"; do echo "$BUILD/$module/$(basename "$source" .c)" >> "$list"; done
  echo "${syscalls%.asm}" >> "$list"
  (cd "$BUILD" && "$TOOLS/q3asm" -f "$list")
}

GAME="game/g_main.c game/bg_misc.c game/bg_lib.c game/bg_pmove.c game/bg_slidemove.c game/q_math.c game/q_shared.c
 game/ai_dmnet.c game/ai_dmq3.c game/ai_team.c game/ai_main.c game/ai_chat.c game/ai_cmd.c game/ai_vcmd.c
 game/g_active.c game/g_arenas.c game/g_bot.c game/g_client.c game/g_cmds.c game/g_combat.c game/g_items.c game/g_mem.c
 game/g_misc.c game/g_missile.c game/g_mover.c game/g_session.c game/g_spawn.c game/g_svcmds.c game/g_target.c
 game/g_team.c game/g_trigger.c game/g_utils.c game/g_weapon.c"
CGAME="cgame/cg_main.c cgame/cg_consolecmds.c cgame/cg_draw.c cgame/cg_drawtools.c cgame/cg_effects.c cgame/cg_ents.c
 cgame/cg_event.c cgame/cg_info.c cgame/cg_localents.c cgame/cg_marks.c cgame/cg_players.c cgame/cg_playerstate.c
 cgame/cg_predict.c cgame/cg_scoreboard.c cgame/cg_servercmds.c cgame/cg_snapshot.c cgame/cg_view.c cgame/cg_weapons.c
 game/bg_slidemove.c game/bg_pmove.c game/bg_lib.c game/bg_misc.c game/q_math.c game/q_shared.c"
UI="q3_ui/ui_main.c q3_ui/ui_gameinfo.c q3_ui/ui_atoms.c q3_ui/ui_cinematics.c q3_ui/ui_connect.c q3_ui/ui_controls2.c
 q3_ui/ui_demo2.c q3_ui/ui_mfield.c q3_ui/ui_credits.c q3_ui/ui_menu.c q3_ui/ui_ingame.c q3_ui/ui_confirm.c q3_ui/ui_setup.c
 q3_ui/ui_options.c q3_ui/ui_display.c q3_ui/ui_sound.c q3_ui/ui_network.c q3_ui/ui_playermodel.c q3_ui/ui_players.c
 q3_ui/ui_playersettings.c q3_ui/ui_preferences.c q3_ui/ui_qmenu.c q3_ui/ui_serverinfo.c q3_ui/ui_servers2.c
 q3_ui/ui_sparena.c q3_ui/ui_specifyserver.c q3_ui/ui_sppostgame.c q3_ui/ui_splevel.c q3_ui/ui_spskill.c
 q3_ui/ui_startserver.c q3_ui/ui_team.c q3_ui/ui_video.c q3_ui/ui_addbots.c q3_ui/ui_removebots.c q3_ui/ui_teamorders.c
 q3_ui/ui_loadconfig.c q3_ui/ui_saveconfig.c q3_ui/ui_cdkey.c q3_ui/ui_mods.c game/bg_misc.c game/bg_lib.c game/q_math.c game/q_shared.c"

compile game $GAME
assemble game qagame "$CODE/game/g_syscalls.asm" $GAME
compile cgame $CGAME
assemble cgame cgame "$CODE/cgame/cg_syscalls.asm" $CGAME
compile ui $UI
assemble ui ui "$CODE/ui/ui_syscalls.asm" $UI
ls -l "$OUT"
