"""Базовые данные игры WALKER: всё, что движок, cgame и меню загружают по именам.

python3 walker/tools/make_basedata.py <папка вывода>

Создаёт: default.cfg, шрифты (консоль, меню), фон и кнопки меню, прицелы, цифры HUD,
иконки, текстуры и шейдеры эффектов (трассеры, дым, взрывы, следы попаданий).
Всё рисуется здесь же (PIL + numpy), сторонних файлов нет. Шрифты для отрисовки — DejaVu и Anton (OFL).
"""
import math, os, re, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODE = os.path.join(ROOT, 'code')
FONT_MONO = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
FONT_BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'
FONT_TITLE = '/usr/share/fonts/truetype/google-fonts/ofl/anton/Anton-Regular.ttf'
if not os.path.exists(FONT_TITLE):
    FONT_TITLE = FONT_BOLD
rng = np.random.default_rng(7)

ACCENT = (232, 150, 40)      # оранжевый
STEEL = (150, 168, 178)


def save(path, image, quality=88):
    full = os.path.join(OUT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    if path.endswith('.jpg'):
        image.convert('RGB').save(full, quality=quality)
    else:
        image.save(full)


def write(path, text):
    full = os.path.join(OUT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'w', encoding='ascii', newline='\n') as file:
        file.write(text)


# ---------------------------------------------------------------- default.cfg
DEFAULT_CFG = r"""// WALKER default settings (read by the engine at startup)
unbindall
bind TAB "+scores"
bind ENTER "+button2"
bind ESCAPE "togglemenu"
bind SPACE "+moveup"
bind 1 "weapon 1"
bind 2 "weapon 2"
bind 3 "weapon 3"
bind 4 "weapon 4"
bind 5 "weapon 5"
bind w "+forward"
bind s "+back"
bind a "+moveleft"
bind d "+moveright"
bind c "+movedown"
bind t "messagemode"
bind y "messagemode2"
bind ` "toggleconsole"
bind ~ "toggleconsole"
bind UPARROW "+forward"
bind DOWNARROW "+back"
bind LEFTARROW "+left"
bind RIGHTARROW "+right"
bind SHIFT "+speed"
bind CTRL "+attack"
bind F1 "vote yes"
bind F2 "vote no"
bind F11 "screenshot"
bind F12 "screenshotJPEG"
bind PAUSE "pause"
bind MOUSE1 "+attack"
bind MOUSE2 "+zoom"
bind MWHEELUP "weapprev"
bind MWHEELDOWN "weapnext"
seta cg_thirdPerson "1"
seta cg_drawGun "0"
seta cg_fov "85"
seta cg_zoomfov "35"
seta cg_drawCrosshair "4"
seta cg_crosshairSize "28"
seta cg_simpleItems "0"
seta cg_marks "1"
// 2: real shadows of the robots (stencil); 1: a round spot, for very weak video cards
seta cg_shadows "2"
seta cg_drawFPS "1"
seta r_drawfog "1"
seta r_waterRipple "0.35"
seta r_noportals "0"
seta r_stencilbits "8"
seta r_textureMode "GL_LINEAR_MIPMAP_LINEAR"
seta cg_gibs "0"
seta cg_brassTime "0"
seta com_blood "0"
seta sensitivity "5"
seta cl_run "1"
seta com_hunkmegs "128"
seta com_zonemegs "64"
seta r_picmip "0"
seta r_texturebits "32"
seta r_colorbits "32"
seta r_depthbits "24"
seta r_vertexlight "0"
seta r_dynamiclight "1"
seta r_gamma "1.0"
// overbright via the hardware gamma ramp is unreliable on Vista/7 (the ramp gets rejected and
// the picture turns dark); with 0 the lightmaps are brightened in software, the same everywhere
seta r_overBrightBits "0"
seta r_mapOverBrightBits "2"
seta s_volume "0.8"
seta s_musicvolume "0.25"
seta name "Pilot"
seta model "walker"
seta headmodel "walker"
seta team_model "walker"
seta team_headmodel "walker"
seta sv_pure "0"
seta bot_enable "1"
seta bot_minplayers "0"
seta bot_nochat "0"
seta g_spSkill "2"
seta g_speed "110"
seta fraglimit "20"
seta timelimit "10"
"""


# ---------------------------------------------------------------- шрифты
def outlined(image, color=(16, 16, 16, 255), radius=3):
    """белые глифы с тёмной обводкой: читаются и на светлом небе."""
    alpha = image.split()[3]
    shadow = alpha.filter(ImageFilter.MaxFilter(radius))
    result = Image.new('RGBA', image.size, (0, 0, 0, 0))
    result.paste(color, mask=shadow)
    result.alpha_composite(image)
    return result


def console_font():
    """Два шрифта с раскладкой 16x16 символов (код символа = номер клетки).

    gfx/2d/bigchars (512x1024, клетка 32x64): для cgame и меню. Они рисуют символы в
    виртуальных координатах 640x480, чаще всего 8x16, поэтому клетка вытянута 1:2 и глиф
    не искажается; текстура с mip-уровнями, чтобы при уменьшении не рябило.

    gfx/2d/consolechars (128x256, клетка 8x16): для консоли и сообщений в левом верхнем углу.
    Движок рисует их в настоящих пикселях экрана 8x16, поэтому глиф нарисован пиксель
    в пиксель и не масштабируется."""
    cell_width, cell_height = 32, 64
    image = Image.new('RGBA', (cell_width * 16, cell_height * 16), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(FONT_MONO, 50)
    for code in range(32, 127):
        x, y = (code % 16) * cell_width, (code // 16) * cell_height
        draw.text((x + cell_width / 2, y + cell_height * 0.52), chr(code), font=font, fill=(255, 255, 255, 255), anchor='mm')
    for code in (10, 11):
        x, y = (code % 16) * cell_width, (code // 16) * cell_height
        draw.rectangle((x + 2, y + 26, x + 29, y + 37), fill=(255, 255, 255, 255))
    save('gfx/2d/bigchars.tga', outlined(image, radius=5))

    small = Image.new('RGBA', (8 * 16, 16 * 16), (0, 0, 0, 0))
    draw = ImageDraw.Draw(small)
    font = ImageFont.truetype(FONT_MONO, 13)
    for code in range(32, 127):
        x, y = (code % 16) * 8, (code // 16) * 16
        draw.text((x + 4, y + 8), chr(code), font=font, fill=(255, 255, 255, 255), anchor='mm')
    for code in (10, 11):
        x, y = (code % 16) * 8, (code // 16) * 16
        draw.rectangle((x, y + 7, x + 7, y + 8), fill=(255, 255, 255, 255))
    # чёткие края: полупрозрачные пиксели доводим до непрозрачных или убираем
    alpha = small.split()[3].point(lambda value: 255 if value > 110 else (value * 2 if value > 40 else 0))
    small.putalpha(alpha)
    save('gfx/2d/consolechars.tga', outlined(small, radius=3))


def parse_prop_map(name):
    source = open(os.path.join(CODE, 'q3_ui', 'ui_atoms.c'), encoding='latin-1').read()
    start = source.index(f'static int {name}' if name == 'propMapB' else f'static int\t{name}')
    block = source[start:source.index('};', start)]
    block = block.replace('PROP_SPACE_WIDTH', '8')
    return [tuple(int(value) for value in match) for match in re.findall(r'\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}', block)]


PROP_SCALE = 2   # шрифты меню в 512x512: таблицы propMap заданы для 256, движок делит координаты на 256


def prop_fonts():
    """menu/art/font1_prop (+_glo): строчный шрифт меню по таблице propMap (высота 27);
    menu/art/font2_prop: крупные заглавные по propMapB (высота 36).
    Текстуры вдвое больше оригинала (512), координаты из таблиц умножены на 2."""
    scale = PROP_SCALE
    size = 256 * scale
    table = parse_prop_map('propMap')
    image = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font_size = 21 * scale
    font = ImageFont.truetype(FONT_BOLD, font_size)
    for code, (x, y, width) in enumerate(table):
        if width <= 0 or code == 32:
            continue
        glyph = chr(code).upper()     # в шрифте Q3 только заглавные: строчные ссылаются на те же клетки
        box = draw.textbbox((0, 0), glyph, font=font)
        glyph_width = box[2] - box[0]
        scale_font = font
        if glyph_width > width * scale:          # не влезает — шрифт меньше
            scale_font = ImageFont.truetype(FONT_BOLD, max(10, int(font_size * width * scale / glyph_width)))
        draw.text(((x + width / 2) * scale, (y + 27 * 0.80) * scale), glyph, font=scale_font, fill=(255, 255, 255, 255), anchor='ms')
    save('menu/art/font1_prop.tga', image)
    glow = image.split()[3].filter(ImageFilter.GaussianBlur(2.2 * scale)).point(lambda value: min(255, value * 2))
    glow_image = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    glow_image.putalpha(glow)
    save('menu/art/font1_prop_glo.tga', glow_image)

    table_b = parse_prop_map('propMapB')
    image = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    font = ImageFont.truetype(FONT_TITLE, 46 * scale)
    for index, (x, y, width) in enumerate(table_b):
        glyph = chr(ord('A') + index)
        cell = Image.new('L', (80 * scale, 64 * scale), 0)
        cell_draw = ImageDraw.Draw(cell)
        cell_draw.text((40 * scale, 56 * scale), glyph, font=font, fill=255, anchor='ms')
        box = cell.getbbox()
        piece = cell.crop(box).resize((max(1, (width - 2) * scale), 32 * scale), Image.LANCZOS)   # глиф растянут на всю клетку
        layer = Image.new('RGBA', piece.size, (255, 255, 255, 0))
        layer.putalpha(piece)
        image.alpha_composite(layer, ((x + 1) * scale, (y + 2) * scale))
    save('menu/art/font2_prop.tga', image)


# ---------------------------------------------------------------- меню
def panel(width, height, border=ACCENT, fill=(18, 24, 28, 210), bevel=10):
    image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    polygon = [(bevel, 0), (width - 1, 0), (width - 1, height - 1 - bevel), (width - 1 - bevel, height - 1), (0, height - 1), (0, bevel)]
    draw.polygon(polygon, fill=fill, outline=border + (255,))
    return image


def text_button(name, label, active):
    """кнопки вида back_0 / back_1: 128x64, надпись по центру."""
    image = Image.new('RGBA', (128, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    color = ACCENT if active else STEEL
    draw.polygon([(12, 14), (126, 14), (126, 40), (114, 52), (2, 52), (2, 26)], fill=(16, 22, 26, 225), outline=color + (255,))
    font = ImageFont.truetype(FONT_BOLD, 18)
    draw.text((64, 33), label, font=font, fill=color + (255,), anchor='mm')
    if active:
        glow = image.filter(ImageFilter.GaussianBlur(3))
        glow.alpha_composite(image)
        image = glow
    save(f'menu/art/{name}.tga', image)


def side_bracket():
    """боковая рамка меню (frame2_l, frame1_r): тонкая скоба у внешнего края экрана и лёгкая
    тень к краю; середина прозрачная, чтобы не мешать тексту. Рисуется для левой стороны,
    правая — зеркально."""
    width, height = 256, 512
    pixels = np.zeros((height, width, 4), dtype=np.uint8)
    ramp = np.clip(1 - np.arange(width) / 90, 0, 1) ** 1.5
    pixels[..., :3] = (8, 11, 14)
    pixels[..., 3] = (ramp * 110).astype(np.uint8)[None, :]
    image = Image.fromarray(pixels, 'RGBA')
    draw = ImageDraw.Draw(image)
    steel = (110, 128, 138, 220)
    edge = 14
    draw.line([(edge, 30), (edge, height - 30)], fill=steel, width=3)
    draw.line([(edge + 9, 60), (edge + 9, height - 60)], fill=(70, 84, 92, 200), width=2)
    for y in (30, height - 32):
        draw.line([(edge, y), (edge + 60, y)], fill=steel, width=3)
    draw.rectangle((edge - 4, height // 2 - 40, edge + 4, height // 2 + 40), fill=ACCENT + (255,))
    for y in range(90, height - 90, 32):
        draw.line([(edge + 9, y), (edge + 17, y)], fill=(90, 104, 112, 200), width=2)
    return image


def outline_frame(width, height, color, thickness, accent=False):
    """рамка без заливки (выбор карты, соперника)."""
    image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    half = thickness // 2
    draw.rectangle((half, half, width - 1 - half, height - 1 - half), outline=color + (255,), width=thickness)
    if accent:
        corner = width // 5
        for x0, y0, dx, dy in [(0, 0, 1, 1), (width - 1, 0, -1, 1), (0, height - 1, 1, -1), (width - 1, height - 1, -1, -1)]:
            draw.line([(x0, y0 + dy * corner), (x0, y0), (x0 + dx * corner, y0)], fill=(255, 220, 140, 255), width=thickness + 4)
    return image


def framed_panel(width, height, fill_alpha):
    """панель диалогов и внутриигрового меню: тёмная заливка, стальная кромка, оранжевые углы."""
    image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    bevel = 18
    polygon = [(bevel, 2), (width - 3, 2), (width - 3, height - 3 - bevel), (width - 3 - bevel, height - 3), (2, height - 3), (2, 2 + bevel)]
    draw.polygon(polygon, fill=(14, 18, 22, fill_alpha))
    draw.line(polygon + [polygon[0]], fill=(96, 112, 122, 255), width=3)
    draw.line([(2, 2 + bevel + 30), (2, 2 + bevel), (bevel, 2), (bevel + 40, 2)], fill=ACCENT + (255,), width=5)
    draw.line([(width - 3, height - 3 - bevel - 30), (width - 3, height - 3 - bevel), (width - 3 - bevel, height - 3),
               (width - 3 - bevel - 40, height - 3)], fill=ACCENT + (255,), width=5)
    return image


def hangar_art():
    """фон ангара: зал в перспективе, светильники под потолком, поворотный круг в центре."""
    width, height = 1024, 512
    vanish_x, vanish_y = width / 2, height * 0.42
    y, x = np.mgrid[0:height, 0:width].astype(float)
    base = np.zeros((height, width, 3))
    # стены и потолок: тёмная сталь, светлее к центру
    distance = np.hypot((x - vanish_x) / width, (y - vanish_y) / height)
    base[:] = np.array([34, 42, 50])[None, None, :] * (1.15 - 0.6 * distance[..., None])
    noise = (fft_like_noise(height, width, 3) * 10)[..., None]
    image = Image.fromarray(np.clip(base + noise, 0, 255).astype(np.uint8))
    draw = ImageDraw.Draw(image, 'RGBA')
    back = (300, 70, 724, 300)                       # дальняя стена
    draw.rectangle(back, fill=(44, 54, 62, 255), outline=(70, 84, 92, 255), width=3)
    for panel_x in range(back[0], back[2], 53):
        draw.line([(panel_x, back[1]), (panel_x, back[3])], fill=(30, 36, 42, 255), width=2)
    draw.rectangle((back[0], 200, back[2], 214), fill=(200, 160, 40, 255))
    for stripe in range(back[0], back[2], 28):
        draw.polygon([(stripe, 214), (stripe + 14, 214), (stripe + 28, 200), (stripe + 14, 200)], fill=(30, 28, 24, 255))
    # пол: решётка к точке схода
    floor_top = back[3]
    draw.polygon([(back[0], floor_top), (back[2], floor_top), (width, height), (0, height)], fill=(40, 46, 50, 255))
    for step in range(-12, 13):
        draw.line([(vanish_x + step * 35, floor_top), (vanish_x + step * 120, height)], fill=(56, 64, 70, 255), width=2)
    row = floor_top
    gap = 8
    while row < height:
        draw.line([(0, row), (width, row)], fill=(56, 64, 70, 255), width=2)
        gap *= 1.35
        row += gap
    # потолочные балки и светильники
    for beam in range(4):
        top = 10 + beam * 16
        draw.line([(back[0] - 120 + beam * 30, top + 40), (back[2] + 120 - beam * 30, top + 40)], fill=(26, 30, 34, 255), width=8)
        for lamp in range(5):
            lamp_x = back[0] + 30 + lamp * 90
            draw.rectangle((lamp_x - 22, top + 30, lamp_x + 22, top + 36), fill=(255, 236, 190, 255))
    glow = Image.new('L', (width, height), 0)
    glow_draw = ImageDraw.Draw(glow)
    for lamp in range(5):
        glow_draw.ellipse((back[0] + 30 + lamp * 90 - 70, 20, back[0] + 30 + lamp * 90 + 70, 120), fill=90)
    glow = glow.filter(ImageFilter.GaussianBlur(30))
    image = Image.composite(Image.new('RGB', (width, height), (255, 230, 180)), image, glow.point(lambda value: value // 2))
    draw = ImageDraw.Draw(image, 'RGBA')
    # поворотный круг под роботом
    center_x, center_y = width / 2, 318
    draw.ellipse((center_x - 250, center_y - 42, center_x + 250, center_y + 42), fill=(30, 34, 38, 255), outline=ACCENT + (255,), width=4)
    draw.ellipse((center_x - 200, center_y - 32, center_x + 200, center_y + 32), outline=(90, 104, 112, 255), width=2)
    # боковые колонны
    for column_x in (90, 934):
        draw.rectangle((column_x - 26, 0, column_x + 26, height), fill=(28, 32, 36, 255), outline=(70, 84, 92, 255), width=2)
        for mark in range(40, height, 70):
            draw.rectangle((column_x - 26, mark, column_x + 26, mark + 10), fill=(200, 160, 40, 230))
    save('menu/art/hangar.jpg', image)


def fft_like_noise(height, width, seed):
    local = np.random.default_rng(seed)
    white = local.normal(0, 1, (height, width))
    spectrum = np.fft.fft2(white)
    freq_y = np.fft.fftfreq(height)[:, None]
    freq_x = np.fft.fftfreq(width)[None, :]
    radius = np.hypot(freq_x, freq_y)
    radius[0, 0] = 1
    field = np.real(np.fft.ifft2(spectrum / radius ** 1.4))
    return field / (np.abs(field).max() or 1)


def menu_art():
    # фон меню: тёмное небо, силуэт скал и шагающего робота, сетка
    width, height = 1024, 512
    y = np.linspace(0, 1, height)[:, None, None]
    sky = np.zeros((height, width, 3))
    sky[:] = (1 - y) * np.array([46, 58, 66]) + y * np.array([12, 15, 18])
    noise = rng.normal(0, 3, (height, width, 1))
    image = Image.fromarray(np.clip(sky + noise, 0, 255).astype(np.uint8))
    draw = ImageDraw.Draw(image)
    ridge = [(0, height)]
    for x in range(0, width + 16, 16):
        ridge.append((x, height * 0.72 - 40 * math.sin(x / 90) - 30 * math.sin(x / 37 + 1) - rng.random() * 12))
    ridge.append((width, height))
    draw.polygon(ridge, fill=(14, 18, 20))
    for line in range(0, width, 32):
        draw.line([(line, height * 0.72), (line - 200 + line * 0.4, height)], fill=(30, 44, 50), width=1)
    save('menu/art/menuback.jpg', image)
    write('scripts/walker_menu.shader', """// menu and interface
menuback
{
	nopicmip
	nomipmaps
	{
		map menu/art/menuback.jpg
		rgbgen identity
	}
}
menubackRagePro
{
	nopicmip
	nomipmaps
	{
		map menu/art/menuback.jpg
	}
}
console
{
	nopicmip
	nomipmaps
	{
		map menu/art/menuback.jpg
		tcMod scroll 0.02 0
		rgbGen const ( 0.55 0.55 0.55 )
	}
}
medal_excellent
{
	nopicmip
	{
		clampmap menu/medals/medal_excellent.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
medal_impressive
{
	nopicmip
	{
		clampmap menu/medals/medal_impressive.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
medal_gauntlet
{
	nopicmip
	{
		clampmap menu/medals/medal_gauntlet.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
medal_defend
{
	nopicmip
	{
		clampmap menu/medals/medal_defend.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
medal_assist
{
	nopicmip
	{
		clampmap menu/medals/medal_assist.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
medal_capture
{
	nopicmip
	{
		clampmap menu/medals/medal_capture.tga
		blendfunc blend
		rgbgen vertex
		alphagen vertex
	}
}
sprites/balloon3
{
	nopicmip
	{
		clampmap sprites/balloon3.tga
		blendfunc blend
	}
}
sprites/foe
{
	nopicmip
	{
		clampmap sprites/foe.tga
		blendfunc blend
	}
}
lagometer
{
	nopicmip
	{
		map gfx/2d/lag.tga
		blendfunc blend
	}
}
disconnected
{
	nopicmip
	{
		map gfx/2d/net.tga
		blendfunc blend
	}
}
gfx/2d/bigchars
{
	nopicmip
	{
		map gfx/2d/bigchars.tga
		blendfunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbgen vertex
		alphagen vertex
	}
}
white
{
	{
		map $whiteimage
		blendfunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbgen vertex
		alphagen vertex
	}
}
menubacknologo
{
	nopicmip
	nomipmaps
	{
		map menu/art/menuback.jpg
		rgbgen const ( 0.6 0.6 0.6 )
	}
}
menu/art/hangar
{
	nopicmip
	nomipmaps
	{
		map menu/art/hangar.jpg
		rgbgen identity
	}
}
levelShotDetail
{
	nopicmip
	{
		map gfx/2d/white.tga
		blendfunc gl_dst_color gl_src_color
		rgbGen const ( 0.5 0.5 0.5 )
	}
}
""")
    # курсор
    # курсор: меню рисует картинку 32x32 центром в точке мыши, поэтому кончик стрелки — в центре
    cursor = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(cursor)
    draw.polygon([(32, 32), (60, 44), (46, 47), (44, 61)], fill=ACCENT + (255,), outline=(20, 20, 20, 255))
    draw.line([(32, 32), (60, 44), (46, 47), (44, 61), (32, 32)], fill=(20, 20, 20, 255), width=2)
    save('menu/art/3_cursor2.tga', cursor)
    # кнопки
    labels = {
        'accept': 'ACCEPT', 'back': 'BACK', 'create': 'CREATE', 'delete': 'DELETE', 'fight': 'FIGHT', 'load': 'LOAD',
        'menu': 'MENU', 'model': 'MODEL', 'next': 'NEXT', 'play': 'PLAY', 'refresh': 'REFRESH', 'replay': 'REPLAY',
        'reset': 'RESET', 'save': 'SAVE', 'skirmish': 'SKIRMISH', 'specify': 'SPECIFY', 'narrow': 'FILTER',
        'pilot': 'PILOT',
    }
    for name, label in labels.items():
        text_button(f'{name}_0', label, False)
        text_button(f'{name}_1', label, True)
    # стрелки
    for name, direction in [('arrows_horz_0', None), ('arrows_horz_left', 'left'), ('arrows_horz_right', 'right'),
                            ('arrows_vert_0', None), ('arrows_vert_top', 'up'), ('arrows_vert_bot', 'down'),
                            ('gs_arrows_0', None), ('gs_arrows_l', 'left'), ('gs_arrows_r', 'right')]:
        vertical = 'vert' in name
        image = Image.new('RGBA', (64, 128) if vertical else (128, 64), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        shapes = ([(32, 8), (56, 52), (8, 52)], [(32, 120), (56, 76), (8, 76)]) if vertical else ([(8, 32), (52, 8), (52, 56)], [(120, 32), (76, 8), (76, 56)])
        for index, shape in enumerate(shapes):
            lit = (direction in ('up', 'left') and index == 0) or (direction in ('down', 'right') and index == 1)
            draw.polygon(shape, fill=(ACCENT if lit else STEEL) + (255,))
        save(f'menu/art/{name}.tga', image)
    # переключатели, ползунок, рамки
    for name, on in [('switch_on', True), ('switch_off', False)]:
        image = Image.new('RGBA', (32, 16), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.rectangle((1, 2, 30, 13), outline=STEEL + (255,), fill=(16, 22, 26, 255))
        draw.rectangle((16, 4, 28, 11) if on else (3, 4, 15, 11), fill=(ACCENT if on else (90, 98, 104)) + (255,))
        save(f'menu/art/{name}.tga', image)
    slider = Image.new('RGBA', (128, 32), (0, 0, 0, 0))
    ImageDraw.Draw(slider).rectangle((4, 13, 123, 18), fill=(80, 92, 100, 255))
    save('menu/art/slider2.tga', slider)
    for name, color in [('sliderbutt_0', STEEL), ('sliderbutt_1', ACCENT)]:
        image = Image.new('RGBA', (16, 32), (0, 0, 0, 0))
        ImageDraw.Draw(image).rectangle((3, 4, 12, 27), fill=color + (255,))
        save(f'menu/art/{name}.tga', image)
    side_frame = side_bracket()
    save('menu/art/frame2_l.tga', side_frame)
    save('menu/art/frame1_l.tga', side_frame)
    save('menu/art/frame1_r.tga', side_frame.transpose(Image.FLIP_LEFT_RIGHT))
    dialog = framed_panel(256, 256, fill_alpha=230)
    save('menu/art/cut_frame.tga', dialog)
    save('menu/art/addbotframe.tga', dialog)
    save('menu/art/player_models_ports.tga', framed_panel(256, 256, fill_alpha=70))
    save('menu/art/maps_select.tga', outline_frame(256, 256, (84, 98, 106), 3, accent=False))
    save('menu/art/opponents_select.tga', outline_frame(256, 256, STEEL, 4, accent=False))
    save('menu/art/maps_selected.tga', outline_frame(256, 256, ACCENT, 8, accent=True))
    save('menu/art/opponents_selected.tga', outline_frame(256, 256, ACCENT, 8, accent=True))
    unknown = panel(128, 96, fill=(30, 36, 40, 255))
    ImageDraw.Draw(unknown).text((64, 48), '?', font=ImageFont.truetype(FONT_BOLD, 40), fill=STEEL + (255,), anchor='mm')
    save('menu/art/unknownmap.tga', unknown)
    for index in range(1, 6):
        image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for bar in range(index):
            draw.rectangle((6 + bar * 11, 50 - bar * 9, 14 + bar * 11, 56), fill=ACCENT + (255,))
        save(f'menu/art/skill{index}.tga', image)
    for color_name, color in [('base', (60, 60, 60)), ('blue', (60, 90, 255)), ('cyan', (40, 220, 230)), ('grn', (60, 220, 80)),
                              ('red', (240, 60, 40)), ('teal', (40, 170, 150)), ('white', (240, 240, 240)), ('yel', (240, 210, 50))]:
        image = Image.new('RGBA', (128, 16), color + (255,))
        save(f'menu/art/fx_{color_name}.tga', image)
    for name in ['tab/name', 'tab/ping', 'tab/score', 'tab/time']:
        image = Image.new('RGBA', (128, 32), (0, 0, 0, 0))
        ImageDraw.Draw(image).text((64, 16), name.split('/')[1].upper(), font=ImageFont.truetype(FONT_BOLD, 16), fill=STEEL + (255,), anchor='mm')
        save(f'menu/{name}.tga', image)


# ---------------------------------------------------------------- HUD
def crosshairs():
    styles = ['plus', 'circle', 'dot', 'brackets', 'cross', 'ring_dot', 'chevron', 'square', 'tick', 'diamond']
    for index, style in enumerate(styles):
        image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        white = (255, 255, 255, 255)
        if style in ('plus', 'cross'):
            gap = 6 if style == 'plus' else 0
            draw.line((32, 6, 32, 32 - gap), fill=white, width=3); draw.line((32, 32 + gap, 32, 58), fill=white, width=3)
            draw.line((6, 32, 32 - gap, 32), fill=white, width=3); draw.line((32 + gap, 32, 58, 32), fill=white, width=3)
        elif style == 'circle':
            draw.ellipse((10, 10, 54, 54), outline=white, width=3)
        elif style == 'dot':
            draw.ellipse((28, 28, 36, 36), fill=white)
        elif style == 'brackets':
            for x0, x1 in [(10, 18), (54, 46)]:
                draw.line((x0, 14, x1, 14), fill=white, width=3); draw.line((x0, 14, x0, 50), fill=white, width=3); draw.line((x0, 50, x1, 50), fill=white, width=3)
            draw.ellipse((30, 30, 34, 34), fill=white)
        elif style == 'ring_dot':
            draw.ellipse((14, 14, 50, 50), outline=white, width=2); draw.ellipse((29, 29, 35, 35), fill=white)
            for angle in range(0, 360, 90):
                x, y = 32 + 24 * math.cos(math.radians(angle)), 32 + 24 * math.sin(math.radians(angle))
                draw.line((x, y, 32 + 30 * math.cos(math.radians(angle)), 32 + 30 * math.sin(math.radians(angle))), fill=white, width=3)
        elif style == 'chevron':
            draw.line((16, 40, 32, 28, 48, 40), fill=white, width=3); draw.ellipse((30, 30, 34, 34), fill=white)
        elif style == 'square':
            draw.rectangle((18, 18, 46, 46), outline=white, width=3)
        elif style == 'tick':
            for x, y, x1, y1 in [(32, 8, 32, 22), (32, 42, 32, 56), (8, 32, 22, 32), (42, 32, 56, 32)]:
                draw.line((x, y, x1, y1), fill=white, width=3)
        else:
            draw.polygon([(32, 12), (52, 32), (32, 52), (12, 32)], outline=white, width=3)
        save(f'gfx/2d/crosshair{chr(ord("a") + index)}.tga', image)


def numbers():
    names = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'minus']
    for index, name in enumerate(names):
        image = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        glyph = '-' if name == 'minus' else str(index)
        draw.text((16, 16), glyph, font=ImageFont.truetype(FONT_TITLE, 30), fill=(255, 255, 255, 255), anchor='mm')
        save(f'gfx/2d/numbers/{name}_32b.tga', image)


def icon(path, glyph, color=STEEL, shape=None):
    image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.polygon([(10, 2), (62, 2), (62, 52), (52, 62), (2, 62), (2, 12)], fill=(14, 20, 24, 200), outline=color + (255,))
    if shape:
        shape(draw, color)
    else:
        draw.text((32, 33), glyph, font=ImageFont.truetype(FONT_BOLD, 22 if len(glyph) > 2 else 30), fill=color + (255,), anchor='mm')
    save(path, image)


def icons():
    weapons = {
        'gauntlet': 'MEL', 'machinegun': 'AC', 'shotgun': 'SG', 'grenade': 'GL', 'rocket': 'RKT', 'lightning': 'LG',
        'railgun': 'RG', 'plasma': 'PL', 'bfg': 'BFG', 'grapple': 'GR', 'nailgun': 'NG', 'proxlauncher': 'PX', 'chaingun': 'CG',
    }
    for weapon, glyph in weapons.items():
        icon(f'icons/iconw_{weapon}.tga', glyph, ACCENT)
        icon(f'icons/icona_{weapon}.tga', glyph, STEEL)
    for name, glyph, color in [('iconr_yellow', 'ARM', (240, 200, 60)), ('iconr_red', 'ARM', (240, 80, 60)), ('iconr_shard', 'A', (240, 200, 60)),
                               ('iconh_green', '+', (80, 230, 90)), ('iconh_yellow', '+', (240, 200, 60)), ('iconh_red', '+', (240, 80, 60)),
                               ('iconh_mega', '++', (80, 160, 255)), ('noammo', '0', (240, 60, 40)), ('skull_red', 'X', (240, 60, 40)),
                               ('skull_blue', 'X', (60, 110, 255)), ('medkit', '+', (240, 80, 60)), ('teleporter', 'TP', STEEL),
                               ('quad', 'Q', (80, 120, 255)), ('haste', 'H', (240, 200, 60)), ('regen', 'R', (240, 80, 60)),
                               ('invis', 'I', STEEL), ('envirosuit', 'E', STEEL), ('flight', 'F', STEEL)]:
        icon(f'icons/{name}.tga', glyph, color)


def misc_2d():
    save('gfx/2d/white.tga', Image.new('RGBA', (8, 8), (255, 255, 255, 255)))
    tile = Image.fromarray(np.clip(rng.normal(24, 4, (64, 64, 3)), 0, 255).astype(np.uint8))
    save('gfx/2d/backtile.jpg', tile)
    select = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    ImageDraw.Draw(select).rectangle((1, 1, 62, 62), outline=ACCENT + (255,), width=3)
    save('gfx/2d/select.tga', select)
    bar = Image.new('RGBA', (64, 8), (255, 255, 255, 255))
    save('gfx/2d/colorbar.tga', bar)
    for name, glyph in [('net', 'NET'), ('defer', '...')]:
        image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
        ImageDraw.Draw(image).text((32, 32), glyph, font=ImageFont.truetype(FONT_BOLD, 16), fill=(240, 80, 60, 255), anchor='mm')
        save(f'gfx/2d/{name}.tga', image)


# ---------------------------------------------------------------- награды и значки над игроками
def badge(symbol):
    """медаль: шестиугольный жетон с оранжевой кромкой и символом."""
    size = 256
    image = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    center = size / 2
    hexagon = [(center + 118 * math.cos(math.radians(60 * index + 30)), center + 118 * math.sin(math.radians(60 * index + 30))) for index in range(6)]
    inner = [(center + 100 * math.cos(math.radians(60 * index + 30)), center + 100 * math.sin(math.radians(60 * index + 30))) for index in range(6)]
    draw.polygon(hexagon, fill=ACCENT + (255,))
    draw.polygon(inner, fill=(26, 32, 38, 255))
    light = (240, 232, 210, 255)
    if symbol == 'chevrons':
        for offset in (-28, 28):
            draw.line([(center - 52, center + 30 + offset), (center, center - 22 + offset), (center + 52, center + 30 + offset)], fill=light, width=22)
    elif symbol == 'target':
        draw.ellipse((center - 60, center - 60, center + 60, center + 60), outline=light, width=14)
        draw.ellipse((center - 14, center - 14, center + 14, center + 14), fill=ACCENT + (255,))
        for dx, dy in [(1, 0), (-1, 0), (0, 1), (0, -1)]:
            draw.line([(center + dx * 40, center + dy * 40), (center + dx * 82, center + dy * 82)], fill=light, width=14)
    elif symbol == 'cross':
        draw.line([(center - 55, center - 55), (center + 55, center + 55)], fill=light, width=26)
        draw.line([(center + 55, center - 55), (center - 55, center + 55)], fill=light, width=26)
    elif symbol == 'shield':
        draw.polygon([(center - 55, center - 60), (center + 55, center - 60), (center + 55, center), (center, center + 70), (center - 55, center)], fill=light)
        draw.polygon([(center - 30, center - 38), (center + 30, center - 38), (center + 30, center - 4), (center, center + 36), (center - 30, center - 4)], fill=ACCENT + (255,))
    elif symbol == 'arrows':
        draw.polygon([(center - 70, center - 50), (center - 10, center), (center - 70, center + 50)], fill=light)
        draw.polygon([(center + 70, center - 50), (center + 10, center), (center + 70, center + 50)], fill=light)
    elif symbol == 'flag':
        draw.line([(center - 45, center - 65), (center - 45, center + 70)], fill=light, width=14)
        draw.polygon([(center - 40, center - 65), (center + 65, center - 40), (center - 40, center - 10)], fill=ACCENT + (255,))
    return image.resize((128, 128), Image.LANCZOS)


def rewards_and_sprites():
    for name, symbol in [('excellent', 'chevrons'), ('impressive', 'target'), ('gauntlet', 'cross'),
                         ('defend', 'shield'), ('assist', 'arrows'), ('capture', 'flag'), ('frags', 'chevrons'),
                         ('victory', 'chevrons'), ('accuracy', 'target')]:
        save(f'menu/medals/medal_{name}.tga', badge(symbol))
    # облачко над говорящим игроком
    size = 128
    bubble = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(bubble)
    draw.rounded_rectangle((8, 16, 120, 92), 26, fill=(240, 236, 224, 235), outline=(30, 30, 30, 255), width=5)
    draw.polygon([(40, 88), (64, 88), (34, 118)], fill=(240, 236, 224, 235), outline=(30, 30, 30, 255))
    draw.rectangle((38, 84, 62, 92), fill=(240, 236, 224, 235))
    for x in (40, 64, 88):
        draw.ellipse((x - 8, 46, x + 8, 62), fill=(40, 40, 40, 255))
    save('sprites/balloon3.tga', bubble)
    # метка союзника над головой (командные игры)
    marker = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    ImageDraw.Draw(marker).polygon([(8, 10), (56, 10), (32, 52)], fill=(90, 220, 120, 230), outline=(10, 40, 20, 255))
    save('sprites/foe.tga', marker)
    # фон лагометра
    save('gfx/2d/lag.tga', Image.new('RGBA', (64, 64), (10, 14, 18, 150)))


# ---------------------------------------------------------------- эффекты
def radial(size, inner, outer, color, power=1.6):
    y, x = np.mgrid[0:size, 0:size]
    distance = np.hypot(x - size / 2 + 0.5, y - size / 2 + 0.5) / (size / 2)
    alpha = np.clip((1 - np.clip((distance - inner) / (outer - inner), 0, 1)) ** power, 0, 1)
    image = np.zeros((size, size, 4), dtype=np.uint8)
    image[..., :3] = color
    image[..., 3] = (alpha * 255).astype(np.uint8)
    return Image.fromarray(image, 'RGBA')


def smoke(size, seed, darkness=1.0):
    local = np.random.default_rng(seed)
    y, x = np.mgrid[0:size, 0:size] / size
    field = np.zeros((size, size))
    for octave in range(5):
        frequency = 2 ** octave * 2
        phase = local.random(4) * 10
        field += (np.sin(x * frequency * 6.28 + phase[0]) * np.sin(y * frequency * 6.28 + phase[1]) +
                  np.sin((x + y) * frequency * 4.4 + phase[2])) / (octave + 1)
    distance = np.hypot(x - 0.5, y - 0.5) * 2
    alpha = np.clip(1 - distance + field * 0.18, 0, 1) ** 1.4
    image = np.zeros((size, size, 4), dtype=np.uint8)
    gray = (210 * darkness + field * 18).clip(0, 255)
    image[..., 0] = gray; image[..., 1] = gray; image[..., 2] = gray
    image[..., 3] = (alpha * 255).astype(np.uint8)
    return Image.fromarray(image, 'RGBA')


def explosion_frames(prefix, count, size=128):
    for frame in range(count):
        t = frame / (count - 1)
        local = np.random.default_rng(100 + frame)
        y, x = np.mgrid[0:size, 0:size] / size - 0.5
        distance = np.hypot(x, y) * 2
        angle = np.arctan2(y, x)
        ragged = 0.75 + 0.12 * np.sin(angle * 7 + frame) + 0.08 * np.sin(angle * 13 + frame * 2)
        radius = 0.35 + 0.6 * t
        core = np.clip(1 - distance / (radius * ragged), 0, 1)
        heat = core ** (0.6 + 1.6 * t)
        image = np.zeros((size, size, 4))
        image[..., 0] = 255 * np.clip(heat * 1.6, 0, 1)
        image[..., 1] = 255 * np.clip(heat * 1.1 - 0.1, 0, 1)
        image[..., 2] = 255 * np.clip(heat * 0.6 - 0.25, 0, 1)
        image[..., 3] = 255 * np.clip(core * 1.6 * (1 - t * 0.7), 0, 1)
        image[..., :3] *= (1 - t * 0.6)
        save(f'{prefix}{frame + 1}.tga', premultiply(Image.fromarray(image.clip(0, 255).astype(np.uint8), 'RGBA')))


def wake_ring():
    """кольцо ряби вокруг ног в воде."""
    size = 64
    y, x = np.mgrid[0:size, 0:size] / size - 0.5
    distance = np.hypot(x, y) * 2
    level = np.exp(-((distance - 0.75) / 0.12) ** 2) * 0.55
    image = np.zeros((size, size, 4))
    image[..., :3] = (210, 225, 230)
    image[..., 3] = level * 255
    return Image.fromarray(image.astype(np.uint8), 'RGBA')


def premultiply(image):
    """для аддитивного смешивания (GL_ONE GL_ONE) движок не смотрит на альфу: умножаем цвет на неё."""
    pixels = np.array(image.convert('RGBA')).astype(float)
    pixels[..., :3] *= pixels[..., 3:4] / 255
    pixels[..., 3] = 255
    return Image.fromarray(pixels.astype(np.uint8), 'RGBA')


def mark(image):
    """для следов (GL_ZERO GL_ONE_MINUS_SRC_COLOR): цвет = сила затемнения."""
    pixels = np.array(image.convert('RGBA')).astype(float)
    darkness = (255 - pixels[..., :3]) * pixels[..., 3:4] / 255
    result = np.zeros_like(pixels)
    result[..., :3] = darkness
    result[..., 3] = 255
    return Image.fromarray(result.astype(np.uint8), 'RGBA')


def effects():
    save('gfx/misc/tracer.tga', premultiply(radial(32, 0.0, 1.0, (255, 220, 160), 1.2).resize((64, 16))))
    save('gfx/misc/smokepuff3.tga', smoke(64, 3))
    save('gfx/misc/smokepuff_dark.tga', smoke(64, 5, 0.45))
    save('gfx/damage/bullet_mrk.tga', mark(radial(32, 0.0, 1.0, (30, 28, 26), 1.0)))
    save('gfx/damage/burn_med_mrk.tga', mark(smoke(128, 9, 0.12)))
    save('gfx/damage/hole_lg_mrk.tga', mark(radial(64, 0.2, 1.0, (20, 18, 16), 0.8)))
    save('gfx/damage/plasma_mrk.tga', radial(64, 0.2, 1.0, (40, 60, 90), 0.8))
    save('gfx/marks/shadow.tga', mark(radial(64, 0.1, 1.0, (60, 60, 60), 1.2)))
    save('gfx/misc/spark.tga', premultiply(radial(32, 0.0, 1.0, (255, 200, 120), 2.5)))
    save('gfx/misc/flare.tga', premultiply(radial(64, 0.0, 1.0, (255, 230, 190), 2.0)))
    save('gfx/misc/wake.tga', premultiply(wake_ring()))
    save('gfx/misc/bubble.tga', premultiply(radial(16, 0.5, 1.0, (220, 235, 255), 1.0)))
    explosion_frames('models/weaphits/rlboom/rlboom_', 8)
    explosion_frames('models/weaphits/bullet', 3, 64)
    write('scripts/walker_fx.shader', """// weapon and impact effects (names expected by cgame)
gfx/misc/tracer
{
	cull none
	{
		map gfx/misc/tracer.tga
		blendFunc GL_ONE GL_ONE
	}
}
smokePuff
{
	cull none
	entityMergable
	{
		map gfx/misc/smokepuff3.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen vertex
		rgbGen vertex
	}
}
smokePuffRagePro
{
	cull none
	entityMergable
	{
		map gfx/misc/smokepuff3.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen vertex
		rgbGen vertex
	}
}
shotgunSmokePuff
{
	cull none
	{
		map gfx/misc/smokepuff3.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen entity
		rgbGen entity
	}
}
hasteSmokePuff
{
	cull none
	{
		map gfx/misc/smokepuff_dark.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		alphaGen vertex
		rgbGen vertex
	}
}
bulletExplosion
{
	cull disable
	{
		animmap 15 models/weaphits/bullet1.tga models/weaphits/bullet2.tga models/weaphits/bullet3.tga gfx/2d/black.tga
		rgbGen wave inversesawtooth 0 1 0 15
		blendfunc add
	}
}
rocketExplosion
{
	cull disable
	{
		animmap 8 models/weaphits/rlboom/rlboom_1.tga models/weaphits/rlboom/rlboom_2.tga models/weaphits/rlboom/rlboom_3.tga models/weaphits/rlboom/rlboom_4.tga models/weaphits/rlboom/rlboom_5.tga models/weaphits/rlboom/rlboom_6.tga models/weaphits/rlboom/rlboom_7.tga models/weaphits/rlboom/rlboom_8.tga
		rgbGen wave inversesawtooth 0 1 0 8
		blendfunc add
	}
}
grenadeExplosion
{
	cull disable
	{
		animmap 8 models/weaphits/rlboom/rlboom_1.tga models/weaphits/rlboom/rlboom_2.tga models/weaphits/rlboom/rlboom_3.tga models/weaphits/rlboom/rlboom_4.tga models/weaphits/rlboom/rlboom_5.tga models/weaphits/rlboom/rlboom_6.tga models/weaphits/rlboom/rlboom_7.tga models/weaphits/rlboom/rlboom_8.tga
		rgbGen wave inversesawtooth 0 1 0 8
		blendfunc add
	}
}
// robots do not bleed: "blood" is sparks and oil
bloodExplosion
{
	cull disable
	{
		animmap 15 models/weaphits/bullet1.tga models/weaphits/bullet2.tga models/weaphits/bullet3.tga gfx/2d/black.tga
		rgbGen wave inversesawtooth 0 1 0 15
		blendfunc add
	}
}
bloodTrail
{
	nopicmip
	entityMergable
	{
		map gfx/misc/smokepuff_dark.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}
bloodMark
{
	nopicmip
	polygonOffset
	{
		map gfx/damage/burn_med_mrk.tga
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		rgbGen identityLighting
		alphaGen vertex
	}
}
viewBloodBlend
{
	sort nearest
	{
		map gfx/2d/white.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 1 0.35 0.1 )
		alphaGen vertex
	}
}
gfx/damage/bullet_mrk
{
	polygonOffset
	{
		map gfx/damage/bullet_mrk.tga
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		rgbGen exactVertex
	}
}
gfx/damage/burn_med_mrk
{
	polygonOffset
	{
		map gfx/damage/burn_med_mrk.tga
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		rgbGen exactVertex
	}
}
gfx/damage/hole_lg_mrk
{
	polygonOffset
	{
		map gfx/damage/hole_lg_mrk.tga
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		rgbGen exactVertex
	}
}
gfx/damage/plasma_mrk
{
	polygonOffset
	{
		map gfx/damage/plasma_mrk.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}
markShadow
{
	polygonOffset
	{
		map gfx/marks/shadow.tga
		blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR
		rgbGen exactVertex
	}
}
wake
{
	{
		clampmap gfx/misc/wake.tga
		blendfunc GL_ONE GL_ONE
		rgbgen wave sin 0.7 0.3 0.25 0.5
		tcmod rotate 250
		tcMod stretch sin .9 0.1 0 0.7
	}
}
waterBubble
{
	sort underwater
	cull none
	entityMergable
	{
		map gfx/misc/bubble.tga
		blendFunc GL_ONE GL_ONE
		rgbGen vertex
		alphaGen vertex
	}
}
sprites/plasma1
{
	cull disable
	{
		clampmap gfx/misc/flare.tga
		blendfunc GL_ONE GL_ONE
	}
}
""")
    save('gfx/2d/black.tga', Image.new('RGBA', (8, 8), (0, 0, 0, 255)))


def main():
    write('default.cfg', DEFAULT_CFG)
    console_font()
    prop_fonts()
    menu_art()
    hangar_art()
    crosshairs()
    numbers()
    icons()
    misc_2d()
    effects()
    rewards_and_sprites()
    count = sum(len(files) for _, _, files in os.walk(OUT))
    print('base data files:', count)


main()
