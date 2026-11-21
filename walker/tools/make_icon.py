"""Иконка Walker.exe: шагающий робот на тёмной плитке.

python3 walker/tools/make_icon.py code/win32/walker.ico [превью.png]

Крупные размеры (256, 48, 32) рисуются с деталями, мелкие (24, 16) — упрощённым силуэтом,
чтобы робот читался и в заголовке окна.
"""
import io, struct, sys
from PIL import Image, ImageDraw

OUT = sys.argv[1]
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None

CANVAS = 1024
OLIVE = (122, 132, 66)
OLIVE_DARK = (84, 92, 44)
OLIVE_LIGHT = (156, 166, 92)
GLASS = (78, 186, 206)
GLASS_LIGHT = (190, 236, 244)
STEEL = (112, 120, 126)
STEEL_DARK = (58, 62, 66)
ORANGE = (236, 146, 38)
OUTLINE = (16, 18, 20)


def tile(draw, size):
    """тёмная плитка со скруглёнными углами и светлой кромкой."""
    radius = size * 0.18
    for step in range(size):
        level = step / size
        color = tuple(int(46 * (1 - level) + 18 * level + extra) for extra in (0, 8, 14))
        draw.line([(0, step), (size, step)], fill=color + (255,))
    mask = Image.new('L', (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=255)
    return mask


def robot_detailed(draw):
    width = 18
    # ноги: бедро наружу, колено назад (обратный сустав), голень к ступне
    for side in (-1, 1):
        hip = (512 + side * 100, 600)
        knee = (512 + side * 190, 730)
        ankle = (512 + side * 120, 868)
        draw.line([hip, knee], fill=OUTLINE, width=92)
        draw.line([knee, ankle], fill=OUTLINE, width=80)
        draw.line([hip, knee], fill=STEEL, width=60)
        draw.line([knee, ankle], fill=STEEL, width=50)
        draw.line([(hip[0] + side * 26, hip[1] + 18), (knee[0] - side * 4, knee[1] - 36)], fill=ORANGE, width=16)
        draw.ellipse((knee[0] - 44, knee[1] - 44, knee[0] + 44, knee[1] + 44), fill=STEEL_DARK, outline=OUTLINE, width=width)
        foot = [(ankle[0] - 92, 912), (ankle[0] + 92, 912), (ankle[0] + 62, 860), (ankle[0] - 62, 860)]
        draw.polygon(foot, fill=STEEL_DARK, outline=OUTLINE)
        draw.line(foot + [foot[0]], fill=OUTLINE, width=width)
    # орудийные модули по бокам
    for side in (-1, 1):
        x0, x1 = (170, 316) if side < 0 else (708, 854)
        draw.rounded_rectangle((x0, 300, x1, 452), 22, fill=STEEL_DARK, outline=OUTLINE, width=width)
        center = ((x0 + x1) // 2, 376)
        draw.ellipse((center[0] - 46, center[1] - 46, center[0] + 46, center[1] + 46), fill=OUTLINE)
        draw.ellipse((center[0] - 34, center[1] - 34, center[0] + 34, center[1] + 34), outline=ORANGE, width=12)
        draw.rectangle((x1 - 6 if side < 0 else x0 - 30, 352, x1 + 30 if side < 0 else x0 + 6, 400), fill=STEEL, outline=OUTLINE, width=10)
    # корпус
    body = [(330, 250), (694, 250), (750, 320), (730, 600), (294, 600), (274, 320)]
    draw.polygon(body, fill=OLIVE)
    draw.polygon([(694, 250), (750, 320), (730, 600), (650, 600), (660, 300)], fill=OLIVE_DARK)
    draw.polygon([(330, 250), (694, 250), (660, 300), (360, 300)], fill=OLIVE_LIGHT)
    draw.line(body + [body[0]], fill=OUTLINE, width=width)
    # фонарь кабины
    draw.rounded_rectangle((404, 300, 620, 404), 34, fill=GLASS, outline=OUTLINE, width=width)
    draw.polygon([(430, 322), (500, 322), (466, 386), (424, 386)], fill=GLASS_LIGHT)
    # решётки радиаторов
    for x0 in (352, 532):
        draw.rectangle((x0, 444, x0 + 140, 560), fill=OUTLINE)
        for row in range(4):
            y = 456 + row * 26
            draw.rectangle((x0 + 12, y, x0 + 128, y + 14), fill=ORANGE)


def robot_simple(draw):
    """упрощённый силуэт для 16 и 24 пикселей."""
    for side in (-1, 1):
        draw.line([(512 + side * 110, 600), (512 + side * 210, 740), (512 + side * 140, 900)], fill=STEEL, width=120, joint='curve')
        draw.rectangle((512 + side * 140 - 110, 860, 512 + side * 140 + 110, 940), fill=STEEL)
    draw.rectangle((150, 300, 874, 440), fill=STEEL_DARK)
    draw.polygon([(310, 200), (714, 200), (790, 290), (760, 640), (264, 640), (234, 290)], fill=OLIVE)
    draw.rectangle((380, 270, 644, 420), fill=GLASS)
    draw.rectangle((340, 480, 684, 580), fill=ORANGE)


def render(size, detailed):
    image = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    background = Image.new('RGBA', (CANVAS, CANVAS))
    mask = tile(ImageDraw.Draw(background), CANVAS)
    image.paste(background, (0, 0), mask)
    edge = ImageDraw.Draw(image)
    edge.rounded_rectangle((8, 8, CANVAS - 9, CANVAS - 9), CANVAS * 0.17, outline=(96, 110, 120, 255), width=16)
    (robot_detailed if detailed else robot_simple)(ImageDraw.Draw(image))
    return image.resize((size, size), Image.LANCZOS)


def bmp_entry(image):
    """32-битный BMP внутри ICO: Windows XP понимает только такой формат (PNG — с Vista)."""
    size = image.width
    pixels = image.convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM).tobytes('raw', 'BGRA')
    mask_row = ((size + 31) // 32) * 4
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(pixels) + mask_row * size, 0, 0, 0, 0)
    return header + pixels + bytes(mask_row * size)


def png_entry(image):
    buffer = io.BytesIO()
    image.save(buffer, format='PNG')
    return buffer.getvalue()


def write_ico(path, images):
    """256 — PNG (так принято с Vista), остальные — BMP для Windows XP."""
    sizes = sorted(images, reverse=True)
    blobs = [png_entry(images[size]) if size >= 256 else bmp_entry(images[size]) for size in sizes]
    offset = 6 + 16 * len(sizes)
    data = struct.pack('<HHH', 0, 1, len(sizes))
    for size, blob in zip(sizes, blobs):
        data += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, 'wb') as file:
        file.write(data + b''.join(blobs))


def main():
    images = {size: render(size, size >= 32) for size in (256, 48, 32, 24, 16)}
    write_ico(OUT, images)
    if PREVIEW:
        sheet = Image.new('RGBA', (256 + 48 + 32 + 24 + 16 + 60, 256), (240, 240, 240, 255))
        position = 0
        for size in (256, 48, 32, 24, 16):
            sheet.alpha_composite(images[size], (position, 0))
            position += size + 12
        sheet.save(PREVIEW)
    print('icon:', OUT)


main()
