# Сборка Walker локально

Для работы с кодом хватит двух инструментов. Остальное нужно только для пересборки
данных игры, и эта часть пока работает только под Linux.

## 1. Сборка игры (Walker.exe и модули QVM) — нужно всегда

- **Git for Windows** — для работы с репозиторием.
- **32-битный MinGW**: `gcc`, `windres`, `mingw32-make`.
  64-битный не подойдёт: ни движок, ни компилятор lcc для QVM не рассчитаны на 64 бита.
  Для Windows 7 подходит MinGW-w64 8.1.0 в варианте i686, его ставят с SourceForge.
  Новые сборки GCC (например, winlibs на UCRT) могут не запуститься на Windows 7.

Проверка в Git Bash:

```
gcc -dumpmachine        # должно быть i686-w64-mingw32 (или mingw32)
mingw32-make --version
windres --version
```

Больше ничего ставить не нужно:

- компилятор lcc и ассемблер q3asm для модулей QVM `code/Makefile.qvm` собирает сам
  из исходников репозитория;
- данные игры (`pak0.pk3`, `wk_islands.pk3`) уже лежат готовыми в `walker/dist/basewalker`.

### Сборка

Из папки `code`:

```
mingw32-make -f Makefile.mingw          # всё: Walker.exe и QVM (цель all)
mingw32-make -f Makefile.mingw engine   # только Walker.exe
mingw32-make -f Makefile.mingw qvm      # только модули QVM
mingw32-make -f Makefile.mingw clean    # удалить результаты сборки
```

Результат появляется в `walker/dist`:

```
walker/dist/Walker.exe
walker/dist/basewalker/pak0.pk3        (данные, в репозитории)
walker/dist/basewalker/wk_islands.pk3  (карта, в репозитории)
walker/dist/basewalker/vm/*.qvm        (только что собранные модули)
```

Собранные модули из `basewalker/vm` заменяют их копии внутри `pak0.pk3`.

### Запуск

Рабочая папка — `walker/dist` (движок ищет `basewalker` в текущей папке):

```
Walker.exe +set bot_minplayers 6 +map wk_islands
```

Без параметров открывается меню.

## 2. Пересборка данных — только если меняете генераторы

Сюда относятся шрифты, текстуры, звуки, карта и боты: скрипты в `walker/tools`,
общий запуск — `walker/tools/build_all.sh`.

Понадобится:

- **Python 3** с пакетами numpy, scipy и Pillow. Для Windows 7 последняя подходящая
  версия — Python 3.8.
- Для запекания текстур робота (`bake_walker.py`) ещё **xatlas** и **embreex**:
  `pip install xatlas embreex meshoptimizer scikit-image scipy` (meshoptimizer — для упрощённых моделей, `make_lods.py`). У embreex нет сборки для 32-битного Python под Windows,
  поэтому запекание пока делается под Linux или 64-битным Python.
- **ffmpeg** — для нарезки шагов по воде из записи (`import_splash.py`).
- Утилиты **zip** и **sh** — для `build_all.sh`.
- **q3map** (компилятор карт) и **bspc** (навигация ботов) — собираются из репозитория.
  В q3map из репозитория внесены изменения: свет от неба (`-skylight`), многопоточность,
  поднятые лимиты, а освещение вершин моделей (misc_model) берётся в точке, вынесенной
  из блоков коллизии. Ключ шейдера `q3map_nosunshadow`: поверхность не загораживает
  солнце (тени построек рисует игра), свет неба загораживает.
  После изменения `q3map/shaders.h` пересобирайте все объектные файлы q3map: структура
  шейдера используется почти во всех модулях.
  q3map собирается только 32-битным (`-m32`): в коде есть приведения указателей к `int`, и
  64-битная сборка падает при расчёте освещения. Под Linux:
  ```
  cd q3map
  for f in *.c; do [ $f = gldraw.c ] || gcc -m32 -c -O2 -fcommon -I../common -w -DCLK_TCK=CLOCKS_PER_SEC $f -o /tmp/q3mapbuild/${f%.c}.o; done
  for f in aselib bspfile cmdlib imagelib mathlib md4 mutex polylib scriplib threads trilib; do gcc -m32 -c -O2 -fcommon -I../common -w ../common/$f.c -o /tmp/q3mapbuild/c_$f.o; done
  gcc -m32 -o /tmp/q3mapbuild/q3map /tmp/q3mapbuild/*.o -lm -lpthread
  ```
  Лимиты карты q3map берёт из `code/qcommon/qfiles.h` (через `common/bspfile.h`):
  MAX_MAP_DRAW_VERTS и MAX_MAP_DRAW_INDEXES подняты до 0x400000; движок их не использует.

### Ограничения под Windows

Сейчас эта часть под Windows как есть не заработает:

- в генераторах прописаны пути к шрифтам в стиле Linux (`/usr/share/fonts/...`);
- `q3map` и `bspc` собирались только под Linux, makefile под MinGW для них нет;
- в Git Bash нет команды `zip`.

Пока данные пересобираются под Linux, а для работы с кодом используются готовые pk3
из репозитория. Чтобы пересобирать данные под Windows, нужно:

- перевести генераторы на шрифты, лежащие в репозитории;
- добавить сборку `q3map` и `bspc` через MinGW;
- заменить `zip` на упаковку средствами Python.
