#[compute]

#version 450

#VERSION_DEFINES

// Temporal accumulation for the ray-traced shadow mask: reprojects last
// frame's accumulated mask and blends it with the current frame, using a
// 3x3 neighborhood clamp to reject stale history.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D current_mask;
layout(set = 0, binding = 1) uniform sampler2D history_mask;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
// Motion vectors from the previous frame's color pass (uv_prev = uv + velocity).
// A dummy binding when motion vectors are not rendered (FLAG_HAS_VELOCITY unset).
layout(set = 0, binding = 3) uniform sampler2D velocity_texture;
// The previous frame pair's reprojection, the camera-only motion the (one
// frame stale) velocity buffer was rendered with.
layout(set = 0, binding = 4, std140) uniform ReprojectUBO {
	mat4 prev_reproject;
	// The GI denoiser's block (stochastic_denoise.glsl), shared: its GI
	// parameters are not read here.
	vec4 gi_params;
	vec2 jitter_delta; // Half the previous frame's TAA jitter minus this frame's, NDC (FLAG_VELOCITY_CURRENT).
}
reprojection;
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D dest_mask;
// r: accumulated mask, g: frames accumulated / 255.
layout(set = 1, binding = 1, rg8) uniform restrict writeonly image2D dest_history;

#define FLAG_HAS_VELOCITY 1u
#define FLAG_VELOCITY_CURRENT 1048576u // The velocity buffer is this frame's (the motion-vector prepass): every history at uv + velocity, no classification.
// Frame-edge history borrowing (see the reprojection block); the band is
// fixed here where the stochastic denoiser's comes from GODOT_GI_BORROW,
// equal at that knob's default.
#define BORROW_BAND 0.15
#define BORROW_FRAMES 4.0

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC.
	ivec2 screen_size;
	float blend_alpha; // Steady-state weight of the current frame.
	uint flags;
	float frames_max; // Accumulation cap.
	float pad0;
	float pad1;
	float pad2;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float depth = texelFetch(depth_texture, pixel, 0).r;
	float current = texelFetch(current_mask, pixel, 0).r;

	if (depth == 0.0) {
		imageStore(dest_mask, pixel, vec4(1.0));
		imageStore(dest_history, pixel, vec4(1.0, 0.0, 0.0, 1.0));
		return;
	}

	// Neighborhood bounds of the current frame, used to clamp history.
	float mn = current;
	float mx = current;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 sample_pixel = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float v = texelFetch(current_mask, sample_pixel, 0).r;
			mn = min(mn, v);
			mx = max(mx, v);
		}
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, depth, 1.0);
	float result = current;
	// Frames of history behind this pixel. A running count lets the first
	// frames after a reveal average exactly (1/n) and only then settle to the
	// steady-state weight, so the accumulation can be made long -- which is
	// where the noise reduction comes from -- without the slow convergence a
	// long fixed blend would otherwise cost.
	float frames = 1.0;
	if (prev_ndc.w > 0.0) {
		vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
		// The camera reprojection is exact for static geometry, but a nonzero
		// velocity does not mean a moving object: Godot velocity buffers carry
		// camera motion for static geometry too, and this buffer is one frame
		// stale. Classify by comparing against the camera-only motion of the
		// frame pair the buffer was rendered with; only where the two disagree
		// is the pixel a moving object, and the stale velocity is then the
		// best predictor available of where its history lives.
		// The stale buffer holds the point's velocity at its previous-frame
		// pixel, prev_uv, not at the current one (see the GI temporal pass:
		// read here, a fast yaw classified every pixel as a moving object).
		bool velocity_in_frame = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));
		if ((params.flags & FLAG_VELOCITY_CURRENT) != 0u && (params.flags & FLAG_HAS_VELOCITY) != 0u) {
			// This frame's motion vectors (the prepass, section 71): every
			// history at uv + velocity, the camera's and the object's motion
			// together, no classification.
			prev_uv = uv + texelFetch(velocity_texture, pixel, 0).xy + reprojection.jitter_delta;
		} else if ((params.flags & FLAG_HAS_VELOCITY) != 0u && velocity_in_frame) {
			ivec2 velocity_pixel = ivec2(prev_uv * vec2(params.screen_size));
			vec2 velocity = texelFetch(velocity_texture, velocity_pixel, 0).xy;
			vec4 prevprev_ndc = reprojection.prev_reproject * vec4(prev_ndc.xyz / prev_ndc.w, 1.0);
			if (velocity != vec2(0.0) && prevprev_ndc.w > 0.0) {
				vec2 static_motion = (prevprev_ndc.xy / prevprev_ndc.w) * 0.5 + 0.5 - prev_uv;
				// 2px: the velocity buffer is unjittered while the matrices
				// carry the TAA jitter of both frames.
				vec2 object_pixels = (velocity - static_motion) * vec2(params.screen_size);
				if (any(greaterThan(abs(object_pixels), vec2(2.0)))) {
					prev_uv = uv + velocity;
				}
			}
		}
		// History just off the previous frame (the band a camera pan sweeps in
		// along the entering edge): borrow the nearest in-frame history as a
		// warm start rather than restarting from one frame's rays, which draws
		// the band as a noisy stripe against the converged interior. It counts
		// as only a few frames, so the pixel's own rays take over quickly, and
		// the neighborhood clamp below still bounds it. Not for history far
		// outside (a camera cut): stretching the edge columns over the whole
		// screen would be worse than the noise.
		bool borrowed = false;
		if (!(all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) &&
				all(greaterThanEqual(prev_uv, vec2(-BORROW_BAND))) && all(lessThanEqual(prev_uv, vec2(1.0 + BORROW_BAND)))) {
			prev_uv = clamp(prev_uv, vec2(0.0), vec2(1.0));
			borrowed = true;
		}
		if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			vec2 hist = textureLod(history_mask, prev_uv, 0.0).rg;
			float frames_prev = borrowed ? min(hist.g * 255.0, BORROW_FRAMES - 1.0) : hist.g * 255.0;
			// A young pixel's neighborhood is itself noisy, so clamping it
			// tightly would reject good history; the window closes as the
			// estimate settles.
			float widen = mix(0.25, 0.05, clamp(frames_prev / 8.0, 0.0, 1.0));
			float clamped = clamp(hist.r, mn - widen, mx + widen);
			if (abs(clamped - hist.r) > 0.35) {
				// Far outside the current neighborhood: this is stale history
				// from another surface, not a noisy sample. Start over.
				result = current;
			} else {
				frames = min(frames_prev + 1.0, params.frames_max);
				result = mix(clamped, current, max(1.0 / frames, params.blend_alpha));
			}
		}
	}

	imageStore(dest_mask, pixel, vec4(result));
	imageStore(dest_history, pixel, vec4(result, frames / 255.0, 0.0, 1.0));
}
