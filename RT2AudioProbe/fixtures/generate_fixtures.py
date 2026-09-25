#!/usr/bin/env python3
"""Generate deterministic A4 audio fixtures (no third-party content).

Writes 48 kHz sine fixtures used by RT2AudioProbe's production
decoder/mixer/cache oracles:
  - tone440_mono_f32.wav    1 s mono 440 Hz float32, 0.5 amplitude
  - tone660_stereo_f32.wav  1 s stereo 440 Hz L / 660 Hz R float32, 0.5 amp
  - tone440_mono_s16.wav    1 s mono 440 Hz int16, 0.5 amplitude
  - tone660_stereo_s16.wav  1 s stereo 440 Hz L / 660 Hz R int16, 0.5 amp
  - corrupt_truncated.wav   valid f32 mono header, data cut mid-frame
  - corrupt_magic.bin       256 bytes no decoder accepts

FLAC/MP3 conversions are produced with ffmpeg from the s16 masters
(lossless FLAC stays bit-exact; MP3 is lossy and gets energy/completion
oracles only):
  - tone660_stereo_s16.flac
  - tone660_stereo.mp3

All files are generated; see README.md in this directory.
"""
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RATE = 48000
SECONDS = 1
FRAMES = RATE * SECONDS
AMP = 0.5


def sine(freq: float, n: int) -> float:
    return AMP * math.sin(2.0 * math.pi * freq * n / RATE)


def write_wav(path: Path, channels: int, frames: list, sampwidth: int) -> None:
    assert sampwidth in (2, 4)
    fmt = 1 if sampwidth == 2 else 3  # PCM int16 vs IEEE float32
    if sampwidth == 2:
        raw = b"".join(
            struct.pack("<h", max(-32768, min(32767, int(round(s * 32767)))))
            for frame in frames
            for s in frame
        )
    else:
        raw = b"".join(struct.pack("<f", s) for frame in frames for s in frame)
    data = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(raw),
        b"WAVE",
        b"fmt ",
        16,
        fmt,
        channels,
        RATE,
        RATE * channels * sampwidth,
        channels * sampwidth,
        sampwidth * 8,
        b"data",
        len(raw),
    ) + raw
    path.write_bytes(data)
    print(f"wrote {path.name} ({len(data)} bytes)")


def main() -> int:
    mono_f32 = [[sine(440.0, n)] for n in range(FRAMES)]
    stereo_f32 = [[sine(440.0, n), sine(660.0, n)] for n in range(FRAMES)]
    mono_s16 = [[sine(440.0, n)] for n in range(FRAMES)]
    stereo_s16 = [[sine(440.0, n), sine(660.0, n)] for n in range(FRAMES)]

    write_wav(HERE / "tone440_mono_f32.wav", 1, mono_f32, 4)
    write_wav(HERE / "tone660_stereo_f32.wav", 2, stereo_f32, 4)
    write_wav(HERE / "tone440_mono_s16.wav", 1, mono_s16, 2)
    write_wav(HERE / "tone660_stereo_s16.wav", 2, stereo_s16, 2)

    good = (HERE / "tone440_mono_f32.wav").read_bytes()
    # Mid-data cut: still decodes, with fewer frames (graceful prefix).
    (HERE / "corrupt_truncated.wav").write_bytes(good[: len(good) - 3])
    print("wrote corrupt_truncated.wav")
    # Valid header, zero data frames: must refuse (empty decode).
    (HERE / "corrupt_empty_data.wav").write_bytes(good[:44])
    print("wrote corrupt_empty_data.wav")
    # No decoder magic anywhere (ASCII text: no RIFF/fLaC/MP3 sync): must
    # refuse in every decoder. (0xFF bytes would false-sync as MP3.) No
    # trailing whitespace: the file is binary-pinned (see .gitattributes).
    (HERE / "corrupt_magic.bin").write_bytes(
        (b"THIS IS NOT AUDIO DATA - no RIFF, fLaC, or MP3 sync lives here. " * 4).rstrip(
            b" "
        )
    )
    print("wrote corrupt_magic.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
