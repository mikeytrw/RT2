#!/usr/bin/env python3
"""Split a Wavefront OBJ into stable connected mesh components.

The source pinball asset exports most gameplay geometry as one named OBJ
object.  This tool does not guess gameplay roles.  It exposes each connected
piece as its own object and writes a measured manifest so an artist can label
the flippers, plunger, bumpers, rails, and decorative parts explicitly.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Face:
    refs: list[str]
    vertices: list[int]
    source_object: str
    material: str


class DisjointSet:
    def __init__(self, count: int) -> None:
        self.parent = list(range(count))
        self.rank = [0] * count

    def find(self, value: int) -> int:
        while self.parent[value] != value:
            self.parent[value] = self.parent[self.parent[value]]
            value = self.parent[value]
        return value

    def union(self, left: int, right: int) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root == right_root:
            return
        if self.rank[left_root] < self.rank[right_root]:
            left_root, right_root = right_root, left_root
        self.parent[right_root] = left_root
        if self.rank[left_root] == self.rank[right_root]:
            self.rank[left_root] += 1


def safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return cleaned or "unnamed"


def parse_vertex_index(ref: str, vertex_count: int) -> int:
    raw = int(ref.split("/", 1)[0])
    if raw == 0:
        raise ValueError("OBJ vertex index 0 is invalid")
    index = raw - 1 if raw > 0 else vertex_count + raw
    if index < 0 or index >= vertex_count:
        raise ValueError(f"OBJ vertex index {raw} is out of range")
    return index


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--output-obj", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()

    source_bytes = args.source.read_bytes()
    lines = source_bytes.decode("utf-8-sig").splitlines()
    vertices: list[tuple[float, float, float]] = []
    geometry_lines: list[str] = []
    faces: list[Face] = []
    material_libraries: list[str] = []
    source_object = "unnamed"
    material = ""

    for line_number, line in enumerate(lines, start=1):
        if line.startswith("v "):
            fields = line.split()
            if len(fields) < 4:
                raise ValueError(f"invalid vertex at line {line_number}")
            vertices.append((float(fields[1]), float(fields[2]), float(fields[3])))
            geometry_lines.append(line)
        elif line.startswith("vt ") or line.startswith("vn "):
            geometry_lines.append(line)
        elif line.startswith("mtllib "):
            material_libraries.append(line[len("mtllib "):].strip())
        elif line.startswith("o "):
            source_object = line[2:].strip() or "unnamed"
        elif line.startswith("usemtl "):
            material = line[len("usemtl "):].strip()
        elif line.startswith("f "):
            refs = line.split()[1:]
            if len(refs) < 3:
                raise ValueError(f"face has fewer than three vertices at line {line_number}")
            indices = [parse_vertex_index(ref, len(vertices)) for ref in refs]
            faces.append(Face(refs, indices, source_object, material))

    if not vertices or not faces:
        raise ValueError("source OBJ has no vertices or faces")

    sets = DisjointSet(len(vertices))
    for face in faces:
        first = face.vertices[0]
        for vertex in face.vertices[1:]:
            sets.union(first, vertex)

    grouped: dict[tuple[str, int], list[Face]] = {}
    object_order: dict[str, int] = {}
    for face in faces:
        object_order.setdefault(face.source_object, len(object_order))
        key = (face.source_object, sets.find(face.vertices[0]))
        grouped.setdefault(key, []).append(face)

    ordered_groups = sorted(
        grouped.items(),
        key=lambda item: (
            object_order[item[0][0]],
            -len(item[1]),
            min(vertex for face in item[1] for vertex in face.vertices),
        ),
    )

    component_counts: dict[str, int] = {}
    components: list[dict[str, object]] = []
    emitted: list[tuple[str, list[Face]]] = []
    for (object_name, _), component_faces in ordered_groups:
        component_counts[object_name] = component_counts.get(object_name, 0) + 1
        component_name = (
            f"{safe_name(object_name)}__part_{component_counts[object_name]:03d}"
        )
        vertex_ids = sorted({v for face in component_faces for v in face.vertices})
        points = [vertices[index] for index in vertex_ids]
        minimum = [min(point[axis] for point in points) for axis in range(3)]
        maximum = [max(point[axis] for point in points) for axis in range(3)]
        centroid = [sum(point[axis] for point in points) / len(points) for axis in range(3)]
        materials = sorted({face.material for face in component_faces if face.material})
        components.append(
            {
                "name": component_name,
                "source_object": object_name,
                "materials": materials,
                "face_count": len(component_faces),
                "vertex_count": len(vertex_ids),
                "bounds_min": minimum,
                "bounds_max": maximum,
                "extent": [maximum[axis] - minimum[axis] for axis in range(3)],
                "centroid": centroid,
            }
        )
        emitted.append((component_name, component_faces))

    args.output_obj.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    with args.output_obj.open("w", encoding="utf-8", newline="\n") as output:
        output.write("# Connected-component split generated by RT2 PinballAssetProbe\n")
        output.write(f"# source_sha256 {hashlib.sha256(source_bytes).hexdigest()}\n")
        for library in material_libraries:
            output.write(f"mtllib {library}\n")
        output.write("\n")
        for line in geometry_lines:
            output.write(line + "\n")
        output.write("\n")
        for component_name, component_faces in emitted:
            output.write(f"o {component_name}\n")
            output.write(f"g {component_name}\n")
            active_material = None
            for face in component_faces:
                if face.material != active_material:
                    if face.material:
                        output.write(f"usemtl {face.material}\n")
                    active_material = face.material
                output.write("f " + " ".join(face.refs) + "\n")
            output.write("\n")

    manifest = {
        "schema": 1,
        "source": str(args.source.resolve()),
        "source_size": len(source_bytes),
        "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
        "vertex_count": len(vertices),
        "face_count": len(faces),
        "source_object_count": len(object_order),
        "connected_component_count": len(components),
        "material_libraries": material_libraries,
        "components": components,
    }
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(
        f"split {len(object_order)} source objects into {len(components)} connected components; "
        f"wrote {args.output_obj} and {args.manifest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
