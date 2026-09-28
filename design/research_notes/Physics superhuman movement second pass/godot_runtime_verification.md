# Godot runtime verification of the Jolt joint-drive chain (Phase 0)

Built 2026-09-27 and run 2026-09-27/28 (UTC) against fork commit `efc1d2ea978f675e5a184cf0adab57dbb15cbfd3`
(`4.8.dev.custom_build.efc1d2ea9`, Jolt 5.6.0 vendored in `thirdparty/jolt_physics`).
The design report's Phase 0 lists four engine claims as "verified in source but not at
runtime". This note records a headless build of the fork, a small GDScript test project that
exercises each claim, and the numbers it printed. Everything here was produced by running the
engine; where something is still only a source-read inference it is marked as such.

**Outcome in one paragraph.** All four claims hold at runtime. `Generic6DOFJoint3D.set_angular_target_rotation()`
plus `PARAM_ANGULAR_DRIVE_TORQUE_LIMIT` is a working solver-side orientation drive under Jolt
(a 1 kg, 1 m bar tracks a 45 deg / 0.25 Hz swing with 1.12 deg mean error, 0.38 deg with
velocity feed-forward; a 2 Nm cap holds the bar at 23.1 deg against a 24.1 deg analytic
equilibrium). The identical drive is reachable for a `PhysicalBone3D` through `get_joint_rid()`
and `PhysicsServer3D.generic_6dof_joint_set_angular_target_rotation()` /
`generic_6dof_joint_set_param(rid, axis, G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT, v)`, with
bit-identical tracking numbers, **provided the script also calls
`PhysicsServer3D.joint_disable_collisions_between_bodies(rid, true)`**: under Jolt a
server-created joint leaves the two bodies colliding (GodotPhysics and the `Joint3D` nodes
disable that by default; `PhysicalBone3D::_reload_joint()` does not), and without it the bone
bodies fight the drive (7.1 deg mean error, cap case stuck at 0.3 deg). 120 Hz with physics
interpolation runs, tick-rate changes at runtime work, and the drive's behaviour is the same at
60, 120 and 240 Hz. There is no sub-stepping (`physics_system->Update(p_step, 1, ...)`), and the
measured per-step cost is flat, so per-second cost scales linearly with tick rate: a 15-body /
14-drive chain costs ~200 us per step with four Jolt worker threads (~80 us single-threaded) on
this 2.1 GHz VM, i.e. 2.5 % of the 120 Hz tick budget and 4.8 % of the 240 Hz budget.

## 1. Machine and toolchain

| item | value |
| --- | --- |
| CPU | Intel Xeon @ 2.10 GHz, 4 cores / 4 threads (VM), L2 4x2 MiB, L3 260 MiB reported |
| RAM | 15 GiB, no swap |
| OS / kernel | Ubuntu 24.04 userland, Linux 6.18.44 |
| compiler / linker | Ubuntu clang 18.1.3, LLD 18.1.3 (`use_llvm=yes linker=lld`); gcc 13.3 also present, unused |
| Python / SCons | Python 3.11.15, SCons 4.11.1 (`pip install scons`; not preinstalled) |
| disk | 29 GB free before the build; `bin/obj` is 277 MB after it |

## 2. Build

Options were checked against `SConstruct` (`grep opts.Add`), `platform/linuxbsd/detect.py` and
the modules' `config.py` before use. Exact command (repository root):

```
scons platform=linuxbsd target=template_debug arch=x86_64 use_llvm=yes linker=lld \
  optimize=speed debug_symbols=no lto=none scu_build=yes scu_limit=32 \
  disable_path_overrides=no disable_advanced_gui=yes disable_physics_2d=yes \
  disable_navigation_2d=yes disable_navigation_3d=yes disable_xr=yes \
  rendering_device=no vulkan=no opengl3=no forward_plus_renderer=no forward_mobile_renderer=no \
  sdl=no accesskit=no wayland=no alsa=no pulseaudio=no speechd=no dbus=no udev=no fontconfig=no libdecor=no \
  modules_enabled_by_default=no module_gdscript_enabled=yes module_jolt_physics_enabled=yes \
  module_text_server_fb_enabled=yes module_freetype_enabled=yes progress=no -j4
```

| item | value |
| --- | --- |
| wall time | **6 min 35 s** (`real 6m35.119s`, `user 18m20s`, `sys 0m40s`; 17:48:27Z to 17:55:02Z); memory use was about 2 GB when sampled mid-build |
| result | exit 0, one compiler warning (`platform/linuxbsd/x11/display_server_x11.cpp:7149` unused variable `executable_name`) |
| binary | `bin/godot.linuxbsd.template_debug.x86_64.llvm`, **50 826 760 bytes (48.5 MiB)**, ELF x86-64 PIE; `bin/` is gitignored (`git check-ignore bin` matches `.gitignore:263`) |
| `--version` | `4.8.dev.custom_build.efc1d2ea9` |

Notes on the option set:

- `disable_path_overrides=no` is mandatory for a template build: export templates default to
  `disable_path_overrides=True`, and `--path`, `--script` and running from a CWD `project.godot`
  are refused (`main/main.cpp:1736,1773,4085`). An editor build does not need it.
- `rendering_device=no` (with `vulkan=no opengl3=no forward_*_renderer=no`) removes
  `servers/rendering/renderer_rd` from the build, which is where the fork's ray-tracing work
  lives (`git log` shows only `renderer_rd/shaders/**` and `glsl_builders.py` touched by the
  "Ray tracing:" commits). The headless build therefore neither compiled nor exercised that
  code; the `glslang`, `ocio` and `texture_streaming` modules are auto-disabled by it.
- `modules_enabled_by_default=no` plus four explicit enables replaced the long
  `module_*_enabled=no` list in the brief. `text_server_fb` is off by default and lists
  `freetype`, `msdfgen`, `svg` as *optional* dependencies (`modules/text_server_fb/config.py`);
  `gdscript` lists `jsonrpc`, `websocket` as optional. Nothing refused to be dropped.
- Spelling differences from the brief: `use_lto=no` does not exist, the option is `lto=none`
  (default); `module_navigation_3d`, `raycast`, `theora`, `webxr` have `can_build` returning
  `False` in this fork anyway; `disable_physics_2d=yes`, `disable_navigation_*`, `disable_xr=yes`
  exist as top-level switches and were used instead of the per-module flags.
- `deprecated=no` was **not** used: the saving is negligible and a failure late in the build
  would have forced a full rebuild (the define is global). `optimize=speed` was used so the
  benchmark reflects release-like codegen; `template_debug` keeps `DEBUG_ENABLED` (warnings,
  `WARN_PRINT_ONCE`), which is what surfaced the Jolt job-system warning below.
- `scu_build=yes scu_limit=32` (single compilation units of at most 32 sources) is what made a
  six-and-a-half-minute build possible on four cores; RAM never became an issue at that limit.
- `sdl=no accesskit=no wayland=no ...` only trim drivers that a headless run cannot use
  (`wayland-scanner` was missing anyway, and AccessKit needs an SDK download).

## 3. Test project

`design/prototype/phase0_joint_drive/` (see its `README.md` for usage). `project.godot` sets
`physics/3d/physics_engine="Jolt Physics"` (the string is
`PhysicsServer3DManager::JOLT_PHYSICS_NAME`), `physics/common/physics_ticks_per_second=120`,
`physics/common/max_physics_steps_per_frame=16` and `physics/common/physics_interpolation=true`.
The main scene is a `Node3D` with `main.gd`; the three tests build their scenes in GDScript, so
nothing needs importing and a template build can run the folder directly. Test selection and CSV
verbosity are command-line user arguments after `--`. The runner starts by printing an API
presence table (`ClassDB.class_has_method` / `class_has_integer_constant`) and ends with a
`SUMMARY` block and exit code 0/1.

Run command used for the correctness suites (about 2 s wall):

```
bin/godot.linuxbsd.template_debug.x86_64.llvm --headless --fixed-fps 10 \
  --path design/prototype/phase0_joint_drive -- --test=pendulum,physical_bone --csv=none --out=<dir>
```

and for the benchmark, in real time so nothing else shares the frame:

```
bin/godot.linuxbsd.template_debug.x86_64.llvm --headless \
  --path design/prototype/phase0_joint_drive -- --test=chain_bench --csv=none
```

Runner header as printed:

```
[phase0] Godot 4.8-dev (custom_build) (hash efc1d2ea978f675e5a184cf0adab57dbb15cbfd3) | PhysicsServer3D class=PhysicsServer3D | physics/3d/physics_engine=Jolt Physics
[phase0] physics_ticks_per_second=120 max_physics_steps_per_frame=16 physics_interpolation=true jitter_fix=0.00
[phase0] CPU=Intel(R) Xeon(R) Processor @ 2.10GHz x4 | OS=Linux
```

(`jitter_fix=0.00` because enabling physics interpolation disables the jitter fix, as the
engine note says. `PhysicsServer3D.get_class()` returns the base name and is not a way to tell
which backend runs; Jolt is established here by the module set of the binary, the project
setting, and the Jolt-specific warnings in the log.)

## 4. Results

### 4.1 Test 1: `Generic6DOFJoint3D` orientation drive (`tests/pendulum_test.gd`)

Rig: `StaticBody3D` anchor (0.2 m box) at the origin; `RigidBody3D` bar 0.1 x 1.0 x 0.1 m,
1 kg, centre at y = -0.5 (inertia about the pivot 0.334 kg m^2), gravity 9.8, body damping set
to `DAMP_MODE_REPLACE` 0 (3.0 in the static cap cases to settle faster); `Generic6DOFJoint3D`
at the origin, `node_a` = anchor, `node_b` = bar, for all three angular axes
`FLAG_ENABLE_ANGULAR_LIMIT=false`, `FLAG_ENABLE_ANGULAR_SPRING=true`,
`PARAM_ANGULAR_SPRING_STIFFNESS=k`, `PARAM_ANGULAR_SPRING_DAMPING=c`,
`PARAM_ANGULAR_DRIVE_TORQUE_LIMIT=tau`; linear axes left at their default locked limits. Each
tick the script measures the bar's rotation relative to the anchor against the target the solver
was given on the previous tick (error = angle of `q_rel^-1 * q_target`), then sets the next
target `Quaternion(Vector3(0,0,1), theta(t))`. Jolt applies the target as
`SixDOFConstraint::SetTargetOrientationBS`, i.e. `R_bar = R_anchor * q` in body space
(`modules/jolt_physics/joints/jolt_generic_6dof_joint_3d.cpp:214-226`); the measured signed
angle confirms that convention (target -45 deg gives actual -44.8 deg, no sign flip, no Euler
decomposition).

| case | tps | k / c / tau | target | result | mean err | max err | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `sine_120hz_spring` | 120 | 2000 / 50 / 1000 | 45 deg sin(2 pi 0.25 t) | tracks | **1.124 deg** | 1.764 deg | PASS |
| `sine_120hz_spring_ff_neg` | 120 | same + angular motor, target velocity = -(d theta/dt) | same | tracks | **0.380 deg** | 0.597 deg | PASS |
| `sine_120hz_spring_ff_pos` | 120 | same, target velocity = +(d theta/dt) | same | worse than no feed-forward | 2.621 deg | 4.112 deg | PASS (threshold), wrong sign |
| `sine_60hz_spring` | 60 | 2000 / 50 / 1000 | same | tracks | 1.126 deg | 1.763 deg | PASS |
| `sine_240hz_spring` | 240 | 2000 / 50 / 1000 | same | tracks | 1.123 deg | 1.765 deg | PASS |
| `cap_2Nm_static45_120hz` | 120 | 2000 / 50 / **2.0** | 45 deg static | settles at **23.08 deg**; analytic `asin(2 / (1 * 9.8 * 0.5))` = 24.09 deg; deviation 1.01 deg | (21.9 deg vs target, by design) | | PASS |
| `cap_1000Nm_static45_120hz` | 120 | 2000 / 50 / 1000 | 45 deg static | settles at 44.90 deg (steady-state 0.10 deg = gravity torque 3.46 Nm / k) | 0.105 deg | | PASS |
| `stiff_1e6_c0_60hz` | 60 | 1e6 / **0** / 1e12 | 45 deg static | finite; peak 47.1 rad/s on tick 1, within 0.2 deg by tick 5 | 0.0000 deg (t >= 2 s) | 0.0000 deg | PASS |
| `stiff_1e6_c0_120hz` | 120 | 1e6 / **0** / 1e12 | 45 deg static | finite; peak 43.4 rad/s on tick 1, within 0.2 deg by tick 5 | 0.0000 deg | 0.0000 deg | PASS |
| `sleep_wake_120hz_send_on_change` | 120 | 2000 / 50 / 1000, `can_sleep=true` | 0 deg held (sent once), step to 45 deg at t = 3 s | `sleeping=true` before the step; woke and reached 44.7 deg within 0.12 s | 0.105 deg (t >= 5 s) | | PASS |
| `sleep_wake_120hz_send_every_tick` | 120 | same, unchanged target re-sent every tick | same | never slept (`sleeping=false`), tracked | 0.105 deg | | INFO |

CSV excerpt, `sine_120hz_spring` (every 0.5 s; columns `tick,time_s,target_deg,actual_deg,error_deg,angvel_rad_s`):

```
240,2.00000,0.5890,2.3421,1.7530,1.2279
300,2.50000,-31.4006,-30.0262,1.3740,0.9196
360,3.00000,-44.9961,-44.8121,0.1856,0.0723
420,3.50000,-32.2336,-33.3405,1.1071,0.8174
480,4.00000,-0.5890,-2.3421,1.7530,1.2279
```

Same with velocity feed-forward (`ff_neg`): `240,2.00000,0.5890,-0.0039,0.5935,1.2317` ...
`360,3.00000,-44.9961,-44.9349,0.0685,0.0080`. The 1.1 deg error without feed-forward is the
expected lag of a `Position` motor whose damping term opposes the body's own velocity
(`c * theta_dot / k` = 50 * 1.23 / 2000 = 0.031 rad = 1.8 deg at peak rate), and it is the
same at 60, 120 and 240 Hz: for this soft drive the implicit solver spring is tick-rate
independent, in contrast to the hinge motor's step-dependent torque conversion flagged in the
engine note.

Torque-cap tail (`cap_2Nm_static45_120hz`): `720,6.00000,45.0000,26.5525,18.4476,0.0291`; the
mean over the last second is 23.08 deg because the bar is still creeping toward the 24.09 deg
equilibrium against the added body damping, well within the 3 deg criterion. The cap is applied
by Jolt as a per-axis impulse clamp `[-tau*dt, +tau*dt]` on the motor lambda
(`MotorSettings::SetTorqueLimit`, `jolt_generic_6dof_joint_3d.cpp:147-168`), so it is a true
torque ceiling, not a spring-strength scale.

Stiff-spring transient at 60 Hz (`k = 1e6 Nm/rad`, zero damping):

```
1,0.01667,45.0000,26.8049,18.1952,47.1239
2,0.03333,45.0000,49.3900,4.3900,44.4052
3,0.05000,45.0000,46.9960,1.9960,7.0662
4,0.06667,45.0000,44.3537,0.6465,6.9945
5,0.08333,45.0000,45.1941,0.1938,0.8656
```

Sleep/wake step (`send_on_change`): `360,3.00000,0.0000,0.0000,0.0000,0.0000` then
`362,3.01667,45.0000,15.1633,29.8367,17.6971` ... `374,3.11667,45.0000,44.6627,0.3380,0.2936`.

### 4.2 Test 2: `PhysicalBone3D` + `PhysicsServer3D` path (`tests/physical_bone_test.gd`)

Rig: `Skeleton3D` with bones `root` (rest identity) and `child` (rest translated (0, -0.1, 0));
`PhysicalBoneSimulator3D` child of the skeleton; `PhysicalBone3D` `PB_root` (bone `root`, 0.2 m
box) and `PB_child` (bone `child`, `joint_type = JOINT_TYPE_6DOF`,
`body_offset = Transform3D(Basis(), Vector3(0, -0.5, 0))`, 0.1 x 1.0 x 0.1 box, 1 kg,
`can_sleep=false`). `bone_name` is a dynamic property, set with `set("bone_name", "child")`.
The spring is enabled through the node's `joint_constraints/<x|y|z>/angular_limit_enabled=false`,
`angular_spring_enabled=true`, `angular_spring_stiffness=2000`, `angular_spring_damping=50`
properties (these are re-applied by `_reload_joint()`, unlike anything set through the server).
`simulator.physical_bones_start_simulation([&"child"])` simulates only the child, so the root
stays kinematic as the anchor. Then:

```
joint_rid = pb_child.get_joint_rid()
PhysicsServer3D.joint_disable_collisions_between_bodies(joint_rid, true)   # see below
PhysicsServer3D.generic_6dof_joint_set_param(joint_rid, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT, tau)  # x, y, z
# every tick:
PhysicsServer3D.generic_6dof_joint_set_angular_target_rotation(joint_rid, Quaternion(Vector3(0,0,1), theta))
```

Chain checks printed at start-up: `get_joint_rid().is_valid()=true`, `joint_get_type=4
(JOINT_TYPE_6DOF=4)`, `child.is_simulating_physics()=true`, `root.is_simulating_physics()=false`,
server readback on the Z axis `angular_spring flag=true stiffness=2000.0 damping=50.0
drive_torque_limit=1000.0 angular_limit flag=false`, `joint_is_disabled_collisions_between_bodies=true`,
`child.get_collision_exceptions()=[&"PB_root"]`.

| case | collision exception | result | mean err | max err | verdict |
| --- | --- | --- | --- | --- | --- |
| `pb_sine_120hz` | yes | tracks; numbers identical to Test 1 to four decimals | **1.124 deg** | 1.764 deg | PASS |
| `pb_cap_2Nm_static45_120hz` | yes | settles at **23.080 deg** (analytic 24.09) | | | PASS |
| `pb_sine_120hz_colliding` | **no** | bodies collide at the joint; drive disturbed | 7.144 deg | 10.946 deg | INFO (documents the failure) |
| `pb_cap_2Nm_static45_120hz_colliding` | **no** | stuck at 0.276 deg (contact friction exceeds the 2 Nm cap) | | | INFO |

The server target readback `generic_6dof_joint_get_angular_target_rotation(rid)` returned the
last quaternion set in every case. The CSV of `pb_sine_120hz` is row-for-row identical to
`sine_120hz_spring` (e.g. `240,2.00000,0.5890,2.3421,1.7530,1.2279`), which is what one expects
if node and server paths end in the same `JoltGeneric6DOFJoint3D` calls; the `_colliding` CSV
shows the disturbance: `300,2.50000,-31.4006,-24.7301,8.3040,0.8817`.

Two findings the report should absorb:

1. **Jolt joints do not exclude collision between their bodies by default.**
   `JoltJoint3D::collision_disabled = false` (`modules/jolt_physics/joints/jolt_joint_3d.h:47`),
   whereas GodotPhysics3D constraints default to `disabled_collisions_between_bodies = true`
   (`modules/godot_physics_3d/godot_constraint_3d.h:54`). `Joint3D` nodes call
   `joint_disable_collisions_between_bodies(joint, exclude_nodes_from_collision)` (default true,
   `scene/3d/physics/joints/joint_3d.cpp:124`), which is why Test 1 needed nothing; `PhysicalBone3D::_reload_joint()`
   (`scene/3d/physics/physical_bone_3d.cpp:950-1043`) never does. A Jolt ragdoll built from
   `PhysicalBone3D` must therefore call `PhysicsServer3D.joint_disable_collisions_between_bodies(pb.get_joint_rid(), true)`
   per bone (the flag survives `_reload_joint()` because the new `JoltJoint3D` copies it from the
   old one, `jolt_joint_3d.cpp:114`), or use `PhysicalBoneSimulator3D.physical_bones_add_collision_exception()`
   / `PhysicsBody3D.add_collision_exception_with()`, or keep adjacent shapes from overlapping.
2. **The simulated pose is only visible to script inside `skeleton_updated`.** `Skeleton3D`
   saves the bone poses before running modifiers and restores them afterwards
   (`scene/3d/skeleton_3d.cpp:363-457`), so `skeleton.get_bone_global_pose(child)` read from
   `_physics_process` returned 0.000 deg while the body was at 26.55 deg; the same call inside a
   `skeleton_updated` handler returned 26.50 deg (61 signals over the 6 s case, one per process
   frame at the batched rate). Skins see the simulated pose; scripts that want it must read it
   in that signal or from the `PhysicalBone3D` nodes.

### 4.3 Test 3: 15-body / 14-drive chain cost (`tests/chain_benchmark.gd`)

Rig: 15 `RigidBody3D` boxes 0.1 x 0.5 x 0.1 m, 1 kg, `can_sleep=false`, hanging in a line;
body 0 `freeze=true` (static anchor); 14 `Generic6DOFJoint3D` between consecutive bodies with
angular limits off, angular spring k = 2000 Nm/rad, c = 50 Nm s/rad, cap 200 Nm; each tick the
script sets `set_angular_target_rotation(Quaternion(Z, 10 deg * sin(pi t + 0.4 i)))` on all 14.
After 60 warm-up ticks, 600 ticks are measured. The per-step figure is the wall-clock gap from
the end of the test node's `_physics_process` on tick N to its start on tick N+1 when both ticks
belong to the same process frame; that interval contains `PhysicsServer3D::end_sync()`,
`step()` (the Jolt `PhysicsSystem::Update`), the next `sync()` and `flush_queries()` (body
transform write-back to the nodes) and the scene-tree dispatch, and nothing else. Ticks were
batched 2/4/8 per frame by `Engine.max_fps = 30`. The time the script spends issuing the 14
target calls is reported separately.

Per-step wall time in microseconds (n = samples):

| configuration | 60 Hz driven | 120 Hz driven | 240 Hz driven | 120 Hz passive (spring off) |
| --- | --- | --- | --- | --- |
| 4 worker threads, real time (primary) | mean **175.5**, p50 165, p95 291, max 457 (n=300) | mean **209.4**, p50 194, p95 356, max 619 (n=450) | mean **201.3**, p50 199, p95 300, max 431 (n=525) | mean **159.8**, p50 151, p95 281, max 368 (n=450) |
| 4 worker threads, `--fixed-fps 30` | mean 209.6, p50 203, p95 340, max 430 | mean 200.1, p50 198, p95 279, max 410 | mean 196.6, p50 195, p95 308, max 1973 | mean 202.7, p50 194, p95 401, max 3982 |
| 1 worker thread (`threading/worker_pool/max_threads=1` via `override.cfg`), real time | mean **88.4**, p50 80, p95 135, max 340 | mean **77.8**, p50 73, p95 125, max 208 | mean **80.9**, p50 78, p95 123, max 195 | mean **65.2**, p50 66, p95 113, max 156 |
| script cost of 14 `set_angular_target_rotation` calls | 15.5 | 14.6 | 11.9 | (1.0, no calls) |
| tick budget | 16 667 | 8 333 | 4 167 | 8 333 |
| load, 4 threads / 1 thread | **1.05 % / 0.53 %** | **2.51 % / 0.93 %** | **4.83 % / 1.94 %** | 1.92 % / 0.78 % |

Reading the numbers:

- The cost per step is flat across 60, 120 and 240 Hz (about 200 us with four threads, about
  80 us with one), so the cost per second is linear in the tick rate, as the report assumed from
  the docs; 240 Hz costs twice 120 Hz, which costs twice 60 Hz.
- At 15 bodies the multi-threaded job dispatch dominates: one worker thread is 2.5x faster per
  step than four. The Jolt module sizes its job system from `WorkerThreadPool::get_thread_count()`
  (`modules/jolt_physics/spaces/jolt_job_system.cpp:161-165`) and each step goes through
  `WorkerThreadPool::add_native_task`. A single ragdoll is far below the body count where the
  parallel solver pays off; this is worth a follow-up with a whole scene, and it means small-scene
  cost figures should be quoted with the thread count.
- The 14 drives add roughly 15-50 us per step over the passive chain with four threads
  (13 us single-threaded), i.e. 1-3.5 us per driven joint; the GDScript side of pushing 14
  quaternion targets costs another 10-15 us. Neither is a concern at any of the three rates.
- `Performance.TIME_PHYSICS_PROCESS` is the largest single physics iteration in the last second
  (`main/main.cpp:5045-5046,5137`), so its maxima (2.0 ms at 120 Hz) include the ticks in which
  the rig was built or freed and are not per-step figures; under `--fixed-fps` it read 0.0. The
  direct gap measurement above is the number to use.
- `Performance.PHYSICS_3D_ACTIVE_OBJECTS`, `PHYSICS_3D_COLLISION_PAIRS` and
  `PHYSICS_3D_ISLAND_COUNT` are always 0 under Jolt: `JoltPhysicsServer3D::get_process_info()`
  returns 0 (`modules/jolt_physics/jolt_physics_server_3d.cpp:1687-1689`). The report's plan to
  profile with the `Performance.PHYSICS_3D_*` monitors will not work on this backend.
- The end-of-run joint error printed by the benchmark (1.5 deg at 120 Hz, 13.7 deg at 240 Hz,
  33.9 deg at 60 Hz) is a sanity check, not a tracking metric: the 200 Nm cap is below the static
  gravity load on the upper joints (about 14 kg x 9.8 x 3.5 m lever), so those joints sag by
  design, and the cases end at different simulated times (11 s, 5.5 s, 2.75 s).

## 5. API names: what worked and what did not exist as assumed

Present and working exactly as the engine note names them (runner output `present` / constant values):

- `Generic6DOFJoint3D.set_angular_target_rotation(Quaternion)`, `get_angular_target_rotation()`,
  `has_target_rotation()`, `clear_angular_target_rotation()` (note the third is
  `has_target_rotation`, not `has_angular_target_rotation`, matching `doc/classes/Generic6DOFJoint3D.xml:61`).
- `Generic6DOFJoint3D.PARAM_ANGULAR_DRIVE_TORQUE_LIMIT = 23`, `PARAM_LINEAR_DRIVE_FORCE_LIMIT = 22`,
  `PARAM_ANGULAR_SPRING_STIFFNESS = 19`, `FLAG_ENABLE_ANGULAR_SPRING = 2`; set through
  `set_param_x/y/z` and `set_flag_x/y/z` (there is no per-axis-argument setter on the node).
- `PhysicsServer3D.generic_6dof_joint_set_angular_target_rotation(joint, target_rotation)`,
  `generic_6dof_joint_get_angular_target_rotation(joint)`, `generic_6dof_joint_set_param(joint, axis, param, value)`,
  `generic_6dof_joint_set_flag(joint, axis, flag, enable)`, `joint_get_type`, `joint_disable_collisions_between_bodies`,
  `joint_is_disabled_collisions_between_bodies`; constants
  `G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT = 23`, `G6DOF_JOINT_FLAG_ENABLE_ANGULAR_SPRING = 2`,
  `G6DOF_JOINT_ANGULAR_SPRING_STIFFNESS = 19`, `JOINT_TYPE_6DOF = 4`.
- `PhysicalBone3D.get_joint_rid()`, `joint_type`, `body_offset`, `is_simulating_physics()`,
  `get_bone_id()`, `angular_velocity`, `DAMP_MODE_REPLACE`; `PhysicalBoneSimulator3D.physical_bones_start_simulation(Array[StringName])`.
- Project settings `physics/3d/physics_engine`, `physics/common/physics_ticks_per_second`,
  `physics/common/max_physics_steps_per_frame`, `physics/common/physics_interpolation`; runtime
  `Engine.physics_ticks_per_second` (changed between cases without restarting), `Engine.max_physics_steps_per_frame`,
  `SceneTree.physics_interpolation`.
- CLI: `--headless`, `--path`, `--fixed-fps N`, `--quit-after N` (frames, not ticks; the runner quits itself).

Not as assumed, or worth knowing:

- Velocity feed-forward sign: with `FLAG_ENABLE_ANGULAR_MOTOR` on (Jolt `PositionAndVelocity`),
  `PARAM_ANGULAR_MOTOR_TARGET_VELOCITY` must be set to **minus** the target's right-handed
  angular rate about the axis (0.38 deg error) - the positive sign made tracking worse than no
  feed-forward (2.62 deg). The Jolt wrapper negates motor velocities and Euler equilibria to keep
  GodotPhysics' legacy convention (`jolt_generic_6dof_joint_3d.cpp:127-144,190-211`); the
  quaternion target itself is passed through unchanged.
- `PhysicalBone3D.bone_name` is not a declared property (`_get_property_list` adds it), so from
  GDScript it is `pb.set("bone_name", ...)`; the same holds for `joint_constraints/<axis>/...`.
- `PhysicsServer3D.get_class()` returns `"PhysicsServer3D"`; it does not identify the backend.
- `Performance.PHYSICS_3D_*` counters are zero under Jolt; `TIME_PHYSICS_PROCESS` is a
  one-second maximum, not a per-step time.
- `Skeleton3D.get_bone_global_pose()` shows the simulated pose only inside `skeleton_updated`.
- Start-up prints `ERROR: Could not load global script cache` because the project was never
  opened in an editor (no `.godot/global_script_class_cache.cfg`); harmless as long as no script
  uses `class_name` (the tests extend by path for that reason).

## 6. Observed limits

- **Torque cap engages** with the expected magnitude: 2 Nm holds the bar at 23.1 deg versus the
  24.1 deg analytic equilibrium against the 3.46 Nm gravity torque, through both the node and
  the server path; 1000 Nm reaches 44.9 deg. Without a collision exception the PhysicalBone3D
  case shows the cap being defeated by contact friction (0.28 deg) - a diagnostic for "drive
  looks dead" symptoms.
- **Large stiffness does not explode at 60 Hz or 120 Hz.** `k = 1e6 Nm/rad` with zero damping
  produced a one-tick spike (47 rad/s at 60 Hz, 43 rad/s at 120 Hz) and converged to the target
  within five ticks at both rates, with no non-finite values. This matches Jolt's soft-constraint
  formulation (`SpringPart::CalculateSpringPropertiesWithStiffnessAndDamping`, implicit Euler,
  "unconditionally stable but has built in damping"), and it answers the report's open question:
  a spring with stiffness and **zero damping does count as active** (Jolt's Position motor only
  requires `SpringSettings::HasStiffness()`, `SixDOFConstraint.cpp:571-577`). The practical limit
  is therefore not stability but that very stiff drives become effectively rigid within the
  tick; the report's 120 Hz choice is about drive frequency headroom and contact fidelity, not
  about avoiding spring blow-up.
- **Tick-rate independence of the drive.** Errors of 1.126 / 1.124 / 1.123 deg at 60 / 120 /
  240 Hz for k = 2000, c = 50. Untested here: stiff drives under contact and the hinge motor's
  step-dependent torque conversion the engine note flags.
- **Sleeping.** Every `set_angular_target_rotation` call wakes both bodies
  (`JoltGeneric6DOFJoint3D::set_angular_target_rotation` ends with `_wake_up_bodies()`,
  `jolt_generic_6dof_joint_3d.cpp:544-550`), so a rig that pushes targets every tick never
  sleeps even when the pose is static. Sending only on change lets the bar sleep in under 3 s
  and the next target wakes it (45 deg reached in 0.12 s). An idle ragdoll should stop pushing
  targets if it is meant to sleep.
- **PhysicalBone3D joints need explicit collision exclusion under Jolt** (section 4.2).
- **Jolt job-system warning when several ticks run in one frame.** `WARNING: Jolt Physics job
  system exceeded the maximum number of jobs. This should not happen. Please report this.
  Waiting for jobs to become available...` (`modules/jolt_physics/spaces/jolt_job_system.cpp:124`)
  appeared once (it is a `WARN_PRINT_ONCE`) in every run that batched 4 or more ticks per frame
  (`--fixed-fps 30/20/10`, and the `Engine.max_fps = 30` benchmark under fixed-fps), and never in
  the real-time runs (1 tick per frame, 720 ticks). Jobs are reclaimed after every step
  (`JoltPhysicsServer3D::step`, `jolt_physics_server_3d.cpp:1648-1654`), so this is not a plain
  per-frame leak; the handler sleeps 100 us and retries, so the cost is small, but the same
  batching happens legitimately whenever `max_physics_steps_per_frame` catches up after a hitch
  at 120 Hz. Worth a look inside `modules/jolt_physics` during the Phase 0 patch work; the
  benchmark's occasional 2-4 ms `max` outliers under `--fixed-fps` may be this wait or ordinary
  VM scheduling.
- **Server-set joint parameters on a PhysicalBone3D are not owned by the node** (source-read
  inference, not exercised): `_reload_joint()` re-creates the joint from the node's stored
  `joint_constraints/*` values whenever `joint_type`, `body_offset`/`joint_offset`, the
  physical-bone parent or tree membership changes, so a drive torque limit or target set through
  the server must be re-sent after such events. `collision_disabled` is the one server-side flag
  that is carried over.
- **Physics monitors** (`Performance.PHYSICS_3D_*`) are not usable for profiling on Jolt (section 4.3).

## 7. What remains unverified

- Cost and stability of a full humanoid ragdoll (15 bodies with realistic limits, self-contact,
  ground contact, capsule shapes) rather than a hanging box chain; the thread-count effect in a
  scene with other physics.
- Cone-twist / hinge `Position` motors, the Jolt-only parameter bindings and the applied-force
  readback (`GetTotalLambda`) that the report lists as Phase 0 patches - none exist yet, nothing to run.
- Whether `_integrate_forces` on a `PhysicalBone3D` receives reliable contact impulses.
- The `_reload_joint()` parameter-loss inference above, and behaviour of the target when
  angular limits are enabled (Jolt clamps `SetTargetOrientationCS` to the swing/twist limits,
  `SixDOFConstraint.cpp:343-348`; untested here).
- GodotPhysics3D fallback behaviour (the module was not compiled into this binary; the docs say
  the target is ignored and the getter returns identity).
- Rendering-side physics interpolation was enabled but not observed (headless, no frames drawn).
- The fork's ray-tracing renderer was excluded from this build and is untouched by these tests.

## 8. How to reproduce locally

1. Toolchain: clang or gcc, `pkg-config`, Python 3, `pip install scons`. Build with the exact
   command in section 2 from the repository root (drop `use_llvm=yes linker=lld` for gcc;
   `-j$(nproc)`). Expect 5-10 minutes on four cores; the binary lands in `bin/`, which is
   gitignored. For an editor build instead, `scons platform=linuxbsd target=editor` also works
   with the project (the `disable_path_overrides` switch is template-only).
2. Run everything: `design/prototype/phase0_joint_drive/run_tests.sh bin/godot.linuxbsd.template_debug.x86_64.llvm`
   (about 3 s; exit code 0 means every non-INFO case passed).
3. Repeat the benchmark numbers in real time, alone on the machine:
   `bin/<binary> --headless --path design/prototype/phase0_joint_drive -- --test=chain_bench --csv=none`
   (25 s). For the single-thread row, copy the project folder somewhere writable, add an
   `override.cfg` containing `[threading]` / `worker_pool/max_threads=1`, and run the same
   command against the copy.
4. To get the per-tick CSVs, add `--out=/some/dir` (and `--csv=full` to print them); to run a
   subset, `--test=pendulum --cases=sine_120hz,cap_`.
5. On a stock upstream build, the API-presence table at the top tells you within a second
   whether the build has PR #118997 (`set_angular_target_rotation` and constant 23 present);
   builds before 4.8 will print `MISSING` and the tests will fail to parse.

Raw logs and CSVs from this run were kept outside the repository (session scratchpad); every
figure quoted above is reproducible with the commands in this section.
