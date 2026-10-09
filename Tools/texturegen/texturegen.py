#!/usr/bin/env python3
"""Generates tileable PBR texture sets with ComfyUI (SDXL base, refine, CHORD material estimation) and finishes them.

The approach comes from the massif texture pipeline (neoworks/massif-texture-generation): a seamless-tiling SDXL
photo of the surface is turned into colour, normal, roughness and height by CHORD, then upscaled with wrap-around
borders so the tile still repeats. Here the workflow is written straight in ComfyUI's API format, and the finishing
(albedo level, normal micro detail, ambient occlusion) happens in this script.

Run through Tools/texturegen/run_texturegen.sh, which holds the gpu lock and starts and stops the server:
  run_texturegen.sh [--only name,name] [--force] [--budget-minutes 10] [--draft]
Output: <data root>/texturegen/<name>/T_<name>_{BaseColor,Normal,Roughness,AO,Height}.png and a review sheet.
"""
import argparse
import json
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as functional
import yaml
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "buildingkit", "props"))
sys.path.insert(0, os.path.join(HERE, "..", "bootstrap"))
import comfy_client  # noqa: E402
import data_root  # noqa: E402

DATA_ROOT = data_root.data_root()
OUTPUT_ROOT = os.path.join(DATA_ROOT, "texturegen")
COMFY_OUT = os.path.join(OUTPUT_ROOT, "_comfy", "out")
COMFY_IN = os.path.join(OUTPUT_ROOT, "_comfy", "in")
SERVER = os.environ.get("TEXTUREGEN_SERVER", "http://127.0.0.1:8190")
comfy_client.SERVER = SERVER

CHECKPOINT = "RealVisXL_V5.0_fp16.safetensors"
UPSCALER = "4x-UltraSharp.pth"
CHORD_CHECKPOINT = "chord_v1.safetensors"


def sdxl_photo_stage(spec):
    """SDXL with seamless-tiling model and VAE: base pass, then a refine pass. Returns (nodes, photo reference)."""
    graph = {
        "ckpt": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": CHECKPOINT}},
        "tile_model": {"class_type": "SeamlessTile",
                       "inputs": {"model": ["ckpt", 0], "tiling": "enable", "copy_model": "Make a copy"}},
        "tile_vae": {"class_type": "MakeCircularVAE",
                     "inputs": {"vae": ["ckpt", 2], "tiling": "enable", "copy_vae": "Make a copy"}},
        "positive": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["ckpt", 1], "text": spec["prompt"]}},
        "negative": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["ckpt", 1], "text": spec["negative"]}},
    }
    base_denoise = 1.0
    if spec.get("seed_image_uploaded"):
        # Text-to-image alone turns organic ground into mud; starting from a real photo fixes the structure.
        graph.update(seed_image_nodes(spec, "tile_vae", spec["base_size"]))
        base_denoise = spec["seed_denoise"]
    else:
        graph["start_latent"] = {"class_type": "EmptyLatentImage", "inputs": {
            "width": spec["base_size"], "height": spec["base_size"], "batch_size": 1}}
    graph.update({
        "base": {"class_type": "KSampler", "inputs": {
            "model": ["tile_model", 0], "positive": ["positive", 0], "negative": ["negative", 0],
            "latent_image": ["start_latent", 0], "seed": spec["seed"], "steps": spec["base_steps"],
            "cfg": spec["cfg"], "sampler_name": "dpmpp_2m", "scheduler": "karras", "denoise": base_denoise}},
        "latent_up": {"class_type": "LatentUpscale", "inputs": {
            "samples": ["base", 0], "upscale_method": "bislerp", "width": spec["refine_size"],
            "height": spec["refine_size"], "crop": "disabled"}},
        "refine": {"class_type": "KSampler", "inputs": {
            "model": ["tile_model", 0], "positive": ["positive", 0], "negative": ["negative", 0],
            "latent_image": ["latent_up", 0], "seed": spec["seed"], "steps": spec["refine_steps"],
            "cfg": spec["cfg"], "sampler_name": "dpmpp_2m", "scheduler": "karras",
            "denoise": spec["refine_denoise"]}},
        "decode": {"class_type": "VAEDecode", "inputs": {"samples": ["refine", 0], "vae": ["tile_vae", 0]}},
    })
    return graph, ["decode", 0]


def seed_image_nodes(spec, vae_node, size):
    """Loads the uploaded seed photo, fits it to the size and encodes it as the starting latent."""
    return {
        "load": {"class_type": "LoadImage", "inputs": {"image": spec["seed_image_uploaded"]}},
        "fit": {"class_type": "ImageScale", "inputs": {
            "image": ["load", 0], "upscale_method": "lanczos", "width": size, "height": size, "crop": "center"}},
        "start_latent": {"class_type": "VAEEncode", "inputs": {"pixels": ["fit", 0], "vae": [vae_node, 0]}},
    }


def dit_photo_stage(spec):
    """FLUX.2 klein or Z-Image-Turbo: a plain generation, then the seam is repainted.

    These models have no circular-padding trick like SDXL. The image is shifted by half its size so the wrap seam sits
    in the middle as a cross, which is repainted by an inpainting pass; the result tiles at its own borders.
    """
    size = spec["dit_size"]
    flux = spec["model"] == "flux2"
    graph = {"positive": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["clip", 0], "text": spec["prompt"]}},
             "negative": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["positive", 0]}}}
    if flux:
        graph["unet"] = {"class_type": "UNETLoader", "inputs": {"unet_name": "flux-2-klein-4b-fp8.safetensors",
                                                                "weight_dtype": "default"}}
        graph["clip"] = {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b_fp8_mixed.safetensors",
                                                                "type": "flux2"}}
        graph["vae"] = {"class_type": "VAELoader", "inputs": {"vae_name": "flux2-vae.safetensors"}}
        model_ref = ["unet", 0]
        steps, sampler, scheduler = spec["flux_steps"], "euler", "simple"
        empty_class = "EmptyFlux2LatentImage"
    else:
        graph["unet"] = {"class_type": "UNETLoader", "inputs": {"unet_name": "z_image_turbo_bf16.safetensors",
                                                                "weight_dtype": "default"}}
        graph["clip"] = {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b.safetensors",
                                                                "type": "lumina2"}}
        graph["vae"] = {"class_type": "VAELoader", "inputs": {"vae_name": "ae.safetensors"}}
        graph["aura"] = {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": ["unet", 0], "shift": 3}}
        model_ref = ["aura", 0]
        steps, sampler, scheduler = spec["zimage_steps"], "res_multistep", "simple"
        empty_class = "EmptySD3LatentImage"
    graph["empty"] = {"class_type": empty_class, "inputs": {"width": size, "height": size, "batch_size": 1}}
    start_latent = ["empty", 0]
    denoise = 1.0
    if spec.get("seed_image_uploaded"):
        graph.update(seed_image_nodes(spec, "vae", size))
        start_latent = ["start_latent", 0]
        denoise = spec["seed_denoise"]

    def sampler_inputs(latent, seed_offset, sampling_denoise):
        return {"model": model_ref, "positive": ["positive", 0], "negative": ["negative", 0], "latent_image": latent,
                "seed": spec["seed"] + seed_offset, "steps": steps, "cfg": 1.0, "sampler_name": sampler,
                "scheduler": scheduler, "denoise": sampling_denoise}

    graph["first"] = {"class_type": "KSampler", "inputs": sampler_inputs(start_latent, 0, denoise)}
    graph["first_decode"] = {"class_type": "VAEDecode", "inputs": {"samples": ["first", 0], "vae": ["vae", 0]}}
    graph["roll"] = {"class_type": "TgRollHalf", "inputs": {"image": ["first_decode", 0]}}
    graph["rolled_latent"] = {"class_type": "VAEEncode", "inputs": {"pixels": ["roll", 0], "vae": ["vae", 0]}}
    graph["seam_mask"] = {"class_type": "TgSeamMask", "inputs": {
        "size": size, "band": int(size * spec["seam_band"]), "feather": int(size * spec["seam_band"] * 0.3)}}
    graph["masked"] = {"class_type": "SetLatentNoiseMask", "inputs": {"samples": ["rolled_latent", 0],
                                                                       "mask": ["seam_mask", 0]}}
    graph["repaint"] = {"class_type": "KSampler", "inputs": sampler_inputs(["masked", 0], 1, spec["seam_denoise"])}
    graph["decode"] = {"class_type": "VAEDecode", "inputs": {"samples": ["repaint", 0], "vae": ["vae", 0]}}
    return graph, ["decode", 0]


def build_workflow(spec):
    """Returns the ComfyUI API-format graph for one material spec (a dict from materials.yaml with defaults merged)."""
    if spec["model"] == "sdxl":
        graph, photo = sdxl_photo_stage(spec)
    else:
        graph, photo = dit_photo_stage(spec)
    graph.update({
        "chord": {"class_type": "ChordLoadModel", "inputs": {"ckpt_name": CHORD_CHECKPOINT}},
        "material": {"class_type": "ChordMaterialEstimation", "inputs": {"chord_model": ["chord", 0], "image": photo}},
        "height": {"class_type": "ChordNormalToHeight", "inputs": {"normal": ["material", 1]}},
        "upscaler": {"class_type": "UpscaleModelLoader", "inputs": {"model_name": UPSCALER}},
    })
    prefix = spec["work_name"] + "/"
    # (key, source, ESRGAN, 16 bit grey, file); the photo is upscaled with the model to carry fine detail back
    # into the colour map, the CHORD maps are only resized.
    saves = [
        ("photo", photo, True, False, "photo"),
        ("basecolor", ["material", 0], False, False, "basecolor"),
        ("normal", ["material", 1], False, False, "normal"),
        ("roughness", ["material", 2], False, False, "roughness"),
        ("metalness", ["material", 3], False, False, "metalness"),
        ("height", ["height", 0], False, True, "height"),
    ]
    for key, source, use_model, sixteen_bit, file_name in saves:
        upscale_inputs = {"image": source, "target_size": spec["size"], "pad": 64}
        if use_model:
            upscale_inputs["upscale_model"] = ["upscaler", 0]
        graph[f"up_{key}"] = {"class_type": "TgSeamlessUpscale", "inputs": upscale_inputs}
        graph[f"save_{key}"] = {"class_type": "TgSaveMap", "inputs": {
            "image": [f"up_{key}", 0], "path": prefix + file_name, "sixteen_bit_grey": sixteen_bit}}
    return graph


def load_specs(materials_file, only_names, model, variant):
    """Reads materials.yaml and returns the specs (defaults merged) in file order, optionally filtered by name."""
    with open(materials_file) as handle:
        document = yaml.safe_load(handle)
    defaults = document["defaults"]
    specs = [{**defaults, **material} for material in document["materials"]]
    for spec in specs:
        spec["model"] = model
        spec["variant"] = variant
        spec["work_name"] = f"{variant}__{spec['name']}" if variant else spec["name"]
        spec["out_dir"] = os.path.join(OUTPUT_ROOT, "_shootout", variant, spec["name"]) if variant \
            else os.path.join(OUTPUT_ROOT, spec["name"])
    if only_names:
        unknown = set(only_names) - {spec["name"] for spec in specs}
        if unknown:
            sys.exit(f"unknown material names: {sorted(unknown)}")
        specs = [spec for spec in specs if spec["name"] in only_names]
    return specs


# --- finishing -----------------------------------------------------------------------------------------------------

def wrap_pad(image, pad):
    """Pads [B, C, H, W] by wrapping around the edges."""
    rows = torch.arange(-pad, image.shape[-2] + pad) % image.shape[-2]
    columns = torch.arange(-pad, image.shape[-1] + pad) % image.shape[-1]
    return image[..., rows, :][..., :, columns]


def wrapped_blur(image, radius):
    """Separable gaussian blur of [B, C, H, W] that treats the image as a torus."""
    sigma = max(radius / 2.0, 0.5)
    offsets = torch.arange(-radius, radius + 1, dtype=image.dtype)
    kernel = torch.exp(-(offsets ** 2) / (2 * sigma ** 2))
    kernel = kernel / kernel.sum()
    channels = image.shape[1]
    padded = wrap_pad(image, radius)
    horizontal = functional.conv2d(padded, kernel.view(1, 1, 1, -1).expand(channels, 1, 1, -1), groups=channels)
    return functional.conv2d(horizontal, kernel.view(1, 1, -1, 1).expand(channels, 1, -1, 1), groups=channels)


def luminance(image):
    """Rec. 709 luma of [B, 3, H, W]."""
    weights = torch.tensor([0.2126, 0.7152, 0.0722]).view(1, 3, 1, 1)
    return (image[:, :3] * weights).sum(dim=1, keepdim=True)


def normal_from_height(height, strength):
    """OpenGL-style tangent-space normal (0..1 encoded) from a [B, 1, H, W] height field."""
    sobel_x = torch.tensor([[-1.0, 0.0, 1.0], [-2.0, 0.0, 2.0], [-1.0, 0.0, 1.0]]).view(1, 1, 3, 3) / 8.0
    padded = wrap_pad(height, 1)
    gradient_x = functional.conv2d(padded, sobel_x)
    gradient_y = functional.conv2d(padded, sobel_x.transpose(-1, -2))
    scale = strength * height.shape[-1] / 256.0
    normal = torch.cat([-gradient_x * scale, -gradient_y * scale, torch.ones_like(gradient_x)], dim=1)
    return normal / normal.norm(dim=1, keepdim=True).clamp_min(1e-6)


def blend_normal_detail(normal01, detail_source, strength, radius):
    """Adds the high-frequency relief of the colour map to the (1024 px resolution) CHORD normal, UDN blend."""
    base = normal01 * 2.0 - 1.0
    base = base / base.norm(dim=1, keepdim=True).clamp_min(1e-6)
    luma = luminance(detail_source)
    highpass = luma - wrapped_blur(luma, radius)
    detail = normal_from_height(highpass, 1.0)
    combined = torch.cat([base[:, :2] + detail[:, :2] * strength, base[:, 2:3]], dim=1)
    combined = combined / combined.norm(dim=1, keepdim=True).clamp_min(1e-6)
    return combined * 0.5 + 0.5


def ambient_occlusion(height, strength):
    """Multi-scale cavity occlusion from a [B, 1, H, W] height field (how far a texel sits below its surroundings)."""
    size = height.shape[-1]
    radii = [max(2, size // 512), max(3, size // 128), max(4, size // 64)]
    total = torch.zeros_like(height)
    weight = 0.0
    for index, radius in enumerate(radii):
        total = total + (wrapped_blur(height, radius) - height).clamp_min(0.0) / (index + 1)
        weight += 1.0 / (index + 1)
    cavity = total / weight
    cavity = cavity / cavity.amax().clamp_min(1e-6)
    return (1.0 - strength * cavity).clamp(0.0, 1.0)


def srgb_to_linear(values):
    return torch.where(values <= 0.04045, values / 12.92, ((values + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(values):
    return torch.where(values <= 0.0031308, values * 12.92, 1.055 * values.clamp_min(0) ** (1 / 2.4) - 0.055)


def set_albedo_level(basecolor, target_reflectance):
    """Scales the colour in linear light so its mean reflectance (linear luminance) hits the target; keeps hue."""
    linear = srgb_to_linear(basecolor)
    current = luminance(linear).mean().item()
    wanted = target_reflectance
    gain = wanted / max(current, 1e-6)
    return linear_to_srgb((linear * gain).clamp(0, 1)), gain


def add_photo_detail(basecolor, photo, strength, radius=10):
    """Restores the sharpness the CHORD colour map lost (it works at 1024 px) from the upscaled source photo.

    The photo's luminance divided by its own blur is a flat-lit detail layer without large-scale shading, so it can
    be multiplied onto the de-lit colour map.
    """
    if strength <= 0:
        return basecolor
    luma = luminance(photo)
    ratio = (luma / wrapped_blur(luma, radius).clamp_min(0.02)).clamp(0.4, 2.5)
    return (basecolor * (1.0 + (ratio - 1.0) * strength)).clamp(0, 1)


def set_saturation(basecolor, factor):
    """Scales the colour saturation around the luminance of each pixel (1.0 keeps it)."""
    grey = luminance(basecolor)
    return (grey + (basecolor - grey) * factor).clamp(0, 1)


def stretch_height(height, low_percentile=0.5, high_percentile=99.5):
    """Maps the height field's robust range to 0..1 so displacement uses the full depth range."""
    flat = height.flatten()
    sample = flat[::37]
    low = torch.quantile(sample, low_percentile / 100.0)
    high = torch.quantile(sample, high_percentile / 100.0)
    return ((height - low) / (high - low).clamp_min(1e-4)).clamp(0, 1)


def read_rgb(path):
    array = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    return torch.from_numpy(array).permute(2, 0, 1).unsqueeze(0)


def read_grey16(path):
    array = np.asarray(Image.open(path), dtype=np.float32)
    if array.ndim == 3:
        array = array[..., 0]
    maximum = 65535.0 if array.max() > 255 else 255.0
    return torch.from_numpy(array / maximum)[None, None]


def write_rgb(tensor, path):
    array = (tensor[0].clamp(0, 1).permute(1, 2, 0).numpy() * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(array, mode="RGB").save(path)


def write_grey8(tensor, path):
    array = (tensor[0, 0].clamp(0, 1).numpy() * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(array, mode="L").save(path)


def finish_material(spec):
    """Turns the raw ComfyUI maps of one material into the final set and a review sheet; returns the statistics."""
    name = spec["name"]
    raw = os.path.join(COMFY_OUT, spec["work_name"])
    out = spec["out_dir"]
    os.makedirs(out, exist_ok=True)

    basecolor = read_rgb(os.path.join(raw, "basecolor.png"))
    basecolor = add_photo_detail(basecolor, read_rgb(os.path.join(raw, "photo.png")), spec["photo_detail"])
    gain = 1.0
    if spec.get("albedo_mean"):
        basecolor, gain = set_albedo_level(basecolor, spec["albedo_mean"])
    if spec["saturation"] != 1.0:
        basecolor = set_saturation(basecolor, spec["saturation"])
    normal = blend_normal_detail(read_rgb(os.path.join(raw, "normal.png")), basecolor,
                                 spec["detail"], spec["detail_radius"])
    roughness = read_rgb(os.path.join(raw, "roughness.png"))[:, :1]
    roughness = (roughness * spec["roughness_scale"] + spec["roughness_offset"]).clamp(0.0, 1.0)
    roughness = spec["roughness_min"] + (1.0 - spec["roughness_min"]) * roughness  # keeps the variation above the floor
    height_path = os.path.join(raw, "height.png")
    height = stretch_height(read_grey16(height_path))
    occlusion = ambient_occlusion(height, spec["ao_strength"])

    write_rgb(basecolor, os.path.join(out, f"T_{name}_BaseColor.png"))
    write_rgb(normal, os.path.join(out, f"T_{name}_Normal.png"))
    write_grey8(roughness, os.path.join(out, f"T_{name}_Roughness.png"))
    write_grey8(occlusion, os.path.join(out, f"T_{name}_AO.png"))
    Image.fromarray((height[0, 0].numpy() * 65535.0 + 0.5).astype(np.uint16), mode="I;16").save(
        os.path.join(out, f"T_{name}_Height.png"))
    Image.open(os.path.join(raw, "photo.png")).save(os.path.join(out, f"{name}_photo.png"))
    make_review_sheet(name, basecolor, normal, roughness, os.path.join(out, f"{name}_sheet.png"))
    return {"name": name, "albedo_gain": round(gain, 3),
            "albedo_srgb": round(float(luminance(basecolor).mean()), 3), "reflectance": round(float(luminance(srgb_to_linear(basecolor)).mean()), 3),
            "roughness_mean": round(float(roughness.mean()), 3)}


def make_review_sheet(name, basecolor, normal, roughness, path, cell=700):
    """One image for judging a set: 3x3 repetition of the colour map, a 1:1 crop, normal and roughness."""
    def to_image(tensor, size):
        if tensor.shape[1] == 1:
            tensor = tensor.repeat(1, 3, 1, 1)
        resized = functional.interpolate(tensor, size=(size, size), mode="bilinear", antialias=True, align_corners=False)
        return Image.fromarray((resized[0].clamp(0, 1).permute(1, 2, 0).numpy() * 255 + 0.5).astype(np.uint8))

    small = to_image(basecolor, cell // 3)
    repeated = Image.new("RGB", (cell, cell))
    for row in range(3):
        for column in range(3):
            repeated.paste(small, (column * (cell // 3), row * (cell // 3)))
    full = basecolor.shape[-1]
    crop = basecolor[:, :, full // 2:full // 2 + 1024, full // 2:full // 2 + 1024]
    crop_image = Image.fromarray((crop[0].clamp(0, 1).permute(1, 2, 0).numpy() * 255 + 0.5).astype(np.uint8))
    crop_image = crop_image.resize((cell, cell), Image.LANCZOS)
    panels = [("tiled 3x3", repeated), ("1024 px crop at 1:1", crop_image),
              ("normal", to_image(normal, cell)), ("roughness", to_image(roughness, cell))]
    bar = 28
    sheet = Image.new("RGB", (cell * len(panels), cell + bar), (24, 24, 27))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=18)
    for index, (label, image) in enumerate(panels):
        sheet.paste(image, (index * cell, bar))
        draw.text((index * cell + 8, 5), f"{name}: {label}", fill=(225, 225, 230), font=font)
    sheet.save(path)


# --- running ---------------------------------------------------------------------------------------------------------

def upload_seed_image(spec):
    """Uploads the photo a material starts from (path relative to the raw assets) and records the server-side name."""
    relative = spec.get("seed_image")
    if not relative:
        return
    source = os.path.join(data_root.raw_assets_dir(), relative)
    spec["seed_image_uploaded"] = comfy_client.upload_image(source, f"tg_seed_{spec['name']}.png")


def generate(spec, draft, arguments_no_seed=False):
    """Queues one material, waits for it and downloads nothing: the save nodes write straight into the data root."""
    if draft:
        spec.update(size=1024, refine_size=1024, base_steps=20, refine_steps=12, dit_size=1024)
    if arguments_no_seed:
        spec.pop("seed_image", None)
    upload_seed_image(spec)
    comfy_client.run_workflow(build_workflow(spec))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--materials", default=os.path.join(HERE, "materials.yaml"))
    parser.add_argument("--only", help="comma-separated material names")
    parser.add_argument("--force", action="store_true", help="regenerate sets that already exist")
    parser.add_argument("--budget-minutes", type=float, default=10.0, help="stop starting new materials after this")
    parser.add_argument("--model", choices=["sdxl", "flux2", "zimage"], default="flux2")
    parser.add_argument("--no-seed-image", action="store_true", help="ignore seed_image, generate from text only")
    parser.add_argument("--variant", default="", help="writes to _shootout/<variant>/ instead of the final folder")
    parser.add_argument("--dit-size", type=int, help="generation size of the FLUX and Z-Image stages (default 2048)")
    parser.add_argument("--draft", action="store_true", help="1024 px, fewer steps: a quick look at prompts and seeds")
    parser.add_argument("--finish-only", action="store_true", help="redo the finishing on the existing raw maps")
    arguments = parser.parse_args()

    only = arguments.only.split(",") if arguments.only else None
    specs = load_specs(arguments.materials, only, arguments.model, arguments.variant)
    if arguments.dit_size:
        for spec in specs:
            spec["dit_size"] = arguments.dit_size
    started = time.monotonic()
    statistics = []
    for spec in specs:
        final = os.path.join(spec["out_dir"], f"T_{spec['name']}_BaseColor.png")
        if os.path.exists(final) and not arguments.force and not arguments.finish_only:
            print(f"{spec['name']}: exists, skipped")
            continue
        if time.monotonic() - started > arguments.budget_minutes * 60:
            print(f"{spec['name']}: time budget used up, run again for the rest")
            continue
        began = time.monotonic()
        if not arguments.finish_only:
            generate(spec, arguments.draft, arguments.no_seed_image)
        statistics.append(finish_material(spec))
        print(f"{spec['name']}: done in {time.monotonic() - began:.0f} s  {statistics[-1]}", flush=True)
    print(json.dumps(statistics))


if __name__ == "__main__":
    main()
