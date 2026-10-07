/**************************************************************************/
/*  policy_sampler.h                                                      */
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

#include "core/math/random_pcg.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// A learned policy's actions for a batch of observations, sampled where the environments run: the
// observation normalised by running statistics (clamped to +-10), a multilayer perceptron (ELU between
// its layers) giving each action's mean, Gaussian noise of a learned standard deviation, and each
// sample's log-probability. A training server samples its rigs' actions itself instead of a round trip
// to the trainer every control step (ProjectEX's learn/train.py --server-policy: the weights and
// statistics sent once an iteration). The arithmetic is the trainer's (PyTorch's, in float32; the ELU as
// exp(min(x, 0)) - 1 + max(x, 0)); the trainer checks every sample's log-probability against its own.
// On macOS the layers' products run on Accelerate (the CPU's matrix units).
class PolicySampler : public RefCounted {
	GDCLASS(PolicySampler, RefCounted);

	struct Layer {
		int inputs = 0;
		int outputs = 0;
		LocalVector<float> weight; // outputs x inputs, row-major (PyTorch's nn.Linear)
		LocalVector<float> bias;
	};

	LocalVector<float> mean;
	LocalVector<float> scale; // sqrt(var + 1e-8)
	LocalVector<Layer> layers;
	LocalVector<float> log_std;
	LocalVector<float> std;
	RandomPCG rng;
	LocalVector<float> buffer_a;
	LocalVector<float> buffer_b;

	const float *_forward(const float *p_obs, int p_rows);
	float _normal();

protected:
	static void _bind_methods();

public:
	void set_normalizer(const PackedFloat32Array &p_mean, const PackedFloat32Array &p_var);
	void set_layers(const PackedInt32Array &p_shapes, const PackedFloat32Array &p_parameters);
	void set_log_std(const PackedFloat32Array &p_log_std);
	void set_seed(int64_t p_seed);
	int get_input_size() const { return layers.is_empty() ? 0 : layers[0].inputs; }
	int get_output_size() const { return layers.is_empty() ? 0 : layers[layers.size() - 1].outputs; }

	// The actions' means for p_rows observations (row-major).
	PackedFloat32Array mean_actions(const PackedFloat32Array &p_obs, int p_rows);
	// Sampled actions (p_rows x outputs, row-major), then each row's log-probability (p_rows values).
	PackedFloat32Array sample(const PackedFloat32Array &p_obs, int p_rows);
};
