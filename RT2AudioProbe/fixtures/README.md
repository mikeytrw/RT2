# RT2AudioProbe A4 fixtures

All fixtures are **generated** by `generate_fixtures.py` (plus the two
ffmpeg conversions below). No third-party or licensed content: 1 second
of 48 kHz sine at 0.5 amplitude, 440 Hz mono / 440 Hz left + 660 Hz
right stereo.

| File | Content | Oracle |
| --- | --- | --- |
| `tone440_mono_f32.wav` | mono float32 | bit-exact decode + spatial-voice source |
| `tone660_stereo_f32.wav` | stereo float32 | bit-exact decode, non-spatial voice |
| `tone440_mono_s16.wav` | mono int16 | exact decode (s16 scales exactly to f32) |
| `tone660_stereo_s16.wav` | stereo int16 | FLAC/MP3 conversion master |
| `tone660_stereo_s16.flac` | converted from the s16 stereo master | exact decode (lossless) |
| `tone660_stereo_48k.mp3` | converted from the s16 stereo master | energy/completion decode (lossy) |
| `corrupt_truncated.wav` | valid header, data cut mid-frame (header over-advertises) | typed decode refusal (length mismatch) |
| `corrupt_empty_data.wav` | valid header, zero data frames | typed decode refusal (empty decode) |
| `corrupt_magic.bin` | ASCII text, no decoder magic | typed decode refusal in every decoder |

Conversions (run once, outputs checked in):

```powershell
ffmpeg -y -v error -i tone660_stereo_s16.wav -c:a flac tone660_stereo_s16.flac
ffmpeg -y -v error -i tone660_stereo_s16.wav -c:a libmp3lame -b:a 128k -ar 48000 tone660_stereo_48k.mp3
```

To regenerate the WAV/corrupt fixtures: `python RT2AudioProbe/fixtures/generate_fixtures.py`
from the repository root.
