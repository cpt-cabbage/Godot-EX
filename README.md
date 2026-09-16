# Godot-EX

> **Godot-EX is my personal fork of [Godot Engine](https://github.com/godotengine/godot)**,
> tracking upstream `master` and shaped around how I work and the game I'm building.

## Godot-EX Features

### Hardware ray tracing

Ray-traced lighting on the Forward+ renderer, built on Godot's own acceleration-structure API in
`RenderingDevice` rather than on any one GPU vendor. Where a backend has no ray-query support the
engine falls back to stock shadow maps, SDFGI and SSR.

| Backend | Status |
| --- | --- |
| **macOS / Metal** — Apple Silicon | Working, but still WIP (`--rendering-driver metal`) |
| **Windows / Vulkan** — NVIDIA RTX and other `VK_KHR_ray_query` GPUs | Planned; upstream's Vulkan driver implements the API, the passes have not been run on it yet |

**Metal driver**
- Acceleration structures and ray queries: `GL_EXT_ray_query` compute shaders → SPIR-V → MSL
- A BLAS per mesh and a per-frame TLAS over the scene, including skinned and blend-shaped meshes,
  multimeshes and GPU particles
- GPU timestamp queries, so `--gpu-profile` gives per-pass timings on Metal

**Shadows**
- Directional and area-light shadows traced in place of shadow maps
- Penumbrae from the sun's angular size and the light's size, temporal accumulation, depth-aware
  denoise
- Alpha-tested casters, per-light and per-instance control, traced volumetric-fog shadows

**Stochastic direct lighting**
- Many-light sampling with blue-noise candidates and visibility-guided light lists
- Area lights, screen-space contact traces, material-weighted light selection
- A ratio estimator with an SVGF-style variance-driven denoiser; half-resolution mode

**Global illumination**
- Hardware final gather with directional irradiance and traced specular occlusion
- Glossy reflections, with stock SSR skipped wherever the traced band covers it
- Depth-validated temporal history, an à-trous denoiser with propagated variance
- Restart on lighting changes, extra rays on young pixels, screen-radiance memory

**Surface cache**
- Per-instance orthographic material "cards" — albedo, normal, emission and depth atlases —
  read at every ray hit instead of running the material
- Lit on the GPU every frame under a budget: direct light with shadow rays, a world light grid,
  area lights, spot cookies and projectors, bounce histories for moving lights, a mip chain
- Deferred hit shading runs the scene shader's material in compute on the binned hits the cards
  cannot answer

**Transparency, mirrors and probes**
- A froxel translucency volume with traced bounce rays replaces the transparent pass's
  per-fragment light loops
- Planar mirrors detected from the scene, with image lights, image chains and glossy lobes through
  the direct and GI passes
- SDFGI probe rays traced with hardware ray queries; VoxelGI as a fallback radiance cache;
  reflection probes re-fit to the frame's irradiance

**Shared infrastructure**
- A prepass G-buffer (albedo, F0, flags) feeding the direct and GI passes
- Per-viewport temporal state, moving-object reprojection, frame-edge history borrowing,
  working-space luminance, NaN-safe histories
- MetalFX / FSR2 aware velocity and jitter
- Cluster builder: the bake as a compute cull, an exponential-depth cluster for the sampling pass

### Lights

- **AreaLight3D**: added visibility toggle and the ability to see in camera, its own gizmo (clicking
  the rect selects the light) and traced soft shadows
- **Range display**: selecting a light no longer draws the orange AABB selection box around its
  attenuation range, which read as a cube-shaped radius. The gizmo alone shows the range — a
  sphere for omni and area lights, a cone for spots — and an eye button next to `omni_range`,
  `spot_range` and `area_range` in the inspector shows or hides it per light, saved with the scene
- Shadow-map-only properties are hidden on lights whose shadows are traced

### Colour management

- OpenColorIO 2.4.2 vendored as a module, with a config-driven working space (e.g. ACEScg)
- OCIO views spliced into the tonemapper; display and view chosen separately
- Texture import converted into the working space

### Editor

- Gizmos kept out of SSR and screen-space shadow traces
- The editor keeps repainting until the temporal histories have settled, so a still viewport
  converges instead of freezing mid-denoise

---

# Godot Engine

<p align="center">
  <a href="https://godotengine.org">
    <img src="misc/logo/logo_outlined.svg" width="400" alt="Godot Engine logo">
  </a>
</p>

## 2D and 3D cross-platform game engine

**[Godot Engine](https://godotengine.org) is a feature-packed, cross-platform
game engine to create 2D and 3D games from a unified interface.** It provides a
comprehensive set of [common tools](https://godotengine.org/features), so that
users can focus on making games without having to reinvent the wheel. Games can
be exported with one click to a number of platforms, including the major desktop
platforms (Linux, macOS, Windows), mobile platforms (Android, iOS), as well as
Web-based platforms and [consoles](https://godotengine.org/consoles).

## Free, open source and community-driven

Godot is completely free and open source under the very permissive [MIT license](https://godotengine.org/license).
No strings attached, no royalties, nothing. The users' games are theirs, down
to the last line of engine code. Godot's development is fully independent and
community-driven, empowering users to help shape their engine to match their
expectations. It is supported by the [Godot Foundation](https://godot.foundation/)
not-for-profit.

Before being open sourced in [February 2014](https://github.com/godotengine/godot/commit/0b806ee0fc9097fa7bda7ac0109191c9c5e0a1ac),
Godot had been developed by [Juan Linietsky](https://github.com/reduz) and
[Ariel Manzur](https://github.com/punto-) for several years as an in-house
engine, used to publish several work-for-hire titles.

![Screenshot of a 3D scene in the Godot Engine editor](https://raw.githubusercontent.com/godotengine/godot-design/master/screenshots/editor_tps_demo_1920x1080.jpg)

## Getting the engine

### Binary downloads

Official binaries for the Godot editor and the export templates can be found
[on the Godot website](https://godotengine.org/download).

### Compiling from source

[See the official docs](https://docs.godotengine.org/en/latest/engine_details/development/compiling)
for compilation instructions for every supported platform.

## Community and contributing

Godot is not only an engine but an ever-growing community of users and engine
developers. The main community channels are listed [on the homepage](https://godotengine.org/community).

The best way to get in touch with the core engine developers is to join the
[Godot Contributors Chat](https://chat.godotengine.org).

To get started contributing to the project, see the [contributing guide](CONTRIBUTING.md).
This document also includes guidelines for reporting bugs.

## Documentation and demos

The official documentation is hosted on [Read the Docs](https://docs.godotengine.org).
It is maintained by the Godot community in its own [GitHub repository](https://github.com/godotengine/godot-docs).

The [class reference](https://docs.godotengine.org/en/latest/classes/)
is also accessible from the Godot editor.

We also maintain official demos in their own [GitHub repository](https://github.com/godotengine/godot-demo-projects)
as well as the [Asset Store](https://store.godotengine.org/).

There are also a number of other
[learning resources](https://docs.godotengine.org/en/latest/community/tutorials.html)
provided by the community, such as text and video tutorials, demos, etc.
Consult the [community channels](https://godotengine.org/community)
for more information.

[![Code Triagers Badge](https://www.codetriage.com/godotengine/godot/badges/users.svg)](https://www.codetriage.com/godotengine/godot)
[![Translate on Weblate](https://hosted.weblate.org/widgets/godot-engine/-/godot/svg-badge.svg)](https://hosted.weblate.org/engage/godot-engine/?utm_source=widget)
