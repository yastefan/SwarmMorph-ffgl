#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a few simple OBJ meshes for testing SwarmMorph into examples/meshes."""
import math
import os

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "examples", "meshes")


def write_obj(name, vertices, faces):
    path = os.path.join(OUT_DIR, name)
    with open(path, "w") as f:
        f.write("# SwarmMorph test mesh\n")
        for x, y, z in vertices:
            f.write("v %.5f %.5f %.5f\n" % (x, y, z))
        for face in faces:
            f.write("f " + " ".join(str(i + 1) for i in face) + "\n")
    print("wrote", os.path.relpath(path))


def grid_faces(rows, cols, wrap_rows, wrap_cols):
    """Quad faces over a rows x cols vertex grid, optionally wrapping around."""
    faces = []
    for r in range(rows if wrap_rows else rows - 1):
        for c in range(cols if wrap_cols else cols - 1):
            r2, c2 = (r + 1) % rows, (c + 1) % cols
            faces.append((r * cols + c, r * cols + c2, r2 * cols + c2, r2 * cols + c))
    return faces


def torus(major=1.0, minor=0.35, rows=64, cols=24):
    vertices = []
    for r in range(rows):
        u = 2 * math.pi * r / rows
        for c in range(cols):
            v = 2 * math.pi * c / cols
            ring = major + minor * math.cos(v)
            vertices.append((ring * math.cos(u), minor * math.sin(v), ring * math.sin(u)))
    return vertices, grid_faces(rows, cols, True, True)


def trefoil_knot(radius=0.12, rows=256, cols=12):
    def curve(t):
        return (math.sin(t) + 2 * math.sin(2 * t), math.cos(t) - 2 * math.cos(2 * t), -math.sin(3 * t))

    vertices = []
    for r in range(rows):
        t = 2 * math.pi * r / rows
        p = curve(t)
        q = curve(t + 1e-3)
        tangent = [q[i] - p[i] for i in range(3)]
        length = math.sqrt(sum(x * x for x in tangent))
        tangent = [x / length for x in tangent]
        # Any vector not parallel to the tangent gives a frame for the tube.
        up = (0.0, 0.0, 1.0) if abs(tangent[2]) < 0.9 else (1.0, 0.0, 0.0)
        normal = [tangent[1] * up[2] - tangent[2] * up[1], tangent[2] * up[0] - tangent[0] * up[2], tangent[0] * up[1] - tangent[1] * up[0]]
        length = math.sqrt(sum(x * x for x in normal))
        normal = [x / length for x in normal]
        binormal = [tangent[1] * normal[2] - tangent[2] * normal[1], tangent[2] * normal[0] - tangent[0] * normal[2], tangent[0] * normal[1] - tangent[1] * normal[0]]
        for c in range(cols):
            a = 2 * math.pi * c / cols
            vertices.append(tuple((p[i] + 3 * radius * (math.cos(a) * normal[i] + math.sin(a) * binormal[i])) / 3.0 for i in range(3)))
    return vertices, grid_faces(rows, cols, True, True)


def cube():
    vertices = [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    return vertices, faces


def wave(size=2.0, n=80):
    vertices = []
    for r in range(n):
        for c in range(n):
            x = (c / (n - 1) - 0.5) * size
            z = (r / (n - 1) - 0.5) * size
            d = math.sqrt(x * x + z * z)
            vertices.append((x, 0.25 * math.cos(6 * d) * math.exp(-d), z))
    return vertices, grid_faces(n, n, False, False)


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    write_obj("torus.obj", *torus())
    write_obj("knot.obj", *trefoil_knot())
    write_obj("cube.obj", *cube())
    write_obj("wave.obj", *wave())
