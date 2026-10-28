# SPDX-License-Identifier: GPL-3.0-or-later
"""Exports looping Alembic creature animations for SwarmMorph from Blender.

Run headless:
    /Applications/Blender.app/Contents/MacOS/Blender --background --factory-startup \
        --python tools/blender/make_creature_animations.py [-- NAME ...]

Writes into examples/alembic/creatures (not checked in). Every creature is
built procedurally from tubes and surface grids, centred, about 2 units tall
or wide, made of a few named parts. Each frame becomes a shape key, and every
motion is a whole number of cycles per loop, so the last frame flows back into
the first without a jump.
"""
import math
import os
import sys

import bpy

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples", "alembic", "creatures")
TAU = 2.0 * math.pi
FPS = 24


# ---------------------------------------------------------------------------
# Vector helpers on plain tuples (Blender's Z is up; the exporter writes Y-up).
# ---------------------------------------------------------------------------
def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    length = math.sqrt(dot(a, a))
    return mul(a, 1.0 / length) if length > 1e-12 else (0.0, 0.0, 1.0)


def lerp(a, b, t):
    return a + (b - a) * t


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


# ---------------------------------------------------------------------------
# Building blocks. Each returns (vertices, faces) with a topology that depends
# only on the counts, never on time.
# ---------------------------------------------------------------------------
def tube(points, radius, around=8, ref=(0.0, 0.0, 1.0), cap=True):
    """A tube through `points`. `radius(i)` gives a float or (side, up) radii.
    The cross-section is oriented against a fixed reference axis, so it
    doesn't twist from frame to frame."""
    verts, faces = [], []
    count = len(points)
    for i, p in enumerate(points):
        tangent = norm(sub(points[min(i + 1, count - 1)], points[max(i - 1, 0)]))
        side = cross(tangent, ref)
        if dot(side, side) < 1e-6:
            side = cross(tangent, (1.0, 0.0, 0.0))
        side = norm(side)
        up = cross(side, tangent)
        r = radius(i)
        ra, rb = r if isinstance(r, tuple) else (r, r)
        for j in range(around):
            a = TAU * j / around
            verts.append(add(p, add(mul(side, math.cos(a) * ra), mul(up, math.sin(a) * rb))))
    for i in range(count - 1):
        for j in range(around):
            k = (j + 1) % around
            faces.append((i * around + j, i * around + k, (i + 1) * around + k, (i + 1) * around + j))
    if cap:
        start = len(verts)
        verts.append(points[0])
        verts.append(points[-1])
        last = (count - 1) * around
        for j in range(around):
            k = (j + 1) % around
            faces.append((start, k, j))
            faces.append((start + 1, last + j, last + k))
    return verts, faces


def grid(nu, nv, fn, wrap_u=False):
    """A surface fn(u, v) -> point with u, v in [0, 1]; u can wrap around."""
    verts, faces = [], []
    cols = nu if wrap_u else nu
    for j in range(nv):
        v = j / (nv - 1)
        for i in range(cols):
            u = i / nu if wrap_u else i / (nu - 1)
            verts.append(fn(u, v))
    for j in range(nv - 1):
        for i in range(cols if wrap_u else cols - 1):
            k = (i + 1) % cols
            faces.append((j * cols + i, j * cols + k, (j + 1) * cols + k, (j + 1) * cols + i))
    return verts, faces


def merge(*meshes):
    verts, faces = [], []
    for v, f in meshes:
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(base + i for i in face) for face in f)
    return verts, faces


def posed(parts, pitch=0.0, yaw=0.0):
    """Turns a creature to face the default camera, which looks along +Y
    (Blender axes): yaw about Z first, then pitch about X."""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def turn(v):
        x, y, z = v[0] * cy - v[1] * sy, v[0] * sy + v[1] * cy, v[2]
        return (x, y * cp - z * sp, y * sp + z * cp)

    return [(name, ([turn(v) for v in verts], faces)) for name, (verts, faces) in parts]


def revolve(nu, nv, profile, wrap=True):
    """Surface of revolution around Z: profile(v, angle) -> (radius, z)."""
    def fn(u, v):
        a = TAU * u
        r, z = profile(v, a)
        return (r * math.cos(a), r * math.sin(a), z)
    return grid(nu, nv, fn, wrap_u=wrap)


# ---------------------------------------------------------------------------
# Creatures: build(t) with t in [0, 1) returns [(part name, (verts, faces))].
# ---------------------------------------------------------------------------
def jellyfish_pulse(t):
    ph = TAU * t
    c = 0.5 - 0.5 * math.cos(ph)  # 0 relaxed, 1 contracted
    radius, height = 0.8 * (1.0 - 0.2 * c), 0.6 * (1.0 + 0.2 * c)
    top = 0.35

    def bell_point(v, a):
        r = radius * math.sin(v * 1.45) / math.sin(1.45) * (1.0 - 0.18 * c * v * v)
        r *= 1.0 + 0.05 * v ** 3 * math.sin(8.0 * a + ph)
        z = top + height * (math.cos(v * 1.45) - math.cos(1.45)) / (1.0 - math.cos(1.45))
        return r, z - 0.06 * c * v ** 4

    bell = revolve(64, 28, bell_point)

    tentacles = []
    for i in range(24):
        a = TAU * i / 24
        r0, z0 = bell_point(1.0, a)
        out, side = (math.cos(a), math.sin(a), 0.0), (-math.sin(a), math.cos(a), 0.0)
        points = []
        for k in range(40):
            s = k / 39
            sway = 0.12 * s * math.sin(ph - 5.0 * s + i * 0.7)
            flare = 0.18 * s * math.sin(ph - 2.5 * s)
            p = mul(out, r0 * (1.0 - 0.25 * s) + flare)
            p = add(p, mul(side, sway))
            points.append(add(p, (0.0, 0.0, z0 - 1.5 * s * (1.0 - 0.1 * c))))
        tentacles.append(tube(points, lambda k: 0.022 * (1.0 - 0.6 * k / 39), around=5))

    arms = []
    for i in range(4):
        points = []
        for k in range(50):
            s = k / 49
            a = TAU * i / 4 + 1.5 * s + 0.3 * math.sin(ph - 3.0 * s)
            r = 0.06 + 0.12 * s
            points.append((r * math.cos(a), r * math.sin(a), top + 0.05 - 1.1 * s))
        arms.append(tube(points, lambda k: 0.05 * (1.0 - 0.6 * k / 49) * (1.0 + 0.35 * math.sin(k * 1.6)), around=8))

    return [("jelly_bell", bell), ("jelly_tentacles", merge(*tentacles)), ("jelly_oral_arms", merge(*arms))]


def manta_ray(t):
    ph = TAU * t
    bob = 0.06 * math.sin(ph + math.pi)

    def wing(u, v, sign):
        x = (u * 2.0 - 1.0)
        width = math.sin(math.pi * v ** 0.75) ** 0.8 * (1.0 - 0.25 * v)
        px = x * width
        py = 0.55 - 1.1 * v
        thick = 0.09 * max(1.0 - x * x, 0.0) ** 1.5 * math.sin(math.pi * v) ** 0.5
        flap = 0.5 * abs(px) ** 1.6 * math.sin(ph - 1.8 * v - 0.4)
        return (px, py, bob + flap + sign * thick)

    top = grid(60, 40, lambda u, v: wing(u, v, 1.0))
    bottom = grid(60, 40, lambda u, v: wing(u, v, -1.0))

    tail_points = []
    for k in range(40):
        s = k / 39
        tail_points.append((0.03 * math.sin(ph - 4.0 * s), -0.55 - 1.0 * s, bob + 0.12 * s * math.sin(ph - 3.0 * s - 1.5)))
    tail = tube(tail_points, lambda k: 0.02 * (1.0 - 0.85 * k / 39), around=6)

    fins = []
    for sign in (-1.0, 1.0):
        def fin(u, v, sign=sign):
            curl = 1.6 * u * (0.8 + 0.2 * math.sin(ph))
            x = sign * (0.2 + 0.05 * math.cos(curl) - 0.05)
            return (x + sign * 0.04 * (v - 0.5), 0.55 + 0.18 * math.sin(curl) * 0.8 + 0.02, bob - 0.05 * (1.0 - math.cos(curl)) * 1.4)
        fins.append(grid(10, 6, fin))

    return posed([("manta_body", merge(top, bottom)), ("manta_tail", tail), ("manta_fins", merge(*fins))], pitch=35.0)


def koi_fish(t):
    ph = TAU * t
    count = 64

    def spine(s):
        amp = 0.02 + 0.2 * s * s
        y = amp * math.sin(2.0 * ph - 5.5 * s)
        return (0.9 - 1.75 * s, y, 0.02 * math.sin(2.0 * ph))

    def girth(s):
        return 0.27 * math.sin(math.pi * (s * 0.9 + 0.06)) ** 0.7 * (1.0 - 0.6 * s) + 0.012

    points = [spine(k / (count - 1)) for k in range(count)]
    body = tube(points, lambda k: (girth(k / (count - 1)) * 0.7, girth(k / (count - 1))), around=16)

    end, before = points[-1], points[-3]
    back = norm(sub(end, before))
    side = norm(cross((0.0, 0.0, 1.0), back))

    def tail(u, v):
        a = u
        b = v * 2.0 - 1.0
        reach = 0.45 * a * (1.0 - 0.4 * (1.0 - abs(b)) ** 2)
        flex = 0.12 * a * a * math.sin(2.0 * ph - 5.5 - 1.2 * a)
        p = add(end, mul(back, reach))
        p = add(p, mul(side, flex))
        return add(p, (0.0, 0.0, b * (0.05 + 0.32 * a)))

    tail_fin = grid(14, 16, tail)

    def dorsal(u, v):
        s = 0.25 + 0.35 * u
        base = spine(s)
        h = 0.2 * math.sin(math.pi * u) ** 0.8
        return add(base, (-0.06 * v, 0.0, girth(s) + h * v))

    dorsal_fin = grid(16, 6, dorsal)

    pectorals = []
    for sign in (-1.0, 1.0):
        def pectoral(u, v, sign=sign):
            base = spine(0.22)
            flap = 0.5 + 0.4 * math.sin(2.0 * ph + sign)
            reach = 0.25 * u
            return add(base, (-0.12 * v - 0.1 * u, sign * (girth(0.22) * 0.7 + reach * math.cos(flap)), -girth(0.22) * 0.6 - reach * math.sin(flap)))
        pectorals.append(grid(8, 6, pectoral))

    return [("koi_body", body), ("koi_tail", tail_fin), ("koi_fins", merge(dorsal_fin, *pectorals))]


def sea_serpent(t):
    ph = TAU * t
    count = 160

    def spine(s):
        return (1.0 - 2.0 * s,
                0.32 * math.sin(TAU * 1.25 * s - ph),
                0.22 * math.sin(TAU * 0.75 * s - ph + 1.0))

    def girth(s):
        head = 0.6 * math.exp(-((s - 0.04) / 0.04) ** 2)
        return 0.085 * (1.0 - s) ** 0.6 * (1.0 + head) + 0.006

    points = [spine(k / (count - 1)) for k in range(count)]
    body = tube(points, lambda k: girth(k / (count - 1)), around=12)

    spikes = []
    for i in range(32):
        s = 0.1 + 0.8 * i / 31
        p, q = spine(s), spine(s + 0.002)
        tangent = norm(sub(q, p))
        up = norm(cross(cross(tangent, (0.0, 0.0, 1.0)), tangent))
        if up[2] < 0.0:
            up = mul(up, -1.0)
        root = add(p, mul(up, girth(s) * 0.8))
        length = 0.1 * (1.0 - 0.6 * abs(s - 0.4))
        tip = add(add(root, mul(up, length)), mul(tangent, length * 0.6))
        spikes.append(tube([root, add(root, mul(sub(tip, root), 0.5)), tip], lambda k: 0.02 * (1.0 - k / 2.0) + 0.002, around=4, ref=tangent))

    horns = []
    head, neck = spine(0.02), spine(0.06)
    forward = norm(sub(head, neck))
    for sign in (-1.0, 1.0):
        side = norm(cross((0.0, 0.0, 1.0), forward))
        root = add(head, add(mul(side, sign * 0.08), (0.0, 0.0, 0.1)))
        horn = [add(root, add(mul(forward, -0.12 * k / 5), (0.0, 0.0, 0.05 * k / 5 + 0.02 * (k / 5) ** 2))) for k in range(6)]
        horns.append(tube(horn, lambda k: 0.025 * (1.0 - k / 5.0) + 0.002, around=5))

    return [("serpent_body", body), ("serpent_spikes", merge(*spikes)), ("serpent_horns", merge(*horns))]


def butterfly(t):
    ph = TAU * t
    flaps = 3.0
    bob = -0.06 * math.sin(flaps * ph)

    body_points = [(0.32 - 0.64 * k / 23, 0.0, bob) for k in range(24)]
    body = tube(body_points, lambda k: 0.045 * math.sin(math.pi * (k + 1) / 25) ** 0.5 + 0.01, around=8)

    wings = []
    for sign in (-1.0, 1.0):
        for fore in (True, False):
            lag = 0.0 if fore else 0.35
            angle = 0.2 + 0.85 * math.sin(flaps * ph - lag)

            def wing(u, v, sign=sign, fore=fore, angle=angle):
                if fore:
                    g = math.radians(lerp(5.0, 75.0, u))
                    reach = 1.0 * (0.8 + 0.2 * math.sin(math.pi * u)) * (1.0 - 0.15 * u)
                    root = 0.08
                else:
                    g = math.radians(lerp(-80.0, -5.0, u))
                    reach = 0.72 * (0.75 + 0.25 * math.sin(math.pi * u)) * (1.0 + 0.25 * (1.0 - u) ** 4)
                    root = -0.06
                rho = v * reach
                forward = root + rho * math.sin(g)
                outward = rho * math.cos(g) + 0.03
                camber = 0.06 * v * v
                return (forward,
                        sign * (outward * math.cos(angle) - camber * math.sin(angle)),
                        bob + outward * math.sin(angle) + camber * math.cos(angle))

            wings.append(grid(22, 14, wing))

    antennae = []
    for sign in (-1.0, 1.0):
        points = [(0.32 + 0.35 * k / 15, sign * (0.03 + 0.15 * k / 15), bob + 0.04 + 0.25 * (k / 15) ** 1.3 + 0.02 * math.sin(flaps * ph - k * 0.2)) for k in range(16)]
        antennae.append(tube(points, lambda k: 0.008 + (0.018 if k > 13 else 0.0), around=5))

    return posed([("butterfly_body", body), ("butterfly_wings", merge(*wings)), ("butterfly_antennae", merge(*antennae))], pitch=30.0, yaw=90.0)


def lotus_bloom(t):
    ph = TAU * t
    bloom = smooth(0.5 - 0.5 * math.cos(ph))
    rings = [  # petals, offset, length, width, closed tilt, open tilt, base radius
        (8, 0.0, 0.95, 0.34, 18.0, 82.0, 0.16),
        (8, 0.5, 0.82, 0.30, 12.0, 60.0, 0.12),
        (8, 0.0, 0.62, 0.24, 6.0, 34.0, 0.08),
    ]
    petals = []
    for count, offset, length, width, closed, opened, base_r in rings:
        tilt = math.radians(lerp(closed, opened, bloom))
        for i in range(count):
            a = TAU * (i + offset) / count
            radial = (math.cos(a), math.sin(a), 0.0)
            tangent = (-math.sin(a), math.cos(a), 0.0)
            along = add(mul((0.0, 0.0, 1.0), math.cos(tilt)), mul(radial, math.sin(tilt)))
            inward = norm(cross(along, tangent))
            base = add(mul(radial, base_r), (0.0, 0.0, -0.25))

            def petal(u, v, along=along, inward=inward, tangent=tangent, base=base, length=length, width=width):
                w = u * 2.0 - 1.0
                half = width * math.sin(math.pi * min(v * 0.95 + 0.05, 1.0)) ** 0.8 * (1.0 - 0.35 * v)
                cup = 0.35 * (w * half) ** 2 / max(width, 1e-3) * (1.0 - 0.4 * bloom)
                curl = (0.1 - 0.18 * bloom) * v * v
                p = add(base, mul(along, v * length))
                p = add(p, mul(tangent, w * half))
                return add(p, mul(inward, -cup + curl))

            petals.append(grid(12, 16, petal))

    pod = revolve(24, 8, lambda v, a: (0.12 * (1.0 - 0.3 * v * v) * (1.0 + 0.06 * bloom), -0.25 + 0.25 * v + 0.05 * bloom * v))

    def pad(u, v):
        a = TAU * lerp(0.03, 0.97, u)
        r = 0.2 + 0.55 * v
        z = -0.42 + 0.025 * math.sin(6.0 * r - ph) + 0.02 * v
        return (r * math.cos(a), r * math.sin(a), z)

    leaf = grid(48, 12, pad)
    return [("lotus_petals", merge(*petals)), ("lotus_pod", pod), ("lotus_pad", leaf)]


def sea_anemone(t):
    ph = TAU * t
    sway = (0.06 * math.sin(ph), 0.03 * math.sin(2.0 * ph), 0.0)

    def column(v, a):
        r = 0.32 + 0.1 * v ** 3 + 0.02 * math.sin(5.0 * a + v * 6.0)
        return r, -0.7 + 0.6 * v

    stem = grid(40, 16, lambda u, v: add((column(v, TAU * u)[0] * math.cos(TAU * u), column(v, TAU * u)[0] * math.sin(TAU * u), column(v, TAU * u)[1]), mul(sway, v * v)), wrap_u=True)
    disc = grid(40, 6, lambda u, v: add(((0.42 * (1.0 - v)) * math.cos(TAU * u), (0.42 * (1.0 - v)) * math.sin(TAU * u), -0.1 - 0.05 * v), sway), wrap_u=True)

    tentacles = []
    for ring, (count, radius, length) in enumerate(((26, 0.4, 0.75), (18, 0.27, 0.6))):
        for i in range(count):
            a = TAU * (i + 0.5 * ring) / count
            out = (math.cos(a), math.sin(a), 0.0)
            side = (-math.sin(a), math.cos(a), 0.0)
            base = add(add(mul(out, radius), (0.0, 0.0, -0.1)), sway)
            reach = length * (1.0 + 0.08 * math.sin(ph + a))
            points = []
            for k in range(22):
                s = k / 21
                lean = 0.55 + 0.35 * s
                current = 0.28 * s ** 1.5 * math.sin(ph - 2.0 * s + 0.4 * math.cos(a))
                wiggle = 0.05 * s * math.sin(2.0 * ph - 4.0 * s + i)
                p = add(base, mul(out, reach * s * lean))
                p = add(p, (current, 0.0, reach * s * (1.0 - 0.5 * s) + 0.15 * s))
                points.append(add(p, mul(side, wiggle)))
            tentacles.append(tube(points, lambda k: 0.028 * (1.0 - 0.6 * k / 21) + (0.012 if k == 21 else 0.0), around=6))

    return [("anemone_column", merge(stem, disc)), ("anemone_tentacles", merge(*tentacles))]


def kelp_forest(t):
    ph = TAU * t
    stalks, blades = [], []
    plants = [(-0.55, -0.2, 1.8), (-0.25, 0.3, 1.6), (0.05, -0.35, 1.9), (0.3, 0.15, 1.7),
              (0.6, -0.1, 1.85), (-0.05, 0.05, 1.75), (0.45, 0.45, 1.5), (-0.5, 0.45, 1.55)]
    for index, (x0, y0, height) in enumerate(plants):
        def stalk(s, x0=x0, y0=y0, height=height):
            sway = 0.32 * s ** 1.7 * math.sin(ph - 1.6 * x0 - 2.2 * s)
            drift = 0.08 * s ** 1.5 * math.sin(2.0 * ph - 2.0 * y0 - 3.0 * s + 1.0)
            return (x0 + sway, y0 + drift, -0.95 + height * s)

        points = [stalk(k / 39) for k in range(40)]
        stalks.append(tube(points, lambda k: 0.016 * (1.0 - 0.5 * k / 39), around=5))

        for b in range(9):
            s = 0.15 + 0.8 * b / 8
            root = stalk(s)
            tangent = norm(sub(stalk(s + 0.01), root))
            a = TAU * (b * 0.38 + index * 0.17)
            out = norm(sub((math.cos(a), math.sin(a), 0.0), mul(tangent, dot((math.cos(a), math.sin(a), 0.0), tangent))))
            direction = norm(add(mul(tangent, 0.8), mul(out, 0.6)))
            flat = norm(cross(direction, out))

            def blade(u, v, root=root, direction=direction, flat=flat, out=out, b=b):
                w = u * 2.0 - 1.0
                width = 0.07 * math.sin(math.pi * min(v * 0.9 + 0.1, 1.0))
                ripple = 0.025 * v * math.sin(2.0 * ph - 12.0 * v + w * 2.0 + b)
                p = add(root, mul(direction, 0.4 * v))
                p = add(p, mul(flat, w * width))
                return add(p, mul(out, ripple + 0.08 * v * v * math.sin(ph - b)))

            blades.append(grid(4, 12, blade))

    def rock(v, a):
        r = 0.35 * math.sin(v * math.pi * 0.5 + 0.05) * (1.0 + 0.12 * math.sin(3.0 * a) * math.sin(math.pi * v))
        return r, -0.95 + 0.15 * math.cos(v * math.pi * 0.5)

    return [("kelp_stalks", merge(*stalks)), ("kelp_blades", merge(*blades)), ("kelp_rock", revolve(32, 10, rock))]


def dna_helix(t):
    ph = TAU * t
    count, rungs = 180, 34

    def bump(s):
        d = (s - t) % 1.0
        d = min(d, 1.0 - d)
        return math.exp(-(d / 0.1) ** 2)

    def strand(s, offset):
        a = TAU * 1.5 * s + ph + offset
        r = 0.3 * (1.0 + 0.7 * bump(s))
        return (r * math.cos(a), r * math.sin(a), -1.0 + 2.0 * s)

    groove = 0.8 * math.pi
    a_points = [strand(k / (count - 1), 0.0) for k in range(count)]
    b_points = [strand(k / (count - 1), groove) for k in range(count)]
    backbone = merge(tube(a_points, lambda k: 0.05, around=8), tube(b_points, lambda k: 0.05, around=8))

    pairs = []
    for i in range(rungs):
        s = (i + 0.5) / rungs
        p, q = strand(s, 0.0), strand(s, groove)
        mid = mul(add(p, q), 0.5)
        reach = 1.0 - 0.85 * bump(s)
        for end in (p, q):
            tip = add(end, mul(sub(mid, end), reach * 0.97))
            pairs.append(tube([end, add(end, mul(sub(tip, end), 0.5)), tip], lambda k: 0.025, around=6, ref=(0.0, 0.0, 1.0)))

    return [("dna_backbone", backbone), ("dna_base_pairs", merge(*pairs))]


def octopus(t):
    ph = TAU * t
    pulse = 1.0 + 0.07 * math.sin(ph)
    tilt = 0.08 * math.sin(ph + 1.0)

    def mantle(v, a):
        r = 0.34 * math.sin(math.pi * (0.08 + 0.92 * v)) ** 0.8 * pulse * (1.0 - 0.2 * v)
        z = 0.15 + 0.85 * (1.0 - math.cos(math.pi * v)) * 0.5
        return r, z

    def tilted(p):
        return (p[0] + tilt * (p[2] - 0.15), p[1], p[2])

    head_verts, head_faces = revolve(48, 24, mantle)
    head = ([tilted(p) for p in head_verts], head_faces)

    eyes = []
    for sign in (-1.0, 1.0):
        centre = (0.28 * pulse, sign * 0.13, 0.28)
        verts, faces = revolve(12, 8, lambda v, a: (0.06 * math.sin(math.pi * v), 0.06 * math.cos(math.pi * v)))
        eyes.append(([tilted(add(p, centre)) for p in verts], faces))

    arms = []
    for i in range(8):
        a = TAU * i / 8 + math.pi / 8
        out, up, side = (math.cos(a), math.sin(a), 0.0), (0.0, 0.0, 1.0), (-math.sin(a), math.cos(a), 0.0)
        curl = 2.5 + 3.5 * (0.5 + 0.5 * math.sin(ph + i * TAU * 3.0 / 8.0))
        p = add(mul(out, 0.2 * pulse), (0.0, 0.0, 0.15))
        angle = math.radians(-70.0)
        length, steps = 1.15, 56
        points = [p]
        for k in range(1, steps):
            s = k / (steps - 1)
            angle += curl * s * (length / steps)
            p = add(p, mul(add(mul(out, math.cos(angle)), mul(up, math.sin(angle))), length / steps))
            points.append(add(p, mul(side, 0.06 * s * math.sin(2.0 * ph - 4.0 * s + i))))
        arms.append(tube(points, lambda k: 0.075 * (1.0 - k / (steps - 1)) ** 0.8 + 0.005, around=8))

    return [("octopus_mantle", merge(head, *eyes)), ("octopus_arms", merge(*arms))]


CREATURES = [  # name, builder, frames per loop
    ("jellyfish_pulse", jellyfish_pulse, 96),
    ("manta_ray", manta_ray, 96),
    ("koi_fish", koi_fish, 72),
    ("sea_serpent", sea_serpent, 120),
    ("butterfly", butterfly, 72),
    ("lotus_bloom", lotus_bloom, 144),
    ("sea_anemone", sea_anemone, 120),
    ("kelp_forest", kelp_forest, 144),
    ("dna_helix", dna_helix, 192),
    ("octopus", octopus, 120),
]


# ---------------------------------------------------------------------------
# Blender: one object per part, one shape key per frame, then export.
# ---------------------------------------------------------------------------
def build_object(name, verts, faces):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.shape_key_add(name="Basis", from_mix=False)
    return obj


def add_frame_key(obj, frame, verts, last_frame):
    """Shape key that is fully on at `frame` only (constant interpolation)."""
    key = obj.shape_key_add(name="f%04d" % frame, from_mix=False)
    flat = [c for v in verts for c in v]
    key.data.foreach_set("co", flat)
    for f, value in ((frame - 1, 0.0), (frame, 1.0), (frame + 1, 0.0)):
        if 1 <= f <= last_frame:
            key.value = value
            key.keyframe_insert("value", frame=f)


def export_creature(name, build, frames):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    scene.render.fps_base = 1.0
    scene.frame_start, scene.frame_end = 1, frames

    first = build(0.0)
    objects = [build_object(part, verts, faces) for part, (verts, faces) in first]
    for frame in range(2, frames + 1):
        parts = build((frame - 1) / frames)
        for obj, (part, (verts, faces)) in zip(objects, parts):
            if len(verts) != len(obj.data.vertices):
                raise RuntimeError("%s: %s changed its vertex count" % (name, part))
            add_frame_key(obj, frame, verts, frames)
    for obj in objects:
        for curve in obj.data.shape_keys.animation_data.action.fcurves:
            for point in curve.keyframe_points:
                point.interpolation = "CONSTANT"

    path = os.path.join(OUT_DIR, name + ".abc")
    bpy.ops.wm.alembic_export(filepath=path, start=1, end=frames, selected=False, flatten=False,
                              uvs=False, normals=False, face_sets=False, export_hair=False, export_particles=False)
    vertex_count = sum(len(obj.data.vertices) for obj in objects)
    print("wrote %s: %d frames, %d vertices" % (os.path.relpath(path), frames, vertex_count))


if __name__ == "__main__":
    wanted = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, build, frames in CREATURES:
        if not wanted or name in wanted:
            export_creature(name, build, frames)
