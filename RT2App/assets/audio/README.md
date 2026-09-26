# Audio acceptance clips (A8)

Both clips are **generated**, not licensed: byte-identical copies of two
`RT2AudioProbe/fixtures` outputs from
`RT2AudioProbe/fixtures/generate_fixtures.py` (1 second of 48 kHz sine at
0.5 amplitude). No third-party content, no licensing manifest needed.

| File | Content | Acceptance role |
| --- | --- | --- |
| `acceptance_loop_mono_s16.wav` | mono int16, 440 Hz | looping spatial Effects emitter (`LoopEmitter`) |
| `acceptance_music_stereo_s16.wav` | stereo int16, 440 Hz L / 660 Hz R | non-spatial Music source (`MusicBed`) |

Decode behavior for these exact bytes is proven by the RT2AudioProbe A4
oracles (bit-exact s16 decode, completion, corrupt refusal); the
acceptance scene exercises the authored-to-audible path on top.

Each clip carries a `.rt2meta` sidecar holding its asset UUID, matching
the sidecar-driven project identity used by every other source asset.
The scene `../audio-acceptance.rt2scene` (schema v9) stores the same
UUIDs in its `audioSource.clip.assetId` fields.
