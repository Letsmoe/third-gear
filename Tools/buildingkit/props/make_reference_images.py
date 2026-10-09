"""Step 1 of the prop pipeline: FLUX.2 klein makes a reference image per prop, BiRefNet cuts it out and it is
composited on mid grey, ready for Hunyuan 3D. Needs ComfyUI listening on 127.0.0.1:8189 (see run_props.sh).

Usage: python make_reference_images.py <output_dir> [prop ...]"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import comfy_client  # noqa: E402
from prop_list import PROPS  # noqa: E402


def build_workflow(prompt_text, seed, filename_prefix):
    """FLUX.2 klein text to image (4 steps, no CFG), then background removal and a white composite."""
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "flux-2-klein-4b-fp8.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b_fp8_mixed.safetensors", "type": "flux2"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": "flux2-vae.safetensors"}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt_text, "clip": ["2", 0]}},
        "5": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["4", 0]}},
        "6": {"class_type": "CFGGuider", "inputs": {"model": ["1", 0], "positive": ["4", 0], "negative": ["5", 0], "cfg": 1.0}},
        "7": {"class_type": "RandomNoise", "inputs": {"noise_seed": seed}},
        "8": {"class_type": "KSamplerSelect", "inputs": {"sampler_name": "euler"}},
        "9": {"class_type": "Flux2Scheduler", "inputs": {"steps": 4, "width": 1024, "height": 1024}},
        "10": {"class_type": "EmptyFlux2LatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
        "11": {"class_type": "SamplerCustomAdvanced", "inputs": {"noise": ["7", 0], "guider": ["6", 0], "sampler": ["8", 0], "sigmas": ["9", 0], "latent_image": ["10", 0]}},
        "12": {"class_type": "VAEDecode", "inputs": {"samples": ["11", 0], "vae": ["3", 0]}},
        "13": {"class_type": "LoadBackgroundRemovalModel", "inputs": {"bg_removal_name": "birefnet.safetensors"}},
        "14": {"class_type": "RemoveBackground", "inputs": {"bg_removal_model": ["13", 0], "image": ["12", 0]}},
        "15": {"class_type": "EmptyImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1, "color": 8355711}},
        "16": {"class_type": "ImageCompositeMasked", "inputs": {"destination": ["15", 0], "source": ["12", 0], "x": 0, "y": 0, "resize_source": False, "mask": ["14", 0]}},
        "17": {"class_type": "SaveImage", "inputs": {"images": ["16", 0], "filename_prefix": filename_prefix + "_cut"}},
        "18": {"class_type": "SaveImage", "inputs": {"images": ["12", 0], "filename_prefix": filename_prefix + "_raw"}},
    }


def main():
    output_dir = sys.argv[1]
    wanted = sys.argv[2:] or list(PROPS.keys())
    os.makedirs(output_dir, exist_ok=True)
    for name in wanted:
        prop = PROPS[name]
        entry = comfy_client.run_workflow(build_workflow(prop["prompt"], prop.get("seed", 7), "ref_" + name))
        for node_output in entry["outputs"].values():
            for image in node_output.get("images", []):
                suffix = "cut" if "_cut" in image["filename"] else "raw"
                comfy_client.download_output(image, os.path.join(output_dir, "%s_%s.png" % (name, suffix)))
        print("reference image", name)


if __name__ == "__main__":
    main()
