"""Draws the typographic textures of a bus stop, where exact text matters more than a photo look: the white stop panel
on the mast (Zeichen 224, stop name, line strip), a timetable poster, and the joke stickers of the street bin.

    python -I draw_signs.py [output_dir]   (default <data root>/building_kit/posters)

Sizes match the faces in kit/shelters.py at 2 pixels per millimetre (stop panel, stickers) or about 1.4 (timetable).
Stop names and line numbers are real Bergedorf ones; there are no operator logos.
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_BOLD = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"
FONT_REGULAR = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
SIGN_GREEN = (0, 132, 82)
SIGN_YELLOW = (250, 203, 0)
HVV_RED = (226, 0, 26)
INK = (20, 20, 20)

STOP_NAME = "Lohbrügger Markt"
LINES = ["12", "135", "227", "8800"]
STICKER_JOKES = [
    "Wirf mir was\nNettes zu.",
    "Hunger!\nBitte füttern.",
    "Bin dann\nmal voll.",
    "Kippen? Gern.\nAber hier rein.",
    "Ich nehm's dir\nnicht übel.",
]


def font(size, bold=True):
    """The Liberation Sans face at a pixel size."""
    return ImageFont.truetype(FONT_BOLD if bold else FONT_REGULAR, size)


def centred_text(draw, centre_x, top, text, text_font, fill):
    """Draws text horizontally centred on centre_x with its top at top; returns the bottom."""
    left, upper, right, lower = draw.multiline_textbbox((0, 0), text, font=text_font, align="center")
    draw.multiline_text((centre_x - (right - left) / 2 - left, top - upper), text, font=text_font, fill=fill,
                        align="center")
    return top + (lower - upper)


def draw_zeichen_224(draw, centre, radius):
    """Zeichen 224 (Haltestelle): a green H on a yellow disc with a green ring."""
    x, y = centre
    draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=SIGN_GREEN)
    inner = radius * 0.86
    draw.ellipse((x - inner, y - inner, x + inner, y + inner), fill=SIGN_YELLOW)
    stroke = radius * 0.2
    half_width, half_height = radius * 0.42, radius * 0.56
    draw.rectangle((x - half_width, y - half_height, x - half_width + stroke, y + half_height), fill=SIGN_GREEN)
    draw.rectangle((x + half_width - stroke, y - half_height, x + half_width, y + half_height), fill=SIGN_GREEN)
    draw.rectangle((x - half_width, y - stroke / 2, x + half_width, y + stroke / 2), fill=SIGN_GREEN)


def draw_stop_panel(path):
    """The white panel beside the mast top (440 by 780 mm): the H sign, the stop name, a bus pictogram line and the
    red strip with the line numbers."""
    width, height = 880, 1560
    image = Image.new("RGB", (width, height), (250, 250, 248))
    draw = ImageDraw.Draw(image)
    draw.rectangle((6, 6, width - 7, height - 7), outline=(200, 200, 200), width=4)
    draw_zeichen_224(draw, (width / 2, 360), 300)
    draw.line((60, 720, width - 60, 720), fill=INK, width=4)
    bottom = centred_text(draw, width / 2, 770, STOP_NAME.replace(" ", "\n"), font(118), INK)
    draw.line((60, bottom + 50, width - 60, bottom + 50), fill=INK, width=4)
    centred_text(draw, width / 2, bottom + 85, "Bus", font(90), HVV_RED)
    strip_top = height - 300
    draw.rectangle((0, strip_top, width, height), fill=HVV_RED)
    centred_text(draw, width / 2, strip_top + 60, "  ".join(LINES[:3]), font(120), (255, 255, 255))
    centred_text(draw, width / 2, strip_top + 200, "Nachtbus " + LINES[3], font(56, bold=False), (255, 255, 255))
    image.save(path)


def departure_minutes(rng, hour, day):
    """Plausible departure minutes for one hour: every 10 minutes in the weekday rush, 20 otherwise, 30 at night."""
    if hour < 6 or hour >= 21:
        interval = 30
    elif day == "Mo-Fr" and (6 <= hour < 9 or 15 <= hour < 19):
        interval = 10
    else:
        interval = 20
    offset = rng.randrange(0, interval)
    return list(range(offset, 60, interval))


def draw_timetable(path):
    """An A-format departure timetable (800 by 1130 mm): header with line and direction, a grid of hours with the
    minutes for weekdays, Saturday and Sunday, and a footer."""
    width, height = 1132, 1600
    rng = random.Random(12)
    image = Image.new("RGB", (width, height), (252, 252, 250))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, width, 210), fill=HVV_RED)
    draw.text((50, 30), "12", font=font(130), fill=(255, 255, 255))
    draw.text((260, 40), "Richtung Bergedorf, ZOB", font=font(58), fill=(255, 255, 255))
    draw.text((260, 120), "Haltestelle " + STOP_NAME, font=font(44, bold=False), fill=(255, 255, 255))
    columns = [("Uhr", 50), ("Montag – Freitag", 160), ("Samstag", 560), ("Sonntag", 830)]
    header_top = 240
    for title, x in columns:
        draw.text((x, header_top), title, font=font(34), fill=INK)
    draw.line((40, header_top + 55, width - 40, header_top + 55), fill=INK, width=3)
    row_top = header_top + 70
    row_height = (height - 160 - row_top) / 19
    for index, hour in enumerate(range(5, 24)):
        y = row_top + index * row_height
        if index % 2 == 0:
            draw.rectangle((40, y - 4, width - 40, y + row_height - 8), fill=(238, 238, 234))
        draw.text((60, y), "%02d" % hour, font=font(36), fill=INK)
        for (title, x), day in zip(columns[1:], ("Mo-Fr", "Sa", "So")):
            minutes = departure_minutes(rng, hour, day)
            draw.text((x, y + 2), "  ".join("%02d" % minute for minute in minutes), font=font(32, bold=False),
                      fill=INK)
    draw.line((40, height - 140, width - 40, height - 140), fill=INK, width=3)
    draw.text((50, height - 120), "Gültig ab 14. Dezember. Fahrten an Feiertagen wie sonntags.", font=font(30, bold=False),
              fill=INK)
    draw.text((50, height - 75), "Fahrplanauskunft an der Haltestelle und im Internet.", font=font(30, bold=False),
              fill=INK)
    image.save(path)


def draw_sticker(path, joke):
    """A bin sticker (380 by 160 mm): a white speech bubble with the joke in heavy black type."""
    width, height = 760, 320
    image = Image.new("RGB", (width, height), (226, 0, 26))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((12, 40, width - 12, height - 12), radius=26, fill=(255, 255, 255))
    draw.polygon([(width * 0.62, 44), (width * 0.66, 0), (width * 0.72, 44)], fill=(255, 255, 255))
    joke_font = font(100)
    left, upper, right, lower = draw.multiline_textbbox((0, 0), joke, font=joke_font, align="center")
    bubble_middle = (40 + height - 12) / 2
    centred_text(draw, width / 2, bubble_middle - (lower - upper) / 2, joke, joke_font, INK)
    image.save(path)


def main():
    output_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear"), "building_kit", "posters")
    os.makedirs(output_dir, exist_ok=True)
    draw_stop_panel(os.path.join(output_dir, "stop_panel.png"))
    draw_timetable(os.path.join(output_dir, "timetable.png"))
    for index, joke in enumerate(STICKER_JOKES):
        draw_sticker(os.path.join(output_dir, "bin_sticker_%d.png" % index), joke)
    print("signs written to", output_dir)


if __name__ == "__main__":
    main()
