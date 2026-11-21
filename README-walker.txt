WALKER - игра про шагающих роботов на движке Quake III Arena (GPL)
=================================================================

Отдельная игра со своими данными: файлы Quake III (baseq3, pak0.pk3) не нужны.
Цель - аналог War Robots, который работает на старом железе и старых ОС (Windows XP/7, OpenGL 1.2).

Сейчас есть:
- робот walker (вид от третьего лица, без прыжков, автопушка с бесконечным боезапасом);
- карта wk_islands "Islands": острова с мшистыми руинами в мелкой воде, скалы по краям,
  башня с кольцевой платформой, стартовая ферма с ракетой, вертолётные площадки,
  платформы на опорах, маяки A-E (пока только как ориентиры, захвата нет);
- свои шрифты, меню, прицелы, HUD, эффекты и синтезированные звуки.

Пока нет: ботов (одиночная игра из меню не работает), захвата маяков, выбора роботов.


Установка
---------

1. Собрать quake3.exe (32-битный MinGW, из папки code):
     mingw32-make -f Makefile.mingw
   Результат: code/build-mingw/quake3.exe

2. Сложить в одну папку:
     WALKER/quake3.exe
     WALKER/basewalker/pak0.pk3        (из walker/dist/basewalker)
     WALKER/basewalker/wk_islands.pk3  (из walker/dist/basewalker)

3. Запуск сразу на карту:
     quake3.exe +map wk_islands
   Или из меню: MULTIPLAYER -> START SERVER -> Islands.

Движок теперь ищет данные в папке basewalker (BASEGAME в code/qcommon/qcommon.h) и не
требует CD-ключа и файлов Quake III.


Управление
----------

W A S D - ходьба, мышь - поворот корпуса и прицел, ЛКМ - огонь, ПКМ - приближение,
TAB - счёт, ~ - консоль, ESC - меню, F12 - снимок экрана (basewalker/screenshots).


Пересборка данных
-----------------

Данные создаются скриптами (Linux или Windows с Python 3, numpy, scipy, Pillow):

  walker/tools/build_all.sh <папка с cpp, rcc, q3asm> <q3map>

Что делает каждый скрипт:
  make_basedata.py    default.cfg, шрифты, меню, прицелы, цифры HUD, иконки, эффекты
  make_sounds.py      звуки: выстрелы автопушки, рикошеты, шаги, всплески, удары по корпусу,
                      взрывы, звуки меню
  make_map_islands.py карта: текстуры, небо, шейдеры, рельеф (сетка 64 ед. из треугольных
                      брашей), вода, постройки, точки появления; пишет .map для q3map
  build_qvm.sh        qagame.qvm, cgame.qvm, ui.qvm (lcc + q3asm из этого репозитория)

Инструменты собираются из исходников репозитория:
  lcc:   cd lcc && make BUILDDIR=<папка> rcc cpp
  q3asm: q3asm/q3asm.c + q3asm/cmdlib.c
  q3map: q3map/*.c + common/*.c (правки в common/cmdlib.c позволяют собрать его gcc под Linux)

Компиляция карты: q3map wk_islands.map; q3map -vis wk_islands.map; q3map -light wk_islands.map
(свет считается около 5 минут). Путь к .map должен содержать папку quake*/basewalker,
по нему q3map находит текстуры и шейдеры.

Масштаб: робот высотой 56 единиц соответствует ~10 м, то есть 1 м ~ 5.6 единицы.
Карта 3584 x 2688 единиц, игровая зона ~2900 x 2050 (около 520 x 370 м).
Глубина воды не больше 20 единиц: робот идёт по дну, а не плывёт.


Изменения в коде движка и игры (помечены комментарием WALKER)
-------------------------------------------------------------

  qcommon/qcommon.h      BASEGAME "basewalker"
  qcommon/files.c        нет демо-режима и проверки productid; basewalker вместо baseq3
  qcommon/common.c       CD-ключ пишется в basewalker
  win32/win_glimp.c      заголовок окна WALKER
  renderer/tr_image.c    GL_CLAMP_TO_EDGE вместо GL_CLAMP (убирает швы на небе и спрайтах)
  game/bg_pmove.c        без прыжков
  game/g_client.c        только автопушка, боезапас бесконечный
  game/g_main.c          g_speed 110
  cgame/cg_local.h       модель по умолчанию walker
  cgame/cg_main.c        вид от третьего лица по умолчанию, дистанция камеры 110
  cgame/cg_servercmds.c  вид от третьего лица не сбрасывается при перезапуске карты
  cgame/cg_view.c        камера выше (робот крупнее человека)
  cgame/cg_draw.c        прицел виден и в виде от третьего лица
  cgame/cg_weapons.c     вспышка выстрела у tag_weapon робота, если нет модели оружия
  cgame/cg_playerstate.c нет предупреждения о боезапасе при бесконечных патронах
  q3_ui/ui_menu.c        заголовок WALKER, без проверки CD-ключа, свои цвета

Лицензии: LICENSES.md.
