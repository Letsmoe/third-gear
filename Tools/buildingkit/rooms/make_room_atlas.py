"""Generates the back-wall atlas of the window interiors: FLUX.2 klein draws twelve frontal views of a room's far wall
(furniture, shelves, pictures, wallpaper), which are packed into a 4 by 3 atlas of 512 x 384 tiles.
Needs ComfyUI listening on 127.0.0.1:8189 (run_rooms.sh starts it).

Usage: python make_room_atlas.py <output_dir>   writes <output_dir>/raw/room_NN.png and <output_dir>/room_atlas.png"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "props"))

import comfy_client  # noqa: E402
from PIL import Image  # noqa: E402

STYLE = ("straight-on frontal photograph of the far wall of a room, camera at the middle of the room facing the wall, "
         "no people, soft even interior light, lived-in German home, realistic photo, sharp, 35mm")
ROOMS = [
    "living room, grey sofa below three framed pictures on a white wall, floor lamp",
    "wall of bookshelves full of books with a green houseplant and a reading chair",
    "bedroom, double bed with a wooden headboard and two bedside lamps, patterned wallpaper",
    "kitchen wall with white cabinets, tiled splashback, hob and extractor hood, window blind edge",
    "white wardrobe with a mirrored door beside a chest of drawers, beige painted wall",
    "home office, desk with a monitor and shelves with folders, pale blue wall",
    "children's room, colourful wall, low shelf with toys, small desk",
    "dining area, wooden table with chairs in front of a sideboard and a large painting",
    "old Hamburg Altbau room with a high white wall, stucco ceiling edge, tall plant, armchair and a standing lamp",
    "television on a dark wooden sideboard with shelves, magazines and plants, warm walls",
    "hallway with a coat rack, shoes, a mirror and a white door",
    "almost empty room with a bare painted wall, a radiator and a stack of moving boxes",
]
TILE_SIZE = (512, 384)
COLUMNS = 4


def build_workflow(prompt_text, seed, prefix):
    """FLUX.2 klein text to image, 4 steps, 1024 x 768."""
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "flux-2-klein-4b-fp8.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b_fp8_mixed.safetensors", "type": "flux2"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": "flux2-vae.safetensors"}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt_text, "clip": ["2", 0]}},
        "5": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["4", 0]}},
        "6": {"class_type": "CFGGuider", "inputs": {"model": ["1", 0], "positive": ["4", 0], "negative": ["5", 0], "cfg": 1.0}},
        "7": {"class_type": "RandomNoise", "inputs": {"noise_seed": seed}},
        "8": {"class_type": "KSamplerSelect", "inputs": {"sampler_name": "euler"}},
        "9": {"class_type": "Flux2Scheduler", "inputs": {"steps": 4, "width": 1024, "height": 768}},
        "10": {"class_type": "EmptyFlux2LatentImage", "inputs": {"width": 1024, "height": 768, "batch_size": 1}},
        "11": {"class_type": "SamplerCustomAdvanced", "inputs": {"noise": ["7", 0], "guider": ["6", 0], "sampler": ["8", 0], "sigmas": ["9", 0], "latent_image": ["10", 0]}},
        "12": {"class_type": "VAEDecode", "inputs": {"samples": ["11", 0], "vae": ["3", 0]}},
        "13": {"class_type": "SaveImage", "inputs": {"images": ["12", 0], "filename_prefix": prefix}},
    }


def main():
    output_dir = sys.argv[1]
    raw_dir = os.path.join(output_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)
    rows = (len(ROOMS) + COLUMNS - 1) // COLUMNS
    atlas = Image.new("RGB", (TILE_SIZE[0] * COLUMNS, TILE_SIZE[1] * rows))
    for index, description in enumerate(ROOMS):
        entry = comfy_client.run_workflow(build_workflow(description + ", " + STYLE, 100 + index, "room_%02d" % index))
        destination = os.path.join(raw_dir, "room_%02d.png" % index)
        for node_output in entry["outputs"].values():
            for image in node_output.get("images", []):
                comfy_client.download_output(image, destination)
        tile = Image.open(destination).convert("RGB").resize(TILE_SIZE, Image.LANCZOS)
        atlas.paste(tile, ((index % COLUMNS) * TILE_SIZE[0], (index // COLUMNS) * TILE_SIZE[1]))
        print("room", index, description)
    atlas.save(os.path.join(output_dir, "room_atlas.png"))


if __name__ == "__main__":
    main()
