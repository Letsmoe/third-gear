"""ComfyUI nodes for the Third Gear texture pipeline: wrap-around upscaling and map saving.

Approach taken from the massif texture pipeline (ComfyUI-MassifTex): every spatial operation sees the texture as a
torus, so a texture that tiles on input still tiles on output.
Image tensors follow ComfyUI's convention: float32 [batch, height, width, channels], 0..1.
"""
import math
import os

import numpy as np
import torch
import torch.nn.functional as functional
from PIL import Image

import folder_paths

CATEGORY = "ThirdGear"


def to_channels_first(image):
    """[B, H, W, C] (or channel-less [B, H, W] from the CHORD height node) to [B, C, H, W]."""
    if image.ndim == 3:
        return image.unsqueeze(1)
    return image.movedim(-1, 1)


def wrap_pad(image, pad):
    """Pads [B, C, H, W] by wrapping around the edges, also when the pad is larger than the image."""
    if pad <= 0:
        return image
    height, width = image.shape[-2], image.shape[-1]
    rows = torch.arange(-pad, height + pad, device=image.device) % height
    columns = torch.arange(-pad, width + pad, device=image.device) % width
    return image[..., rows, :][..., :, columns]


def wrap_resize(image, size):
    """Bicubic resize of a square [B, C, H, W] map that keeps the wrap-around seam intact."""
    if image.shape[-1] == size:
        return image
    pad = max(1, min(16, image.shape[-1] // 4))
    scale = size / image.shape[-1]
    padded_out = int(round(pad * scale))
    resized = functional.interpolate(
        wrap_pad(image, pad),
        size=(size + 2 * padded_out, size + 2 * padded_out),
        mode="bicubic", align_corners=False, antialias=True)
    return resized[:, :, padded_out:padded_out + size, padded_out:padded_out + size]


def upscale_with_model_wrapped(image, upscale_model, pad, tile=512, overlap=32):
    """One pass of a spandrel upscale model over a wrap-padded image, with the padding cropped off afterwards."""
    import comfy.model_management
    import comfy.utils

    scale = upscale_model.scale
    padded = wrap_pad(image, pad).to(upscale_model.patcher.load_device)
    memory_required = (512 * 512 * 3) * padded.element_size() * max(scale, 1.0) * 384.0
    memory_required += padded.nelement() * padded.element_size()
    comfy.model_management.load_models_gpu([upscale_model.patcher], memory_required=memory_required,
                                           force_full_load=True)
    output_device = comfy.model_management.intermediate_device()
    while True:
        try:
            steps = padded.shape[0] * comfy.utils.get_tiled_scale_steps(
                padded.shape[3], padded.shape[2], tile_x=tile, tile_y=tile, overlap=overlap)
            progress = comfy.utils.ProgressBar(steps)
            output = comfy.utils.tiled_scale(
                padded, lambda block: upscale_model(block.float()), tile_x=tile, tile_y=tile, overlap=overlap,
                upscale_amount=scale, pbar=progress, output_device=output_device)
            break
        except Exception as error:
            comfy.model_management.raise_non_oom(error)
            tile //= 2
            if tile < 128:
                raise
    crop = int(round(pad * scale))
    if crop > 0:
        output = output[:, :, crop:output.shape[2] - crop, crop:output.shape[3] - crop]
    return output.clamp(0.0, 1.0)


class TgSeamlessUpscale:
    """Upscales a tileable map to a square target size; with a model the detail is invented by ESRGAN, without it bicubic."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "target_size": ("INT", {"default": 4096, "min": 64, "max": 16384, "step": 64}),
                "pad": ("INT", {"default": 64, "min": 0, "max": 512, "step": 16}),
            },
            "optional": {"upscale_model": ("UPSCALE_MODEL",)},
        }

    RETURN_TYPES = ("IMAGE",)
    FUNCTION = "run"
    CATEGORY = CATEGORY

    def run(self, image, target_size, pad, upscale_model=None):
        x = to_channels_first(image).float()
        if x.shape[1] == 1:
            x = x.repeat(1, 3, 1, 1)
        if upscale_model is not None:
            scale = float(upscale_model.scale)
            passes = 0
            if scale > 1.0 and x.shape[-1] < target_size:
                passes = int(math.ceil(math.log(target_size / x.shape[-1]) / math.log(scale)))
            for _ in range(passes):
                x = upscale_with_model_wrapped(x, upscale_model, pad)
        x = wrap_resize(x, target_size).clamp(0, 1)
        return (x.movedim(1, -1).cpu(),)


class TgSaveMap:
    """Writes one map as PNG to <output dir>/<path>.png; 16 bit grey for height, 8 bit RGB otherwise."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "path": ("STRING", {"default": "texturegen/material/map"}),
                "sixteen_bit_grey": ("BOOLEAN", {"default": False}),
            }
        }

    RETURN_TYPES = ()
    FUNCTION = "save"
    OUTPUT_NODE = True
    CATEGORY = CATEGORY

    def save(self, image, path, sixteen_bit_grey):
        destination = os.path.join(folder_paths.get_output_directory(), path + ".png")
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        pixels = image[0].clamp(0, 1).cpu().numpy() if image.ndim == 4 else image[0].clamp(0, 1).cpu().numpy()[..., None]
        if sixteen_bit_grey:
            grey = (pixels[..., 0] * 65535.0 + 0.5).astype(np.uint16)
            Image.fromarray(grey, mode="I;16").save(destination)
        else:
            Image.fromarray((pixels[..., :3] * 255.0 + 0.5).astype(np.uint8), mode="RGB").save(destination)
        return {}


class TgRollHalf:
    """Shifts a map by half its size on both axes, so the tile seam ends up in the middle of the image."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"image": ("IMAGE",)}}

    RETURN_TYPES = ("IMAGE",)
    FUNCTION = "run"
    CATEGORY = CATEGORY

    def run(self, image):
        return (torch.roll(image, shifts=(image.shape[1] // 2, image.shape[2] // 2), dims=(1, 2)),)


class TgSeamMask:
    """Cross-shaped mask over the middle lines of a square image, feathered at its edges, for repainting the seam."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {
            "size": ("INT", {"default": 2048, "min": 64, "max": 16384, "step": 8}),
            "band": ("INT", {"default": 400, "min": 16, "max": 4096, "step": 8}),
            "feather": ("INT", {"default": 96, "min": 0, "max": 1024, "step": 8}),
        }}

    RETURN_TYPES = ("MASK",)
    FUNCTION = "run"
    CATEGORY = CATEGORY

    def run(self, size, band, feather):
        distance = (torch.arange(size, dtype=torch.float32) - size / 2 + 0.5).abs()
        line = ((band / 2 - distance) / max(feather, 1) + 0.5).clamp(0, 1)
        mask = torch.maximum(line[:, None], line[None, :])
        return (mask.unsqueeze(0),)


NODE_CLASS_MAPPINGS = {"TgSeamlessUpscale": TgSeamlessUpscale, "TgSaveMap": TgSaveMap, "TgRollHalf": TgRollHalf,
                      "TgSeamMask": TgSeamMask}
NODE_DISPLAY_NAME_MAPPINGS = {"TgSeamlessUpscale": "Third Gear - Seamless Upscale", "TgSaveMap": "Third Gear - Save Map"}
