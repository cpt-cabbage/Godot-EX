/**************************************************************************/
/*  policy_sampler.cpp                                                    */
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


#include "policy_sampler.h"

#include "core/object/class_db.h"

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

#include <cmath>

void PolicySampler::set_normalizer(const PackedFloat32Array &p_mean, const PackedFloat32Array &p_var) {
	ERR_FAIL_COND(p_mean.size() != p_var.size());
	mean.resize(p_mean.size());
	scale.resize(p_mean.size());
	for (int i = 0; i < p_mean.size(); i++) {
		mean[i] = p_mean[i];
		scale[i] = std::sqrt(p_var[i] + 1e-8f);
	}
}

// p_shapes: each layer's inputs and outputs in order; p_parameters: each layer's weight (outputs x inputs,
// row-major) then its bias, in order.
void PolicySampler::set_layers(const PackedInt32Array &p_shapes, const PackedFloat32Array &p_parameters) {
	ERR_FAIL_COND(p_shapes.size() < 2 || p_shapes.size() % 2 != 0);
	layers.clear();
	int k = 0;
	for (int i = 0; i < p_shapes.size(); i += 2) {
		Layer l;
		l.inputs = p_shapes[i];
		l.outputs = p_shapes[i + 1];
		ERR_FAIL_COND(l.inputs <= 0 || l.outputs <= 0);
		ERR_FAIL_COND_MSG(!layers.is_empty() && layers[layers.size() - 1].outputs != l.inputs, "A layer's inputs must be the last one's outputs.");
		ERR_FAIL_COND_MSG(k + l.inputs * l.outputs + l.outputs > p_parameters.size(), "Too few parameters for the layers.");
		l.weight.resize(l.inputs * l.outputs);
		memcpy(l.weight.ptr(), p_parameters.ptr() + k, sizeof(float) * l.weight.size());
		k += l.weight.size();
		l.bias.resize(l.outputs);
		memcpy(l.bias.ptr(), p_parameters.ptr() + k, sizeof(float) * l.outputs);
		k += l.outputs;
		layers.push_back(l);
	}
	ERR_FAIL_COND_MSG(k != p_parameters.size(), "More parameters than the layers take.");
}

void PolicySampler::set_log_std(const PackedFloat32Array &p_log_std) {
	log_std.resize(p_log_std.size());
	std.resize(p_log_std.size());
	for (int i = 0; i < p_log_std.size(); i++) {
		log_std[i] = p_log_std[i];
		std[i] = std::exp(p_log_std[i]);
	}
}

void PolicySampler::set_seed(int64_t p_seed) {
	rng.seed(uint64_t(p_seed));
}

// A standard normal draw (Box-Muller; the uniform kept off 0).
float PolicySampler::_normal() {
	const double u1 = (double(rng.rand()) + 1.0) / 4294967297.0;
	const double u2 = double(rng.rand()) / 4294967296.0;
	return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(Math::TAU * u2));
}

// The means for p_rows observations, in buffer_a or buffer_b (returned).
const float *PolicySampler::_forward(const float *p_obs, int p_rows) {
	const int in = layers[0].inputs;
	buffer_a.resize(p_rows * in);
	float *x = buffer_a.ptr();
	for (int r = 0; r < p_rows; r++) {
		for (int i = 0; i < in; i++) {
			const float v = (p_obs[r * in + i] - mean[i]) / scale[i];
			x[r * in + i] = v < -10.0f ? -10.0f : (v > 10.0f ? 10.0f : v);
		}
	}
	LocalVector<float> *src = &buffer_a;
	LocalVector<float> *dst = &buffer_b;
	for (uint32_t li = 0; li < layers.size(); li++) {
		const Layer &l = layers[li];
		dst->resize(p_rows * l.outputs);
		float *y = dst->ptr();
		const float *a = src->ptr();
#ifdef __APPLE__
		cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, p_rows, l.outputs, l.inputs, 1.0f, a, l.inputs, l.weight.ptr(), l.inputs, 0.0f, y, l.outputs);
#else
		for (int r = 0; r < p_rows; r++) {
			for (int o = 0; o < l.outputs; o++) {
				float s = 0.0f;
				const float *w = l.weight.ptr() + o * l.inputs;
				for (int i = 0; i < l.inputs; i++) {
					s += a[r * l.inputs + i] * w[i];
				}
				y[r * l.outputs + o] = s;
			}
		}
#endif
		const bool hidden = li + 1 < layers.size();
		for (int r = 0; r < p_rows; r++) {
			float *row = y + r * l.outputs;
			for (int o = 0; o < l.outputs; o++) {
				const float h = row[o] + l.bias[o];
				row[o] = hidden ? std::exp(h < 0.0f ? h : 0.0f) - 1.0f + (h > 0.0f ? h : 0.0f) : h;
			}
		}
		SWAP(src, dst);
	}
	return src->ptr();
}

PackedFloat32Array PolicySampler::mean_actions(const PackedFloat32Array &p_obs, int p_rows) {
	ERR_FAIL_COND_V(layers.is_empty() || int(mean.size()) != layers[0].inputs, PackedFloat32Array());
	ERR_FAIL_COND_V(p_obs.size() != p_rows * layers[0].inputs, PackedFloat32Array());
	const int out = get_output_size();
	const float *mu = _forward(p_obs.ptr(), p_rows);
	PackedFloat32Array r;
	r.resize(p_rows * out);
	memcpy(r.ptrw(), mu, sizeof(float) * p_rows * out);
	return r;
}

PackedFloat32Array PolicySampler::sample(const PackedFloat32Array &p_obs, int p_rows) {
	ERR_FAIL_COND_V(layers.is_empty() || int(mean.size()) != layers[0].inputs, PackedFloat32Array());
	ERR_FAIL_COND_V(p_obs.size() != p_rows * layers[0].inputs, PackedFloat32Array());
	const int out = get_output_size();
	ERR_FAIL_COND_V(int(log_std.size()) != out, PackedFloat32Array());
	const float *mu = _forward(p_obs.ptr(), p_rows);
	PackedFloat32Array r;
	r.resize(p_rows * (out + 1));
	float *a = r.ptrw();
	float *logp = a + p_rows * out;
	constexpr float HALF_LOG_TAU = 0.9189385332046727f;
	for (int k = 0; k < p_rows; k++) {
		float lp = 0.0f;
		for (int o = 0; o < out; o++) {
			const float eps = _normal();
			a[k * out + o] = mu[k * out + o] + std[o] * eps;
			lp += -0.5f * eps * eps - log_std[o] - HALF_LOG_TAU;
		}
		logp[k] = lp;
	}
	return r;
}

void PolicySampler::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_normalizer", "mean", "var"), &PolicySampler::set_normalizer);
	ClassDB::bind_method(D_METHOD("set_layers", "shapes", "parameters"), &PolicySampler::set_layers);
	ClassDB::bind_method(D_METHOD("set_log_std", "log_std"), &PolicySampler::set_log_std);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &PolicySampler::set_seed);
	ClassDB::bind_method(D_METHOD("get_input_size"), &PolicySampler::get_input_size);
	ClassDB::bind_method(D_METHOD("get_output_size"), &PolicySampler::get_output_size);
	ClassDB::bind_method(D_METHOD("mean_actions", "obs", "rows"), &PolicySampler::mean_actions);
	ClassDB::bind_method(D_METHOD("sample", "obs", "rows"), &PolicySampler::sample);
}
