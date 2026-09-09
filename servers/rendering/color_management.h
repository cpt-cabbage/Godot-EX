/**************************************************************************/
/*  color_management.h                                                    */
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

#include "core/math/basis.h"
#include "core/math/color.h"

// The engine's view of the working colour space.
//
// Godot has always treated the renderer's linear values as linear Rec.709 and
// every authored Color as sRGB-encoded Rec.709. With colour management enabled
// the working space becomes something wider — ACEScg by default — and those two
// assumptions part company: a Color still means sRGB, but the renderer's numbers
// no longer do.
//
// This class holds the primaries change between them. It is deliberately not a
// dependency on the OpenColorIO module: the module computes the matrix once at
// startup and pushes it here, so the hot paths that convert authored colours
// stay a transfer function and a 3x3 multiply, with no processor lookup and no
// module include. When colour management is off the matrix is the identity and
// every function below is exactly the Color helper it replaces, which is what
// makes converting the call sites safe.
class ColorManagement {
	static inline bool enabled = false;
	static inline Basis rec709_to_working;
	static inline Basis working_to_rec709;
	static inline Vector3 luminance_weights = Vector3(0.2126f, 0.7152f, 0.0722f);

public:
	// Called by the OpenColorIO module whenever the active config changes.
	// Passing `false` restores stock behaviour.
	static void configure(bool p_enabled, const Basis &p_rec709_to_working, const Basis &p_working_to_rec709);

	static bool is_enabled() { return enabled; }
	static const Basis &get_rec709_to_working() { return rec709_to_working; }
	static const Basis &get_working_to_rec709() { return working_to_rec709; }
	// The Y row of the working space: dot a working-space colour with this for
	// its luminance. Rec.709's (0.2126, 0.7152, 0.0722) when colour management
	// is off; ACEScg's is about (0.272, 0.674, 0.054), a quarter off on saturated
	// reds and blues, so a shader that keeps the Rec.709 constants measures the
	// wrong scalar there.
	static const Vector3 &get_luminance_weights() { return luminance_weights; }

	// Linear Rec.709 -> working space. Identity when colour management is off.
	static _FORCE_INLINE_ Color linear_to_working(const Color &p_color) {
		if (!enabled) {
			return p_color;
		}
		const Vector3 v = rec709_to_working.xform(Vector3(p_color.r, p_color.g, p_color.b));
		return Color(v.x, v.y, v.z, p_color.a);
	}

	static _FORCE_INLINE_ Color working_to_linear(const Color &p_color) {
		if (!enabled) {
			return p_color;
		}
		const Vector3 v = working_to_rec709.xform(Vector3(p_color.r, p_color.g, p_color.b));
		return Color(v.x, v.y, v.z, p_color.a);
	}

	// An inspector Color is display-referred sRGB; this is what turns it into the
	// scene-referred value the renderer lights with. Equivalent to
	// Color::srgb_to_linear() when colour management is off, so it is a safe
	// drop-in at every site that was doing that conversion for that reason.
	static _FORCE_INLINE_ Color authored_to_working(const Color &p_color) {
		return linear_to_working(p_color.srgb_to_linear());
	}

	// The inverse, for showing a working-space value back to the user.
	static _FORCE_INLINE_ Color working_to_authored(const Color &p_color) {
		return working_to_linear(p_color).linear_to_srgb();
	}
};
