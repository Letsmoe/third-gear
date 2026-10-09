"""Step 2 of the prop pipeline: Hunyuan 3D 2.1 turns each reference image into a raw mesh (shape only, no texture).

Usage: python make_meshes.py <reference_dir> <output_dir> [prop ...]; needs ComfyUI on 127.0.0.1:8189."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import comfy_client  # noqa: E402
from prop_list import PROPS  # noqa: E402


def build_workflow(image_name, seed, filename_prefix):
    """Hunyuan 3D 2.1 image to shape: DINOv2 conditioning, flow sampling, VAE volume decode, surface-net mesh."""
    return {
        "1": {"class_type": "ImageOnlyCheckpointLoader", "inputs": {"ckpt_name": "hunyuan_3d_v2.1.safetensors"}},
        "2": {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": ["1", 0], "shift": 1.0}},
        "3": {"class_type": "LoadImage", "inputs": {"image": image_name}},
        "4": {"class_type": "CLIPVisionEncode", "inputs": {"clip_vision": ["1", 1], "image": ["3", 0], "crop": "center"}},
        "5": {"class_type": "Hunyuan3Dv2Conditioning", "inputs": {"clip_vision_output": ["4", 0]}},
        "6": {"class_type": "EmptyLatentHunyuan3Dv2", "inputs": {"resolution": 4096, "batch_size": 1}},
        "7": {"class_type": "KSampler", "inputs": {"model": ["2", 0], "seed": seed, "steps": 40, "cfg": 5.0,
                                                    "sampler_name": "euler", "scheduler": "normal",
                                                    "positive": ["5", 0], "negative": ["5", 1], "latent_image": ["6", 0], "denoise": 1.0}},
        "8": {"class_type": "VAEDecodeHunyuan3D", "inputs": {"samples": ["7", 0], "vae": ["1", 2], "num_chunks": 8000, "octree_resolution": 320}},
        "9": {"class_type": "VoxelToMesh", "inputs": {"voxel": ["8", 0], "algorithm": "surface net", "threshold": 0.6}},
        "10": {"class_type": "SaveGLB", "inputs": {"mesh": ["9", 0], "filename_prefix": filename_prefix}},
    }


def main():
    reference_dir, output_dir = sys.argv[1], sys.argv[2]
    wanted = sys.argv[3:] or list(PROPS.keys())
    os.makedirs(output_dir, exist_ok=True)
    for name in wanted:
        uploaded = comfy_client.upload_image(os.path.join(reference_dir, name + "_cut.png"), "kit_" + name + ".png")
        entry = comfy_client.run_workflow(build_workflow(uploaded, PROPS[name].get("seed", 7), "raw_" + name))
        for node_output in entry["outputs"].values():
            for item in node_output.get("images", []) + node_output.get("3d", []):
                comfy_client.download_output(item, os.path.join(output_dir, name + ".glb"))
        print("raw mesh", name, flush=True)


if __name__ == "__main__":
    main()
