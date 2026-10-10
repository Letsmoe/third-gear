"""Draws the printed faces of the three made-up petrol station brands: the logo panel on the canopy fascia and the shop,
the price pylon's face, the pump face and the car wash sign.

    python -I draw_brands.py [output_dir]   (default <data root>/building_kit/posters)

The brands copy the look of real chains without their marks: NORDTANK is the blue premium chain, hopp! the red and
yellow discounter, Vierländer Öl the green regional independent. Sizes match the faces in kit/petrol.py.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS = "/usr/share/fonts/liberation/"
WHITE = (255, 255, 255)
INK = (20, 20, 20)
LED_PANEL = (18, 18, 20)


class Brand:
    """Colours and wordmark of one brand."""

    def __init__(self, key, name, background, wordmark_colour, accent, font_file, emblem, led):
        self.key = key
        self.name = name
        self.background = background
        self.wordmark_colour = wordmark_colour
        self.accent = accent
        self.font_file = font_file
        self.emblem = emblem
        self.led = led


BRANDS = [
    Brand("nordtank", "NORDTANK", (0, 72, 153), WHITE, (120, 190, 240), "LiberationSans-Bold.ttf", "star",
          (255, 255, 255)),
    Brand("hopp", "hopp!", (218, 18, 26), (255, 214, 0), (255, 214, 0), "LiberationSans-BoldItalic.ttf", None,
          (255, 60, 40)),
    Brand("vierlaender", "Vierländer Öl", (0, 100, 56), WHITE, (250, 200, 30), "LiberationSerif-Bold.ttf", "sun",
          (120, 255, 120)),
]
# Prices per litre in euros; the third decimal is the small superscript nine of every German price board.
GRADES = [("Super E10", "1,73"), ("Super E5", "1,79"), ("Diesel", "1,65")]
PRICE_OFFSETS = {"nordtank": 0.04, "hopp": -0.02, "vierlaender": 0.0}


def font(file_name, size):
    """A Liberation face at a pixel size."""
    return ImageFont.truetype(FONTS + file_name, size)


def text_size(draw, text, text_font):
    """Width and height of a single line of text."""
    left, top, right, bottom = draw.textbbox((0, 0), text, font=text_font)
    return right - left, bottom - top, left, top


def draw_centred(draw, centre, text, text_font, fill):
    """Draws one line of text centred on a point."""
    width, height, left, top = text_size(draw, text, text_font)
    draw.text((centre[0] - width / 2 - left, centre[1] - height / 2 - top), text, font=text_font, fill=fill)


def draw_emblem(draw, brand, centre, radius):
    """The brand's small emblem: a four-pointed north star or a rising sun; nothing for the discounter."""
    x, y = centre
    if brand.emblem == "star":
        inner = radius * 0.28
        points = []
        for index in range(8):
            reach = radius if index % 2 == 0 else inner
            direction = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)][index]
            scale = reach if index % 2 == 0 else inner * 0.7071 * 2 ** 0.5
            length = (direction[0] ** 2 + direction[1] ** 2) ** 0.5
            points.append((x + direction[0] / length * scale, y + direction[1] / length * scale))
        draw.polygon(points, fill=brand.wordmark_colour)
    elif brand.emblem == "sun":
        draw.pieslice((x - radius, y - radius, x + radius, y + radius), 180, 360, fill=brand.accent)
        draw.rectangle((x - radius * 1.15, y + radius * 0.08, x + radius * 1.15, y + radius * 0.22), fill=brand.accent)
        draw.rectangle((x - radius * 1.15, y + radius * 0.36, x + radius * 1.15, y + radius * 0.48), fill=brand.accent)


def wordmark_width(draw, brand, size):
    """Width of emblem and name together at a font size."""
    width, _height, _left, _top = text_size(draw, brand.name, font(brand.font_file, size))
    if brand.emblem is None:
        return width
    return width + size * 1.1


def draw_wordmark(draw, brand, box, size):
    """Emblem and brand name centred in a box (left, top, right, bottom), shrunk until it fits 88 % of the width."""
    left, top, right, bottom = box
    while size > 10 and wordmark_width(draw, brand, size) > 0.88 * (right - left):
        size -= 4
    wordmark_font = font(brand.font_file, size)
    width, height, _left, _top = text_size(draw, brand.name, wordmark_font)
    emblem_width = 0 if brand.emblem is None else size * 1.1
    start = (left + right) / 2 - (width + emblem_width) / 2
    middle = (top + bottom) / 2
    if brand.emblem is not None:
        draw_emblem(draw, brand, (start + size * 0.4, middle), size * 0.42)
    draw_centred(draw, (start + emblem_width + width / 2, middle), brand.name, wordmark_font, brand.wordmark_colour)


def draw_logo_panel(brand, path):
    """The 3.2 by 0.8 m logo panel on the fascia: the wordmark on the brand colour."""
    image = Image.new("RGB", (1600, 400), brand.background)
    draw = ImageDraw.Draw(image)
    draw_wordmark(draw, brand, (0, 0, 1600, 400), 230 if len(brand.name) < 10 else 170)
    image.save(path)


def draw_price(draw, position, price, led, size):
    """An LED price like 1,73 with the superscript 9."""
    x, y = position
    price_font = font("LiberationSans-Bold.ttf", size)
    draw.text((x, y), price, font=price_font, fill=led)
    width, _height, _left, _top = text_size(draw, price, price_font)
    draw.text((x + width + size * 0.05, y - size * 0.02), "9", font=font("LiberationSans-Bold.ttf", size // 2),
              fill=led)


def offset_price(price, offset):
    """A price string shifted by a few cents."""
    value = float(price.replace(",", ".")) + offset
    return ("%.2f" % value).replace(".", ",")


def draw_price_board(brand, path):
    """The price pylon's face, 1.5 by 5.6 m: the wordmark at the top, three grades with LED prices, and the services."""
    width, height = 600, 2240
    image = Image.new("RGB", (width, height), brand.background)
    draw = ImageDraw.Draw(image)
    draw_wordmark(draw, brand, (20, 60, width - 20, 400), 120 if len(brand.name) < 10 else 76)
    row_top = 470
    for grade, price in GRADES:
        draw.rectangle((30, row_top, width - 30, row_top + 380), fill=LED_PANEL)
        draw.rectangle((30, row_top, width - 30, row_top + 90), fill=WHITE)
        draw_centred(draw, (width / 2, row_top + 45), grade, font("LiberationSans-Bold.ttf", 62), INK)
        draw_price(draw, (60, row_top + 130), offset_price(price, PRICE_OFFSETS[brand.key]), brand.led, 200)
        row_top += 430
    services = ["Shop", "Waschanlage", "24 h"]
    service_font = font("LiberationSans-Bold.ttf", 58)
    for index, service in enumerate(services):
        top = row_top + 20 + index * 110
        draw.rounded_rectangle((60, top, width - 60, top + 90), radius=20, fill=WHITE)
        draw_centred(draw, (width / 2, top + 45), service, service_font, brand.background)
    image.save(path)


def draw_pump_face(brand, path):
    """The pump face, 0.9 by 0.85 m: brand strip, the amount and litre display, and a button per grade."""
    width, height = 720, 680
    image = Image.new("RGB", (width, height), (232, 234, 236))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, width, 110), fill=brand.background)
    draw_wordmark(draw, brand, (0, 0, width, 110), 70 if len(brand.name) < 10 else 52)
    draw.rounded_rectangle((60, 140, width - 60, 400), radius=12, fill=(40, 52, 44))
    label_font = font("LiberationSans-Regular.ttf", 30)
    digit_font = font("LiberationMono-Bold.ttf", 76)
    for index, (label, value) in enumerate([("Betrag EUR", "42,17"), ("Liter", "24,38")]):
        top = 160 + index * 120
        draw.text((85, top + 30), label, font=label_font, fill=(180, 210, 180))
        draw.text((330, top), value.rjust(6), font=digit_font, fill=(210, 240, 200))
    button_width = (width - 120 - 40) / 3
    for index, (grade, price) in enumerate(GRADES):
        left = 60 + index * (button_width + 20)
        draw.rounded_rectangle((left, 430, left + button_width, 600), radius=14, fill=brand.background)
        draw_centred(draw, (left + button_width / 2, 480), grade, font("LiberationSans-Bold.ttf", 32),
                     brand.wordmark_colour)
        draw_centred(draw, (left + button_width / 2, 550), offset_price(price, PRICE_OFFSETS[brand.key]) + "9",
                     font("LiberationSans-Bold.ttf", 40), brand.wordmark_colour)
    draw.rectangle((width / 2 - 70, 620, width / 2 + 70, 640), fill=(60, 60, 60))
    image.save(path)


def draw_wash_sign(brand, path):
    """The car wash sign, 4 by 1 m: Waschanlage on the brand colour with the brand's name small."""
    image = Image.new("RGB", (1600, 400), brand.background)
    draw = ImageDraw.Draw(image)
    draw_centred(draw, (800, 170), "Waschanlage", font("LiberationSans-Bold.ttf", 190), brand.wordmark_colour)
    draw_centred(draw, (800, 330), "Einfahrt  ·  " + brand.name, font("LiberationSans-Regular.ttf", 64),
                 brand.wordmark_colour)
    image.save(path)


def main():
    output_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear"), "building_kit", "posters")
    os.makedirs(output_dir, exist_ok=True)
    for brand in BRANDS:
        draw_logo_panel(brand, os.path.join(output_dir, "fuel_logo_%s.png" % brand.key))
        draw_price_board(brand, os.path.join(output_dir, "fuel_prices_%s.png" % brand.key))
        draw_pump_face(brand, os.path.join(output_dir, "fuel_pump_%s.png" % brand.key))
        draw_wash_sign(brand, os.path.join(output_dir, "fuel_wash_%s.png" % brand.key))
    print("brands written to", output_dir)


if __name__ == "__main__":
    main()
