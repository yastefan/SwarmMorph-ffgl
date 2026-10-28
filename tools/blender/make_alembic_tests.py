# SPDX-License-Identifier: GPL-3.0-or-later
"""Exports Alembic test files for SwarmMorph from Blender.

Run headless:
    blender --background --factory-startup --python tools/blender/make_alembic_tests.py

Writes into examples/alembic (not checked in). Each file covers one case the
Alembic loader has to handle.
"""
import math
import os

import bpy

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples", "alembic")


def reset(fps=24):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = fps
    scene.render.fps_base = 1.0
    return scene


def make_linear(obj):
    """Linear keyframes, so cycles loop at constant speed."""
    if obj.animation_data and obj.animation_data.action:
        for curve in obj.animation_data.action.fcurves:
            for key in curve.keyframe_points:
                key.interpolation = "LINEAR"


def subdivide(obj, levels):
    """The exporter evaluates render settings, so set render levels too."""
    modifier = obj.modifiers.new("Subdivision", "SUBSURF")
    modifier.levels = levels
    modifier.render_levels = levels
    return modifier


def export(name, start, end, **options):
    path = os.path.join(OUT_DIR, name + ".abc")
    bpy.ops.wm.alembic_export(filepath=path, start=start, end=end, selected=False, flatten=False,
                              apply_subdiv=True, **options)
    print("wrote", os.path.relpath(path))


def suzanne_static():
    """A single frame: loads as a static mesh."""
    reset()
    bpy.ops.mesh.primitive_monkey_add()
    monkey = bpy.context.active_object
    subdivide(monkey, 1)
    export("suzanne_static", 1, 1)


def twist_cube():
    """Deformation (animated Simple Deform twist) plus an animated object rotation, 24 fps loop."""
    reset(24)
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.active_object
    cube.scale = (0.6, 0.6, 1.4)
    subdivide(cube, 4)
    twist = cube.modifiers.new("Twist", "SIMPLE_DEFORM")
    twist.deform_method = "TWIST"
    twist.deform_axis = "Z"
    for frame, angle in ((1, -100), (49, 100), (97, -100)):
        twist.angle = math.radians(angle)
        cube.keyframe_insert(data_path='modifiers["Twist"].angle', frame=frame)
    cube.rotation_euler = (0, 0, 0)
    cube.keyframe_insert(data_path="rotation_euler", frame=1)
    cube.rotation_euler = (0, 0, 2 * math.pi)
    cube.keyframe_insert(data_path="rotation_euler", frame=97)
    make_linear(cube)
    export("twist_cube", 1, 96)


def wave_plane_30fps():
    """Wave modifier on a dense grid at 30 fps: tests reading the frame rate from the file."""
    reset(30)
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=80, y_subdivisions=80, size=3)
    grid = bpy.context.active_object
    grid.rotation_euler = (math.radians(-60), 0, 0)
    wave = grid.modifiers.new("Wave", "WAVE")
    wave.height = 0.25
    wave.width = 0.6
    wave.speed = 0.08
    export("wave_plane_30fps", 1, 90)


def orbit_hierarchy():
    """Two meshes; only transforms animate (an empty spins, a moon is parented to it).
    Tests merging meshes and evaluating parent transforms over time."""
    reset(24)
    bpy.ops.mesh.primitive_torus_add(major_radius=1.0, minor_radius=0.25)
    bpy.ops.object.empty_add()
    pivot = bpy.context.active_object
    pivot.rotation_euler = (math.radians(20), 0, 0)
    pivot.keyframe_insert(data_path="rotation_euler", frame=1)
    pivot.rotation_euler = (math.radians(20), 0, 2 * math.pi)
    pivot.keyframe_insert(data_path="rotation_euler", frame=73)
    make_linear(pivot)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.35, location=(1.8, 0, 0))
    moon = bpy.context.active_object
    moon.parent = pivot
    export("orbit_hierarchy", 1, 72)


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    suzanne_static()
    twist_cube()
    wave_plane_30fps()
    orbit_hierarchy()
