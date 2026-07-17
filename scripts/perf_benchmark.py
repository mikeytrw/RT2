#!/usr/bin/env python3
"""JSON-driven GPU performance benchmark runner for RT2."""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import platform
import re
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


TIMING_LINE = re.compile(r"^\[HeadlessTiming\]\s+(\{.*\})$", re.MULTILINE)
SCENE_STATS = re.compile(
    r"GPUSceneData built: meshes=(\d+) instances=(\d+) lights=(\d+) textures=(\d+)"
    r"(?: source_emissive=(\d+) filtered_black=(\d+))?"
)
ENV_LOADED = re.compile(r"\[EnvMap\] Loaded (\d+)x(\d+) (?:EXR|HDR)")


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
    return path.resolve() if path.is_absolute() else (base / path).resolve()


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def detect_gpus() -> list[dict[str, str]]:
    try:
        completed = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5,
            errors="replace",
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    if completed.returncode != 0:
        return []
    result = []
    for line in completed.stdout.splitlines():
        values = [value.strip() for value in line.split(",", 1)]
        if len(values) == 2:
            result.append({"name": values[0], "driver": values[1]})
    return result


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    return ordered[low] * (high - position) + ordered[high] * (position - low)


def summarize(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "min": min(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "stddev": statistics.pstdev(values),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def add_flag(command: list[str], enabled: Any, flag: str) -> None:
    if bool(enabled):
        command.append(flag)


def build_command(
    app: Path,
    config: dict[str, Any],
    pose: dict[str, Any],
    output_png: Path,
) -> tuple[list[str], int, int]:
    render = config.get("render", {})
    restir = config.get("restir", {})
    di = restir.get("di", {})
    gi = restir.get("gi", {})
    warmup = int(config.get("warmup_frames", 12))
    samples = int(config.get("sample_frames", 30))
    # Two drain frames let the two-frame timestamp ring expose the complete
    # requested window without synchronizing after every frame.
    total_frames = warmup + samples + 2
    command = [
        str(app), "--headless", "--benchmark-timings", "--verbose",
        "--scene", str(config["model"]),
        "--env", str(config["environment"]),
        "--output", str(output_png),
        "--frames", str(total_frames),
        "--width", str(render.get("width", 930)),
        "--height", str(render.get("height", 730)),
        "--spp", str(render.get("spp", 1)),
        "--bounces", str(render.get("bounces", 8)),
        "--seed", str(config.get("seed", 0)),
    ]
    if "position" in pose:
        command.extend(["--camera-pos", *(str(value) for value in pose["position"])])
    if "forward" in pose:
        command.extend(["--camera-forward", *(str(value) for value in pose["forward"])])
    add_flag(command, render.get("raster_first", True), "--raster-first")
    add_flag(command, render.get("nrd", False), "--nrd")
    add_flag(command, render.get("no_accumulate", True), "--no-accumulate")
    add_flag(command, di.get("enabled", False), "--restir")
    add_flag(command, not di.get("temporal", True), "--restir-no-temporal")
    add_flag(command, not di.get("spatial", True), "--restir-no-spatial")
    add_flag(command, gi.get("enabled", False), "--restir-gi")
    if di.get("candidates") is not None:
        command.extend(["--restir-candidates", str(di["candidates"])])
    if gi.get("candidates") is not None:
        command.extend(["--restir-gi-candidates", str(gi["candidates"])])
    nrd = config.get("nrd", {})
    if nrd.get("accumulation_frames") is not None:
        command.extend(["--nrd-accum-frames", str(nrd["accumulation_frames"])])
    if nrd.get("responsive_roughness") is not None:
        command.extend(["--nrd-responsive-roughness", str(nrd["responsive_roughness"])])
    if nrd.get("responsive_min_frames") is not None:
        command.extend(["--nrd-responsive-min-frames", str(nrd["responsive_min_frames"])])
    command.extend(str(value) for value in config.get("extra_args", []))
    return command, warmup, samples


def derive_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for record in records:
        regions = dict(record["regions_ms"])
        regions["ReSTIR DI Total"] = regions.get("ReSTIR DI Temporal", 0.0) + regions.get("ReSTIR DI Spatial", 0.0)
        regions["ReSTIR GI Total"] = regions.get("ReSTIR GI Temporal", 0.0) + regions.get("ReSTIR GI History", 0.0)
        gpu_frame = regions.get("GPU Frame", 0.0)
        if gpu_frame > 0.0:
            regions["ReSTIR DI Percent"] = 100.0 * regions["ReSTIR DI Total"] / gpu_frame
            regions["ReSTIR GI Percent"] = 100.0 * regions["ReSTIR GI Total"] / gpu_frame
        result.append({"frame": record["frame"], "regions_ms": regions})
    return result


def aggregate_records(records: list[dict[str, Any]], width: int, height: int) -> dict[str, Any]:
    region_names = sorted({name for record in records for name in record["regions_ms"]})
    aggregates = {
        name: summarize([float(record["regions_ms"][name]) for record in records if name in record["regions_ms"]])
        for name in region_names
    }
    megapixels = width * height / 1_000_000.0
    for name in ("ReSTIR DI Temporal", "ReSTIR DI Spatial", "ReSTIR DI Total", "ReSTIR GI Total"):
        if name in aggregates and aggregates[name].get("count"):
            aggregates[name]["median_ms_per_megapixel"] = float(aggregates[name]["median"]) / megapixels
    return aggregates


def markdown_report(report: dict[str, Any]) -> str:
    lines = ["# RT2 Performance Benchmark", "", f"Generated: `{report['generated_at']}`", ""]
    lines += [
        "| Test | Pose | Status | GPU median | DI temporal | DI total | DI % | DI p95 |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for run in report["runs"]:
        aggregate = run.get("aggregates", {})
        def value(name: str, field: str = "median") -> Any:
            return aggregate.get(name, {}).get(field, "")
        lines.append(
            f"| {run['benchmark']} | {run['pose']} | {run['status']} | "
            f"{value('GPU Frame')} | {value('ReSTIR DI Temporal')} | "
            f"{value('ReSTIR DI Total')} | {value('ReSTIR DI Percent')} | "
            f"{value('ReSTIR DI Total', 'p95')} |"
        )
    lines += ["", "Raw samples and all aggregate regions are in `results.json`.", ""]
    comparisons = [run for run in report["runs"] if run.get("comparison")]
    if comparisons:
        lines += [
            "## Baseline comparison",
            "",
            "| Test | Pose | GPU delta | DI temporal delta | DI total delta | Triangle lights |",
            "|---|---|---:|---:|---:|---:|",
        ]
        for run in comparisons:
            comparison = run["comparison"]
            lines.append(
                f"| {run['benchmark']} | {run['pose']} | "
                f"{comparison.get('GPU Frame', {}).get('delta_percent', '')}% | "
                f"{comparison.get('ReSTIR DI Temporal', {}).get('delta_percent', '')}% | "
                f"{comparison.get('ReSTIR DI Total', {}).get('delta_percent', '')}% | "
                f"{comparison.get('triangle_lights', {}).get('current', '')} |"
            )
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--app", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--label", default="run")
    parser.add_argument("--benchmark", action="append", dest="benchmarks")
    parser.add_argument("--baseline", type=Path, help="prior results.json to compare against")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest_dir = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    app = (args.app or resolve_path(manifest.get("app", "../bin/Release-windows-x86_64/RT2App/RT2App.exe"), manifest_dir)).resolve()
    output_root = (args.output_dir or resolve_path(manifest.get("output_dir", "../artifacts/perf-benchmarks"), manifest_dir)).resolve()
    run_dir = output_root / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.label}"
    run_dir.mkdir(parents=True, exist_ok=False)
    if not app.exists():
        raise SystemExit(f"renderer not found: {app}")

    defaults = manifest.get("defaults", {})
    benchmarks = [merge(defaults, item) for item in manifest.get("benchmarks", [])]
    if args.benchmarks:
        wanted = set(args.benchmarks)
        benchmarks = [item for item in benchmarks if item.get("name") in wanted]
        missing = wanted - {item.get("name") for item in benchmarks}
        if missing:
            raise SystemExit(f"unknown benchmark(s): {', '.join(sorted(missing))}")
    if not benchmarks:
        raise SystemExit("manifest selected no benchmarks")

    runs: list[dict[str, Any]] = []
    failed = False
    for benchmark in benchmarks:
        name = benchmark["name"]
        if not benchmark.get("model"):
            raise SystemExit(f"{name}: model is required")
        if not benchmark.get("environment"):
            raise SystemExit(f"{name}: environment is required for performance benchmarks")
        benchmark["model"] = str(resolve_path(benchmark["model"], manifest_dir))
        benchmark["environment"] = str(resolve_path(benchmark["environment"], manifest_dir))
        for field in ("model", "environment"):
            if not Path(benchmark[field]).exists():
                raise SystemExit(f"{name}: {field} not found: {benchmark[field]}")
        poses = benchmark.get("camera_poses", [])
        if not poses:
            raise SystemExit(f"{name}: camera_poses must contain at least one pose")
        repetitions = int(benchmark.get("repetitions", 1))
        for pose_index, pose in enumerate(poses):
            pose_name = pose.get("name", f"pose_{pose_index}")
            for repetition in range(repetitions):
                run_name = f"{name}__{pose_name}__r{repetition + 1}"
                case_dir = run_dir / run_name
                case_dir.mkdir(parents=True)
                output_png = case_dir / "capture.png"
                command, warmup, sample_count = build_command(app, benchmark, pose, output_png)
                print("[perf]", subprocess.list2cmdline(command), flush=True)
                run: dict[str, Any] = {
                    "benchmark": name,
                    "pose": pose_name,
                    "repetition": repetition + 1,
                    "model": benchmark["model"],
                    "environment": benchmark["environment"],
                    "settings": benchmark,
                    "command": command,
                    "status": "dry-run" if args.dry_run else "pending",
                }
                if not args.dry_run:
                    started = time.perf_counter()
                    completed = subprocess.run(
                        command,
                        cwd=app.parent,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        errors="replace",
                    )
                    log = completed.stdout
                    (case_dir / "renderer.log").write_text(log, encoding="utf-8")
                    parsed = [json.loads(match.group(1)) for match in TIMING_LINE.finditer(log)]
                    parsed.sort(key=lambda item: int(item["frame"]))
                    measured = derive_records(parsed[warmup:warmup + sample_count])
                    render = benchmark.get("render", {})
                    width = int(render.get("width", 930))
                    height = int(render.get("height", 730))
                    scene_match = SCENE_STATS.search(log)
                    environment_match = ENV_LOADED.search(log)
                    run.update({
                        "status": "passed" if completed.returncode == 0 and len(measured) == sample_count and environment_match else "failed",
                        "return_code": completed.returncode,
                        "wall_seconds": time.perf_counter() - started,
                        "timing_records_found": len(parsed),
                        "raw_samples": measured,
                        "aggregates": aggregate_records(measured, width, height),
                        "capture": str(output_png) if output_png.exists() else None,
                    })
                    if environment_match:
                        run["environment_loaded"] = {
                            "width": int(environment_match.group(1)),
                            "height": int(environment_match.group(2)),
                        }
                    if scene_match:
                        run["scene_stats"] = {
                            "meshes": int(scene_match.group(1)),
                            "instances": int(scene_match.group(2)),
                            "triangle_lights": int(scene_match.group(3)),
                            "textures": int(scene_match.group(4)),
                        }
                        if scene_match.group(5) is not None:
                            run["scene_stats"]["source_emissive_triangles"] = int(scene_match.group(5))
                            run["scene_stats"]["filtered_black_emissive_triangles"] = int(scene_match.group(6))
                    if run["status"] == "failed":
                        failed = True
                        print(
                            f"[perf] {run_name} failed: return={completed.returncode}, samples={len(measured)}/{sample_count}",
                            file=sys.stderr,
                        )
                runs.append(run)

    baseline_path = args.baseline.resolve() if args.baseline else None
    if baseline_path:
        baseline_report = json.loads(baseline_path.read_text(encoding="utf-8"))
        baseline_runs = {
            (run["benchmark"], run["pose"], run["repetition"]): run
            for run in baseline_report.get("runs", [])
        }
        for run in runs:
            baseline = baseline_runs.get((run["benchmark"], run["pose"], run["repetition"]))
            if not baseline or run.get("status") != "passed" or baseline.get("status") != "passed":
                continue
            comparison: dict[str, Any] = {}
            for region in ("GPU Frame", "ReSTIR DI Temporal", "ReSTIR DI Spatial", "ReSTIR DI Total", "ReSTIR DI Percent", "ReSTIR GI Total", "RT Shading"):
                before = baseline.get("aggregates", {}).get(region, {}).get("median")
                after = run.get("aggregates", {}).get(region, {}).get("median")
                if before is None or after is None:
                    continue
                comparison[region] = {
                    "baseline": before,
                    "current": after,
                    "delta": after - before,
                    "delta_percent": 100.0 * (after - before) / before if before != 0 else None,
                }
            before_lights = baseline.get("scene_stats", {}).get("triangle_lights")
            after_lights = run.get("scene_stats", {}).get("triangle_lights")
            if before_lights is not None and after_lights is not None:
                comparison["triangle_lights"] = {
                    "baseline": before_lights,
                    "current": after_lights,
                    "delta": after_lights - before_lights,
                    "delta_percent": 100.0 * (after_lights - before_lights) / before_lights if before_lights else 0.0,
                }
            run["comparison"] = comparison

    report = {
        "schema_version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "manifest": str(manifest_path),
        "label": args.label,
        "baseline": str(baseline_path) if baseline_path else None,
        "system": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "python": sys.version,
            "processor": platform.processor(),
            "gpus": detect_gpus(),
        },
        "app": {"path": str(app), "sha256": file_hash(app)},
        "runs": runs,
    }
    (run_dir / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (run_dir / "report.md").write_text(markdown_report(report), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["benchmark", "pose", "repetition", "status", "gpu_median_ms", "di_temporal_median_ms", "di_total_median_ms", "di_percent_median", "di_total_p95_ms", "triangle_lights"])
        for run in runs:
            aggregate = run.get("aggregates", {})
            def median(name: str) -> Any:
                return aggregate.get(name, {}).get("median", "")
            writer.writerow([run["benchmark"], run["pose"], run["repetition"], run["status"], median("GPU Frame"), median("ReSTIR DI Temporal"), median("ReSTIR DI Total"), median("ReSTIR DI Percent"), aggregate.get("ReSTIR DI Total", {}).get("p95", ""), run.get("scene_stats", {}).get("triangle_lights", "")])
    print(f"[perf] results: {run_dir / 'results.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
