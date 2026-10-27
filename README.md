# SwarmMorph-ffgl

A native FFGL source plugin for Resolume Arena/Avenue. It turns up to eight
`.obj` meshes into a GPU particle swarm. When you switch the target mesh,
every particle flies to its point on the new surface.

- Up to 1,048,576 particles, simulated and rendered entirely on the GPU
- OpenGL 4.1 core, so one code base runs on macOS and Windows
- Meshes are loaded on a background thread, so switching files mid-show doesn't drop frames
- Polygon meshes are sampled uniformly by surface area; OBJ files without faces are used as point clouds

## Installing

Download the latest build from the
[Actions tab](https://github.com/yastefan/SwarmMorph-ffgl/actions) (or a release) and copy the plugin into
Resolume's extra effects folder, then restart Resolume:

| OS      | File                | Folder                                         |
|---------|---------------------|------------------------------------------------|
| Windows | `SwarmMorph.dll`    | `Documents\Resolume Arena\Extra Effects`       |
| macOS   | `SwarmMorph.bundle` | `~/Documents/Resolume Arena/Extra Effects`     |

On macOS, remove the download quarantine flag first:

```bash
xattr -dr com.apple.quarantine SwarmMorph.bundle
```

SwarmMorph shows up under **Sources**.

## Parameters

| Group  | Parameter        | What it does |
|--------|------------------|--------------|
| Meshes | Mesh 1–8         | `.obj` file per slot |
|        | Normalize        | Centre each mesh and scale it to the unit sphere |
|        | Target           | Slot the swarm flies to (1–8). Empty or failed slots are ignored. |
|        | Next             | Jump to the next loaded slot (map it to a MIDI pad or the BPM) |
| Motion | Particles        | Particle count, 10k – 1M |
|        | Attraction       | Spring strength: how fast particles reach the new shape |
|        | Damping          | Low values overshoot and wobble, high values glide in |
|        | Chaos            | Per-particle variation of the attraction, so the swarm arrives in waves |
|        | Turbulence       | Swirl while travelling; fades out as particles arrive |
|        | Shimmer          | Small swirl that stays once the shape has formed |
|        | Noise Scale      | Size of the swirls |
|        | Scatter          | Kick all particles outward |
| Look   | Point Size       | In pixels at 1080p |
|        | Color            | Base colour |
|        | Speed Glow       | Fast particles glow towards white |
|        | Brightness       | Additive intensity |
| Camera | Spin             | Continuous rotation, centre = still |
|        | Rotate X/Y, Zoom | Camera orientation and distance |
| Forces | Wind X/Y/Z       | Constant force along each axis, centre = none. Wind Y below centre acts as gravity. |
|        | Gusts            | Waves of stronger and weaker wind travelling through the shape |
|        | Vortex           | Twist around the vertical axis, either direction |
|        | Explode          | Push outward from the centre; below centre pulls inward |

Forces work against the attraction: the shape settles into a stretched,
displaced or twisted version of itself and springs back when the force is
released. Lower **Attraction** to let the forces win. Particles react with
individual strength, so wind leaves streaks. Forces act in the mesh's own
axes, so with **Spin** on, the wind direction turns with the object.

## Building

Clone with the FFGL SDK submodule:

```bash
git clone --recursive git@github.com:yastefan/SwarmMorph-ffgl.git
```

**macOS** (Xcode command line tools, CMake 3.20+):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Windows** (Visual Studio 2022 with C++, CMake 3.20+):

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

GitHub Actions builds both platforms on every push (universal macOS binary
and x64 DLL). Pushing a `v*` tag publishes a release with both zips.

## Testing without Resolume (macOS)

`swarmmorph-harness` loads the built plugin through `plugMain` like a host,
renders offscreen on an OpenGL 4.1 core context and writes PNG frames:

```bash
python3 tools/make_test_meshes.py
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
./build-debug/swarmmorph-harness --plugin build-debug/SwarmMorph.bundle --out renders \
    --mesh 1 examples/meshes/torus.obj --mesh 2 examples/meshes/knot.obj \
    --frames 360 --every 30 --at 180 Target=2
```

In Debug builds the FFGL SDK asserts whenever the plugin leaves the GL context
in a non-default state, which is the most common reason plugins misbehave in
Resolume. Run `--help` for all options, `--list` for the parameter names, and
`--throughput` for performance measurements.

## How it works

- `src/MeshSampler.cpp` parses OBJ files and draws one million random,
  area-weighted surface points per mesh. Since they are independent samples,
  any prefix is uniform too, which is how the particle count changes without
  resampling.
- Particle state lives in two RGBA32F textures (position + seed, velocity),
  ping-ponged each frame. The update is a fragment pass: a damped spring
  towards the particle's target point plus a divergence-free swirl field.
- Rendering draws one point per particle, fetching its position from the
  state texture.

Two findings from profiling on macOS (Intel Iris Plus): transform feedback was
about 15× slower than the fragment pass, and attribute-less point drawing
(`gl_VertexID` only) about 4× slower than drawing from a static index buffer.
The current design avoids both: 270k particles take about 2 ms per frame and
1M about 7 ms on that GPU.
