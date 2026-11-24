#pragma once

// The shapes of the organs and bones a burst trunk throws out (docs/design/body-health.md "Trunks burst"): heart, lungs,
// liver, stomach, kidneys, spleen, windpipe, a length of spine, a rack of ribs, lumps of fat, and the intestines as a
// tube along any path (the rope actor rebuilds it every frame while it flops). Plain C++ with no Unreal types, so the
// cloud can build and render them (tools/gore_organs_preview.cpp); GoreOrgans.cpp turns them into procedural meshes.
//
// Sizes are a grown man's in cm, the organ's middle at the origin; the caller scales them by the body's size, so the
// same organs serve every body. They look the same on every race (sneed, 2026-11-23). Colours are linear base colours;
// the alpha is dryness as M_Gore and M_Organ read it (0 wet and glossy, 1 dry). Triangles wind so that
// cross(p1 - p0, p2 - p0) points out of the surface (GoreOrgans.cpp swaps two corners for Unreal's clockwise fronts).

#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace goreshape
{
	constexpr float kPi = 3.14159265358979f;

	struct V3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		V3() = default;
		V3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
		V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
		V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
		V3 operator*(float s) const { return {x * s, y * s, z * s}; }
		V3 operator-() const { return {-x, -y, -z}; }
		V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
	};
	inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
	inline float length(const V3& a) { return std::sqrt(dot(a, a)); }
	inline V3 normalize(const V3& a, const V3& fallback = V3(0, 0, 1))
	{
		const float l = length(a);
		return l > 1e-6f ? a * (1.0f / l) : fallback;
	}
	inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
	inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
	inline float smooth(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }

	struct C4
	{
		float r = 1.0f, g = 1.0f, b = 1.0f, a = 0.0f;
		C4() = default;
		C4(float R, float G, float B, float A = 0.0f) : r(R), g(G), b(B), a(A) {}
	};
	inline C4 mix(const C4& a, const C4& b, float t)
	{
		t = clamp01(t);
		return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t), lerpf(a.a, b.a, t)};
	}
	inline C4 scaled(const C4& c, float s) { return {c.r * s, c.g * s, c.b * s, c.a}; }

	// The tissues (linear base colours; alpha dryness).
	inline const C4 kBlood(0.16f, 0.008f, 0.006f, 0.02f);
	inline const C4 kMyocardium(0.20f, 0.028f, 0.022f, 0.04f);
	inline const C4 kEpicardialFat(0.66f, 0.47f, 0.17f, 0.06f);
	inline const C4 kVessel(0.50f, 0.30f, 0.26f, 0.08f);
	inline const C4 kVein(0.13f, 0.045f, 0.075f, 0.05f);
	inline const C4 kLung(0.46f, 0.22f, 0.22f, 0.10f);
	inline const C4 kLungDark(0.13f, 0.06f, 0.07f, 0.10f);
	inline const C4 kLiver(0.16f, 0.034f, 0.021f, 0.02f);
	inline const C4 kBile(0.10f, 0.16f, 0.03f, 0.02f);
	inline const C4 kStomach(0.52f, 0.29f, 0.23f, 0.06f);
	inline const C4 kSmallGut(0.50f, 0.25f, 0.21f, 0.03f);
	inline const C4 kLargeGut(0.40f, 0.24f, 0.20f, 0.04f);
	inline const C4 kTaenia(0.66f, 0.52f, 0.42f, 0.08f);
	inline const C4 kKidney(0.21f, 0.048f, 0.034f, 0.03f);
	inline const C4 kSpleen(0.12f, 0.022f, 0.035f, 0.03f);
	inline const C4 kFat(0.72f, 0.55f, 0.22f, 0.08f);
	inline const C4 kBone(0.45f, 0.40f, 0.32f, 0.45f);
	inline const C4 kMarrow(0.30f, 0.06f, 0.035f, 0.05f);
	inline const C4 kDisc(0.66f, 0.60f, 0.52f, 0.25f);
	inline const C4 kCord(0.78f, 0.72f, 0.62f, 0.10f);
	inline const C4 kCartilage(0.72f, 0.58f, 0.52f, 0.20f);
	inline const C4 kMuscle(0.30f, 0.026f, 0.018f, 0.05f);
	inline const C4 kHole(0.03f, 0.006f, 0.005f, 0.05f);

	// ---------------------------------------------------------------------------------------------------------------
	// Noise

	inline float hash3(int x, int y, int z, uint32_t seed)
	{
		uint32_t h = seed * 0x9E3779B9u ^ static_cast<uint32_t>(x) * 0x85EBCA6Bu ^ static_cast<uint32_t>(y) * 0xC2B2AE35u ^ static_cast<uint32_t>(z) * 0x27D4EB2Fu;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		h *= 0x297A2D39u;
		h ^= h >> 15;
		return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
	}

	/** Smooth value noise, 0 to 1. */
	inline float noise(const V3& p, uint32_t seed)
	{
		const int ix = static_cast<int>(std::floor(p.x));
		const int iy = static_cast<int>(std::floor(p.y));
		const int iz = static_cast<int>(std::floor(p.z));
		const float fx = smooth(p.x - ix);
		const float fy = smooth(p.y - iy);
		const float fz = smooth(p.z - iz);
		float c[2][2][2];
		for (int dx = 0; dx < 2; ++dx)
			for (int dy = 0; dy < 2; ++dy)
				for (int dz = 0; dz < 2; ++dz)
					c[dx][dy][dz] = hash3(ix + dx, iy + dy, iz + dz, seed);
		const float x00 = lerpf(c[0][0][0], c[1][0][0], fx);
		const float x10 = lerpf(c[0][1][0], c[1][1][0], fx);
		const float x01 = lerpf(c[0][0][1], c[1][0][1], fx);
		const float x11 = lerpf(c[0][1][1], c[1][1][1], fx);
		return lerpf(lerpf(x00, x10, fy), lerpf(x01, x11, fy), fz);
	}

	/** A few octaves of noise, about -1 to 1. */
	inline float fbm(const V3& p, uint32_t seed, int octaves = 3)
	{
		float sum = 0.0f;
		float amp = 0.5f;
		float freq = 1.0f;
		float norm = 0.0f;
		for (int i = 0; i < octaves; ++i)
		{
			sum += amp * (noise(p * freq, seed + static_cast<uint32_t>(i) * 101u) * 2.0f - 1.0f);
			norm += amp;
			amp *= 0.5f;
			freq *= 2.03f;
		}
		return sum / norm;
	}

	/** A small random number source (deterministic per seed). */
	struct Rand
	{
		uint32_t s;
		explicit Rand(uint32_t seed) : s(seed * 747796405u + 2891336453u) {}
		float next()
		{
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			return static_cast<float>(s & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
		}
		float range(float a, float b) { return a + (b - a) * next(); }
	};

	// ---------------------------------------------------------------------------------------------------------------
	// Mesh

	struct Mesh
	{
		std::vector<V3> P;
		std::vector<V3> N;
		std::vector<C4> C;
		std::vector<float> U;
		std::vector<float> V;
		std::vector<int> T;

		int add(const V3& p, const C4& c, float u = 0.0f, float v = 0.0f)
		{
			P.push_back(p);
			C.push_back(c);
			U.push_back(u);
			V.push_back(v);
			return static_cast<int>(P.size()) - 1;
		}
		void tri(int a, int b, int c)
		{
			T.push_back(a);
			T.push_back(b);
			T.push_back(c);
		}
		/** Smooth normals from the faces round each vertex (area weighted). */
		void computeNormals()
		{
			N.assign(P.size(), V3());
			for (size_t i = 0; i + 2 < T.size(); i += 3)
			{
				const V3 n = cross(P[T[i + 1]] - P[T[i]], P[T[i + 2]] - P[T[i]]);
				N[T[i]] += n;
				N[T[i + 1]] += n;
				N[T[i + 2]] += n;
			}
			for (V3& n : N)
			{
				n = normalize(n);
			}
		}
		void append(const Mesh& o)
		{
			const int base = static_cast<int>(P.size());
			P.insert(P.end(), o.P.begin(), o.P.end());
			C.insert(C.end(), o.C.begin(), o.C.end());
			U.insert(U.end(), o.U.begin(), o.U.end());
			V.insert(V.end(), o.V.begin(), o.V.end());
			for (int t : o.T)
			{
				T.push_back(base + t);
			}
		}
		/** Half the size of the box round it along its smallest side (how high its middle rests over the ground). */
		float restHeight() const
		{
			if (P.empty())
			{
				return 1.0f;
			}
			V3 lo = P[0], hi = P[0];
			for (const V3& p : P)
			{
				lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
				hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
			}
			const V3 s = hi - lo;
			return 0.5f * std::fmin(s.x, std::fmin(s.y, s.z));
		}
		/** The longest side of the box round it. */
		float span() const
		{
			if (P.empty())
			{
				return 0.0f;
			}
			V3 lo = P[0], hi = P[0];
			for (const V3& p : P)
			{
				lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
				hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
			}
			const V3 s = hi - lo;
			return std::fmax(s.x, std::fmax(s.y, s.z));
		}
		void transform(const std::function<V3(const V3&)>& f)
		{
			for (V3& p : P)
			{
				p = f(p);
			}
		}
		/** Moved so the middle of its box is at the origin. */
		void centre()
		{
			if (P.empty())
			{
				return;
			}
			V3 lo = P[0], hi = P[0];
			for (const V3& p : P)
			{
				lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
				hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
			}
			const V3 m = (lo + hi) * 0.5f;
			for (V3& p : P)
			{
				p = p - m;
			}
		}
	};

	/**
	 * A closed blob: `rows` rings from the top pole (v 0) to the bottom (v 1), `cols` round each (u 0 to 1). `pos(u, v,
	 * unit)` gives the surface point from the unit sphere's point there; `col(u, v, unit, p)` its colour.
	 */
	inline void addBlob(Mesh& m, int rows, int cols, const std::function<V3(float, float, const V3&)>& pos,
		const std::function<C4(float, float, const V3&, const V3&)>& col)
	{
		const int base = static_cast<int>(m.P.size());
		for (int r = 0; r <= rows; ++r)
		{
			const float v = static_cast<float>(r) / rows;
			const float phi = kPi * v;
			for (int c = 0; c < cols; ++c)
			{
				const float u = static_cast<float>(c) / cols;
				const float th = 2.0f * kPi * u;
				const V3 unit(std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi));
				const V3 p = pos(u, v, unit);
				m.add(p, col(u, v, unit, p), u, v);
			}
		}
		for (int r = 0; r < rows; ++r)
		{
			for (int c = 0; c < cols; ++c)
			{
				const int a = base + r * cols + c;
				const int b = base + (r + 1) * cols + c;
				const int d = base + r * cols + (c + 1) % cols;
				const int e = base + (r + 1) * cols + (c + 1) % cols;
				if (r > 0)
				{
					m.tri(a, b, d);
				}
				if (r + 1 < rows)
				{
					m.tri(b, e, d);
				}
			}
		}
	}

	/** Frames along a path that don't twist (parallel transport): for each point its tangent, side and up. */
	inline void pathFrames(const std::vector<V3>& path, std::vector<V3>& tangent, std::vector<V3>& side, std::vector<V3>& up)
	{
		const size_t n = path.size();
		tangent.assign(n, V3(0, 0, 1));
		side.assign(n, V3(1, 0, 0));
		up.assign(n, V3(0, 1, 0));
		if (n < 2)
		{
			return;
		}
		for (size_t i = 0; i < n; ++i)
		{
			const V3 a = path[i == 0 ? 0 : i - 1];
			const V3 b = path[i + 1 < n ? i + 1 : n - 1];
			tangent[i] = normalize(b - a);
		}
		V3 ref = std::fabs(tangent[0].z) < 0.9f ? V3(0, 0, 1) : V3(1, 0, 0);
		side[0] = normalize(cross(tangent[0], ref));
		up[0] = cross(side[0], tangent[0]);
		for (size_t i = 1; i < n; ++i)
		{
			// Carry the last side over to this tangent (project out the tangent).
			V3 s = side[i - 1] - tangent[i] * dot(side[i - 1], tangent[i]);
			s = normalize(s, side[i - 1]);
			side[i] = s;
			up[i] = cross(s, tangent[i]);
		}
	}

	/**
	 * A tube along `path`: `radius(t, angle)` (t 0 to 1 along it) and `col(t, angle, s)` (s: cm along it). The ends are
	 * closed with a rounded dome (`domeStart`/`domeEnd`) or cut open: a rim of wall and a dark hole set a little in.
	 */
	inline void addTube(Mesh& m, const std::vector<V3>& path, int sides, const std::function<float(float, float)>& radius,
		const std::function<C4(float, float, float)>& col, bool domeStart, bool domeEnd, const C4& cutWall = kVessel)
	{
		const size_t n = path.size();
		if (n < 2)
		{
			return;
		}
		std::vector<V3> tan, side, up;
		pathFrames(path, tan, side, up);
		std::vector<float> along(n, 0.0f);
		for (size_t i = 1; i < n; ++i)
		{
			along[i] = along[i - 1] + length(path[i] - path[i - 1]);
		}
		const float total = std::fmax(along[n - 1], 1e-3f);
		const int base = static_cast<int>(m.P.size());
		for (size_t i = 0; i < n; ++i)
		{
			const float t = along[i] / total;
			for (int k = 0; k < sides; ++k)
			{
				const float ang = 2.0f * kPi * k / sides;
				const float r = radius(t, ang);
				const V3 dir = side[i] * std::cos(ang) + up[i] * std::sin(ang);
				m.add(path[i] + dir * r, col(t, ang, along[i]), static_cast<float>(k) / sides, along[i] * 0.1f);
			}
		}
		// Faces out: (side, tangent, up) is right-handed, so cross(along, round) points away from the axis.
		for (size_t i = 0; i + 1 < n; ++i)
		{
			for (int k = 0; k < sides; ++k)
			{
				const int a = base + static_cast<int>(i) * sides + k;
				const int b = base + static_cast<int>(i) * sides + (k + 1) % sides;
				const int c = base + static_cast<int>(i + 1) * sides + k;
				const int d = base + static_cast<int>(i + 1) * sides + (k + 1) % sides;
				m.tri(a, c, b);
				m.tri(b, c, d);
			}
		}
		const auto end = [&](bool atStart, bool dome)
		{
			const size_t i = atStart ? 0 : n - 1;
			const V3 out = atStart ? -tan[i] : tan[i];
			const float t = atStart ? 0.0f : 1.0f;
			const int ring = base + static_cast<int>(i) * sides;
			if (dome)
			{
				// Rounded shut: two smaller rings and a point.
				int prev = ring;
				const float r0 = radius(t, 0.0f);
				for (int step = 1; step <= 2; ++step)
				{
					const float f = step == 1 ? 0.75f : 0.4f;
					const float push = step == 1 ? 0.45f : 0.85f;
					const int next = static_cast<int>(m.P.size());
					for (int k = 0; k < sides; ++k)
					{
						const float ang = 2.0f * kPi * k / sides;
						const V3 dir = side[i] * std::cos(ang) + up[i] * std::sin(ang);
						m.add(path[i] + dir * (radius(t, ang) * f) + out * (r0 * push), col(t, ang, along[i]));
					}
					for (int k = 0; k < sides; ++k)
					{
						const int a = prev + k, b = prev + (k + 1) % sides, c = next + k, d = next + (k + 1) % sides;
						if (atStart) { m.tri(a, b, c); m.tri(b, d, c); }
						else { m.tri(a, c, b); m.tri(b, c, d); }
					}
					prev = next;
				}
				const int tip = m.add(path[i] + out * r0, col(t, 0.0f, along[i]));
				for (int k = 0; k < sides; ++k)
				{
					const int a = prev + k, b = prev + (k + 1) % sides;
					if (atStart) { m.tri(a, b, tip); }
					else { m.tri(a, tip, b); }
				}
				return;
			}
			// Cut open: the wall's rim turns in, then a dark hole set back into the tube.
			const int rim = static_cast<int>(m.P.size());
			for (int k = 0; k < sides; ++k)
			{
				const float ang = 2.0f * kPi * k / sides;
				const V3 dir = side[i] * std::cos(ang) + up[i] * std::sin(ang);
				m.add(path[i] + dir * (radius(t, ang) * 0.72f) + out * (radius(t, ang) * 0.05f), cutWall);
			}
			const int hole = static_cast<int>(m.P.size());
			for (int k = 0; k < sides; ++k)
			{
				const float ang = 2.0f * kPi * k / sides;
				const V3 dir = side[i] * std::cos(ang) + up[i] * std::sin(ang);
				m.add(path[i] + dir * (radius(t, ang) * 0.6f) - out * (radius(t, ang) * 0.5f), kHole);
			}
			const int middle = m.add(path[i] - out * (radius(t, 0.0f) * 0.7f), kHole);
			for (int k = 0; k < sides; ++k)
			{
				const int a = ring + k, b = ring + (k + 1) % sides, c = rim + k, d = rim + (k + 1) % sides;
				const int e = hole + k, f = hole + (k + 1) % sides;
				if (atStart)
				{
					m.tri(a, b, c); m.tri(b, d, c);
					m.tri(c, d, e); m.tri(d, f, e);
					m.tri(e, f, middle);
				}
				else
				{
					m.tri(a, c, b); m.tri(b, c, d);
					m.tri(c, e, d); m.tri(d, e, f);
					m.tri(e, middle, f);
				}
			}
		};
		end(true, domeStart);
		end(false, domeEnd);
	}

	/** Points on a smooth curve through `pts` (Catmull-Rom), `per` points between each two. */
	inline std::vector<V3> smoothPath(const std::vector<V3>& pts, int per)
	{
		std::vector<V3> out;
		const size_t n = pts.size();
		if (n < 2)
		{
			return pts;
		}
		for (size_t i = 0; i + 1 < n; ++i)
		{
			const V3 p0 = pts[i == 0 ? 0 : i - 1];
			const V3 p1 = pts[i];
			const V3 p2 = pts[i + 1];
			const V3 p3 = pts[i + 2 < n ? i + 2 : n - 1];
			for (int k = 0; k < per; ++k)
			{
				const float t = static_cast<float>(k) / per;
				const float t2 = t * t, t3 = t2 * t;
				out.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f);
			}
		}
		out.push_back(pts[n - 1]);
		return out;
	}

	/** Blood smeared over a tissue colour in patches (more where `extra` says). */
	inline C4 bloodied(const C4& c, const V3& p, uint32_t seed, float amount = 0.35f)
	{
		const float n = fbm(p * 0.35f, seed + 77u, 3) * 0.5f + 0.5f;
		const float t = clamp01((n - (1.0f - amount * 1.4f)) * 3.0f);
		C4 out = mix(c, kBlood, t * 0.85f);
		out.a = std::fmin(c.a, 0.03f + 0.5f * c.a * (1.0f - t));
		return out;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The organs

	/** The heart (about 12 cm from base to apex), with the stumps of the aorta, the pulmonary trunk and the vena cava. */
	inline Mesh makeHeart(uint32_t seed)
	{
		Mesh m;
		const float tilt = 0.42f;  // the apex points down, forward and to the left
		addBlob(m, 22, 28,
			[&](float u, float v, const V3& s)
			{
				// An egg narrowing to the apex below; the right ventricle's bulge on the front; lumpy muscle.
				const float low = clamp01((1.0f - s.z) * 0.5f);
				const float taper = lerpf(1.0f, 0.18f, std::pow(low, 1.7f));
				float x = s.x * 4.3f * taper;
				float y = s.y * 3.3f * taper * (s.y > 0.0f ? 1.15f : 0.9f);
				float z = s.z * 6.2f - 0.6f * low * low;
				// The interventricular groove down the front, and the coronary groove round the top.
				const float th = 2.0f * kPi * u;
				const float groove = std::exp(-std::pow(std::remainder(th - 1.25f, 2.0f * kPi) / 0.22f, 2.0f)) * (1.0f - low);
				const float ring = std::exp(-std::pow((s.z - 0.45f) / 0.09f, 2.0f));
				const float lumps = fbm(s * 2.2f, seed, 3) * 0.28f;
				const float in = 0.35f * groove + 0.45f * ring;
				const V3 out = normalize(V3(x, y, z * 0.4f));
				x += out.x * (lumps - in);
				y += out.y * (lumps - in);
				z += out.z * (lumps - in);
				(void)v;
				// Tilted over.
				const float c = std::cos(tilt), sn = std::sin(tilt);
				return V3(x * c + z * sn, y, -x * sn + z * c);
			},
			[&](float u, float v, const V3& s, const V3& p)
			{
				const float th = 2.0f * kPi * u;
				const float low = clamp01((1.0f - s.z) * 0.5f);
				const float groove = std::exp(-std::pow(std::remainder(th - 1.25f, 2.0f * kPi) / 0.35f, 2.0f)) * (1.0f - low);
				const float ring = std::exp(-std::pow((s.z - 0.45f) / 0.16f, 2.0f));
				const float fatNoise = fbm(s * 3.0f, seed + 9u, 2) * 0.5f + 0.5f;
				float fat = clamp01(std::fmax(groove, ring) * 0.9f + (s.z > 0.6f ? (fatNoise - 0.5f) * 1.2f : 0.0f));
				C4 c = mix(kMyocardium, kEpicardialFat, fat);
				c = scaled(c, 0.85f + 0.3f * (fbm(p * 0.9f, seed + 4u, 2) * 0.5f + 0.5f));
				(void)v;
				return bloodied(c, p, seed, 0.3f);
			});
		// The great vessels, cut off above the heart.
		const auto vessel = [&](std::vector<V3> pts, float r, const C4& wall)
		{
			// Tilted with the heart, and rooted a little inside its top.
			const float c = std::cos(tilt), sn = std::sin(tilt);
			for (V3& q : pts)
			{
				q = V3(q.x * c + (q.z - 0.8f) * sn, q.y, -q.x * sn + (q.z - 0.8f) * c);
			}
			addTube(m, smoothPath(pts, 4), 12, [r](float, float) { return r; },
				[&, wall](float t, float a, float s) { return bloodied(mix(wall, scaled(wall, 0.8f), fbm(V3(s, a, t) * 0.5f, seed, 2) * 0.5f + 0.5f), V3(s, a, 0), seed, 0.25f); },
				false, false, wall);
		};
		vessel({V3(-0.6f, 0.2f, 4.2f), V3(-0.4f, 0.2f, 7.0f), V3(-1.2f, -0.6f, 9.0f), V3(-3.2f, -1.6f, 9.2f), V3(-4.4f, -2.4f, 7.6f)}, 1.45f, kVessel);
		vessel({V3(1.2f, 1.6f, 4.4f), V3(1.6f, 1.9f, 7.2f), V3(1.0f, 1.2f, 8.4f)}, 1.3f, mix(kVessel, kVein, 0.25f));
		vessel({V3(-2.4f, -0.6f, 4.0f), V3(-2.6f, -0.8f, 6.6f)}, 1.05f, kVein);
		m.centre();
		m.computeNormals();
		return m;
	}

	/** A lung (about 24 cm tall): pinkish grey and mottled, a fissure across it, the bronchus and vessels cut at its root. */
	inline Mesh makeLung(uint32_t seed, bool bLeft)
	{
		Mesh m;
		const float side = bLeft ? -1.0f : 1.0f;  // +x is the outer side of a right lung
		addBlob(m, 26, 30,
			[&](float u, float v, const V3& s)
			{
				(void)u;
				(void)v;
				float x = s.x, y = s.y, z = s.z;
				// Narrow at the apex, broad and hollowed underneath (the dome of the diaphragm), flat and hollowed inside
				// where it lay against the heart.
				const float top = clamp01(z);
				x *= lerpf(1.0f, 0.45f, top * top);
				y *= lerpf(1.0f, 0.55f, top * top);
				if (z < -0.55f)
				{
					z += 0.35f * (1.0f - (x * x + y * y)) * clamp01((-0.55f - z) * 3.0f);
				}
				if (x < 0.0f)
				{
					x *= 0.5f;
					x += 0.35f * std::exp(-(std::pow(y / 0.35f, 2.0f) + std::pow((z - 0.05f) / 0.3f, 2.0f)));
					if (bLeft)
					{
						x += 0.25f * std::exp(-(std::pow((y - 0.55f) / 0.3f, 2.0f) + std::pow((z + 0.35f) / 0.3f, 2.0f)));  // the heart's notch
					}
				}
				V3 p(x * 5.6f, y * 8.2f, z * 12.0f);
				// The oblique fissure: a groove running down and forward.
				const float d = (z - 0.35f + 0.85f * y);
				const float fis = std::exp(-std::pow(d / 0.05f, 2.0f));
				const V3 n = normalize(V3(s.x / 5.6f, s.y / 8.2f, s.z / 12.0f));
				p += n * (fbm(s * 2.5f, seed, 3) * 0.35f - 0.45f * fis);
				p.x *= side;
				return p;
			},
			[&](float u, float v, const V3& s, const V3& p)
			{
				(void)u;
				(void)v;
				const float d = (s.z - 0.35f + 0.85f * s.y);
				const float fis = std::exp(-std::pow(d / 0.07f, 2.0f));
				const float mottle = fbm(p * 0.8f, seed + 3u, 3) * 0.5f + 0.5f;
				const float spots = clamp01((fbm(p * 2.2f, seed + 8u, 2) * 0.5f + 0.5f - 0.62f) * 4.0f);
				C4 c = mix(kLung, kLungDark, 0.15f + 0.35f * mottle * mottle + 0.5f * spots + 0.6f * fis);
				// Bloody where it was torn from its root.
				const float root = std::exp(-(std::pow(s.y / 0.3f, 2.0f) + std::pow((s.z - 0.05f) / 0.25f, 2.0f))) * (s.x < 0.0f ? 1.0f : 0.0f);
				c = mix(c, kBlood, root * 0.7f);
				return bloodied(c, p, seed, 0.2f);
			});
		// The root: bronchus (cartilage), artery and vein, cut short.
		const float inner = -side;
		addTube(m, {V3(inner * 1.6f, 0.0f, 1.0f), V3(inner * 3.4f, 0.2f, 1.6f)}, 10, [](float, float) { return 0.85f; },
			[](float, float, float) { return kCartilage; }, false, false, kCartilage);
		addTube(m, {V3(inner * 1.6f, 1.4f, 2.0f), V3(inner * 3.2f, 1.6f, 2.8f)}, 10, [](float, float) { return 0.75f; },
			[](float, float, float) { return mix(kVein, kVessel, 0.3f); }, false, false, kVein);
		addTube(m, {V3(inner * 1.6f, -1.2f, -0.2f), V3(inner * 3.0f, -1.4f, -0.6f)}, 10, [](float, float) { return 0.7f; },
			[](float, float, float) { return mix(kVessel, kBlood, 0.4f); }, false, false, kVessel);
		if (bLeft)
		{
			// Mirrored, so its faces turned inside out: wind them back.
			for (size_t i = 0; i + 2 < m.T.size(); i += 3)
			{
				std::swap(m.T[i + 1], m.T[i + 2]);
			}
		}
		m.centre();
		m.computeNormals();
		return m;
	}

	/** The liver (about 24 cm across): a dark glossy wedge, thick on the right and thin on the left, the gall bladder under it. */
	inline Mesh makeLiver(uint32_t seed)
	{
		Mesh m;
		addBlob(m, 22, 34,
			[&](float u, float v, const V3& s)
			{
				(void)u;
				(void)v;
				const float t = clamp01((s.x + 1.0f) * 0.5f);  // 0 right lobe, 1 the thin tip of the left
				const float zs = lerpf(1.0f, 0.22f, std::pow(t, 1.15f));
				const float ys = lerpf(1.0f, 0.62f, t);
				float z = s.z;
				if (z < 0.0f)
				{
					z *= 0.5f;  // flat underneath
				}
				// The sharp lower front edge.
				float y = s.y;
				if (y > 0.2f && z < 0.1f)
				{
					z = lerpf(z, -0.05f, clamp01((y - 0.2f) * 1.6f));
				}
				V3 p(s.x * 12.5f, y * 7.6f * ys, z * 7.0f * zs);
				// The groove between the lobes on top, and a soft lumpiness.
				const float lig = std::exp(-std::pow((s.x - 0.18f) / 0.06f, 2.0f)) * clamp01(s.z * 2.0f);
				const V3 n = normalize(V3(s.x / 12.5f, s.y / 7.6f, s.z / 5.0f));
				p += n * (fbm(s * 1.8f, seed, 3) * 0.4f - lig * 0.6f);
				return p;
			},
			[&](float u, float v, const V3& s, const V3& p)
			{
				(void)u;
				(void)v;
				(void)s;
				const float tone = fbm(p * 0.5f, seed + 5u, 3) * 0.5f + 0.5f;
				C4 c = scaled(kLiver, 0.8f + 0.45f * tone);
				return bloodied(c, p, seed, 0.25f);
			});
		// The gall bladder: a greenish pear tucked under the right lobe.
		Mesh gall;
		addBlob(gall, 10, 14,
			[&](float, float, const V3& s)
			{
				const float pear = s.z > 0.0f ? lerpf(1.0f, 0.6f, s.z) : 1.0f;
				return V3(s.x * 1.9f * pear, s.y * 1.9f * pear, s.z * 4.2f);
			},
			[&](float, float, const V3&, const V3& p) { return bloodied(mix(kBile, scaled(kBile, 1.4f), fbm(p, seed + 11u, 2) * 0.5f + 0.5f), p, seed, 0.15f); });
		gall.transform([](const V3& p) { return V3(p.x * 0.9f - p.z * 0.4f - 2.0f, p.y + 2.6f, p.z * 0.3f + p.x * 0.4f - 2.0f); });
		m.append(gall);
		m.centre();
		m.computeNormals();
		return m;
	}

	/** The stomach (about 25 cm along its curve): a J-shaped bag with the gullet and the duodenum cut off. */
	inline Mesh makeStomach(uint32_t seed)
	{
		Mesh m;
		const std::vector<V3> path = smoothPath({V3(-3.0f, 0.0f, 6.5f), V3(-5.4f, 0.0f, 2.0f), V3(-5.2f, 0.0f, -3.0f), V3(-2.0f, 0.0f, -7.0f),
			V3(3.0f, 0.0f, -7.8f), V3(6.8f, 0.0f, -5.0f), V3(8.0f, 0.0f, -2.0f)}, 5);
		addTube(m, path, 22,
			[&](float t, float a)
			{
				// Fundus wide, body, then the narrowing antrum and the pylorus; flattened front to back.
				const float r = t < 0.15f ? lerpf(4.6f, 6.4f, t / 0.15f) : (t < 0.55f ? lerpf(6.4f, 5.2f, (t - 0.15f) / 0.4f) : lerpf(5.2f, 1.8f, std::pow((t - 0.55f) / 0.45f, 1.3f)));
				const float flat = 0.72f + 0.28f * std::fabs(std::cos(a));
				return r * flat * (1.0f + 0.05f * fbm(V3(t * 8.0f, a, 0.0f), seed, 2));
			},
			[&](float t, float a, float s)
			{
				// Vessels running round from the curves: thin darker lines.
				const float lines = std::pow(std::fabs(std::sin(s * 0.9f + 2.0f * fbm(V3(s * 0.2f, a, 0), seed + 2u, 2))), 18.0f);
				C4 c = mix(kStomach, kVessel, 0.15f * (fbm(V3(s * 0.3f, a * 2.0f, t), seed, 2) * 0.5f + 0.5f));
				c = mix(c, mix(kVein, kBlood, 0.5f), lines * 0.5f);
				return bloodied(c, V3(s, a * 3.0f, t * 9.0f), seed, 0.2f);
			},
			true, false, kStomach);
		// The gullet going in at the top.
		addTube(m, {V3(-1.4f, 0.0f, 5.8f), V3(-0.8f, 0.0f, 8.0f), V3(-0.6f, 0.0f, 10.0f)}, 12, [](float, float) { return 1.15f; },
			[&](float, float, float) { return mix(kStomach, kMuscle, 0.25f); }, false, false, mix(kStomach, kMuscle, 0.3f));
		m.centre();
		m.computeNormals();
		return m;
	}

	/** A kidney (about 11 cm): a dark bean with its dent, a little fat, and the ureter and vessels cut short. */
	inline Mesh makeKidney(uint32_t seed)
	{
		Mesh m;
		addBlob(m, 18, 24,
			[&](float, float, const V3& s)
			{
				float x = s.x;
				if (x > 0.0f)
				{
					x -= 0.75f * std::exp(-std::pow(s.z / 0.42f, 2.0f)) * x;  // the hilum's dent on the inner side
				}
				V3 p(x * 3.0f, s.y * 1.7f, s.z * 5.6f);
				p += normalize(s) * (fbm(s * 2.0f, seed, 2) * 0.15f);
				return p;
			},
			[&](float, float, const V3& s, const V3& p)
			{
				const float hilum = std::exp(-(std::pow(s.z / 0.35f, 2.0f))) * clamp01(s.x * 2.0f);
				C4 c = scaled(kKidney, 0.85f + 0.3f * (fbm(p * 0.8f, seed + 3u, 2) * 0.5f + 0.5f));
				c = mix(c, kFat, hilum * 0.8f);
				return bloodied(c, p, seed, 0.2f);
			});
		addTube(m, smoothPath({V3(1.4f, 0.0f, -0.4f), V3(3.2f, 0.2f, -1.8f), V3(3.6f, 0.4f, -5.0f)}, 3), 8, [](float, float) { return 0.35f; },
			[](float, float, float) { return kCord; }, false, false, kCord);
		addTube(m, {V3(1.4f, 0.0f, 0.6f), V3(3.6f, -0.2f, 1.0f)}, 10, [](float, float) { return 0.55f; },
			[](float, float, float) { return mix(kVessel, kBlood, 0.3f); }, false, false, kVessel);
		m.centre();
		m.computeNormals();
		return m;
	}

	/** The spleen (about 12 cm): a flattened, curved, dark purple-red bean. */
	inline Mesh makeSpleen(uint32_t seed)
	{
		Mesh m;
		addBlob(m, 16, 22,
			[&](float, float, const V3& s)
			{
				V3 p(s.x * 3.6f, s.y * 1.6f, s.z * 6.2f);
				p.x += 1.4f * (1.0f - s.z * s.z);  // curved round the stomach
				p += normalize(s) * (fbm(s * 2.4f, seed, 2) * 0.2f);
				return p;
			},
			[&](float, float, const V3&, const V3& p) { return bloodied(scaled(kSpleen, 0.85f + 0.3f * (fbm(p * 0.7f, seed + 1u, 2) * 0.5f + 0.5f)), p, seed, 0.3f); });
		m.centre();
		m.computeNormals();
		return m;
	}

	/** The windpipe and the gullet behind it (about 12 cm), torn at both ends: rings of cartilage under a wet sheen. */
	inline Mesh makeWindpipe(uint32_t seed)
	{
		Mesh m;
		std::vector<V3> pipe;
		for (int i = 0; i <= 24; ++i)
		{
			pipe.push_back(V3(0.15f * std::sin(i * 0.3f), 0.0f, -6.0f + 12.0f * i / 24.0f));
		}
		addTube(m, pipe, 14,
			[&](float t, float a)
			{
				// The C-shaped rings stand out; the back is flat muscle.
				const float rings = std::pow(std::fabs(std::sin(t * 12.0f * 3.6f)), 3.0f);
				const float back = std::sin(a) < -0.6f ? 0.85f : 1.0f;
				return (1.05f + 0.1f * rings) * back;
			},
			[&](float t, float a, float s)
			{
				const float rings = std::pow(std::fabs(std::sin(t * 12.0f * 3.6f)), 3.0f);
				const C4 c = std::sin(a) < -0.6f ? kMuscle : mix(mix(kCartilage, kMuscle, 0.35f), kCartilage, rings);
				return bloodied(c, V3(s, a, 0), seed, 0.3f);
			},
			false, false, kCartilage);
		std::vector<V3> gullet;
		for (int i = 0; i <= 12; ++i)
		{
			gullet.push_back(V3(0.2f * std::sin(i * 0.5f + 1.0f), -1.9f, -5.5f + 11.0f * i / 12.0f));
		}
		addTube(m, gullet, 10, [](float, float a) { return 0.8f * (0.75f + 0.25f * std::fabs(std::cos(a))); },
			[&](float, float a, float s) { return bloodied(mix(kStomach, kMuscle, 0.4f), V3(s, a, 1), seed, 0.35f); }, false, false, kMuscle);
		m.centre();
		m.computeNormals();
		return m;
	}

	/** A length of backbone (`count` vertebrae with their discs), snapped at both ends, the cord showing in the canal. */
	inline Mesh makeSpine(uint32_t seed, int count)
	{
		Mesh m;
		Rand rand(seed);
		const float step = 3.3f;
		for (int i = 0; i < count; ++i)
		{
			const float z = (i - (count - 1) * 0.5f) * step;
			const bool bottom = i == 0;
			const bool top = i == count - 1;
			// The body: a short, waisted drum.
			Mesh body;
			addBlob(body, 12, 18,
				[&](float, float, const V3& s)
				{
					const float waist = 1.0f - 0.12f * (1.0f - s.z * s.z);
					float zz = s.z > 0.0f ? std::fmin(s.z * 1.6f, 1.0f) : std::fmax(s.z * 1.6f, -1.0f);
					V3 p(s.x * 2.3f * waist, s.y * 1.9f * waist + 0.3f, zz * 1.25f);
					// Jagged where the spine snapped.
					if ((bottom && s.z < -0.3f) || (top && s.z > 0.3f))
					{
						p.z += (s.z > 0 ? 1.0f : -1.0f) * fbm(s * 3.0f, seed + i, 2) * 0.9f;
					}
					return p;
				},
				[&](float, float, const V3& s, const V3& p)
				{
					const bool broken = (bottom && s.z < -0.5f) || (top && s.z > 0.5f);
					return broken ? mix(kMarrow, kBlood, 0.3f) : bloodied(kBone, p * 2.0f, seed + i, 0.3f);
				});
			body.transform([z](const V3& p) { return V3(p.x, p.y, p.z + z); });
			m.append(body);
			// The arch behind, the spinous process down and back, the transverse processes out to the sides.
			const auto bone = [&](std::vector<V3> pts, float r0, float r1)
			{
				addTube(m, pts, 8, [r0, r1](float t, float) { return lerpf(r0, r1, t); },
					[&](float t, float a, float s) { return bloodied(kBone, V3(s, a, t + i), seed, 0.35f); }, false, true, kBone);
			};
			bone({V3(1.2f, -1.5f, z + 0.3f), V3(0.9f, -3.0f, z), V3(0.0f, -3.4f, z - 0.2f)}, 0.55f, 0.5f);
			bone({V3(-1.2f, -1.5f, z + 0.3f), V3(-0.9f, -3.0f, z), V3(0.0f, -3.4f, z - 0.2f)}, 0.55f, 0.5f);
			bone({V3(0.0f, -3.3f, z - 0.1f), V3(0.0f, -5.4f, z - 1.2f), V3(0.0f, -6.3f, z - 1.9f)}, 0.6f, 0.4f);
			bone({V3(1.0f, -2.6f, z + 0.2f), V3(3.2f, -2.9f, z + 0.4f), V3(4.2f, -3.1f, z + 0.3f)}, 0.5f, 0.45f);
			bone({V3(-1.0f, -2.6f, z + 0.2f), V3(-3.2f, -2.9f, z + 0.4f), V3(-4.2f, -3.1f, z + 0.3f)}, 0.5f, 0.45f);
			// The disc above it.
			if (!top)
			{
				Mesh disc;
				addBlob(disc, 6, 18, [&](float, float, const V3& s) { return V3(s.x * 2.4f, s.y * 2.0f + 0.3f, z + step * 0.5f + s.z * 0.45f); },
					[&](float, float, const V3&, const V3& p) { return bloodied(kDisc, p * 2.0f, seed + 50u + i, 0.2f); });
				m.append(disc);
			}
		}
		// The cord through the canal, sticking out of the broken ends.
		const float half = (count - 1) * 0.5f * step + 0.6f;
		addTube(m, {V3(0.0f, -2.2f, -half - rand.range(0.2f, 1.2f)), V3(0.0f, -2.2f, half + rand.range(0.2f, 1.2f))}, 10, [](float, float) { return 0.6f; },
			[](float, float, float) { return kCord; }, false, false, mix(kCord, kBlood, 0.4f));
		m.centre();
		m.computeNormals();
		return m;
	}

	/** Three ribs still joined by the torn muscle between them, about 22 cm long, curved, broken off at both ends. */
	inline Mesh makeRibRack(uint32_t seed)
	{
		Mesh m;
		Rand rand(seed);
		const int ribs = 3;
		std::vector<std::vector<V3>> lines;
		for (int r = 0; r < ribs; ++r)
		{
			std::vector<V3> pts;
			const float z = (r - 1) * 2.6f;
			const float a0 = -0.75f + rand.range(-0.15f, 0.15f);
			const float a1 = 0.75f + rand.range(-0.15f, 0.15f);
			for (int k = 0; k <= 16; ++k)
			{
				const float a = lerpf(a0, a1, static_cast<float>(k) / 16.0f);
				pts.push_back(V3(std::sin(a) * 14.0f, std::cos(a) * 14.0f - 14.0f, z - 0.8f * std::sin(a)));
			}
			lines.push_back(pts);
			addTube(m, pts, 8, [](float, float a) { return 0.55f + 0.3f * std::fabs(std::sin(a)); },
				[&](float t, float a, float s) { return bloodied(kBone, V3(s, a, t * 4.0f + r), seed, 0.45f); }, false, false, kMarrow);
		}
		// The muscle between them: a thick strip, front and back, ragged at its ends.
		for (int r = 0; r + 1 < ribs; ++r)
		{
			const std::vector<V3>& a = lines[r];
			const std::vector<V3>& b = lines[r + 1];
			const size_t n = a.size() < b.size() ? a.size() : b.size();
			for (int face = 0; face < 2; ++face)
			{
				const float off = face == 0 ? 0.35f : -0.35f;
				const int base = static_cast<int>(m.P.size());
				for (size_t k = 2; k + 2 < n; ++k)
				{
					const V3 out = normalize(V3(a[k].x, a[k].y + 14.0f, 0.0f));
					for (int j = 0; j < 3; ++j)
					{
						const float t = j / 2.0f;
						const V3 p = a[k] * (1.0f - t) + b[k] * t + out * (off + 0.15f * fbm(V3(k * 0.7f, j, r), seed, 2));
						m.add(p, bloodied(mix(kMuscle, kBlood, 0.35f), p, seed + 7u, 0.4f));
					}
				}
				const int rows = static_cast<int>(n) - 4;
				for (int k = 0; k + 1 < rows; ++k)
				{
					for (int j = 0; j < 2; ++j)
					{
						const int p0 = base + k * 3 + j, p1 = base + k * 3 + j + 1, p2 = base + (k + 1) * 3 + j, p3 = base + (k + 1) * 3 + j + 1;
						if (face == 0) { m.tri(p0, p1, p2); m.tri(p1, p3, p2); }
						else { m.tri(p0, p2, p1); m.tri(p1, p2, p3); }
					}
				}
			}
		}
		m.centre();
		m.computeNormals();
		return m;
	}

	/** A lump of yellow fat (the omentum), lobed, with fine red vessels through it, about `size` cm. */
	inline Mesh makeFatLump(uint32_t seed, float size)
	{
		Mesh m;
		Rand rand(seed);
		const V3 radii(size * rand.range(0.45f, 0.6f), size * rand.range(0.3f, 0.45f), size * rand.range(0.22f, 0.32f));
		addBlob(m, 14, 20,
			[&](float, float, const V3& s)
			{
				const float lobes = std::fabs(fbm(s * 3.4f, seed, 3)) * 0.35f + fbm(s * 1.4f, seed + 2u, 2) * 0.2f;
				return V3(s.x * radii.x, s.y * radii.y, s.z * radii.z) * (1.0f + lobes);
			},
			[&](float, float, const V3& s, const V3& p)
			{
				const float vein = std::pow(1.0f - std::fabs(fbm(s * 2.6f, seed + 4u, 2)), 14.0f);
				C4 c = scaled(kFat, 0.85f + 0.3f * (fbm(p * 1.2f, seed + 5u, 2) * 0.5f + 0.5f));
				c = mix(c, kBlood, vein * 0.8f);
				return bloodied(c, p, seed, 0.3f);
			});
		m.centre();
		m.computeNormals();
		return m;
	}

	/** How thick the gut is (radius, cm, a grown man's): the small intestine, the large. */
	inline float gutRadius(bool bLarge) { return bLarge ? 2.3f : 1.25f; }

	/**
	 * A length of gut along `nodes` (the rope's points, world or local, cm; `scale` the body's size): the small intestine
	 * smooth, pink and glistening; the large wider, with its pouches and three pale bands. Torn open at both ends.
	 * `bColours` false skips the colours (cheaper, for a rope updating its shape in place).
	 * `sides` round and `per` points between each two nodes. The same number of vertices for the same node count, so a
	 * moving rope can update its mesh in place.
	 */
	inline Mesh makeGut(const std::vector<V3>& nodes, bool bLarge, float scale, uint32_t seed, int sides = 10, int per = 3, bool bColours = true)
	{
		Mesh m;
		const std::vector<V3> path = smoothPath(nodes, per);
		const float r0 = gutRadius(bLarge) * scale;
		float total = 0.0f;
		for (size_t i = 1; i < path.size(); ++i)
		{
			total += length(path[i] - path[i - 1]);
		}
		addTube(m, path, sides,
			[&](float t, float a)
			{
				if (!bLarge)
				{
					return r0 * (1.0f + 0.08f * fbm(V3(t * 30.0f, 0.0f, 0.0f), seed, 2));
				}
				// Pouches every 4 cm, pinched in at the three bands.
				const float s = t * total / std::fmax(scale, 0.1f);
				const float pouch = 0.82f + 0.22f * std::pow(std::fabs(std::sin(s * kPi / 4.0f)), 0.6f);
				float band = 1.0f;
				for (int k = 0; k < 3; ++k)
				{
					const float d = std::remainder(a - k * 2.0f * kPi / 3.0f, 2.0f * kPi);
					band -= 0.12f * std::exp(-std::pow(d / 0.25f, 2.0f));
				}
				return r0 * pouch * band;
			},
			[&](float t, float a, float s)
			{
				if (!bColours)
				{
					return kSmallGut;  // (a moving rope keeps the colours it was made with)
				}
				C4 c = bLarge ? kLargeGut : kSmallGut;
				c = scaled(c, 0.85f + 0.3f * (fbm(V3(s * 0.25f, a, 0.0f), seed, 3) * 0.5f + 0.5f));
				if (bLarge)
				{
					for (int k = 0; k < 3; ++k)
					{
						const float d = std::remainder(a - k * 2.0f * kPi / 3.0f, 2.0f * kPi);
						c = mix(c, kTaenia, std::exp(-std::pow(d / 0.2f, 2.0f)));
					}
				}
				// Fine vessels round it and a bruised purple here and there.
				const float vessels = std::pow(std::fabs(std::sin(s * 1.7f + 1.5f * fbm(V3(s * 0.3f, a, 2.0f), seed + 3u, 2))), 24.0f);
				c = mix(c, kVein, vessels * 0.35f + 0.25f * clamp01(fbm(V3(s * 0.08f, 0, 0), seed + 9u, 2)));
				(void)t;
				return bloodied(c, V3(s * 0.6f, a * 2.0f, 0.0f), seed, 0.22f);
			},
			false, false, bLarge ? kLargeGut : kSmallGut);
		m.computeNormals();
		return m;
	}
}
