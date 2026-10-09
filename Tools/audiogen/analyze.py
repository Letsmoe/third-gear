#!/usr/bin/env python3
"""Spectrograms and checks for generated and recorded audio, since nobody can listen to it overnight.

Usage:
  analyze.py spectrogram <out.png> <in.wav> [more.wav ...]    one spectrogram per file, stacked
  analyze.py loop <in.wav>                                     seam check: level and click measure at the loop point
  analyze.py clip <in.wav>                                     peak, clipped samples, RMS
  analyze.py orders <in.wav> <telemetry.csv> <out.png>         engine order tracks over the recorded rpm
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_io import read_wav, rms_db  # noqa: E402


def spectrogram(out_path, paths, max_hz=8000.0):
    figure, axes = plt.subplots(len(paths), 1, figsize=(12, 2.6 * len(paths)), squeeze=False)
    for axis, path in zip(axes[:, 0], paths):
        data, rate = read_wav(path)
        mono = data.mean(axis=1)
        frequencies, times, power = signal.spectrogram(mono, rate, nperseg=4096, noverlap=3072)
        keep = frequencies <= max_hz
        axis.pcolormesh(times, frequencies[keep], 10 * np.log10(power[keep] + 1e-14), vmin=-130, vmax=-50, shading="auto")
        axis.set_ylabel("Hz")
        axis.set_title(f"{os.path.basename(path)}   {len(mono) / rate:.1f} s   RMS {rms_db(data):.1f} dBFS   peak {np.abs(data).max():.2f}", fontsize=9)
    axes[-1, 0].set_xlabel("s")
    figure.tight_layout()
    figure.savefig(out_path, dpi=80)


def loop_check(path):
    """The seam is the last sample joining the first. Compare the jump there with the typical sample-to-sample step."""
    data, rate = read_wav(path)
    mono = data.mean(axis=1)
    jump = abs(mono[0] - mono[-1])
    typical = np.percentile(np.abs(np.diff(mono)), 99.0)
    window = rate // 4
    level_step = rms_db(mono[:window]) - rms_db(mono[-window:])
    print(f"{os.path.basename(path)}: seam jump {jump:.5f} (99th percentile step {typical:.5f}, ratio {jump / max(typical, 1e-9):.1f}), "
          f"level step across the seam {level_step:+.2f} dB")


def clip_check(path):
    data, rate = read_wav(path)
    peak = np.abs(data).max()
    clipped = int(np.sum(np.abs(data) >= 0.999))
    print(f"{os.path.basename(path)}: peak {peak:.3f} ({20 * np.log10(peak + 1e-12):.1f} dBFS), clipped samples {clipped}, "
          f"RMS {rms_db(data):.1f} dBFS, length {len(data) / rate:.1f} s")


def order_tracks(wav_path, csv_path, out_path):
    """Plots the spectrogram with the predicted firing order lines (2, 4, 6) from the recorded rpm."""
    data, rate = read_wav(wav_path)
    mono = data.mean(axis=1)
    telemetry = np.genfromtxt(csv_path, delimiter=",", names=True)
    frequencies, times, power = signal.spectrogram(mono, rate, nperseg=8192, noverlap=6144)
    keep = frequencies <= 1000.0
    figure, axis = plt.subplots(figsize=(12, 5))
    axis.pcolormesh(times, frequencies[keep], 10 * np.log10(power[keep] + 1e-14), vmin=-120, vmax=-50, shading="auto")
    for order, colour in ((2, "white"), (4, "yellow"), (6, "cyan")):
        axis.plot(telemetry["time"], telemetry["rpm"] / 60.0 * order, color=colour, linewidth=0.7, linestyle=":", label=f"order {order}")
    axis.set_ylim(0, 1000)
    axis.set_xlabel("s")
    axis.set_ylabel("Hz")
    axis.legend(loc="upper right")
    figure.tight_layout()
    figure.savefig(out_path, dpi=80)


def main():
    command = sys.argv[1]
    if command == "spectrogram":
        spectrogram(sys.argv[2], sys.argv[3:])
    elif command == "loop":
        for path in sys.argv[2:]:
            loop_check(path)
    elif command == "clip":
        for path in sys.argv[2:]:
            clip_check(path)
    elif command == "orders":
        order_tracks(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
