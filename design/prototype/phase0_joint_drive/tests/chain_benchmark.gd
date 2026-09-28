# Test 3: cost of a 15-body chain with 14 Generic6DOFJoint3D orientation drives.
#
# Body 0 is frozen (static anchor); bodies 1..14 hang below it, each joined to
# its parent by a 6DOF joint with the angular spring enabled (k = 2000 Nm/rad,
# c = 50 Nm s/rad, 200 Nm cap). Every tick the script sets a small sinusoidal
# target on all 14 drives. After 60 warm-up ticks, 600 ticks are measured at
# 60, 120 and 240 Hz, plus a passive (no drive) control at 120 Hz.
#
# Per-step cost is the wall-clock gap between the end of this node's
# _physics_process on tick N and its start on tick N+1 when both ticks fall in
# the same process frame: that gap contains PhysicsServer3D.end_sync() +
# step() (the Jolt update) + sync() + flush_queries() and the scene-tree
# dispatch, and nothing else. Ticks are batched per frame by Engine.max_fps = 10
# (or --fixed-fps 10 on the command line).
extends "res://tests/drive_test.gd"

const N_BODIES := 15
const SEG_LEN := 0.5
const SEG_MASS := 1.0
const WARMUP_TICKS := 60
const MEASURE_TICKS := 600
const K := 2000.0
const C := 50.0
const TAU_LIMIT := 200.0

var cases: Array = [
	{"name": "chain_60hz_driven", "tps": 60, "driven": true},
	{"name": "chain_120hz_driven", "tps": 120, "driven": true},
	{"name": "chain_240hz_driven", "tps": 240, "driven": true},
	{"name": "chain_120hz_passive", "tps": 120, "driven": false},
]
var case_index := -1
var cur: Dictionary = {}
var rig: Node3D = null
var bodies: Array = []
var joints: Array = []
var tick := 0
var saved_tps := 60
var saved_max_fps := 0
var saved_max_steps := 8
var step_samples: Array = []
var script_samples: Array = []
var monitor_samples: Array = []
var rows: Array = []
var last_exit_usec := -1
var last_exit_frame := -1
var last_monitor_frame := -1
var frames_seen := 0
var non_finite := false
var case_results: Array = []
var all_pass := true


func _ready() -> void:
	saved_tps = Engine.physics_ticks_per_second
	saved_max_fps = Engine.max_fps
	saved_max_steps = Engine.max_physics_steps_per_frame
	Engine.max_fps = 10
	Engine.max_physics_steps_per_frame = 1000
	log_line("chain_bench: %d cases, %d bodies, %d drives, %d measured ticks each" % [cases.size(), N_BODIES, N_BODIES - 1, MEASURE_TICKS])
	_next_case.call_deferred()


func _set_flag(j: Generic6DOFJoint3D, axis: int, flag: int, v: bool) -> void:
	match axis:
		0: j.set_flag_x(flag, v)
		1: j.set_flag_y(flag, v)
		_: j.set_flag_z(flag, v)


func _set_param(j: Generic6DOFJoint3D, axis: int, param: int, v: float) -> void:
	match axis:
		0: j.set_param_x(param, v)
		1: j.set_param_y(param, v)
		_: j.set_param_z(param, v)


func _build_rig() -> void:
	rig = Node3D.new()
	rig.name = "Chain"
	add_child(rig)
	bodies = []
	joints = []
	for i in N_BODIES:
		var b := RigidBody3D.new()
		b.name = "Seg%d" % i
		b.mass = SEG_MASS
		b.can_sleep = false
		b.position = Vector3(0, -SEG_LEN * (0.5 + i), 0)
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(0.1, SEG_LEN, 0.1)
		cs.shape = box
		b.add_child(cs)
		if i == 0:
			b.freeze = true
		rig.add_child(b)
		bodies.append(b)
	for i in range(1, N_BODIES):
		var j := Generic6DOFJoint3D.new()
		j.name = "Drive%d" % i
		j.position = Vector3(0, -SEG_LEN * i, 0)
		rig.add_child(j)
		j.node_a = j.get_path_to(bodies[i - 1])
		j.node_b = j.get_path_to(bodies[i])
		for axis in 3:
			_set_flag(j, axis, Generic6DOFJoint3D.FLAG_ENABLE_ANGULAR_LIMIT, false)
			_set_flag(j, axis, Generic6DOFJoint3D.FLAG_ENABLE_ANGULAR_SPRING, bool(cur["driven"]))
			_set_param(j, axis, Generic6DOFJoint3D.PARAM_ANGULAR_SPRING_STIFFNESS, K)
			_set_param(j, axis, Generic6DOFJoint3D.PARAM_ANGULAR_SPRING_DAMPING, C)
			_set_param(j, axis, Generic6DOFJoint3D.PARAM_ANGULAR_DRIVE_TORQUE_LIMIT, TAU_LIMIT)
		joints.append(j)


func _target_for(i: int, t: float) -> Quaternion:
	return Quaternion(Vector3(0, 0, 1), deg_to_rad(10.0) * sin(TAU * 0.5 * t + 0.4 * (i + 1)))


func _physics_process(_delta: float) -> void:
	if rig == null:
		return
	var now := Time.get_ticks_usec()
	var frame := Engine.get_process_frames()
	var measuring := tick >= WARMUP_TICKS
	var step_us := -1
	if measuring and last_exit_usec >= 0 and frame == last_exit_frame:
		step_us = now - last_exit_usec
		step_samples.append(step_us)
	if frame != last_monitor_frame:
		frames_seen += 1
		if measuring:
			monitor_samples.append(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1.0e6)
		last_monitor_frame = frame
	var t := float(tick) / float(Engine.physics_ticks_per_second)
	var t0 := Time.get_ticks_usec()
	if bool(cur["driven"]):
		for i in joints.size():
			joints[i].set_angular_target_rotation(_target_for(i, t))
	var t1 := Time.get_ticks_usec()
	if measuring:
		script_samples.append(t1 - t0)
		rows.append(PackedStringArray([str(tick), str(step_us), str(t1 - t0)]))
	if not bodies[N_BODIES - 1].global_transform.origin.is_finite():
		non_finite = true
	tick += 1
	last_exit_usec = Time.get_ticks_usec()
	last_exit_frame = frame
	if tick >= WARMUP_TICKS + MEASURE_TICKS:
		_finish_case()


func _finish_case() -> void:
	var tps: int = Engine.physics_ticks_per_second
	var sorted_steps := step_samples.duplicate()
	sorted_steps.sort()
	var s_mean := mean(step_samples)
	var s_p50 := percentile(sorted_steps, 0.5)
	var s_p95 := percentile(sorted_steps, 0.95)
	var s_max := max_of(step_samples)
	var scr_mean := mean(script_samples)
	var mon_max := max_of(monitor_samples)
	var active := Performance.get_monitor(Performance.PHYSICS_3D_ACTIVE_OBJECTS)
	var islands := Performance.get_monitor(Performance.PHYSICS_3D_ISLAND_COUNT)
	var t_last := float(tick - 1) / float(tps)
	var max_err := 0.0
	for i in joints.size():
		var rel: Basis = bodies[i].global_transform.basis.inverse() * bodies[i + 1].global_transform.basis
		var target := _target_for(i, t_last) if bool(cur["driven"]) else Quaternion.IDENTITY
		max_err = maxf(max_err, quat_error_deg(rel.get_rotation_quaternion(), target))
	var budget_us := 1.0e6 / float(tps)
	var passed := (not non_finite) and step_samples.size() >= MEASURE_TICKS / 2
	var cname := String(cur["name"])
	var detail := "step_us mean=%.1f p50=%.1f p95=%.1f max=%.1f (n=%d, frames=%d) | script_us(14 targets) mean=%.1f | TIME_PHYSICS_PROCESS max-step-in-last-second max_us=%.1f | active_objects=%d islands=%d | tick budget %.0f us -> load %.2f%% | max joint err at end %.2f deg | non_finite=%s" % [
		s_mean, s_p50, s_p95, s_max, step_samples.size(), frames_seen, scr_mean, mon_max, int(active), int(islands), budget_us, 100.0 * s_mean / budget_us, max_err, non_finite]
	print("RESULT chain_bench/%s %s tps=%d bodies=%d drives=%d driven=%s | %s" % [
		cname, "PASS" if passed else "FAIL", tps, N_BODIES, N_BODIES - 1, cur["driven"], detail])
	case_results.append({"name": cname, "passed": passed, "detail": detail, "step_mean_us": s_mean, "step_p95_us": s_p95})
	if not passed:
		all_pass = false
	emit_csv("chain_" + cname, "tick,step_us,script_us", rows, int(tps / 10))
	_next_case.call_deferred()


func _next_case() -> void:
	if rig != null:
		rig.free()
		rig = null
	case_index += 1
	if case_index >= cases.size():
		Engine.physics_ticks_per_second = saved_tps
		Engine.max_fps = saved_max_fps
		Engine.max_physics_steps_per_frame = saved_max_steps
		finished.emit({"name": "chain_bench", "passed": all_pass, "cases": case_results})
		return
	cur = cases[case_index]
	Engine.physics_ticks_per_second = int(cur["tps"])
	tick = 0
	step_samples = []
	script_samples = []
	monitor_samples = []
	rows = []
	last_exit_usec = -1
	last_exit_frame = -1
	last_monitor_frame = -1
	frames_seen = 0
	non_finite = false
	_build_rig()
	log_line("chain_bench case %d/%d: %s (tps=%d driven=%s)" % [case_index + 1, cases.size(), cur["name"], cur["tps"], cur["driven"]])
