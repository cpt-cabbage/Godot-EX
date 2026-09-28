# Phase 0 joint-drive verification project

A headless Godot project that exercises, at runtime, the joint-drive chain the
design report depends on: a Jolt `Generic6DOFJoint3D` orientation drive
(`set_angular_target_rotation()` + `PARAM_ANGULAR_DRIVE_TORQUE_LIMIT`), the same
drive reached through `PhysicalBone3D.get_joint_rid()` and the `PhysicsServer3D`
API, and the per-step cost of a 15-body / 14-drive chain at 60, 120 and 240 Hz.
Everything is built in GDScript; there are no imported assets, so the project
runs from a plain folder without an editor import pass.

Results from the 2026-09-27 run on this fork are written up in
`design/research_notes/Physics superhuman movement second pass/godot_runtime_verification.md`.

## Requirements

- A Godot build with the Jolt module and the quaternion-target API, i.e. this
  fork (4.8-dev, `modules/jolt_physics`, Jolt 5.6.0) or an upstream build that
  includes godotengine/godot PR #118997 (merged 2026-06-23, milestone 4.8).
  Releases before 4.8 have Jolt but not `set_angular_target_rotation`; the runner's
  API-presence table at the top of the output tells you immediately.
- Either an **editor** build (`target=editor`; `--path` always works) or an
  **export template** built with `disable_path_overrides=no` (the default
  template refuses `--path`). The fork build used for the write-up was:

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

  (6 m 35 s on 4 cores, 48.5 MB binary at `bin/godot.linuxbsd.template_debug.x86_64.llvm`;
  `bin/` is gitignored.)

## Running

```
# from the repository root, fork build:
design/prototype/phase0_joint_drive/run_tests.sh bin/godot.linuxbsd.template_debug.x86_64.llvm

# any build, real time, all tests:
<godot> --headless --path design/prototype/phase0_joint_drive

# a subset, with the full per-tick CSV printed and also written to files:
<godot> --headless --path design/prototype/phase0_joint_drive -- --test=pendulum --cases=sine_120hz,cap_ --csv=full --out=/tmp/phase0
```

Arguments after `--` go to the runner (`main.gd`):

| argument | meaning |
| --- | --- |
| `--test=pendulum,physical_bone,chain_bench` | which tests to run (default: all three, in this order) |
| `--cases=sub1,sub2` | only cases whose name contains one of the substrings |
| `--csv=sparse\|full\|none` | per-tick CSV on stdout: 10 rows per simulated second (default), every row, or none |
| `--out=DIR` | also write the full CSV of every case to `DIR/<test>_<case>.csv` |
| `--bench-fps=N` | frames per second the chain benchmark batches physics ticks into (default 30) |

`run_tests.sh` adds `--fixed-fps 10`, which runs ~10x faster than real time
without changing the physics step (the engine simply runs
`physics_ticks_per_second / 10` steps per frame). Drop it to run in real time;
the pendulum and PhysicalBone3D results are identical either way. Batching
several steps per frame does trigger a one-time Jolt warning ("job system
exceeded the maximum number of jobs") in this build; it is harmless for the
correctness tests but the benchmark numbers in the write-up were taken from a
real-time run.

The process exits with code 0 when every non-informational case passed, 1
otherwise. Output lines:

- `[phase0] api ... present|MISSING` and `[phase0] const ... = N`: the API names checked with `ClassDB`.
- `RESULT <test>/<case> PASS|FAIL|INFO ... | <numbers>`: one line per case; `INFO` cases document behaviour and never fail the run.
- `CSV BEGIN <name>` ... `CSV END <name>`: `tick,time_s,target_deg,actual_deg,error_deg,angvel_rad_s` (drive tests) or `tick,step_us,script_us` (benchmark).
- `SUMMARY ...` block at the end.

The `ERROR: Could not load global script cache` line at start-up is the engine
noticing that the project has never been opened in an editor (no `.godot/`
folder); it is harmless here because no script uses `class_name`.

## What the tests do

**`tests/pendulum_test.gd`** — `StaticBody3D` anchor, 1 kg / 1 m `RigidBody3D`
bar, `Generic6DOFJoint3D` at the bar's top end with angular limits off and the
angular spring (Jolt `Position` motor) on for all three angular axes, linear
axes locked. Each tick it calls `joint.set_angular_target_rotation(q)` with the
target expressed as the bar's orientation relative to the anchor (Jolt
`SetTargetOrientationBS`: `R_bar = R_anchor * q`) and records the error angle
between the achieved relative rotation and the target the solver was given.
Cases: 45 deg / 0.25 Hz swing at 60, 120 and 240 Hz; the same with velocity
feed-forward through `FLAG_ENABLE_ANGULAR_MOTOR` + `PARAM_ANGULAR_MOTOR_TARGET_VELOCITY`
(both signs, to find the convention); a static 45 deg target with a 2 Nm cap
versus the 3.46 Nm gravity torque (checked against the analytic
`asin(cap / (m g L/2))` equilibrium) and with a 1000 Nm cap; `k = 1e6` with zero
damping at 60 and 120 Hz; and a sleep/wake pair.

**`tests/physical_bone_test.gd`** — `Skeleton3D` with bones `root -> child`,
`PhysicalBoneSimulator3D`, two `PhysicalBone3D` (only `child` is simulated, so
`root` stays kinematic as the anchor). The spring is enabled through the node's
`joint_constraints/<x|y|z>/angular_spring_*` properties, the torque cap and the
target go through `PhysicsServer3D.generic_6dof_joint_set_param(rid, axis,
G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT, v)` and
`generic_6dof_joint_set_angular_target_rotation(rid, q)` on
`pb_child.get_joint_rid()`. The two default cases call
`PhysicsServer3D.joint_disable_collisions_between_bodies(rid, true)` first;
the two `*_colliding` INFO cases leave it off and show why it is needed under
Jolt. The skeleton write-back is sampled inside the `skeleton_updated` signal.

**`tests/chain_benchmark.gd`** — 15 boxes (0.5 m, 1 kg) hanging from a frozen
root body, 14 `Generic6DOFJoint3D` drives (k = 2000 Nm/rad, c = 50 Nm s/rad,
200 Nm cap), a small sinusoidal target on every drive each tick. After 60
warm-up ticks it measures 600 ticks at 60, 120 and 240 Hz plus a passive
(spring off) control at 120 Hz. The per-step cost is the wall-clock gap between
the end of the node's `_physics_process` on tick N and its start on tick N+1
when both fall in the same process frame, which contains exactly
`PhysicsServer3D.end_sync() + step() + sync() + flush_queries()` and the
scene-tree dispatch. The time spent setting the 14 targets is reported
separately as `script_us`.

## Files

```
project.godot        Jolt Physics, 120 ticks/s, max_physics_steps_per_frame 16, physics interpolation on
main.tscn / main.gd  runner: argument parsing, API-presence table, test sequencing, summary, exit code
run_tests.sh         convenience wrapper (adds --headless --fixed-fps 10 --path)
tests/drive_test.gd  shared helpers (error angle, CSV emission, case filter)
tests/pendulum_test.gd, tests/physical_bone_test.gd, tests/chain_benchmark.gd
```
