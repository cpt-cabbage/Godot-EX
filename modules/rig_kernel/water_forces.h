/**************************************************************************/
/*  water_forces.h                                                        */
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

#include "ground_data.h"

#include "core/math/vector3.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"

// Water's forces on a rig's rigid bodies (Jolt has no water), applied before each physics tick by the
// caller (a training server's step loop, the game's ragdoll): the same code in training and the game.
// Each body is its collision primitive (a box, a capsule or a sphere, placed in the body's frame) with a
// volume of its own (its mass over its density, the caller's). Per body, from the water at its centre
// (GroundData's water field):
// - the submerged share: the water's height through the shape's vertical extent (exact for an upright
//   prism, close for the rest);
// - buoyancy: rho g V times that share, up, at the submerged part's centre (the part of the shape's long
//   axis under the surface), so a tilted body is righted or turned as a float is;
// - drag: 0.5 rho Cd A |u| u against u, the velocity at that centre relative to the water's flow, A the
//   shape's area across u (a box's faces by the share they face u, a capsule's side and end), times the
//   share; and a spin drag against the angular velocity across the long axis, the side's quadratic drag
//   integrated along it (rho Cd D L^4 / 64 |w| w for a rod of length L and width D about its centre).
// Forces go through PhysicsServer3D's per-step force (cleared after each step).
class WaterForces : public RefCounted {
	GDCLASS(WaterForces, RefCounted);

	enum Kind {
		BOX,
		CAPSULE,
		SPHERE,
	};

	struct Body {
		RID rid;
		Kind kind = BOX;
		float volume = 0.0f;
		Vector3 center; // the shape's centre in the body's frame
		Vector3 size; // BOX: half extents; CAPSULE: radius, half the segment's length, axis; SPHERE: radius
		int axis = 1; // the long axis (the segment's) in the body's frame
	};

	LocalVector<Body> bodies;
	float density = 1000.0f; // the water's, kg/m^3
	float gravity = 9.8f;
	float drag = 1.0f; // Cd
	float spin_drag = 1.0f; // Cd of the spin drag
	float reach = 1.6f; // m from the first body (the pelvis) to any part of the rig, for the early out
	float submerged = 0.0f;

	void _add(const RID &p_body, Kind p_kind, float p_volume, const Vector3 &p_center, const Vector3 &p_size, int p_axis);

protected:
	static void _bind_methods();

public:
	void clear() { bodies.clear(); }
	void add_box(const RID &p_body, float p_volume, const Vector3 &p_center, const Vector3 &p_half_extents);
	void add_capsule(const RID &p_body, float p_volume, const Vector3 &p_center, int p_axis, float p_radius, float p_height);
	void add_sphere(const RID &p_body, float p_volume, const Vector3 &p_center, float p_radius);
	int get_body_count() const { return bodies.size(); }

	void set_density(float p_density) { density = p_density; }
	float get_density() const { return density; }
	void set_gravity(float p_gravity) { gravity = p_gravity; }
	float get_gravity() const { return gravity; }
	void set_drag(float p_drag) { drag = p_drag; }
	float get_drag() const { return drag; }
	void set_spin_drag(float p_drag) { spin_drag = p_drag; }
	float get_spin_drag() const { return spin_drag; }
	void set_reach(float p_reach) { reach = p_reach; }
	float get_reach() const { return reach; }

	// Applies the water's forces for the coming tick; returns the submerged share of the rig's volume (0
	// out of the water, which costs one look at the field).
	float apply(const Ref<GroundData> &p_water);
	// The last apply()'s submerged share of the rig's volume.
	float get_submerged() const { return submerged; }
};
