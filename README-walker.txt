WALKER - игра про шагающих роботов на движке Quake III Arena (GPL)
=================================================================

Отдельная игра со своими данными: файлы Quake III (baseq3, pak0.pk3) не нужны.
Цель - аналог War Robots, который работает на старом железе и старых ОС (Windows XP/7, OpenGL 1.2).

Сейчас есть:
- робот walker (вид от третьего лица, без прыжков, автопушка с бесконечным боезапасом);
- карта wk_islands "Islands": острова с мшистыми руинами в мелкой воде, скалы по краям,
  башня с кольцевой платформой, стартовая ферма с ракетой, вертолётные площадки,
  платформы на опорах, маяки A-E (пока только как ориентиры, захвата нет);
- боты: 8 пилотов (Anvil, Bastion, Cinder, Drake, Ember, Flint, Granite, Hornet) с разным
  характером, три окраски робота (стандартная, desert, urban);
- свои шрифты, меню, прицелы, HUD, эффекты и синтезированные звуки.

Пока нет: захвата маяков, выбора роботов.


Сборка и запуск
---------------

Нужен 32-битный MinGW (gcc, windres, mingw32-make). 64-битный компилятор не подходит:
ни движок, ни компилятор lcc не рассчитаны на 64 бита.

1. Сборка (из папки code):
     mingw32-make -f Makefile.mingw
   Цели: all (по умолчанию) = engine + qvm; engine - только Walker.exe;
   qvm - только игровые модули; clean - удалить результаты сборки.

   Порядок внутри all:
   - engine: code/*.c -> code/build-mingw/*.o -> walker/dist/Walker.exe
   - qvm (code/Makefile.qvm): сначала инструменты из исходников репозитория
     (lcc/: lburg, rcc, cpp; q3asm/) в code/build-qvm-tools, затем
     code/game, code/cgame, code/q3_ui -> walker/dist/basewalker/vm/qagame.qvm, cgame.qvm, ui.qvm

2. Готовая папка игры - walker/dist:
     Walker.exe
     basewalker/pak0.pk3        (данные, в репозитории)
     basewalker/wk_islands.pk3  (карта, в репозитории)
     basewalker/vm/*.qvm        (только что собранные модули)
   Файлы из папки basewalker имеют приоритет над теми же файлами внутри pk3, поэтому
   собранные QVM заменяют старые копии из pak0.pk3.

3. Запуск (рабочая папка - walker/dist, движок ищет basewalker в текущей папке):
     Walker.exe +map wk_islands
   Или из меню: SINGLE PLAYER (бой с ботами) или MULTIPLAYER -> START SERVER -> Islands.

После правки любого .h пересобирается всё (так проще и надёжнее, чем отслеживать зависимости).
pk3 с данными пересобираются отдельно (Python, см. "Пересборка данных"), для обычной
работы с кодом это не нужно.

Движок теперь ищет данные в папке basewalker (BASEGAME в code/qcommon/qcommon.h) и не
требует CD-ключа и файлов Quake III.


Боты
----

SINGLE PLAYER открывает экран настройки боя: карта, затем список ботов и их уровень
(Bot Skill), кнопка FIGHT. По умолчанию bot_minplayers 6: если игроков меньше шести,
сервер сам добавляет ботов (по одному раз в 10 секунд). Отключить: bot_minplayers 0.

В игре: Esc -> BOTS - список всех ботов, щелчок по имени добавляет бота в бой
(с уровнем из строки "Skill of new bots") или убирает его.

Команды консоли:
  addbot Anvil 3        добавить бота (уровень 1-5)
  kick Anvil            убрать бота
  team spectator        стать зрителем; follow Anvil - смотреть за ботом

Ботам нечего подбирать (на карте нет предметов), поэтому они ходят между маяками A-E
и точками появления и вступают в бой с теми, кого видят.


Управление
----------

W A S D - ходьба, мышь - поворот корпуса и прицел, ЛКМ - огонь, ПКМ - приближение,
TAB - счёт, ~ - консоль, ESC - меню, F12 - снимок экрана (basewalker/screenshots).


Пересборка данных
-----------------

Данные создаются скриптами (Linux или Windows с Python 3, numpy, scipy, Pillow):

  walker/tools/build_all.sh <папка с cpp, rcc, q3asm> <q3map> <bspc>

Что делает каждый скрипт:
  make_basedata.py    default.cfg, шрифты, меню, прицелы, цифры HUD, иконки, эффекты
  make_sounds.py      звуки: выстрелы автопушки, рикошеты, шаги, всплески, удары по корпусу,
                      взрывы, звуки меню
  make_map_islands.py карта: текстуры, небо, шейдеры, рельеф (сетка 64 ед. из треугольных
                      брашей), вода, постройки, точки появления; пишет .map для q3map
  make_botfiles.py    файлы ботов: характеры, оружие, фразы, scripts/bots.txt,
                      окраски робота desert и urban
  build_qvm.sh        qagame.qvm, cgame.qvm, ui.qvm (lcc + q3asm из этого репозитория)

Инструменты lcc и q3asm собирает code/Makefile.qvm (папка code/build-qvm-tools).
  q3map: q3map/*.c + common/*.c (правки в common/cmdlib.c позволяют собрать его gcc под Linux)
  bspc:  code/bspc - файлы из списка GAME_OBJS в code/bspc/Makefile, флаги
         -DLINUX -DBSPC -Dstricmp=strcasecmp

Навигация ботов (AAS) строится bspc после компиляции карты:
  bspc -cfg walker/maps/aas_walker.cfg -bsp2aas wk_islands.bsp
В aas_walker.cfg размеры робота, скорость 110 и запрет прыжков (боты не строят маршрутов,
где нужно прыгать). Расчёт около минуты.

Компиляция карты: q3map wk_islands.map; q3map -vis wk_islands.map; q3map -light wk_islands.map
(свет считается около 5 минут). Путь к .map должен содержать папку quake*/basewalker,
по нему q3map находит текстуры и шейдеры.

Масштаб: робот высотой 56 единиц соответствует ~10 м, то есть 1 м ~ 5.6 единицы.
Карта 3584 x 2688 единиц, игровая зона ~2900 x 2050 (около 520 x 370 м).
Глубина воды не больше 20 единиц: робот идёт по дну, а не плывёт.


Изменения в коде движка и игры (помечены комментарием WALKER)
-------------------------------------------------------------

  qcommon/qcommon.h      BASEGAME "basewalker"
  qcommon/files.c        нет демо-режима и проверки productid; basewalker вместо baseq3;
                         файлы из папки имеют приоритет над pk3 той же папки
  qcommon/common.c       CD-ключ пишется в basewalker
  win32/win_glimp.c      заголовок окна WALKER
  win32/winquake.rc      своя иконка walker.ico (рисует walker/tools/make_icon.py)
  renderer/tr_image.c    GL_CLAMP_TO_EDGE вместо GL_CLAMP (убирает швы на небе и спрайтах)
  renderer/tr_init.c     r_overBrightBits 0 по умолчанию: освещение не зависит от
                         аппаратной гаммы (Vista/7 отклоняют такие таблицы, картинка темнеет)
  win32/win_gamma.c      ограничение таблицы гаммы и для Vista/7, иначе яркость перестаёт меняться
  client/cl_main.c       консоль и сообщения в углу рисуются своим шрифтом gfx/2d/consolechars
                         (8x16 пиксель в пиксель)
  q3_ui/ui_atoms.c       ползунки в меню можно тянуть мышью
  q3_ui/ui_qmenu.c       выделенный пункт тёплым белым вместо жёлтого
  q3_ui/ui_menu.c,       убраны пункты Quake: CINEMATICS, MODS, CD KEY, PunkBuster;
  ui_setup.c,            уровни ботов: Recruit, Regular, Veteran, Elite, Ace
  ui_startserver.c, ui_servers2.c, ui_addbots.c
  q3asm/q3asm.c          при ошибках сборки возвращает код ошибки (make останавливается)
  q3_ui/ui_selectbots.c  новое меню BOTS (Esc в игре): все боты списком, щелчок добавляет
                         бота в бой или убирает; заменяет ADD BOTS и REMOVE BOTS
  q3_ui/ui_players.c     превью робота в меню подгоняется под настоящие размеры модели,
                         без отдельной модели оружия не рисуются оси вместо пушки
  q3_ui/ui_playermodel.c, ui_playersettings.c, ui_controls2.c - окно превью в пределах экрана;
                         убран пункт EFFECTS (цвет следа рейлгана)
  cgame/cg_servercmds.c  надпись FIGHT! вдвое меньше и потому чётче
  q3_ui/ui_qmenu.c       пункты меню в оранжевой гамме вместо красной
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
  q3_ui/ui_menu.c        заголовок WALKER, без проверки CD-ключа, свои цвета;
                         SINGLE PLAYER открывает бой с ботами вместо лестницы Q3
  q3_ui/ui_startserver.c имена ботов по умолчанию для командных игр
  game/ai_dmq3.c         боты с одной автопушкой не отступают (агрессия зависит от
                         повреждений), бесконечный боезапас понятен ботам
  game/ai_dmnet.c        без предметов боты идут к маякам и точкам появления
  cgame/cg_event.c       сообщение об уничтожении "was shot down by"
  bspc/l_bsp_hl.c        исправлена строка, которую не принимает современный gcc

Лицензии: LICENSES.md.
