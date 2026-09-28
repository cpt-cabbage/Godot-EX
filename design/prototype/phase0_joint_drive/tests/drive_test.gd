# Shared helpers for the Phase 0 joint-drive tests. Extended by path (no
# class_name) so the scripts load in export-template builds that have no
# editor-generated global class cache.
extends Node3D

signal finished(result: Dictionary)

var options: Dictionary = {}


func csv_mode() -> String:
	return String(options.get("csv", "sparse"))


func out_dir() -> String:
	return String(options.get("out", ""))


# --cases=a,b keeps only cases whose name contains one of the substrings.
func filter_cases(all_cases: Array) -> Array:
	if not options.has("cases"):
		return all_cases
	var wanted: PackedStringArray = String(options["cases"]).split(",", false)
	var out: Array = []
	for c in all_cases:
		for w in wanted:
			if String(c["name"]).contains(w):
				out.append(c)
				break
	return out


static func verdict(passed: bool, informational: bool) -> String:
	if informational:
		return "INFO"
	return "PASS" if passed else "FAIL"


func log_line(s: String) -> void:
	print("[phase0] " + s)


# Angle (degrees) of the rotation that takes `actual` to `target`.
static func quat_error_deg(actual: Quaternion, target: Quaternion) -> float:
	var d: Quaternion = actual.inverse() * target
	var w: float = clampf(absf(d.w), 0.0, 1.0)
	return rad_to_deg(2.0 * acos(w))


# Signed rotation angle (degrees) of `b` about `axis`, for rotations that are
# nominally about that axis.
static func angle_about_axis_deg(b: Basis, axis: Vector3) -> float:
	var q: Quaternion = b.get_rotation_quaternion()
	if q.w < 0.0:
		q = Quaternion(-q.x, -q.y, -q.z, -q.w)
	var s: float = Vector3(q.x, q.y, q.z).dot(axis)
	return rad_to_deg(2.0 * atan2(s, q.w))


static func gravity() -> float:
	return float(ProjectSettings.get_setting("physics/3d/default_gravity", 9.8))


static func mean(values: Array) -> float:
	if values.is_empty():
		return NAN
	var s := 0.0
	for v in values:
		s += float(v)
	return s / values.size()


static func max_of(values: Array) -> float:
	if values.is_empty():
		return NAN
	var m: float = float(values[0])
	for v in values:
		m = maxf(m, float(v))
	return m


static func percentile(sorted_values: Array, p: float) -> float:
	if sorted_values.is_empty():
		return NAN
	var idx := int(floor(p * (sorted_values.size() - 1)))
	return float(sorted_values[clampi(idx, 0, sorted_values.size() - 1)])


# Prints a CSV block (every `stride`-th row in sparse mode, all rows in full
# mode, nothing in none mode) and writes the full CSV to --out when given.
func emit_csv(csv_name: String, header: String, rows: Array, stride: int) -> void:
	var dir := out_dir()
	if dir != "":
		var path := dir.path_join(csv_name + ".csv")
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f:
			f.store_line(header)
			for r in rows:
				f.store_line(",".join(r))
			f.close()
			log_line("wrote %s (%d rows)" % [path, rows.size()])
		else:
			log_line("could not write %s" % path)
	var mode := csv_mode()
	if mode == "none" or rows.is_empty():
		return
	var step: int = 1 if mode == "full" else maxi(stride, 1)
	print("CSV BEGIN %s (%d rows, printing every %d)" % [csv_name, rows.size(), step])
	print(header)
	for i in range(0, rows.size(), step):
		print(",".join(rows[i]))
	if step > 1 and (rows.size() - 1) % step != 0:
		print(",".join(rows[rows.size() - 1]))
	print("CSV END %s" % csv_name)
