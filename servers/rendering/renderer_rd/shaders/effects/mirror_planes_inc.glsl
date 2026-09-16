// The planar mirrors (RaytracingScene::MirrorPlane, plan section 44): flat
// instances whose material reflects, found in the scene each frame, up to
// MAX_MIRROR_PLANES, each a bounded rectangle. A mirror carries direct
// light no ray can find (a light seen through a delta lobe), so every pass
// that lights a point evaluates each local light's image through each
// mirror the point faces: the light itself at the mirrored point with the
// mirrored normal, times the Fresnel at each crossing, shadowed leg by
// leg (two for a single mirror, up to four along a three-mirror chain).
// Diffuse rays that land on a mirror read its diffuse card and reflect on.
//
// The including shader declares, before its params UBO:
//   struct MirrorPlane { vec4 plane; vec4 params; vec4 center; vec4 u_axis; vec4 v_axis; };
// and in the UBO `MirrorPlane mirrors[MAX_MIRROR_PLANES]; uint mirror_count; uint mirror_order;`
// (mirror_order: the longest image chain evaluated, 1..3)
// (plane: xyz the unit normal out of the reflective face, w its offset,
// n . p = w; params: x F0, y roughness, z half extent along u, w along v;
// center, u_axis, v_axis: the rectangle, u and v unit and in the plane).
// The space (world or view) is the including pass's.

uint mirror_count() {
	return min(params.mirror_count, MAX_MIRROR_PLANES);
}

bool mirror_on() {
	return params.mirror_count > 0u;
}

// Signed height of a point over a mirror's plane (positive on the reflective side).
float mirror_height(uint i, vec3 p) {
	return dot(params.mirrors[i].plane.xyz, p) - params.mirrors[i].plane.w;
}

bool mirror_in_rect(uint i, vec3 p) {
	vec3 d = p - params.mirrors[i].center.xyz;
	return abs(dot(d, params.mirrors[i].u_axis.xyz)) <= params.mirrors[i].params.z && abs(dot(d, params.mirrors[i].v_axis.xyz)) <= params.mirrors[i].params.w;
}

// The mirror a point lies on (within two centimetres of its plane, inside
// its rectangle), or MAX_MIRROR_PLANES for none.
uint mirror_at(vec3 p) {
	for (uint i = 0u; i < mirror_count(); i++) {
		if (abs(mirror_height(i, p)) < 0.02 && mirror_in_rect(i, p)) {
			return i;
		}
	}
	return MAX_MIRROR_PLANES;
}

// Schlick's Fresnel over the mirror's F0 at a crossing of cosine c.
float mirror_fresnel(uint i, float c) {
	float f0 = params.mirrors[i].params.x;
	float k = 1.0 - clamp(c, 0.0, 1.0);
	float k2 = k * k;
	return f0 + (1.0 - f0) * k2 * k2 * k;
}

vec3 mirror_point(uint i, vec3 p) {
	return p - 2.0 * mirror_height(i, p) * params.mirrors[i].plane.xyz;
}

vec3 mirror_dir(uint i, vec3 d) {
	vec3 n = params.mirrors[i].plane.xyz;
	return d - 2.0 * dot(d, n) * n;
}

// Where the segment from a point on the reflective side to an image point
// crosses the mirror, the cosine there, and how much of the reflection
// the rectangle carries: 1 for a crossing inside it, 0 for one well
// outside, and between the two over the lobe's footprint on the plane (a
// glossy mirror spreads a point's reflection over a spot that grows with
// its GGX alpha and the path's length, so a crossing near the edge, or
// just past it, still gets the part of that spot the rectangle covers).
// Zero when the point is not on the reflective side.
float mirror_crossing(uint i, vec3 p, vec3 img, out vec3 r_cross, out float r_cos) {
	vec3 n = params.mirrors[i].plane.xyz;
	float hp = mirror_height(i, p);
	vec3 rel = img - p;
	float len = length(rel);
	vec3 dir = rel / max(len, 1e-6);
	float cos_p = -dot(n, dir);
	r_cross = p;
	r_cos = cos_p;
	if (hp <= 0.005 || cos_p <= 1e-3) {
		return 0.0;
	}
	r_cross = p + dir * (hp / cos_p);
	float alpha = params.mirrors[i].params.y * params.mirrors[i].params.y;
	float footprint = max(alpha * len, 0.005);
	vec3 d = r_cross - params.mirrors[i].center.xyz;
	float du = abs(dot(d, params.mirrors[i].u_axis.xyz));
	float dv = abs(dot(d, params.mirrors[i].v_axis.xyz));
	// The share of [-footprint, footprint] about the crossing that lies
	// within the rectangle, per axis.
	float wu = clamp((params.mirrors[i].params.z - du + footprint) / (2.0 * footprint), 0.0, 1.0);
	float wv = clamp((params.mirrors[i].params.w - dv + footprint) / (2.0 * footprint), 0.0, 1.0);
	return wu * wv;
}

// The mirror's reflection of a direction through a GGX lobe of its
// roughness (a mirror at 0): the half vector sampled around the normal.
vec3 mirror_reflect(uint i, vec3 dir_in, vec2 u) {
	vec3 n = params.mirrors[i].plane.xyz;
	float a = params.mirrors[i].params.y * params.mirrors[i].params.y;
	if (a < 1e-4) {
		return reflect(dir_in, n);
	}
	vec3 t0 = normalize(abs(n.x) < 0.9 ? cross(n, vec3(1.0, 0.0, 0.0)) : cross(n, vec3(0.0, 1.0, 0.0)));
	vec3 t1 = cross(n, t0);
	float phi = u.x * 2.0 * M_PI;
	float ct = sqrt((1.0 - u.y) / (1.0 + (a * a - 1.0) * u.y));
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 h = normalize(t0 * (st * cos(phi)) + t1 * (st * sin(phi)) + n * ct);
	vec3 r = reflect(dir_in, h);
	if (dot(r, n) <= 1e-3) {
		r = reflect(dir_in, n);
	}
	return r;
}

// A light's image through a chain of up to three mirrors as seen from a
// point p with normal n_p: mi the mirror nearest p (the last reflection),
// then mj, then mk nearest the light (MAX_MIRROR_PLANES ends the chain).
// Gives the image position (the light mirrored through the chain from
// the light's end), the point and normal the light is evaluated at (p
// mirrored through the chain from p's end: the light seen through the
// chain is the light itself seen from there), the chain's weight (the
// Fresnel and the rectangle's coverage at each crossing), and the
// crossings (c1 on mi, c2 on mj, c3 on mk, for the shadow legs). False
// when the chain is broken: a point or light on the wrong side of a
// mirror, or a crossing off its rectangle. A second-order image (a lamp
// seen in the floor seen in the ceiling) carries the product of two
// Fresnels, a third-order one three; the box reads 0.87, 0.93 and higher
// of its image-method solve at orders one, two and three.
#define MIRROR_CHAIN_MAX 3u
// The most reflections a diffuse ray takes off mirrors before it stops at
// the mirror's diffuse card; past the first, a reflection goes on by
// Russian roulette on the chain's remaining weight (mirror_roulette), so
// a ray between a mirror floor and a mirror ceiling does not pay a ray
// query per layer for a chain holding a fifth of its energy.
#define MIRROR_BOUNCES_MAX 3u
#define MIRROR_ROULETTE_WEIGHT 0.5
// Whether a chain of weight f goes on, and the weight it goes on with:
// always at MIRROR_ROULETTE_WEIGHT and above, else with probability
// f / MIRROR_ROULETTE_WEIGHT (the survivor scaled back up: unbiased).
bool mirror_roulette(inout float f, float u) {
	if (f >= MIRROR_ROULETTE_WEIGHT) {
		return true;
	}
	float p = f / MIRROR_ROULETTE_WEIGHT;
	if (u >= p) {
		return false;
	}
	f = MIRROR_ROULETTE_WEIGHT;
	return true;
}
// p_inv_range: the light's inverse range (0 for none): a chain whose image
// lies past it is rejected before its crossings are computed.
bool mirror_chain(uint mi, uint mj, uint mk, vec3 p, vec3 n_p, vec3 light, float p_inv_range, out vec3 r_img, out vec3 r_p_img, out vec3 r_n_img, out float r_weight, out vec3 r_c1, out vec3 r_c2, out vec3 r_c3) {
	uint chain[3];
	chain[0] = mi;
	chain[1] = mj;
	chain[2] = mk;
	uint len = mi < MAX_MIRROR_PLANES ? (mj < MAX_MIRROR_PLANES ? (mk < MAX_MIRROR_PLANES ? 3u : 2u) : 1u) : 0u;
	r_img = light;
	r_p_img = p;
	r_n_img = n_p;
	r_weight = 0.0;
	r_c1 = p;
	r_c2 = p;
	r_c3 = p;
	if (len == 0u) {
		return false;
	}
	// The light's images: imgs[s] is the light mirrored through chain[s..].
	vec3 imgs[4];
	imgs[len] = light;
	for (int s = int(len) - 1; s >= 0; s--) {
		if (mirror_height(chain[s], imgs[s + 1]) <= 0.0) {
			return false;
		}
		imgs[s] = mirror_point(chain[s], imgs[s + 1]);
	}
	r_img = imgs[0];
	vec3 rel_img = r_img - p;
	if (dot(rel_img, rel_img) * p_inv_range * p_inv_range >= 1.0) {
		return false; // The image is out of the light's range.
	}
	// The first crossing lies on the segment from p to the image, whose
	// in-plane coordinates on chain[0] are p's and the image's (a
	// reflection through the plane keeps them): both outside the rectangle
	// on the same side, and the segment misses it. The cheap rejection of
	// a mirror in another room, before the crossing math.
	{
		vec3 dp = p - params.mirrors[chain[0]].center.xyz;
		vec3 di = r_img - params.mirrors[chain[0]].center.xyz;
		vec2 up = vec2(dot(dp, params.mirrors[chain[0]].u_axis.xyz), dot(dp, params.mirrors[chain[0]].v_axis.xyz));
		vec2 ui = vec2(dot(di, params.mirrors[chain[0]].u_axis.xyz), dot(di, params.mirrors[chain[0]].v_axis.xyz));
		vec2 h = params.mirrors[chain[0]].params.zw + 0.5;
		if (any(bvec2(up.x > h.x && ui.x > h.x, up.y > h.y && ui.y > h.y)) || any(bvec2(up.x < -h.x && ui.x < -h.x, up.y < -h.y && ui.y < -h.y))) {
			return false;
		}
	}
	// The crossings: from p toward imgs[0] across chain[0], then from that
	// crossing toward imgs[1] across chain[1] (the path folded back into
	// the room), and so on to the light.
	vec3 from = p;
	float w = 1.0;
	for (uint s = 0u; s < len; s++) {
		vec3 c;
		float cs;
		float cover = mirror_crossing(chain[s], from, imgs[s], c, cs);
		if (cover <= 0.0) {
			return false;
		}
		w *= cover * mirror_fresnel(chain[s], cs);
		if (s == 0u) {
			r_c1 = c;
		} else if (s == 1u) {
			r_c2 = c;
		} else {
			r_c3 = c;
		}
		from = c;
	}
	for (uint s = 0u; s < len; s++) {
		r_p_img = mirror_point(chain[s], r_p_img);
		r_n_img = mirror_dir(chain[s], r_n_img);
	}
	r_weight = w;
	return true;
}

// Whether a mirror is a major one (four square metres or more): the
// chains of two and three run between major mirrors only (a floor and a
// ceiling); a tabletop gets single images alone. The chains' cost is in
// the evaluations that the crossing tests reject only after the chain's
// geometry, and a small mirror's are nearly all rejected.
bool mirror_major(uint i) {
	return params.mirrors[i].params.z * params.mirrors[i].params.w >= 1.0;
}

// Whether a chain is worth evaluating: within the order asked for
// (params.mirror_order), no mirror twice in a row, every mirror of a
// chain of two or more major, and the product of the F0s (the image's
// weight at normal incidence) over a twentieth.
bool mirror_chain_counts(uint mi, uint mj, uint mk) {
	uint len = mj < MAX_MIRROR_PLANES ? (mk < MAX_MIRROR_PLANES ? 3u : 2u) : 1u;
	if (len > params.mirror_order || (len >= 2u && mi == mj) || (len >= 3u && mj == mk)) {
		return false;
	}
	if (len == 1u) {
		return true;
	}
	if (!mirror_major(mi) || !mirror_major(mj) || (len >= 3u && !mirror_major(mk))) {
		return false;
	}
	float f = params.mirrors[mi].params.x * params.mirrors[mj].params.x;
	if (len >= 3u) {
		f *= params.mirrors[mk].params.x;
	}
	return f >= 0.05;
}

// The chains a point faces, in priority order (singles, pairs, triples),
// packed three bits per mirror (mi | mj << 3 | mk << 6, MAX_MIRROR_PLANES
// ending the chain), at most MIRROR_CHAINS_MAX of them.
#define MIRROR_CHAINS_MAX 8u
#define MIRROR_CHAIN_A(c) ((c) & 7u)
#define MIRROR_CHAIN_B(c) (((c) >> 3u) & 7u)
#define MIRROR_CHAIN_C(c) (((c) >> 6u) & 7u)
uint mirror_chains_at(vec3 p, out uint r_chains[MIRROR_CHAINS_MAX]) {
	uint n = 0u;
	uint facing = 0u;
	for (uint mi = 0u; mi < mirror_count(); mi++) {
		if (mirror_height(mi, p) > 0.005) {
			facing |= 1u << mi;
			if (n < MIRROR_CHAINS_MAX) {
				r_chains[n++] = mi | (MAX_MIRROR_PLANES << 3u) | (MAX_MIRROR_PLANES << 6u);
			}
		}
	}
	for (uint len = 2u; len <= MIRROR_CHAIN_MAX && len <= params.mirror_order; len++) {
		for (uint mi = 0u; mi < mirror_count(); mi++) {
			if ((facing & (1u << mi)) == 0u) {
				continue;
			}
			for (uint mj = 0u; mj < mirror_count(); mj++) {
				for (uint mk = 0u; mk < (len == 3u ? mirror_count() : 1u); mk++) {
					uint k = len == 3u ? mk : MAX_MIRROR_PLANES;
					if (n < MIRROR_CHAINS_MAX && mirror_chain_counts(mi, mj, k)) {
						r_chains[n++] = mi | (mj << 3u) | (k << 6u);
					}
				}
			}
		}
	}
	return n;
}
