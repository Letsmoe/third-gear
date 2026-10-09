#!/usr/bin/env python3
"""Spectrograms and checks for generated and recorded audio, since nobody can listen to it overnight.

Usage:
  analyze.py spectrogram <out.png> <in.wav> [more.wav ...]    one spectrogram per file, stacked
  analyze.py loop <in.wav>                                     seam check: level and click measure at the loop point
  analyze.py clip <in.wav>                                     peak, clipped samples, RMS
  analyze.py orders <in.wav> <telemetry.csv> <out.png>         engine order tracks over the recorded rpm
  analyze.py scenes <capture_dir>                              level per scene and stem, from an Unreal -AudioCapture run
  analyze.py scenegram <capture_dir> <out.png> <stem> <label> [label ...]   spectrogram of scenes of one stem
  analyze.py clicks <in.wav> [more.wav ...]                    count sudden sample-to-sample jumps far above the local level
  analyze.py ordercheck <capture_dir> <label>                  do the firing order and its harmonics follow the rpm
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


def load_capture(capture_dir):
    """Returns the telemetry rows as a structured array and the labels per row."""
    telemetry = np.genfromtxt(os.path.join(capture_dir, "telemetry.csv"), delimiter=",", names=True, dtype=None, encoding=None)
    return telemetry


def scene_levels(capture_dir):
    """Prints RMS and peak per scene for each stem, with mean rpm and speed."""
    telemetry = load_capture(capture_dir)
    stems = {}
    for name in ("car", "car_engine", "car_road", "car_wind", "car_events"):
        path = os.path.join(capture_dir, name + ".wav")
        if os.path.exists(path):
            stems[name] = read_wav(path)
    rate = next(iter(stems.values()))[1]
    print(f"{'scene':22s} {'sec':>5s} {'rpm':>6s} {'km/h':>6s}  " + "  ".join(f"{name[4:] or 'mix':>7s}" for name in stems) + "   mix peak")
    for label in dict.fromkeys(telemetry["label"]):
        rows = np.nonzero(telemetry["label"] == label)[0]
        start, stop = int(telemetry["time"][rows[0]] * rate), int(telemetry["time"][rows[-1]] * rate)
        skip = min(int(0.5 * rate), (stop - start) // 3)
        levels = []
        for name, (data, _) in stems.items():
            levels.append(f"{rms_db(data[start + skip:stop]):7.1f}")
        mix = stems["car"][0][start:stop]
        print(f"{label:22s} {(stop - start) / rate:5.1f} {telemetry['rpm'][rows].mean():6.0f} {telemetry['speed_kmh'][rows].mean():6.1f}  "
              + "  ".join(levels) + f"   {np.abs(mix).max():.2f}")


def order_check(capture_dir, label):
    """For 0.4 s windows of the engine stem, compares the level at the predicted firing frequency (order 2) and its
    harmonics with the median level of the surrounding spectrum. A working engine is clearly above it, at every rpm."""
    telemetry = load_capture(capture_dir)
    data, rate = read_wav(os.path.join(capture_dir, "car_engine.wav"))
    mono = data.mean(axis=1)
    rows = np.nonzero(telemetry["label"] == label)[0]
    window = int(0.4 * rate)
    print(f"{label}: window start s, rpm, firing Hz, level above the local noise floor in dB (order 2 / 4 / 6), strongest peak Hz")
    for row in rows[:: int(0.5 / (telemetry["time"][1] - telemetry["time"][0]))]:
        start = int(telemetry["time"][row] * rate)
        segment = mono[start:start + window]
        rpm = telemetry["rpm"][row]
        if len(segment) < window or rpm < 300:
            continue
        spectrum = np.abs(np.fft.rfft(segment * np.hanning(len(segment)))) ** 2
        frequencies = np.fft.rfftfreq(len(segment), 1.0 / rate)
        floor = np.median(spectrum[(frequencies > 20) & (frequencies < 3000)])
        above = []
        for order in (2, 4, 6):
            target = rpm / 60.0 * order
            band = (frequencies > target * 0.97) & (frequencies < target * 1.03)
            above.append(10 * np.log10(spectrum[band].max() / floor + 1e-12) if band.any() else float("nan"))
        peak = frequencies[(frequencies > 25) & (frequencies < 1500)][np.argmax(spectrum[(frequencies > 25) & (frequencies < 1500)])]
        print(f"  {telemetry['time'][row]:6.1f}s {rpm:6.0f} rpm  {rpm / 30:6.1f} Hz   {above[0]:6.1f} {above[1]:6.1f} {above[2]:6.1f}   peak {peak:6.1f} Hz")


def scene_spectrograms(capture_dir, out_path, stem, labels, max_hz=6000.0):
    """One spectrogram panel per scene of a stem (car, car_engine, car_road, car_wind, car_events)."""
    telemetry = load_capture(capture_dir)
    data, rate = read_wav(os.path.join(capture_dir, stem + ".wav"))
    mono = data.mean(axis=1)
    figure, axes = plt.subplots(len(labels), 1, figsize=(12, 2.6 * len(labels)), squeeze=False)
    for axis, label in zip(axes[:, 0], labels):
        rows = np.nonzero(telemetry["label"] == label)[0]
        start, stop = int(telemetry["time"][rows[0]] * rate), int(telemetry["time"][rows[-1]] * rate)
        frequencies, times, power = signal.spectrogram(mono[start:stop], rate, nperseg=4096, noverlap=3584)
        keep = frequencies <= max_hz
        axis.pcolormesh(times, frequencies[keep], 10 * np.log10(power[keep] + 1e-14), vmin=-130, vmax=-50, shading="auto")
        axis.set_title(f"{stem}  {label}   RMS {rms_db(mono[start:stop]):.1f} dBFS", fontsize=9)
        axis.set_ylabel("Hz")
    figure.tight_layout()
    figure.savefig(out_path, dpi=80)


def click_check(path, ratio=14.0):
    """A click is a sample-to-sample step many times larger than the typical step around it. Reports how many there are."""
    data, rate = read_wav(path)
    mono = data.mean(axis=1)
    steps = np.abs(np.diff(mono))
    window = int(0.02 * rate)
    local = np.sqrt(np.convolve(steps ** 2, np.ones(window) / window, mode="same")) + 1e-6
    # Ignore digital silence, where the local level is meaningless.
    flagged = np.nonzero((steps > ratio * local) & (steps > 0.004))[0]
    times = ", ".join(f"{index / rate:.2f}s" for index in flagged[:8])
    print(f"{os.path.basename(path)}: {len(flagged)} jumps above {ratio:.0f}x the local step" + (f" at {times}" if len(flagged) else ""))


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
    elif command == "clicks":
        for path in sys.argv[2:]:
            click_check(path)
    elif command == "scenegram":
        scene_spectrograms(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    elif command == "scenes":
        scene_levels(sys.argv[2])
    elif command == "ordercheck":
        order_check(sys.argv[2], sys.argv[3])
    elif command == "orders":
        order_tracks(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
