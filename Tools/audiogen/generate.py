#!/usr/bin/env python3
"""Generates the world and weather sounds with Stable Audio Open through a local ComfyUI server.

Starts the server if none answers, runs the requested sounds (keep a batch to about ten minutes), frees the VRAM and
stops a server it started itself. Run it under `Scripts/lock.sh gpu`, see Tools/audiogen/run_audiogen.sh.

Usage: generate.py <raw_output_dir> [sound_id ...]
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prompts import NEGATIVE_PROMPT, SOUNDS, SUFFIX  # noqa: E402

HOST = "127.0.0.1:8189"
COMFY_ROOT = os.path.expanduser("~/comfy/ComfyUI")
VENV_PYTHON = "/mnt/storage/third-gear/building_kit/comfy/venv/bin/python"
SAMPLER_STEPS = 60
GUIDANCE_SCALE = 5.0


def request(path, payload=None):
    """GET or POST JSON against the ComfyUI server."""
    data = json.dumps(payload).encode() if payload is not None else None
    http_request = urllib.request.Request(f"http://{HOST}{path}", data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(http_request, timeout=30) as response:
        body = response.read()
    return json.loads(body) if body else {}


def server_is_up():
    try:
        request("/system_stats")
        return True
    except OSError:
        return False


def start_server(output_dir):
    """Starts ComfyUI with its output in output_dir and waits until it answers."""
    os.makedirs(output_dir, exist_ok=True)
    log = open(os.path.join(output_dir, "server.log"), "w")
    process = subprocess.Popen(
        [VENV_PYTHON, os.path.join(COMFY_ROOT, "main.py"), "--listen", "127.0.0.1", "--port", HOST.split(":")[1],
         "--output-directory", output_dir, "--disable-all-custom-nodes", "--disable-auto-launch"],
        stdout=log, stderr=subprocess.STDOUT)
    for _ in range(180):
        if server_is_up():
            return process
        time.sleep(1)
    process.kill()
    sys.exit("ComfyUI did not start, see server.log")


def build_graph(sound):
    """API-format Stable Audio Open text-to-audio graph for one sound."""
    return {
        "4": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": "stable-audio-open-1.0.safetensors"}},
        "10": {"class_type": "CLIPLoader", "inputs": {"clip_name": "t5-base.safetensors", "type": "stable_audio", "device": "default"}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": sound["prompt"] + SUFFIX, "clip": ["10", 0]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": NEGATIVE_PROMPT, "clip": ["10", 0]}},
        "11": {"class_type": "EmptyLatentAudio", "inputs": {"seconds": sound["seconds"], "batch_size": 1}},
        "3": {"class_type": "KSampler", "inputs": {
            "seed": sound["seed"], "steps": SAMPLER_STEPS, "cfg": GUIDANCE_SCALE, "sampler_name": "dpmpp_3m_sde_gpu",
            "scheduler": "exponential", "denoise": 1, "model": ["4", 0], "positive": ["6", 0], "negative": ["7", 0],
            "latent_image": ["11", 0]}},
        "12": {"class_type": "VAEDecodeAudio", "inputs": {"samples": ["3", 0], "vae": ["4", 2]}},
        "19": {"class_type": "SaveAudio", "inputs": {"filename_prefix": sound["id"], "audio": ["12", 0]}},
    }


def run_sound(sound):
    """Queues one sound and waits for it to finish."""
    prompt_id = request("/prompt", {"prompt": build_graph(sound)})["prompt_id"]
    for _ in range(600):
        history = request(f"/history/{prompt_id}")
        if prompt_id in history:
            status = history[prompt_id]["status"]
            if status.get("status_str") != "success":
                sys.exit(f"{sound['id']} failed: {status}")
            return
        time.sleep(1)
    sys.exit(f"{sound['id']} timed out")


def free_memory():
    request("/free", {"unload_models": True, "free_memory": True})


def main():
    output_dir = sys.argv[1]
    wanted = sys.argv[2:]
    sounds = [sound for sound in SOUNDS if not wanted or sound["id"] in wanted]
    started = None if server_is_up() else start_server(output_dir)
    try:
        for sound in sounds:
            begin = time.time()
            run_sound(sound)
            print(f"{sound['id']}: {time.time() - begin:.1f} s", flush=True)
    finally:
        try:
            free_memory()
        except OSError:
            pass
        if started:
            started.terminate()
            started.wait(timeout=30)


if __name__ == "__main__":
    main()
