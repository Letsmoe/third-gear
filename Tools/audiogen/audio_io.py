"""Shared audio helpers for the generation tools: decoding, WAV writing and level measurements."""
import subprocess

import numpy as np
from scipy.io import wavfile

SAMPLE_RATE = 48000  # Unreal's mixer rate; the model outputs 44.1 kHz and ffmpeg resamples.


def decode(path, channels=2):
    """Decodes any audio file to a float array of shape (samples, channels) at SAMPLE_RATE."""
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", str(channels), "-ar", str(SAMPLE_RATE), "-"],
        capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="<f4").reshape(-1, channels).astype(np.float64)


def write_wav(path, samples):
    """Writes float samples (clipped to +-1) as 16 bit PCM."""
    clipped = np.clip(samples, -1.0, 1.0)
    wavfile.write(path, SAMPLE_RATE, (clipped * 32767.0).astype(np.int16))


def read_wav(path):
    """Reads a WAV file to float samples of shape (samples, channels) and its sample rate."""
    rate, data = wavfile.read(path)
    if data.dtype == np.int16:
        data = data / 32768.0
    elif data.dtype == np.int32:
        data = data / 2147483648.0
    data = data.astype(np.float64)
    if data.ndim == 1:
        data = data[:, None]
    return data, rate


def rms_db(samples):
    """RMS level in dBFS."""
    return 20.0 * np.log10(np.sqrt(np.mean(samples ** 2)) + 1e-12)
