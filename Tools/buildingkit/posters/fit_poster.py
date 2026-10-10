"""Crops an image to the CityLight poster ratio (1160 by 1710 mm) around its centre and scales it to 1160 by 1710
pixels, one pixel per millimetre.

    python -I fit_poster.py <in.png> <out.png>
"""
import sys

from PIL import Image

WIDTH, HEIGHT = 1160, 1710


def fit(source_path, target_path):
    """Writes the centre crop of the source at the poster ratio, scaled to the poster size."""
    image = Image.open(source_path).convert("RGB")
    ratio = WIDTH / HEIGHT
    if image.width / image.height > ratio:
        crop_width = round(image.height * ratio)
        left = (image.width - crop_width) // 2
        image = image.crop((left, 0, left + crop_width, image.height))
    else:
        crop_height = round(image.width / ratio)
        top = (image.height - crop_height) // 2
        image = image.crop((0, top, image.width, top + crop_height))
    image.resize((WIDTH, HEIGHT), Image.LANCZOS).save(target_path)


if __name__ == "__main__":
    fit(sys.argv[1], sys.argv[2])
