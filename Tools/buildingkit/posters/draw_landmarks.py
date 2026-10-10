"""Draws the facade element texture of the Elbphilharmonie: one 5 by 3.5 m glass element with its curved, clearer
middle (the bubble the bent glass makes), the grey dot print around it, and the dark frame at its edges.

    python -I draw_landmarks.py [output_dir]   (default <data root>/building_kit/posters)

The facade's UVs count elements, so this tiles once per element. It is a preview for the contact sheet and a guide
for the real material, which wants reflections rather than painted sky.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT = 500, 350  # 1 px per centimetre
DOT_SPACING = 7
FRAME = 3


def vertical_gradient(top, bottom):
    """An image fading from one colour at the top to another at the bottom."""
    image = Image.new("RGB", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(image)
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        colour = tuple(round(top[k] + (bottom[k] - top[k]) * t) for k in range(3))
        draw.line((0, y, WIDTH, y), fill=colour)
    return image


def bubble_mask():
    """Soft mask of the curved middle of the element: a rounded oval, blurred at its edge."""
    mask = Image.new("L", (WIDTH, HEIGHT), 0)
    ImageDraw.Draw(mask).rounded_rectangle((60, 95, 440, 250), radius=75, fill=200)
    return mask.filter(ImageFilter.GaussianBlur(30))


def draw_glass(path):
    """The element: sky-tinted flat glass with the dot print, the darker clear bubble, and the frame."""
    flat = vertical_gradient((228, 237, 243), (198, 214, 226))
    dots = ImageDraw.Draw(flat)
    for y in range(DOT_SPACING // 2, HEIGHT, DOT_SPACING):
        offset = (y // DOT_SPACING % 2) * DOT_SPACING // 2
        for x in range(offset, WIDTH, DOT_SPACING):
            dots.ellipse((x - 1.3, y - 1.3, x + 1.3, y + 1.3), fill=(172, 180, 188))
    bubble = vertical_gradient((160, 188, 208), (120, 150, 175))
    highlight = ImageDraw.Draw(bubble)
    highlight.ellipse((120, 105, 300, 140), fill=(205, 222, 234))
    bubble = bubble.filter(ImageFilter.GaussianBlur(6))
    element = Image.composite(bubble, flat, bubble_mask())
    ImageDraw.Draw(element).rectangle((0, 0, WIDTH - 1, HEIGHT - 1), outline=(60, 70, 80), width=FRAME)
    element.save(path)


def main():
    output_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear"), "building_kit", "posters")
    os.makedirs(output_dir, exist_ok=True)
    draw_glass(os.path.join(output_dir, "elbphilharmonie_glass.png"))
    print("landmark textures written to", output_dir)


if __name__ == "__main__":
    main()
