#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify FFT extraction preserves NS and exercise the deployed feature ABI."""

import argparse
import ctypes
import hashlib
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/voicefe_intent"))
from extract_features import Features  # noqa: E402 - workspace tool import


class NoiseState(ctypes.Structure):
    _fields_ = [
        ("real", ctypes.c_int32 * 512),
        ("imag", ctypes.c_int32 * 512),
        ("power", ctypes.c_uint32 * 257),
        ("noise", ctypes.c_uint32 * 257),
        ("minimum", ctypes.c_uint32 * 257),
        ("gain", ctypes.c_uint16 * 257),
        ("previous", ctypes.c_int16 * 160),
        ("overlap", ctypes.c_int32 * 160),
        ("blocks", ctypes.c_uint32),
        ("clipped", ctypes.c_uint32),
        ("gain_q15", ctypes.c_uint32),
        ("floor", ctypes.c_uint16),
    ]


def ns_library(sources, dest, build):
    subprocess.run(
        [
            "cc",
            "-O2",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-dynamiclib",
            f"-I{build}",
            f"-I{ROOT / 'apps/include'}",
            *map(str, sources),
            "-o",
            str(dest),
        ],
        check=True,
    )
    lib = ctypes.CDLL(str(dest))
    lib.voicefe_ns_init.argtypes = [ctypes.c_void_p, ctypes.c_uint16]
    lib.voicefe_ns_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline",
        type=Path,
        required=True,
        help="Original NS source from the authorized baseline backup",
    )
    args = parser.parse_args()
    features = Features()
    silence = features.extract(np.zeros(64000, np.int16))
    assert silence.shape == (199, 40)
    assert np.all(silence == -128)
    tone = (10000 * np.sin(2 * np.pi * 1000 * np.arange(64000) / 16000)).astype(
        np.int16
    )
    assert np.argmax(features.extract(tone).mean(0)) == 12
    impulse = np.zeros(64000, np.int16)
    impulse[400] = 32767
    assert features.extract(impulse).max() > -128
    for samples, rows in [(32000, 99), (64000, 199)]:
        assert features.extract(np.zeros(samples, np.int16)).shape == (rows, 40)
    build = Path("/tmp/voicefe-intent-features")
    source = ROOT / "apps/audioutils/titan_voicefe"
    old = ns_library([args.baseline], build / "ns-baseline.dylib", build)
    new = ns_library(
        [source / "voicefe_ns.c", source / "voicefe_fft.c"],
        build / "ns-shared.dylib",
        build,
    )
    rng = np.random.default_rng(20261012)
    digest = hashlib.sha256()
    for floor in [4096, 8192, 16384]:
        before, after = NoiseState(), NoiseState()
        old.voicefe_ns_init(ctypes.byref(before), floor)
        new.voicefe_ns_init(ctypes.byref(after), floor)
        for block in range(3000):
            if block < 50:
                pcm = rng.integers(-100, 100, 160, dtype=np.int16)
            elif block % 3 == 0:
                pcm = rng.integers(-32768, 32768, 160, dtype=np.int16)
            else:
                phase = np.arange(160) + block * 160
                pcm = (9000 * np.sin(2 * np.pi * 443 * phase / 16000)).astype(np.int16)
            reference = pcm.copy()
            old.voicefe_ns_process(ctypes.byref(before), reference.ctypes.data)
            new.voicefe_ns_process(ctypes.byref(after), pcm.ctypes.data)
            assert np.array_equal(reference, pcm), (floor, block)
            assert bytes(before) == bytes(after), (floor, block, "state")
            digest.update(pcm.tobytes())
    print(
        f"PASS NS: 9000 blocks / 1,440,000 samples byte-identical; "
        f"SHA256 {digest.hexdigest()}"
    )
    print(
        f"PASS features: silence, impulse, 1 kHz Mel band, 99/199 rows; "
        f"state {len(features.state)} bytes"
    )


if __name__ == "__main__":
    main()
