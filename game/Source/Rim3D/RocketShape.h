#pragma once

// A rocket's shape as plain C++ (no Unreal), so the cloud can check it (tools/rocket_preview.cpp) and ShooterFeelSubsystem
// can build its mesh: a slim olive body with a pointed nose, a yellow band, four swept fins and a dark nozzle, about 55 cm
// long along +X with its middle at the origin (sneed 2026-11-23: "use an actual rocket projectile").
//
// Triangles are wound the way Unreal draws front faces (outward = cross(c - a, b - a) for a triangle a, b, c).

#include <cmath>
#include <vector>

namespace rocketshape
{
	struct P3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};
	struct Col
	{
		float r = 0.0f, g = 0.0f, b = 0.0f;
	};

	struct Shape
	{
		std::vector<P3> points;
		std::vector<Col> colours;
		std::vector<int> triangles;  // Unreal winding

		int add(const P3& p, const Col& c)
		{
			points.push_back(p);
			colours.push_back(c);
			return static_cast<int>(points.size()) - 1;
		}
		/** A quad whose corners run anticlockwise seen from outside. */
		void quad(int a, int b, int c, int d)
		{
			// Anticlockwise from outside is outward by the right hand rule; Unreal wants the other way round.
			triangles.insert(triangles.end(), {a, c, b, a, d, c});
		}
		void tri(int a, int b, int c)
		{
			triangles.insert(triangles.end(), {a, c, b});
		}
	};

	struct Ring
	{
		float x;
		float r;
		Col colour;
		bool seam;  // a hard edge here (a new colour or a step): its own ring of points
	};

	inline Shape makeRocket(int sides = 16)
	{
		const float Pi = 3.14159265f;
		const Col olive{0.16f, 0.19f, 0.11f};
		const Col oliveDark{0.11f, 0.13f, 0.08f};
		const Col band{0.55f, 0.43f, 0.06f};
		const Col steel{0.06f, 0.06f, 0.065f};
		const Col soot{0.02f, 0.018f, 0.016f};
		// The profile from the nozzle's mouth to the nose tip.
		std::vector<Ring> profile = {
			{-28.0f, 3.3f, soot, true},
			{-26.5f, 2.6f, steel, false},
			{-25.0f, 2.3f, steel, false},
			{-24.0f, 3.6f, oliveDark, true},
			{-22.0f, 4.2f, olive, true},
			{6.0f, 4.2f, olive, false},
			{6.0f, 4.25f, band, true},
			{9.0f, 4.25f, band, false},
			{9.0f, 4.2f, olive, true},
			{13.0f, 4.2f, olive, false},
			{17.0f, 3.9f, oliveDark, false},
			{21.0f, 3.1f, oliveDark, false},
			{24.5f, 1.9f, oliveDark, false},
			{26.8f, 0.8f, oliveDark, false},
			{27.6f, 0.15f, oliveDark, false},
		};
		Shape s;
		std::vector<int> prev;
		for (size_t i = 0; i < profile.size(); ++i)
		{
			const Ring& ring = profile[i];
			// A seam ring repeats the previous ring's points in the new colour, so the faces before and after don't share.
			if (ring.seam && !prev.empty())
			{
				const Ring& last = profile[i - 1];
				std::vector<int> again;
				for (int j = 0; j < sides; ++j)
				{
					const float t = 2.0f * Pi * j / sides;
					again.push_back(s.add({last.x, last.r * std::cos(t), last.r * std::sin(t)}, ring.colour));
				}
				prev = again;
			}
			std::vector<int> cur;
			for (int j = 0; j < sides; ++j)
			{
				const float t = 2.0f * Pi * j / sides;
				cur.push_back(s.add({ring.x, ring.r * std::cos(t), ring.r * std::sin(t)}, ring.colour));
			}
			if (!prev.empty())
			{
				for (int j = 0; j < sides; ++j)
				{
					const int k = (j + 1) % sides;
					// Seen from outside (on +Y at angle 0, looking down -Y with +Z up), +X is to the left: (x0,j) (x0,k) (x1,k)
					// (x1,j) runs anticlockwise.
					s.quad(prev[j], prev[k], cur[k], cur[j]);
				}
			}
			prev = cur;
		}
		// The nose tip and the nozzle's mouth closed off (the mouth a dark disc a little inside).
		{
			const int tip = s.add({profile.back().x + 0.3f, 0.0f, 0.0f}, profile.back().colour);
			for (int j = 0; j < sides; ++j)
			{
				s.tri(prev[(j + 1) % sides], tip, prev[j]);
			}
			std::vector<int> mouth;
			for (int j = 0; j < sides; ++j)
			{
				const float t = 2.0f * Pi * j / sides;
				mouth.push_back(s.add({profile.front().x, profile.front().r * std::cos(t), profile.front().r * std::sin(t)}, soot));
			}
			const int back = s.add({profile.front().x + 1.5f, 0.0f, 0.0f}, soot);
			for (int j = 0; j < sides; ++j)
			{
				s.tri(mouth[j], back, mouth[(j + 1) % sides]);
			}
		}
		// Four swept fins at the tail, thin slabs standing out from the body.
		for (int f = 0; f < 4; ++f)
		{
			const float a = Pi * 0.25f + f * Pi * 0.5f;
			const float ca = std::cos(a), sa = std::sin(a);
			const float half = 0.3f;
			// In the fin's own frame: u along the rocket (x), v out from the body, w across the fin.
			const auto at = [&](float u, float v, float w) { return P3{u, v * ca - w * sa, v * sa + w * ca}; };
			const float root0 = -23.0f, root1 = -10.0f, tip0 = -25.0f, tip1 = -18.0f, in = 3.9f, out = 9.5f;
			const P3 corners[8] = {
				at(root0, in, -half), at(root1, in, -half), at(tip1, out, -half), at(tip0, out, -half),
				at(root0, in, half), at(root1, in, half), at(tip1, out, half), at(tip0, out, half),
			};
			int c[8];
			for (int k = 0; k < 8; ++k)
			{
				c[k] = s.add(corners[k], oliveDark);
			}
			// Every face its own points would shade it flat; shared corners are fine on a slab this thin.
			s.quad(c[4], c[5], c[6], c[7]);  // +w side
			s.quad(c[3], c[2], c[1], c[0]);  // -w side
			s.quad(c[0], c[1], c[5], c[4]);  // root
			s.quad(c[1], c[2], c[6], c[5]);  // trailing... leading edge (towards the nose)
			s.quad(c[2], c[3], c[7], c[6]);  // tip
			s.quad(c[3], c[0], c[4], c[7]);  // back edge
		}
		return s;
	}

	/** The signed volume by Unreal's winding (positive when every face points out), cm^3. */
	inline double unrealVolume(const Shape& s)
	{
		double v = 0.0;
		for (size_t t = 0; t + 2 < s.triangles.size(); t += 3)
		{
			const P3& a = s.points[s.triangles[t]];
			const P3& b = s.points[s.triangles[t + 1]];
			const P3& c = s.points[s.triangles[t + 2]];
			// Unreal's outward is cross(c - a, b - a), so the usual a . (b x c) changes sign.
			v -= (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
		}
		return v;
	}
}
