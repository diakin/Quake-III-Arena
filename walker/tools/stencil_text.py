"""Трафаретные надписи (WALKER, 28.11.2026).

Шрифт - жирный узкий DejaVu Sans Condensed Bold, заглавные буквы, и у каждого знака
перемычки, как у армейского трафарета (образец - надпись «ИНВ №» от пользователя):

  - у знака с вертикальными стойками во всю высоту (И, Н, В, N) - зазор между стойкой и тем,
    что к ней примыкает изнутри (диагональ, перекладина, полуокружности);
  - у знака с замкнутым просветом (0, 8, 4, О, Р, кружок у №) - вертикальный зазор по
    середине, иначе середина трафарета выпала бы;
  - № - отдельно N и кружок с чертой под ним.

stencil_image(lines, height) -> картинка RGBA: строки по центру, буквы цвета color.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'
STEM_COVERAGE = 0.97        # столбец - прямая стойка, если закрашен почти на всю высоту знака
GAP_SHARE = 0.06            # ширина зазора - доля высоты знака
BOLDER = 0.022              # утолщение штриха - доля размера шрифта
CONDENSE = 0.82             # сжатие по ширине: буквы узкие и высокие


def glyph_mask(char, font):
    left, top, right, bottom = font.getbbox(char)
    image = Image.new('L', (right - left + 4, bottom - top + 4), 0)
    ImageDraw.Draw(image).text((2 - left, 2 - top), char, font=font, fill=255)
    mask = np.asarray(image) > 127
    # штрихи жирнее, как у трафарета с образца
    thicken = max(1, int(font.size * BOLDER))
    mask = np.pad(mask, thicken)
    return ndimage.binary_dilation(mask, iterations=thicken)


def hole_labels(mask):
    """замкнутые просветы знака: (метки, номера просветов)"""
    background, count = ndimage.label(~mask)
    holes = []
    for label in range(1, count + 1):
        ys, xs = np.nonzero(background == label)
        if ys.min() > 0 and xs.min() > 0 and ys.max() < mask.shape[0] - 1 and xs.max() < mask.shape[1] - 1:
            holes.append(label)
    return background, holes


def cut_bridges(mask):
    mask = mask.copy()
    rows = np.nonzero(mask.any(axis=1))[0]
    if len(rows) == 0:
        return mask
    top, bottom = rows[0], rows[-1]
    height = bottom - top + 1
    gap = max(2, int(round(height * GAP_SHARE)))
    # прямые стойки во всю высоту (у округлых 0, 8, О края не прямые - стойками не считаются)
    coverage = mask[top:bottom + 1].sum(axis=0) / height
    stems = coverage >= STEM_COVERAGE
    labels, count = ndimage.label(stems)
    columns = mask.shape[1]
    for stem in range(1, count + 1):
        stem_columns = np.nonzero(labels == stem)[0]
        first, last = stem_columns[0], stem_columns[-1]
        if last - first + 1 < gap:
            continue
        if last + 1 < columns and mask[top:bottom + 1, last + 1:].any():
            mask[:, last + 1:last + 1 + gap] = False
        if first > 0 and mask[top:bottom + 1, :first].any():
            mask[:, max(0, first - gap):first] = False
    # оставшиеся замкнутые просветы - вертикальный зазор по середине каждого
    background, holes = hole_labels(mask)
    cuts = set()
    for label in holes:
        centre = int(round(np.nonzero(background == label)[1].mean()))
        if any(abs(centre - other) < gap for other in cuts):
            continue
        cuts.add(centre)
        mask[:, centre - gap // 2:centre - gap // 2 + gap] = False
    return mask


def numero(font):
    """№ трафаретом: N и кружок с чертой (у кружка свой зазор)"""
    letter = cut_bridges(glyph_mask('N', font))
    height = letter.shape[0]
    small = ImageFont.truetype(FONT, int(font.size * 0.62))
    ring = cut_bridges(glyph_mask('o', small))
    width = ring.shape[1]
    column = np.zeros((height, width + 6), bool)
    column[4:4 + ring.shape[0], 3:3 + width] = ring
    bar_top = 4 + ring.shape[0] + int(height * 0.06)
    column[bar_top:bar_top + max(3, int(height * 0.1)), 3:3 + width] = True
    spacer = np.zeros((height, int(height * 0.05)), bool)
    return np.hstack([letter, spacer, column])


SMALL_MARKS = {'.': 'bottom', ',': 'bottom', '-': 'middle'}    # без перемычек, по своему месту


def line_mask(text, font):
    reference = glyph_mask('Н', font).shape[0]
    pieces = []
    for char in text.upper():
        if char == ' ':
            pieces.append(np.zeros((reference, int(reference * 0.3)), bool))
            continue
        if char in SMALL_MARKS:
            glyph = glyph_mask(char, font)
            rows = np.nonzero(glyph.any(axis=1))[0]; cols = np.nonzero(glyph.any(axis=0))[0]
            glyph = glyph[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]
            canvas = np.zeros((reference, glyph.shape[1] + 4), bool)
            thicken = max(1, int(font.size * BOLDER))
            offset = reference - glyph.shape[0] - thicken - 2 if SMALL_MARKS[char] == 'bottom' else (reference - glyph.shape[0]) // 2
            canvas[offset:offset + glyph.shape[0], 2:2 + glyph.shape[1]] = glyph
        else:
            glyph = numero(font) if char == '№' else cut_bridges(glyph_mask(char, font))
            canvas = np.zeros((reference, glyph.shape[1]), bool)
            height = min(reference, glyph.shape[0])
            canvas[(reference - height) // 2:(reference - height) // 2 + height] = glyph[:height]
        pieces.append(canvas)
        pieces.append(np.zeros((reference, max(2, int(reference * 0.06))), bool))
    return np.hstack(pieces[:-1]) if pieces else np.zeros((reference, 1), bool)


def stencil_image(lines, size, color=(226, 222, 206), alpha=240, font_size=130, line_gap=0.25):
    """строки трафаретом по центру картинки size (ширина, высота)"""
    font = ImageFont.truetype(FONT, font_size)
    masks = [line_mask(text, font) for text in lines]
    width = max(mask.shape[1] for mask in masks)
    line_height = masks[0].shape[0]
    total = line_height * len(masks) + int(line_height * line_gap) * (len(masks) - 1)
    block = np.zeros((total, width), bool)
    y_value = 0
    for mask in masks:
        x_value = (width - mask.shape[1]) // 2
        block[y_value:y_value + mask.shape[0], x_value:x_value + mask.shape[1]] = mask
        y_value += line_height + int(line_height * line_gap)
    rgba = np.zeros(block.shape + (4,), np.uint8)
    rgba[block] = color + (alpha,)
    image = Image.fromarray(rgba, 'RGBA')
    margin = 0.08
    scale = min(size[0] * (1 - 2 * margin) / (width * CONDENSE), size[1] * (1 - 2 * margin) / total)
    image = image.resize((max(1, int(width * CONDENSE * scale)), max(1, int(total * scale))), Image.LANCZOS)
    canvas = Image.new('RGBA', size, (0, 0, 0, 0))
    canvas.paste(image, ((size[0] - image.size[0]) // 2, (size[1] - image.size[1]) // 2))
    return canvas


if __name__ == '__main__':
    import sys
    picture = stencil_image(sys.argv[2:] or ['ИНВ.№', '50-458'], (900, 520), color=(20, 20, 20), alpha=255)
    background = Image.new('RGBA', picture.size, (200, 240, 60, 255))
    background.alpha_composite(picture)
    background.convert('RGB').save(sys.argv[1] if len(sys.argv) > 1 else 'stencil.png')
