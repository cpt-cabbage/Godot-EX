# Godot Engine — experimental ray-tracing fork

> **This is a personal fork of [Godot Engine](https://github.com/godotengine/godot)**, tracking
> upstream `master` and adding my own experimental rendering features on top. It targets the
> Forward+ renderer on macOS/Metal (Apple Silicon); everything else is stock Godot. Expect rough
> edges — this is a research playground, not a release.

## What this fork adds

- **Metal ray tracing**: hardware acceleration structures and ray queries in the Metal driver
  (`GL_EXT_ray_query` compute shaders → SPIR-V → MSL), per-mesh BLAS and a per-frame TLAS over
  the whole scene, including skinned/blend-shaped meshes, multimeshes and GPU particles; GPU
  timestamp queries for per-pass profiling on Metal
- **Ray-traced shadows**: directional and area-light shadows that replace the shadow maps —
  soft shadows from the sun's angular size, temporal accumulation, depth-aware denoise, alpha-tested
  casters, per-light and per-instance control, ray-traced volumetric fog shadows
- **Stochastic direct lighting**: many-light sampling with blue-noise
  candidates, visibility-guided light lists, screen-space contact traces, area lights, a ratio
  estimator with an SVGF-style variance-driven denoiser, half-resolution mode
- **Ray-traced GI**: hardware final gather with directional irradiance and traced specular
  occlusion, glossy reflections (stock SSR skipped where they cover it), depth-validated temporal
  history, a-trous denoiser with propagated variance, restart on lighting changes, extra rays on
  young pixels, screen-radiance memory
- **Surface cache**: per-instance orthographic material "cards" (albedo/normal/emission/depth
  atlases) lit on the GPU every frame with a budget — direct light with shadow rays, a world light
  grid, area lights, spot cookies/projectors, bounce histories for moving lights, a mip chain —
  and read at every ray hit instead of running the material
- **Deferred hit shading**: the scene shader's material runs in compute on binned ray hits the
  cards cannot answer
- **Translucency volume**: a froxel lighting volume for the transparent pass, with traced bounce
  rays, replacing the per-fragment light loops
- **Planar mirrors**: detected from the scene, with image lights, image chains and glossy lobes
  flowing through the stochastic direct and GI passes
- **Prepass G-buffer**: albedo, F0 and flags from the depth prepass feeding the direct and GI
  passes; material-weighted light selection
- **Denoiser infrastructure**: per-viewport temporal state, moving-object reprojection,
  frame-edge history borrowing, working-space luminance, NaN-safe histories, MetalFX / FSR2
  aware velocity
- **SDFGI / VoxelGI**: SDFGI probe rays traced with hardware ray queries; VoxelGI as a fallback
  radiance cache; reflection probes re-fit to the frame's irradiance
- **AreaLight3D**: a visible emitting rect, gizmo selection and range display for lights
- **Colour management (OpenColorIO)**: vendored OCIO 2.4.2, a config-driven working space
  (e.g. ACEScg), OCIO views spliced into the tonemapper, texture import converted into the working
  space, display and view chosen separately
- **Cluster builder**: the bake as a compute cull, an exponential-depth cluster for the sampling pass
- **Editor**: gizmos kept out of SSR/screen-space traces; the editor keeps repainting until the
  temporal histories have settled

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
