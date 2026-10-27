#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes animated OBJ sequences for testing SwarmMorph into examples/sequences.

Each pattern is a seamless loop with constant topology, named like Blender's
OBJ animation export (name_0001.obj, name_0002.obj, ...). Load any frame of a
sequence into a mesh slot; the plugin picks up the rest.

The output is large and regenerated on demand, so it is not checked in.
"""
import argparse
import math
import os

from make_test_meshes import grid_faces

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "examples", "sequences")
TAU = 2.0 * math.pi


def write_sequence(name, frame_count, topology, frame_vertices):
    """topology: list of faces (or None for a point cloud); frame_vertices(phase) -> vertex list."""
    directory = os.path.join(OUT_DIR, name)
    os.makedirs(directory, exist_ok=True)
    faces = "".join("f " + " ".join(str(i + 1) for i in face) + "\n" for face in topology or [])
    for frame in range(frame_count):
        phase = frame / frame_count  # 0..1, never reaches 1, so the loop has no duplicate frame
        vertices = frame_vertices(phase)
        path = os.path.join(directory, "%s_%04d.obj" % (name, frame + 1))
        with open(path, "w") as f:
            f.write("# SwarmMorph test sequence %s, frame %d/%d\n" % (name, frame + 1, frame_count))
            f.write("".join("v %.4f %.4f %.4f\n" % v for v in vertices))
            f.write(faces)
    print("wrote %-10s %3d frames, %5d vertices, %s" % (name, frame_count, len(frame_vertices(0.0)), "point cloud" if topology is None else "%d faces" % len(topology)))


def sphere_grid(rows, cols):
    """Unit directions on a UV sphere (poles included) and quad faces between them."""
    directions = []
    for r in range(rows):
        theta = math.pi * r / (rows - 1)
        for c in range(cols):
            phi = TAU * c / cols
            directions.append((math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)))
    return directions, grid_faces(rows, cols, False, True)


def blob(frames):
    """Lava-lamp blob: a sphere pushed in and out by travelling sine lobes."""
    directions, faces = sphere_grid(48, 64)

    def vertices(phase):
        t = TAU * phase
        out = []
        for x, y, z in directions:
            d = (0.18 * math.sin(3 * x + 2 * t) * math.cos(2 * y - t)
                 + 0.12 * math.sin(4 * z - 3 * t + 1.3)
                 + 0.08 * math.cos(5 * y + 2 * x + t))
            r = 1.0 + d
            out.append((x * r, y * r, z * r))
        return out

    write_sequence("blob", frames, faces, vertices)


def ocean(frames):
    """Water surface: three ripple sources interfering with a swell."""
    n = 72
    size = 2.4
    sources = [(-0.6, -0.4, 2), (0.7, -0.2, 3), (0.0, 0.7, 2)]  # x, z, ripples per loop
    faces = grid_faces(n, n, False, False)

    def vertices(phase):
        t = TAU * phase
        out = []
        for r in range(n):
            for c in range(n):
                x = (c / (n - 1) - 0.5) * size
                z = (r / (n - 1) - 0.5) * size
                h = 0.06 * math.sin(2.0 * x + 1.5 * z - t)
                for sx, sz, k in sources:
                    d = math.hypot(x - sx, z - sz)
                    h += 0.07 * math.sin(9.0 * d - k * t) * math.exp(-1.2 * d)
                out.append((x, h, z))
        return out

    write_sequence("ocean", frames, faces, vertices)


def flag(frames):
    """Flag on a pole: a wave travels away from the pole and grows towards the free edge."""
    cols, rows = 60, 36
    width, height = 2.0, 1.2
    faces = grid_faces(rows, cols, False, False)

    def vertices(phase):
        t = TAU * phase
        out = []
        for r in range(rows):
            for c in range(cols):
                u = c / (cols - 1)
                v = r / (rows - 1)
                amplitude = 0.22 * u ** 1.3
                wave = math.sin(TAU * 1.2 * u - 2 * t + 0.6 * v)
                ripple = 0.25 * math.sin(TAU * 3.0 * u - 3 * t + 2.0 * v)
                x = (u - 0.5) * width - 0.15 * amplitude
                y = (v - 0.5) * height - 0.05 * u * u
                z = amplitude * (wave + ripple)
                out.append((x, y, z))
        return out

    write_sequence("flag", frames, faces, vertices)


def ribbon(frames):
    """Dancing ribbon: a flat band along a torus knot that breathes and twists."""
    rows, cols = 360, 10
    faces = grid_faces(rows, cols, True, False)

    def vertices(phase):
        t = TAU * phase
        out = []
        for r in range(rows):
            s = TAU * r / rows
            p, q = 2, 3
            radius = 1.0 + 0.35 * math.cos(q * s + t)
            centre = (radius * math.cos(p * s), 0.35 * math.sin(q * s + t) + 0.15 * math.sin(2 * s - t), radius * math.sin(p * s))
            twist = 3 * s + 2 * t
            width = 0.12 + 0.06 * math.sin(4 * s - t)
            # Band direction mixes vertical and radial, rotated by the twist.
            radial = (math.cos(p * s), 0.0, math.sin(p * s))
            for c in range(cols):
                w = (c / (cols - 1) - 0.5) * 2 * width
                out.append((centre[0] + w * math.cos(twist) * radial[0],
                            centre[1] + w * math.sin(twist),
                            centre[2] + w * math.cos(twist) * radial[2]))
        return out

    write_sequence("ribbon", frames, faces, vertices)


def heart(frames):
    """Beating heart: lub-dub double pulse once per loop."""
    rows, cols = 40, 96
    faces = []
    # Slices from back (v=0) to front (v=pi); each slice is the classic heart curve scaled by sin(v).
    for r in range(rows - 1):
        for c in range(cols):
            c2 = (c + 1) % cols
            faces.append((r * cols + c, r * cols + c2, (r + 1) * cols + c2, (r + 1) * cols + c))

    def vertices(phase):
        lub = math.exp(-((phase - 0.10) / 0.05) ** 2)
        dub = 0.6 * math.exp(-((phase - 0.30) / 0.06) ** 2)
        scale = 1.0 + 0.14 * (lub + dub)
        out = []
        for r in range(rows):
            v = math.pi * r / (rows - 1)
            for c in range(cols):
                u = TAU * c / cols
                hx = 16 * math.sin(u) ** 3
                hy = 13 * math.cos(u) - 5 * math.cos(2 * u) - 2 * math.cos(3 * u) - math.cos(4 * u)
                k = math.sin(v) ** 0.7
                out.append((hx * k * scale / 16, hy * k * scale / 16, 0.45 * math.cos(v) * scale))
        return out

    write_sequence("heart", frames, faces, vertices)


def galaxy(frames):
    """Spiral galaxy as a point cloud (no faces). The disc turns once per loop while
    the arms wind up and relax again, inner stars leading; the bulge turns twice.
    Both are smooth in radius and periodic, so the cycle is seamless."""
    import random
    rng = random.Random(3)
    stars = []
    for i in range(9000):
        arm = i % 3
        radius = 0.05 + rng.random() ** 0.7
        angle = arm * TAU / 3 + 2.6 * math.log(1 + 4 * radius) + rng.gauss(0, 0.22)
        height = rng.gauss(0, 0.04 * (1.2 - radius))
        stars.append((radius, angle, height, 1))
    for i in range(2500):  # central bulge
        radius = abs(rng.gauss(0, 0.12))
        stars.append((radius, rng.random() * TAU, rng.gauss(0, 0.07), 2))

    def vertices(phase):
        out = []
        for radius, angle, height, turns in stars:
            a = angle + TAU * turns * phase + 0.9 * math.sin(TAU * phase) * (1.0 - radius)
            out.append((radius * math.cos(a), height, radius * math.sin(a)))
        return out

    write_sequence("galaxy", frames, None, vertices)


def supershape(frames):
    """Gielis supershape whose exponents morph: star, flower and blob in one loop."""
    directions, faces = sphere_grid(56, 72)

    def superformula(angle, m, n1, n2, n3):
        a = abs(math.cos(m * angle / 4)) ** n2
        b = abs(math.sin(m * angle / 4)) ** n3
        return (a + b) ** (-1.0 / n1)

    def vertices(phase):
        t = TAU * phase
        n1 = 0.6 + 0.45 * math.sin(t)
        n2 = 1.2 + 0.8 * math.sin(t + 1.0)
        n3 = 1.2 + 0.8 * math.sin(2 * t + 2.0)
        out = []
        for x, y, z in directions:
            lat = math.asin(max(-1.0, min(1.0, y)))
            lon = math.atan2(z, x)
            r1 = superformula(lon, 6, n1, n2, n3)
            r2 = superformula(lat, 4, n1 + 0.3, n2, n2)
            cl = math.cos(lat)
            out.append((r1 * math.cos(lon) * r2 * cl, r2 * math.sin(lat), r1 * math.sin(lon) * r2 * cl))
        return out

    write_sequence("supershape", frames, faces, vertices)


PATTERNS = {
    "blob": (blob, 96),
    "ocean": (ocean, 72),
    "flag": (flag, 48),
    "ribbon": (ribbon, 96),
    "heart": (heart, 48),
    "galaxy": (galaxy, 96),
    "supershape": (supershape, 120),
}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("patterns", nargs="*", help="patterns to write, from: %s (default: all)" % ", ".join(PATTERNS))
    args = parser.parse_args()
    unknown = [name for name in args.patterns if name not in PATTERNS]
    if unknown:
        parser.error("unknown pattern: " + ", ".join(unknown))
    for name in args.patterns or PATTERNS:
        generate, frame_count = PATTERNS[name]
        generate(frame_count)
