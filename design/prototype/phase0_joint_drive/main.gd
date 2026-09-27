# Phase 0 runner: executes the joint-drive verification tests headless and quits
# with exit code 0 when every case passed, 1 otherwise.
#
#   <godot> --headless --path <project> [--fixed-fps 10] -- [--test=a,b] [--csv=sparse|full|none] [--out=DIR]
#
# --test   comma-separated subset of: pendulum, physical_bone, chain_bench (default: all)
# --csv    how much of each per-tick CSV to print (default sparse = 10 rows per simulated second)
# --out    directory that receives the full per-tick CSV files (optional)
extends Node3D

const TEST_SCRIPTS := {
	"pendulum": "res://tests/pendulum_test.gd",
	"physical_bone": "res://tests/physical_bone_test.gd",
	"chain_bench": "res://tests/chain_benchmark.gd",
}
const DEFAULT_ORDER := ["pendulum", "physical_bone", "chain_bench"]

var queue: Array = []
var options: Dictionary = {}
var results: Array = []
var current = null
var start_usec := 0


func _ready() -> void:
	start_usec = Time.get_ticks_usec()
	options = _parse_user_args()
	_print_header()
	_print_api_presence()
	var wanted: Array = DEFAULT_ORDER
	if options.has("test"):
		wanted = Array(String(options["test"]).split(",", false))
	for n in wanted:
		if TEST_SCRIPTS.has(n):
			queue.append(n)
		else:
			print("[phase0] unknown test '%s' (known: %s)" % [n, ", ".join(TEST_SCRIPTS.keys())])
	if queue.is_empty():
		_finish()
		return
	_run_next.call_deferred()


func _parse_user_args() -> Dictionary:
	var d := {}
	for a in OS.get_cmdline_user_args():
		var s := String(a)
		if s.begins_with("--"):
			s = s.substr(2)
		var eq := s.find("=")
		if eq >= 0:
			d[s.substr(0, eq)] = s.substr(eq + 1)
		else:
			d[s] = "1"
	return d


func _print_header() -> void:
	var v := Engine.get_version_info()
	print("[phase0] Godot %s (hash %s) | PhysicsServer3D class=%s | physics/3d/physics_engine=%s" % [
		v["string"], v["hash"], PhysicsServer3D.get_class(), ProjectSettings.get_setting("physics/3d/physics_engine")])
	print("[phase0] physics_ticks_per_second=%d max_physics_steps_per_frame=%d physics_interpolation=%s jitter_fix=%.2f" % [
		Engine.physics_ticks_per_second, Engine.max_physics_steps_per_frame, get_tree().physics_interpolation, Engine.physics_jitter_fix])
	print("[phase0] CPU=%s x%d | OS=%s | user args=%s" % [
		OS.get_processor_name(), OS.get_processor_count(), OS.get_name(), OS.get_cmdline_user_args()])


func _print_api_presence() -> void:
	var methods := [
		["Generic6DOFJoint3D", "set_angular_target_rotation"],
		["Generic6DOFJoint3D", "get_angular_target_rotation"],
		["Generic6DOFJoint3D", "has_target_rotation"],
		["Generic6DOFJoint3D", "clear_angular_target_rotation"],
		["PhysicsServer3D", "generic_6dof_joint_set_angular_target_rotation"],
		["PhysicsServer3D", "generic_6dof_joint_get_angular_target_rotation"],
		["PhysicsServer3D", "generic_6dof_joint_set_param"],
		["PhysicsServer3D", "generic_6dof_joint_set_flag"],
		["PhysicalBone3D", "get_joint_rid"],
		["PhysicalBoneSimulator3D", "physical_bones_start_simulation"],
	]
	for m in methods:
		print("[phase0] api %s.%s: %s" % [m[0], m[1], "present" if ClassDB.class_has_method(m[0], m[1]) else "MISSING"])
	var consts := [
		["Generic6DOFJoint3D", "PARAM_ANGULAR_DRIVE_TORQUE_LIMIT"],
		["Generic6DOFJoint3D", "PARAM_LINEAR_DRIVE_FORCE_LIMIT"],
		["Generic6DOFJoint3D", "PARAM_ANGULAR_SPRING_STIFFNESS"],
		["Generic6DOFJoint3D", "FLAG_ENABLE_ANGULAR_SPRING"],
		["PhysicsServer3D", "G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT"],
		["PhysicsServer3D", "G6DOF_JOINT_FLAG_ENABLE_ANGULAR_SPRING"],
		["PhysicsServer3D", "G6DOF_JOINT_ANGULAR_SPRING_STIFFNESS"],
	]
	for c in consts:
		if ClassDB.class_has_integer_constant(c[0], c[1]):
			print("[phase0] const %s.%s = %d" % [c[0], c[1], ClassDB.class_get_integer_constant(c[0], c[1])])
		else:
			print("[phase0] const %s.%s: MISSING" % [c[0], c[1]])


func _run_next() -> void:
	if current != null:
		current.queue_free()
		current = null
	if queue.is_empty():
		_finish()
		return
	var test_name: String = queue.pop_front()
	var script: Script = load(TEST_SCRIPTS[test_name])
	current = script.new()
	current.name = test_name
	current.options = options
	current.finished.connect(_on_test_finished)
	print("[phase0] ---- starting test: %s ----" % test_name)
	add_child(current)


func _on_test_finished(result: Dictionary) -> void:
	results.append(result)
	_run_next.call_deferred()


func _finish() -> void:
	var all_pass := true
	print("SUMMARY BEGIN")
	for r in results:
		print("SUMMARY %s %s" % [r["name"], "PASS" if r["passed"] else "FAIL"])
		for c in r.get("cases", []):
			print("SUMMARY   %-32s %s  %s" % [c["name"], "PASS" if c["passed"] else "FAIL", c.get("detail", "")])
		if not r["passed"]:
			all_pass = false
	print("SUMMARY END overall=%s wall=%.1fs" % ["PASS" if all_pass else "FAIL", (Time.get_ticks_usec() - start_usec) / 1.0e6])
	get_tree().quit(0 if all_pass else 1)
