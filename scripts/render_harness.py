#!/usr/bin/env python3
"""Manifest-driven deterministic capture and comparison harness for RT2.

The renderer writes linear PFM files so this tool can measure transport before
tone mapping. EXR remains available directly through RT2's --output-hdr flag,
but PFM keeps the automated harness dependency-light and unambiguous.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        "render_harness.py requires numpy and Pillow; install scripts/requirements-render-harness.txt"
    ) from exc


TIMING_RE = re.compile(r"^\[Headless\]\s+(.+?):\s+([0-9.]+)\s+ms$", re.MULTILINE)
TIMING_JSON_RE = re.compile(r"^\[HeadlessTiming\]\s+(\{.*\})$", re.MULTILINE)
DIAGNOSTICS_RE = re.compile(r"^\[HeadlessDiagnostics\]\s+(\{.*\})$", re.MULTILINE)
MEMORY_RE = re.compile(r"^\[HeadlessMemory\]\s+(\{.*\})$", re.MULTILINE)


def merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def resolve_path(value: str | Path, base: Path) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (base / path).resolve()


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        kind = stream.readline().decode("ascii").strip()
        if kind not in ("PF", "Pf"):
            raise ValueError(f"{path}: unsupported PFM header {kind!r}")
        dimensions = stream.readline().decode("ascii").strip()
        while dimensions.startswith("#"):
            dimensions = stream.readline().decode("ascii").strip()
        width, height = (int(value) for value in dimensions.split())
        scale = float(stream.readline().decode("ascii").strip())
        endian = "<" if scale < 0 else ">"
        channels = 3 if kind == "PF" else 1
        data = np.fromfile(stream, dtype=endian + "f4")
    expected = width * height * channels
    if data.size != expected:
        raise ValueError(f"{path}: expected {expected} floats, found {data.size}")
    image = data.reshape((height, width, channels))
    return np.flipud(image).astype(np.float32, copy=False)


def luminance(image: np.ndarray) -> np.ndarray:
    return image[..., :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def finite_image(image: np.ndarray) -> tuple[np.ndarray, float]:
    mask = np.isfinite(image).all(axis=-1)
    finite_ratio = float(mask.mean())
    return np.where(mask[..., None], image, 0.0), finite_ratio


def image_stats(image: np.ndarray) -> dict[str, Any]:
    clean, finite_ratio = finite_image(image)
    lum = luminance(clean)
    return {
        "finite_pixel_ratio": finite_ratio,
        "mean_rgb": [float(value) for value in clean[..., :3].mean(axis=(0, 1))],
        "mean_luminance": float(lum.mean()),
        "luminance_p99_9": float(np.percentile(lum, 99.9)),
        "luminance_p99_99": float(np.percentile(lum, 99.99)),
        "luminance_max": float(lum.max()),
    }


def display_transform(image: np.ndarray) -> np.ndarray:
    linear = np.maximum(image[..., :3], 0.0)
    mapped = linear / (1.0 + linear)
    return np.where(
        mapped <= 0.0031308,
        12.92 * mapped,
        1.055 * np.power(mapped, 1.0 / 2.4) - 0.055,
    )


def compare_images(test: np.ndarray, reference: np.ndarray) -> tuple[dict[str, Any], np.ndarray]:
    if test.shape != reference.shape:
        raise ValueError(f"shape mismatch: test {test.shape}, reference {reference.shape}")
    test_clean, test_finite = finite_image(test)
    ref_clean, ref_finite = finite_image(reference)
    delta = test_clean[..., :3] - ref_clean[..., :3]
    mse = float(np.mean(delta * delta))
    reference_power = float(np.mean(ref_clean[..., :3] ** 2))
    display_mse = float(np.mean((display_transform(test_clean) - display_transform(ref_clean)) ** 2))
    test_stats = image_stats(test)
    ref_stats = image_stats(reference)
    ref_rgb = np.asarray(ref_stats["mean_rgb"], dtype=np.float64)
    test_rgb = np.asarray(test_stats["mean_rgb"], dtype=np.float64)
    rgb_relative = np.divide(test_rgb - ref_rgb, np.maximum(np.abs(ref_rgb), 1e-8))
    metrics = {
        "test_finite_pixel_ratio": test_finite,
        "reference_finite_pixel_ratio": ref_finite,
        "mse_linear_rgb": mse,
        "relative_mse_linear_rgb": mse / max(reference_power, 1e-12),
        "display_psnr_db": float(-10.0 * math.log10(max(display_mse, 1e-12))),
        "mean_rgb_relative_error": [float(value) for value in rgb_relative],
        "mean_luminance_relative_error": (
            test_stats["mean_luminance"] - ref_stats["mean_luminance"]
        ) / max(abs(ref_stats["mean_luminance"]), 1e-8),
        "test": test_stats,
        "reference": ref_stats,
    }
    return metrics, np.abs(delta)


def write_diff(path: Path, difference: np.ndarray) -> None:
    # Robust exposure: 99.9th percentile maps to full scale. Magenta/yellow makes
    # sparse RGB energy errors visible without implying a signed direction.
    magnitude = luminance(difference)
    scale = max(float(np.percentile(magnitude, 99.9)), 1e-8)
    normalized = np.clip(magnitude / scale, 0.0, 1.0)
    heat = np.stack(
        [np.sqrt(normalized), normalized ** 2, np.clip(normalized * 3.0 - 1.0, 0.0, 1.0)],
        axis=-1,
    )
    Image.fromarray(np.uint8(np.round(heat * 255.0)), "RGB").save(path)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def add_flag(command: list[str], enabled: Any, flag: str) -> None:
    if bool(enabled):
        command.append(flag)


def case_command(app: Path, case: dict[str, Any], case_dir: Path) -> tuple[list[str], Path, Path]:
    name = case["name"]
    png = case_dir / f"{name}.png"
    hdr = case_dir / f"{name}.pfm"
    sweep = case.get("sweep", {})
    frames = case.get("frames")
    if frames is None:
        frames = int(sweep.get("warmup", 0)) + int(sweep.get("period", 32)) * int(sweep.get("cycles", 0))
        frames += int(sweep.get("hold_frames", 0))
        frames = max(frames, 1)

    command = [
        str(app), "--headless", "--benchmark-timings",
        "--output", str(png),
        "--output-hdr", str(hdr),
        "--frames", str(frames),
        "--width", str(case.get("width", 640)),
        "--height", str(case.get("height", 360)),
        "--spp", str(case.get("spp", 1)),
        "--bounces", str(case.get("bounces", 8)),
        "--seed", str(case.get("seed", 0)),
    ]
    if case.get("scene"):
        command.extend(["--scene", str(case["scene"])])
    if case.get("env"):
        command.extend(["--env", str(case["env"])])
    camera = case.get("camera", {})
    if "position" in camera:
        command.extend(["--camera-pos", *(str(value) for value in camera["position"])])
    if "forward" in camera:
        command.extend(["--camera-forward", *(str(value) for value in camera["forward"])])
    if float(sweep.get("amplitude", 0.0)) > 0.0:
        command.extend([
            "--camera-sweep", str(sweep["amplitude"]), str(sweep.get("warmup", 0)), str(sweep.get("period", 32)),
            "--camera-sweep-mode", str(sweep.get("mode", "lateral")),
            "--camera-sweep-cycles", str(sweep.get("cycles", 0)),
        ])
        if int(sweep.get("capture_every", 0)) > 0:
            command.extend(["--capture-every", str(sweep["capture_every"])])

    add_flag(command, case.get("raster_first"), "--raster-first")
    add_flag(command, case.get("nrd"), "--nrd")
    add_flag(command, case.get("no_accumulate"), "--no-accumulate")
    add_flag(command, case.get("restir_di"), "--restir")
    add_flag(command, case.get("restir_no_temporal"), "--restir-no-temporal")
    add_flag(command, case.get("restir_no_spatial"), "--restir-no-spatial")
    add_flag(command, case.get("restir_gi"), "--restir-gi")
    if case.get("restir_candidates") is not None:
        command.extend(["--restir-candidates", str(case["restir_candidates"])])
    if case.get("restir_gi_candidates") is not None:
        command.extend(["--restir-gi-candidates", str(case["restir_gi_candidates"])])
    command.extend(str(value) for value in case.get("extra_args", []))
    return command, png, hdr


def temporal_stats(case_dir: Path, name: str) -> dict[str, Any] | None:
    def sequence_key(path: Path) -> int:
        if path.stem.endswith("_still"):
            return -1
        match = re.search(r"_(?:move|hold)_(\d+)$", path.stem)
        return int(match.group(1)) if match else sys.maxsize

    sequence = sorted(case_dir.glob(f"{name}_*.pfm"), key=sequence_key)
    if len(sequence) < 2:
        return None
    deltas = []
    for previous_path, current_path in zip(sequence, sequence[1:]):
        previous = luminance(read_pfm(previous_path))
        current = luminance(read_pfm(current_path))
        difference = np.abs(current - previous)
        deltas.append({
            "from": previous_path.name,
            "to": current_path.name,
            "mean_abs_luminance_delta": float(difference.mean()),
            "p99_9_abs_luminance_delta": float(np.percentile(difference, 99.9)),
            "max_abs_luminance_delta": float(difference.max()),
        })
    return {"capture_count": len(sequence), "transitions": deltas}


def markdown_report(report: dict[str, Any]) -> str:
    lines = ["# RT2 Render Harness Report", "", f"Generated: `{report['generated_at']}`", ""]
    lines += ["| Case | Status | GPU total (ms) | Mean luminance | p99.99 | Relative MSE | Display PSNR |", "|---|---:|---:|---:|---:|---:|---:|"]
    for case in report["cases"]:
        stats = case.get("image_stats", {})
        comparison = case.get("comparison", {})
        timings = case.get("gpu_timings_ms", {})
        lines.append(
            f"| {case['name']} | {case['status']} | {timings.get('GPU Frame', '')} | "
            f"{stats.get('mean_luminance', '')} | {stats.get('luminance_p99_99', '')} | "
            f"{comparison.get('relative_mse_linear_rgb', '')} | {comparison.get('display_psnr_db', '')} |"
        )
    lines += ["", "Full commands, hashes, timing regions, temporal deltas, and comparison metrics are in `report.json`.", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--app", type=Path, help="override the renderer executable")
    parser.add_argument("--output-dir", type=Path, help="override manifest output_dir")
    parser.add_argument("--case", action="append", dest="cases", help="run only a named case (repeatable)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--update-baselines", action="store_true", help="copy captures and report to baseline_dir")
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest_dir = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    app = (args.app or resolve_path(manifest.get("app", "../bin/Release-windows-x86_64/RT2App/RT2App.exe"), manifest_dir)).resolve()
    output_dir = (args.output_dir or resolve_path(manifest.get("output_dir", "../artifacts/render-harness"), manifest_dir)).resolve()
    run_dir = output_dir / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)

    defaults = manifest.get("defaults", {})
    cases = [merge(defaults, case) for case in manifest.get("cases", [])]
    if args.cases:
        wanted = set(args.cases)
        cases = [case for case in cases if case.get("name") in wanted]
        missing = wanted - {case.get("name") for case in cases}
        if missing:
            raise SystemExit(f"unknown case(s): {', '.join(sorted(missing))}")
    if not cases:
        raise SystemExit("manifest selected no cases")

    # Scene and environment paths are relative to the manifest, never the shell cwd.
    for case in cases:
        for field in ("scene", "env"):
            if case.get(field):
                case[field] = str(resolve_path(case[field], manifest_dir))

    results: list[dict[str, Any]] = []
    by_name: dict[str, dict[str, Any]] = {}
    for case in cases:
        name = case["name"]
        case_dir = run_dir / name
        case_dir.mkdir(parents=True, exist_ok=True)
        command, png, hdr = case_command(app, case, case_dir)
        print("[harness]", subprocess.list2cmdline(command), flush=True)
        result: dict[str, Any] = {"name": name, "status": "dry-run" if args.dry_run else "pending", "command": command}
        if not args.dry_run:
            started = time.perf_counter()
            completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, errors="replace")
            elapsed = time.perf_counter() - started
            log = completed.stdout
            (case_dir / "renderer.log").write_text(log, encoding="utf-8")
            timing_records = [json.loads(match.group(1)) for match in TIMING_JSON_RE.finditer(log)]
            diagnostic_records = [json.loads(match.group(1)) for match in DIAGNOSTICS_RE.finditer(log)]
            memory_records = [json.loads(match.group(1)) for match in MEMORY_RE.finditer(log)]
            result.update({
                "status": "passed" if completed.returncode == 0 and hdr.exists() else "failed",
                "return_code": completed.returncode,
                "wall_seconds": elapsed,
                "gpu_timings_ms": {match.group(1): float(match.group(2)) for match in TIMING_RE.finditer(log)},
                "gpu_timing_records": timing_records,
                "gpu_diagnostic_records": diagnostic_records,
                "memory": memory_records[-1] if memory_records else None,
                "png": str(png) if png.exists() else None,
                "hdr": str(hdr) if hdr.exists() else None,
            })
            if hdr.exists():
                image = read_pfm(hdr)
                result["image_stats"] = image_stats(image)
                result["sha256"] = {"hdr": sha256(hdr), "png": sha256(png) if png.exists() else None}
                temporal = temporal_stats(case_dir, name)
                if temporal:
                    result["temporal"] = temporal
            if completed.returncode != 0:
                print(f"[harness] {name} failed; see {case_dir / 'renderer.log'}", file=sys.stderr)
        results.append(result)
        by_name[name] = result

    if not args.dry_run:
        for case in cases:
            name = case["name"]
            reference_name = case.get("reference")
            if not reference_name or not by_name[name].get("hdr"):
                continue
            reference_path: Path | None = None
            if reference_name in by_name and by_name[reference_name].get("hdr"):
                reference_path = Path(by_name[reference_name]["hdr"])
            else:
                candidate = resolve_path(reference_name, manifest_dir)
                if candidate.exists():
                    reference_path = candidate
            if reference_path is None:
                by_name[name]["comparison_error"] = f"reference not found: {reference_name}"
                continue
            metrics, difference = compare_images(read_pfm(Path(by_name[name]["hdr"])), read_pfm(reference_path))
            diff_path = Path(by_name[name]["hdr"]).with_name(f"{name}_diff.png")
            write_diff(diff_path, difference)
            by_name[name]["comparison"] = metrics
            by_name[name]["reference_hdr"] = str(reference_path)
            by_name[name]["difference_png"] = str(diff_path)

    report = {
        "schema_version": 2,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "manifest": str(manifest_path),
        "app": str(app),
        "cases": results,
    }
    (run_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (run_dir / "report.md").write_text(markdown_report(report), encoding="utf-8")

    if args.update_baselines and not args.dry_run:
        baseline_dir = resolve_path(manifest.get("baseline_dir", "../baselines/render-harness"), manifest_dir)
        baseline_dir.mkdir(parents=True, exist_ok=True)
        for case in results:
            if case.get("status") != "passed":
                continue
            destination = baseline_dir / case["name"]
            if destination.exists():
                shutil.rmtree(destination)
            shutil.copytree(Path(case["hdr"]).parent, destination)
        shutil.copy2(run_dir / "report.json", baseline_dir / "report.json")
        shutil.copy2(run_dir / "report.md", baseline_dir / "report.md")
        print(f"[harness] updated baselines: {baseline_dir}")

    print(f"[harness] report: {run_dir / 'report.md'}")
    return 1 if any(case.get("status") == "failed" for case in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
