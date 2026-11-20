# Diamond Engine

A tiny 2D / 2.5D / 3D scene editor and runtime for Android, written without a
game engine dependency. One shared object (`libengine.so`), one APK, no
AndroidX, no Kotlin, no Compose.

```
   ┌──────────────┐   diamonds   ┌───────────────┐
   │  Java UI     │─────────────▶│  C ABI        │
   │  editor      │              │  diamond.h    │
   └──────────────┘              └───────┬───────┘
                                         │
        ┌────────────┬──────────────┬────┴─────────┐
        ▼            ▼              ▼              ▼
     scene        block logic    .diamond       renderer
     graph        + codegen      container      (4 backends)
```

## What it does

* **Scene editing** — a scene tree of nodes (empty, sprite, mesh, light,
  camera) with a transform gizmo, colour, collision flag, and OBJ import.
* **Visual block logic** — drag blocks (`OnStart`, `OnUpdate`, `Move`,
  `Rotate`, `SetColor`, `IfButton`, …) on a canvas. The same graph can be
  exported as Lua, so a project can graduate from blocks to code.
* **Three view modes** — 2D (sprites, no perspective), 2.5D (3D with a fixed
  camera) and 3D (orbit/pan/zoom).
* **Play mode** — runs the block graph with an on-screen gamepad, forward touch
  events to the graph, and pauses cleanly on `onPause`.

## Render backends

Four backends implement one interface (`IRenderBackend`); the active one is
chosen at runtime and reported in the status bar.

| Mode | Name | When to use it | Notes |
|---|---|---|---|
| 1 | OpenGL ES 3.0 | default | VAOs, instancing, textured sprites |
| 2 | Vulkan | opt-in, experimental | real pipeline + SPIR-V, degrades to a clean error |
| 3 | OpenGL ES 2.0 | very old / broken drivers | no VAO, no instancing, same look |
| 4 | Hybrid | CPU-bound 2D on weak GPUs | 3D on GLES3, 2D sprites rasterised on the CPU at half resolution and composited as one quad |

If a backend fails to attach, the engine logs the reason, falls back to
OpenGL ES 3.0 and keeps the editor usable; the failure is surfaced in the log
console rather than as a black screen.

**Vulkan is experimental.** It brings up instance, device, surface, swapchain
(FIFO, `R8G8B8A8` preferred), a `D16` depth buffer, a render pass, per-mesh
host-visible buffers, two frames in flight, and a graphics pipeline built from
SPIR-V baked in at build time. It does **not** yet do lighting parity, sprite
batching or text with the GLES3 backend, and it rebuilds all geometry buffers
whenever the node list changes — acceptable for an editor, wasteful for a game.
Devices without a Vulkan driver are detected up front, so the mode is offered
only when it can actually run.

## Documents

`.diamond` is a small self-describing container:

```
"DMND" | u16 version | u16 flags (bit0 = deflate) | u32 crc32 | u32 raw | u32 stored
then sections: 'S' | id | u32 size | payload        (ids 1..6, each 4-byte aligned)
```

| id | section | contents |
|---|---|---|
| 1 | scene | nodes, hierarchy, transforms, colours, mesh/image references |
| 2 | meshes | positions, normals, uvs, u16 indices, bounds |
| 3 | images | raw RGBA8888 |
| 4 | blocks | block graph and links |
| 5 | logic | generated Lua source as UTF-8 |
| 6 | meta | name, render mode, view mode, counters |

DEFLATE is implemented in-house (`core/deflate.c`) — fixed-Huffman plus
LZ77 matches, cross-checked against zlib in both directions — because the
alternative is linking a compression library into a 12 MB APK budget.

## Build

Requirements: JDK 17+, Android SDK 36, NDK r29/r30, CMake 3.22+.

```bash
./gradlew assembleDebug     # app/build/outputs/apk/debug/app-debug.apk
```

The native core can also be checked on a desktop host, without a device:

```bash
python3 tools/check_repo.py          # CMake lists, JNI contract, manifest, ABI
python3 tools/check_gles_shaders.py  # embedded ESSL sources
tools/compile_shaders.sh             # GLSL -> SPIR-V via the NDK glslc
```

`tools/compile_shaders.sh` is optional: without `glslc` the build still
succeeds, and selecting Vulkan reports *"SPIR-V shaders were not baked in"*
instead of crashing.

CI (`.github/workflows/build.yml`) runs on every push to `main`: it installs the
canary NDK, vendors LuaC, bakes the shaders, runs the static checks above, and
builds the APK with a pinned fallback toolchain if the newest one fails.

## Layout

```
app/src/main/cpp/
  include/diamond.h        public C ABI (the only contract Java sees)
  include/diamond/*.h      subsystem headers (internal, but stable in-tree)
  api/diamond_api.cpp      implementation of the public ABI
  core/                    logging, arena, strings, hash, time, deflate
  scene/                   scene graph, math, OBJ loader
  format/                  .diamond container
  logic/                   block graph, Lua codegen, executor, LuaC host
  render/                  backends, camera, GL helpers, software rasteriser
  platform/                render thread + ANativeWindow, asset/file IO
  jni/                     JNI bridge and JNI_OnLoad
  libengine.map            exported symbol allowlist
app/src/main/java/…        editor activity, viewport, scene tree, engine façade
shaders/                   GLSL for the Vulkan backend
tools/                     shader baking and static checks
```

## Design constraints

* **Size** — `-Os`, hidden visibility, `--gc-sections`, a version script that
  exports only the C ABI and the JNI entry points, two ABIs
  (`armeabi-v7a`, `arm64-v8a`), no x86.
* **Memory** — no exceptions, no RTTI, no iostreams, no `std::filesystem`.
  The scene graph is a flat array with arena-backed names.
* **Frame budget** — rendering happens on its own thread, capped at ~60 fps;
  Java never issues a call per frame.
* **Failure mode** — every subsystem reports through `diamond_fail()` and a
  caller-provided buffer, so the UI can show the real reason instead of dying.

## Licence

See `LICENSE`. Vendored third-party code keeps its original licence under
`app/src/main/cpp/third_party/`.
