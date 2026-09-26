# RT2AudioBackend vendoring record (A1)

Pinned upstream: **miniaudio 0.11.25** at commit
`9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`.

Verified 2026-09-23 against the live upstream before vendoring; do not trust
a stale copy:

- Tag/commit identity: `GET
  https://api.github.com/repos/mackron/miniaudio/tags?per_page=5` lists
  `0.11.25` with `commit.sha ==
  9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`.
- Split layout provenance:
  `GET https://api.github.com/repos/mackron/miniaudio/contents/extras/miniaudio_split?ref=0.11.25`
  lists `miniaudio.h` (372,715 bytes) and `miniaudio.c` (3,541,946 bytes).
  The split README states these files are generated from the main
  `miniaudio.h` and are the supported separate-`.h`/`.c` configuration.
- Version self-description: vendored `miniaudio.h:21-24` defines
  `MA_VERSION_MAJOR 0`, `MA_VERSION_MINOR 11`, `MA_VERSION_REVISION 25`;
  vendored `miniaudio.c` opens with `miniaudio - v0.11.25 - 2026-03-04`.

## Exact source URLs (pinned commit)

- `https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/extras/miniaudio_split/miniaudio.h`
- `https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/extras/miniaudio_split/miniaudio.c`
- `https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/LICENSE`

Human-readable references (same bytes): release
`https://github.com/mackron/miniaudio/releases/tag/0.11.25`, license
`https://github.com/mackron/miniaudio/blob/0.11.25/LICENSE`, manual
`https://miniaud.io/docs/manual/index.html`.

## Byte hashes (SHA256, measured after download and re-verified after copy)

- `vendor/miniaudio/miniaudio.h`:
  `01D3AC6049132BDCCC30BD467B7C1D030C090E8F30EB0CEB2DA14BEA0BA7143A`
- `vendor/miniaudio/miniaudio.c`:
  `721EA23C26F13BFB0E5BACD96BB9F40684DE1B3B31456D0DC168A87EEF18978F`
- `vendor/miniaudio/LICENSE`:
  `457F1B500E0ADF6BC059EDDDFA78A2F62012E7C3BB43476C20E0BD23B25BA0EB`

`run_audio_a1_gates.ps1` recomputes these hashes and fails loudly on any
mismatch. Never edit the vendored files; a pin change re-vendors all three
bytes plus this record together.

## Whitespace-gate exception (narrow, A1 review P1)

The pinned upstream files carry trailing whitespace (for example
`miniaudio.c:59` and `miniaudio.h:7525`; an unqualified `git diff --check`
over the A1 range fails only on these lines). Those bytes must not be
normalized: byte identity above is the invariant. `git diff --check`
therefore runs over every RT2-owned path while excluding exactly the three
manifest-pinned files. `run_audio_a1_gates.ps1` derives the exclusion list
from the hash manifest in this record, so the exemption covers precisely
the pinned set and nothing else; the per-file SHA-256 gate is the
complementary check. To vendor a fourth file, extend this manifest,
`.gitattributes` (`eol=lf`), and the gate together — new vendor files never
inherit the exemption silently.

## License

`vendor/miniaudio/LICENSE` is the upstream license file verbatim (byte
identity pinned above). It offers a choice of Public Domain (Unlicense) or
MIT No Attribution; RT2 uses the vendored bytes under either upstream
alternative without modification.

## Build boundary (review finding 7 closure)

The concrete adapter (`src/MiniaudioNoDeviceAdapter.cpp`) and vendored
`vendor/miniaudio/miniaudio.c` live in top-level `RT2AudioBackend`, outside
both `RT2App/src` and `RT2App/vendor`, so the broad RT2App
`src/**.cpp` / `vendor/**.c` globs cannot compile either translation unit a
second time. `RT2App` and `RT2AudioProbe` link the static library;
`RT2Tests` and `RT2SliceRunner` neither compile nor link it. The A1 gate
script proves exactly one adapter TU and one `miniaudio.c` TU in the
generated `RT2AudioBackend` project and none anywhere else.
