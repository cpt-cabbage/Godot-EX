/**************************************************************************/
/*  wind_zone_3d.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// A local wind, placed and moved like any node (Godot-EX, 2026-11-09): within a cylinder about the node's Y axis
// (`radius`, `height`), fading over the outer `falloff` share of each, it adds to the global wind of every WindField3D:
// DIRECTIONAL blows along the node's -Z (a gust front, a gully that funnels), OUTWARD spreads from its axis and
// down it (a waterfall's downdraft, a rotor's wash), SWIRL turns about its axis (a dust devil, a rotor at a ridge).

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

class WindZone3D : public Node3D {
	GDCLASS(WindZone3D, Node3D);

public:
	enum Mode {
		MODE_DIRECTIONAL,
		MODE_OUTWARD,
		MODE_SWIRL,
	};

private:
	static LocalVector<WindZone3D *> zones;

	Mode mode = MODE_DIRECTIONAL;
	float strength = 5.0; // m/s at the centre
	float radius = 20.0;
	float height = 20.0;
	float falloff = 0.5;
	float turbulence = 0.3;
	float downward = 0.3; // OUTWARD: the share of the speed along -Y near the axis
	bool enabled = true;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// What a zone adds at a world position, given its world transform (read on the main thread before a bake).
	struct Snapshot {
		Transform3D xf;
		Transform3D inv;
		Mode mode;
		float strength, radius, height, falloff, turbulence, downward;
		AABB aabb;
		Vector3 sample(const Vector3 &p_world, float p_noise) const;
	};
	static void snapshot_all(LocalVector<Snapshot> &r_out);

	void set_mode(Mode p_mode) { mode = p_mode; }
	Mode get_mode() const { return mode; }
	void set_strength(float p_strength) { strength = p_strength; }
	float get_strength() const { return strength; }
	void set_radius(float p_radius) { radius = MAX(p_radius, 0.1f); }
	float get_radius() const { return radius; }
	void set_height(float p_height) { height = MAX(p_height, 0.1f); }
	float get_height() const { return height; }
	void set_falloff(float p_falloff) { falloff = CLAMP(p_falloff, 0.0f, 1.0f); }
	float get_falloff() const { return falloff; }
	void set_turbulence(float p_turbulence) { turbulence = MAX(p_turbulence, 0.0f); }
	float get_turbulence() const { return turbulence; }
	void set_downward(float p_downward) { downward = CLAMP(p_downward, 0.0f, 1.0f); }
	float get_downward() const { return downward; }
	void set_enabled(bool p_enabled) { enabled = p_enabled; }
	bool is_enabled() const { return enabled; }
};

VARIANT_ENUM_CAST(WindZone3D::Mode);
