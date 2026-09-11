#!/usr/bin/env python3
"""Copy full embedded PBR materials into a node-preserving geometry GLB.

`obj2gltf` preserves the connected-component nodes produced by the companion
splitter, but its MTL path drops the donor package's metallic/roughness,
normal, and occlusion textures.  This repacker keeps the split geometry and
copies the material/image payloads from the already corrected RT2 GLB.

Selected nodes may receive a duplicated emissive material.  That is an
authoring proof, not automatic semantic recognition: callers must explicitly
name or regex-match the visual parts they intend to glow.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import struct
from pathlib import Path


JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"{path} is too small to be a GLB")
    magic, version, total_length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2 or total_length != len(data):
        raise ValueError(f"{path} has an invalid GLB header")

    offset = 12
    document = None
    binary = b""
    while offset < len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        payload = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == JSON_CHUNK:
            document = json.loads(payload.decode("utf-8").rstrip(" \t\r\n\0"))
        elif chunk_type == BIN_CHUNK:
            binary = payload
    if document is None:
        raise ValueError(f"{path} has no JSON chunk")
    return document, binary


def pad4(data: bytes, fill: bytes) -> bytes:
    return data + fill * ((-len(data)) % 4)


def write_glb(path: Path, document: dict, binary: bytes) -> None:
    binary = pad4(binary, b"\0")
    document["buffers"] = [{"byteLength": len(binary)}]
    json_bytes = pad4(json.dumps(document, separators=(",", ":")).encode("utf-8"), b" ")
    total_length = 12 + 8 + len(json_bytes) + 8 + len(binary)
    header = struct.pack("<III", 0x46546C67, 2, total_length)
    payload = (
        header
        + struct.pack("<II", len(json_bytes), JSON_CHUNK)
        + json_bytes
        + struct.pack("<II", len(binary), BIN_CHUNK)
        + binary
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--geometry", required=True, type=Path)
    parser.add_argument("--material-donor", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--emissive-node-regex", action="append", default=[])
    parser.add_argument("--emissive-strength", type=float, default=1.0)
    args = parser.parse_args()
    if args.emissive_strength < 0:
        raise ValueError("emissive strength cannot be negative")

    target, target_binary = read_glb(args.geometry)
    donor, donor_binary = read_glb(args.material_donor)
    if len(target.get("buffers", [])) != 1 or len(donor.get("buffers", [])) != 1:
        raise ValueError("the spike supports single-buffer GLBs only")

    donor_materials = copy.deepcopy(donor.get("materials", []))
    donor_by_name = {material.get("name", ""): index for index, material in enumerate(donor_materials)}
    target_materials = target.get("materials", [])
    target_names = [material.get("name", "") for material in target_materials]
    missing = sorted({name for name in target_names if name not in donor_by_name})
    if missing:
        raise ValueError(f"material donor is missing target materials: {missing}")

    for mesh in target.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            old_index = primitive.get("material")
            if old_index is not None:
                primitive["material"] = donor_by_name[target_names[old_index]]

    new_binary = bytearray(target_binary)
    target_views = target.setdefault("bufferViews", [])
    donor_views = donor.get("bufferViews", [])
    donor_images = copy.deepcopy(donor.get("images", []))
    for image in donor_images:
        donor_view = donor_views[image["bufferView"]]
        start = donor_view.get("byteOffset", 0)
        end = start + donor_view["byteLength"]
        while len(new_binary) % 4:
            new_binary.append(0)
        replacement = {
            "buffer": 0,
            "byteOffset": len(new_binary),
            "byteLength": donor_view["byteLength"],
        }
        target_views.append(replacement)
        image["bufferView"] = len(target_views) - 1
        new_binary.extend(donor_binary[start:end])

    target["samplers"] = copy.deepcopy(donor.get("samplers", []))
    target["textures"] = copy.deepcopy(donor.get("textures", []))
    target["images"] = donor_images
    target["materials"] = donor_materials

    patterns = [re.compile(pattern) for pattern in args.emissive_node_regex]
    emissive_by_material: dict[int, int] = {}
    selected_nodes: list[str] = []
    for node in target.get("nodes", []):
        name = node.get("name", "")
        if not patterns or not any(pattern.search(name) for pattern in patterns):
            continue
        mesh_index = node.get("mesh")
        if mesh_index is None:
            continue
        selected_nodes.append(name)
        for primitive in target["meshes"][mesh_index].get("primitives", []):
            base_index = primitive.get("material")
            if base_index is None:
                continue
            if base_index not in emissive_by_material:
                material = copy.deepcopy(target["materials"][base_index])
                material["name"] = material.get("name", "material") + "__emissive"
                base_texture = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
                if base_texture is not None:
                    material["emissiveTexture"] = copy.deepcopy(base_texture)
                material["emissiveFactor"] = [args.emissive_strength] * 3
                target["materials"].append(material)
                emissive_by_material[base_index] = len(target["materials"]) - 1
            primitive["material"] = emissive_by_material[base_index]

    if patterns and not selected_nodes:
        raise ValueError("no nodes matched the requested emissive regex")

    target.setdefault("asset", {})["generator"] = "RT2 PinballAssetProbe repack_glb_pbr.py"
    target.setdefault("extras", {})["rt2PinballAssetProbe"] = {
        "geometrySource": args.geometry.name,
        "materialDonor": args.material_donor.name,
        "emissiveNodes": selected_nodes,
        "emissiveStrength": args.emissive_strength,
    }
    write_glb(args.output, target, bytes(new_binary))
    print(
        f"wrote {args.output}: nodes={len(target.get('nodes', []))}, "
        f"meshes={len(target.get('meshes', []))}, materials={len(target.get('materials', []))}, "
        f"images={len(target.get('images', []))}, emissive_nodes={len(selected_nodes)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
