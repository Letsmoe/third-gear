#!/usr/bin/env python3
"""Turns raw Stable Audio takes into game-ready WAVs.

Loops (wind, town, birds, rain): the model makes one-shot clips whose head does not meet their tail, and they often
fade in over the first seconds. The head level is matched to the tail with a slow gain ramp, then the last seconds are
folded back over the first with an equal-power crossfade, so the last sample continues into the first. Equal power,
not linear, because the two sides are uncorrelated noise and linear curves dip by 3 dB in the middle.

One-shots (thunder): trimmed where the tail has decayed, faded in and out, peak normalised.

Usage: postprocess.py <audio_root> [sound_id ...]   (reads <audio_root>/raw, writes loops/ and oneshots/)
"""
import glob
import os
import sys

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_io import SAMPLE_RATE, decode, rms_db, write_wav  # noqa: E402
from prompts import SOUNDS  # noqa: E402

LOOP_TARGET_RMS_DB = -26.0
CROSSFADE_SECONDS = 4.0
TRIM_HEAD_SECONDS = 1.5
TRIM_TAIL_SECONDS = 0.5


def highpass_lowpass(samples, highpass_hz, lowpass_hz):
    """Removes the model's constant sub-bass bed and the artefact band near Nyquist."""
    sos_high = signal.butter(2, highpass_hz, "highpass", fs=SAMPLE_RATE, output="sos")
    sos_low = signal.butter(4, lowpass_hz, "lowpass", fs=SAMPLE_RATE, output="sos")
    return signal.sosfilt(sos_low, signal.sosfilt(sos_high, samples, axis=0), axis=0)


def match_head_to_tail(body, window):
    """Ramps gain in dB across the body so the first and last `window` samples have the same level."""
    head = np.sqrt(np.mean(body[:window] ** 2)) + 1e-9
    tail = np.sqrt(np.mean(body[-window:] ** 2)) + 1e-9
    ramp = np.exp(np.linspace(np.log(tail / head), 0.0, len(body)))
    return body * ramp[:, None]


def crossfade_loop(body, fade_samples):
    """Folds the last fade_samples back over the first fade_samples with equal-power curves."""
    looped = body[fade_samples:].copy()
    angle = np.linspace(0.0, np.pi / 2.0, fade_samples)[:, None]
    looped[-fade_samples:] = looped[-fade_samples:] * np.cos(angle) + body[:fade_samples] * np.sin(angle)
    return looped


def make_loop(raw_path, out_path):
    """Writes a seamless loop and returns the level step across the loop point in dB."""
    samples = decode(raw_path)
    body = samples[int(TRIM_HEAD_SECONDS * SAMPLE_RATE):len(samples) - int(TRIM_TAIL_SECONDS * SAMPLE_RATE)]
    fade_samples = int(CROSSFADE_SECONDS * SAMPLE_RATE)
    # Filter before folding: filtering the finished loop would start the filters from rest at the seam.
    body = highpass_lowpass(body, 40.0, 16000.0)
    body = match_head_to_tail(body, min(len(body) // 4, 8 * SAMPLE_RATE))
    looped = crossfade_loop(body, fade_samples)
    looped *= 10.0 ** ((LOOP_TARGET_RMS_DB - rms_db(looped)) / 20.0)
    write_wav(out_path, looped)
    window = SAMPLE_RATE // 2
    return rms_db(looped[:window]) - rms_db(looped[-window:])


def make_oneshot(raw_path, out_path):
    """Writes a trimmed, faded, peak normalised one-shot and returns its length in seconds."""
    samples = highpass_lowpass(decode(raw_path), 25.0, 16000.0)
    envelope = np.sqrt(signal.sosfilt(signal.butter(2, 4.0, "lowpass", fs=SAMPLE_RATE, output="sos"),
                                      np.mean(samples ** 2, axis=1)).clip(0.0))
    audible = np.nonzero(envelope > envelope.max() * 0.01)[0]
    end = min(len(samples), int(audible[-1]) + SAMPLE_RATE // 2) if len(audible) else len(samples)
    clip = samples[:end]
    fade_out = min(len(clip) // 3, 2 * SAMPLE_RATE)
    clip[-fade_out:] *= np.linspace(1.0, 0.0, fade_out)[:, None] ** 2
    fade_in = int(0.005 * SAMPLE_RATE)
    clip[:fade_in] *= np.linspace(0.0, 1.0, fade_in)[:, None]
    clip *= 10.0 ** (-1.0 / 20.0) / (np.abs(clip).max() + 1e-9)
    write_wav(out_path, clip)
    return len(clip) / SAMPLE_RATE


def main():
    audio_root = sys.argv[1]
    wanted = sys.argv[2:]
    os.makedirs(os.path.join(audio_root, "loops"), exist_ok=True)
    os.makedirs(os.path.join(audio_root, "oneshots"), exist_ok=True)
    for sound in SOUNDS:
        if wanted and sound["id"] not in wanted:
            continue
        candidates = sorted(glob.glob(os.path.join(audio_root, "raw", sound["id"] + "_*.flac")))
        if not candidates:
            print(f"{sound['id']}: no raw take, skipped")
            continue
        if sound["kind"] == "loop":
            step_db = make_loop(candidates[-1], os.path.join(audio_root, "loops", sound["id"] + ".wav"))
            print(f"{sound['id']}: loop, level step across the loop point {step_db:+.2f} dB")
        else:
            seconds = make_oneshot(candidates[-1], os.path.join(audio_root, "oneshots", sound["id"] + ".wav"))
            print(f"{sound['id']}: one-shot, {seconds:.1f} s")


if __name__ == "__main__":
    main()
