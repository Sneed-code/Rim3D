// Renders the organ shapes (game/Source/Rim3D/GoreOrganShapes.h) and the rocket (RocketShape.h) without Unreal, so the
// cloud can look at them.
//   g++ -O2 -std=c++20 -I game/Source/Rim3D tools/gore_organs_preview.cpp -o build/gore_organs_preview
//   ./build/gore_organs_preview out.ppm [cell]
// A sheet of every organ from two sides, lit by a sun and a soft sky, with a wet highlight where the surface is wet.
// Also checks that every mesh is closed (each edge used twice) where it should be, and that its faces point out.

#include "GoreOrganShapes.h"
#include "RocketShape.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

using namespace goreshape;

namespace
{
	struct Image
	{
		int w, h;
		std::vector<float> rgb, depth;
		Image(int W, int H) : w(W), h(H), rgb(W * H * 3, 0.0f), depth(W * H, 1e9f)
		{
			for (int i = 0; i < W * H; ++i)
			{
				rgb[i * 3 + 0] = 0.18f; rgb[i * 3 + 1] = 0.17f; rgb[i * 3 + 2] = 0.15f;
			}
		}
	};

	V3 rotate(const V3& p, float yaw, float pitch)
	{
		const float cy = std::cos(yaw), sy = std::sin(yaw);
		V3 a(p.x * cy - p.y * sy, p.x * sy + p.y * cy, p.z);
		const float cp = std::cos(pitch), sp = std::sin(pitch);
		return V3(a.x, a.y * cp - a.z * sp, a.y * sp + a.z * cp);
	}

	/** Draws the mesh into a cell: screen x = view x, screen y = -view z, depth = view y (smaller nearer). */
	void draw(Image& img, const Mesh& m, int cx, int cy, int cell, float yaw, float pitch, float fit)
	{
		const float scale = cell * 0.42f / std::max(fit, 1.0f);
		std::vector<V3> sp(m.P.size()), sn(m.P.size());
		for (size_t i = 0; i < m.P.size(); ++i)
		{
			const V3 p = rotate(m.P[i], yaw, pitch);
			sp[i] = V3(cx + cell * 0.5f + p.x * scale, cy + cell * 0.5f - p.z * scale, p.y);
			sn[i] = rotate(m.N[i], yaw, pitch);
		}
		const V3 sun = normalize(V3(-0.5f, -0.6f, 0.65f));
		const V3 view(0, -1, 0);
		for (size_t t = 0; t + 2 < m.T.size(); t += 3)
		{
			const int ia = m.T[t], ib = m.T[t + 1], ic = m.T[t + 2];
			const V3 a = sp[ia], b = sp[ib], c = sp[ic];
			// Facing the viewer (the outward normal points towards -y): skip back faces.
			const V3 fn = cross(rotate(m.P[ib], yaw, pitch) - rotate(m.P[ia], yaw, pitch), rotate(m.P[ic], yaw, pitch) - rotate(m.P[ia], yaw, pitch));
			if (fn.y > 0.0f)
			{
				continue;
			}
			const int x0 = std::max(cx, (int)std::floor(std::min({a.x, b.x, c.x})));
			const int x1 = std::min(cx + cell - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
			const int y0 = std::max(cy, (int)std::floor(std::min({a.y, b.y, c.y})));
			const int y1 = std::min(cy + cell - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
			const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
			if (std::fabs(area) < 1e-6f)
			{
				continue;
			}
			for (int y = y0; y <= y1; ++y)
			{
				for (int x = x0; x <= x1; ++x)
				{
					const float px = x + 0.5f, py = y + 0.5f;
					const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
					const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0)
					{
						continue;
					}
					const float d = a.z * w0 + b.z * w1 + c.z * w2;
					const int idx = y * img.w + x;
					if (d >= img.depth[idx])
					{
						continue;
					}
					img.depth[idx] = d;
					const V3 n = normalize(sn[ia] * w0 + sn[ib] * w1 + sn[ic] * w2);
					const C4& ca = m.C[ia];
					const C4& cb = m.C[ib];
					const C4& cc = m.C[ic];
					const C4 col(ca.r * w0 + cb.r * w1 + cc.r * w2, ca.g * w0 + cb.g * w1 + cc.g * w2, ca.b * w0 + cb.b * w1 + cc.b * w2,
						ca.a * w0 + cb.a * w1 + cc.a * w2);
					const float diff = std::max(0.0f, dot(n, sun));
					const float sky = 0.35f + 0.25f * n.z;
					const V3 half = normalize(sun - view);
					const float gloss = lerpf(180.0f, 12.0f, col.a);
					const float spec = std::pow(std::max(0.0f, dot(n, half)), gloss) * lerpf(1.6f, 0.15f, col.a);
					const float fres = 0.04f + 0.3f * std::pow(1.0f - std::max(0.0f, -n.y), 5.0f) * (1.0f - col.a);
					img.rgb[idx * 3 + 0] = col.r * (diff * 1.1f + sky) + spec + fres * 0.4f;
					img.rgb[idx * 3 + 1] = col.g * (diff * 1.1f + sky) + spec + fres * 0.4f;
					img.rgb[idx * 3 + 2] = col.b * (diff * 1.1f + sky) + spec + fres * 0.45f;
				}
			}
		}
	}

	/** Each edge's use count (2 everywhere on a closed surface) and how many faces point inwards (by the centroid). */
	void checkMesh(const char* name, const Mesh& m)
	{
		std::map<std::pair<int, int>, int> edges;
		for (size_t t = 0; t + 2 < m.T.size(); t += 3)
		{
			for (int k = 0; k < 3; ++k)
			{
				const int a = m.T[t + k], b = m.T[t + (k + 1) % 3];
				edges[{std::min(a, b), std::max(a, b)}]++;
			}
		}
		int open = 0;
		for (const auto& e : edges)
		{
			open += e.second != 2 ? 1 : 0;
		}
		// Signed volume: positive when the faces point out.
		double vol = 0.0;
		for (size_t t = 0; t + 2 < m.T.size(); t += 3)
		{
			const V3 a = m.P[m.T[t]], b = m.P[m.T[t + 1]], c = m.P[m.T[t + 2]];
			vol += dot(a, cross(b, c)) / 6.0;
		}
		std::printf("%-12s verts %6zu tris %6zu  span %5.1f cm  rest %4.1f cm  open edges %d  volume %.0f cm3\n", name, m.P.size(), m.T.size() / 3,
			m.span(), m.restHeight(), open, vol);
	}
}

int main(int argc, char** argv)
{
	const std::string out = argc > 1 ? argv[1] : "organs.ppm";
	const int cell = argc > 2 ? std::atoi(argv[2]) : 320;
	struct Item
	{
		const char* name;
		Mesh mesh;
	};
	std::vector<Item> items;
	items.push_back({"heart", makeHeart(1)});
	items.push_back({"lung R", makeLung(2, false)});
	items.push_back({"lung L", makeLung(3, true)});
	items.push_back({"liver", makeLiver(4)});
	items.push_back({"stomach", makeStomach(5)});
	items.push_back({"kidney", makeKidney(6)});
	items.push_back({"spleen", makeSpleen(7)});
	items.push_back({"windpipe", makeWindpipe(8)});
	items.push_back({"spine", makeSpine(9, 4)});
	items.push_back({"ribs", makeRibRack(10)});
	items.push_back({"fat", makeFatLump(11, 12.0f)});
	{
		std::vector<V3> nodes;
		for (int i = 0; i < 24; ++i)
		{
			nodes.push_back(V3(std::sin(i * 0.7f) * 9.0f, std::cos(i * 0.45f) * 5.0f, i * 1.6f - 19.0f));
		}
		Mesh small = makeGut(nodes, false, 1.0f, 12);
		small.centre();
		items.push_back({"small gut", small});
		std::vector<V3> big;
		for (int i = 0; i < 14; ++i)
		{
			big.push_back(V3(std::sin(i * 0.5f) * 8.0f, 0.0f, i * 3.0f - 20.0f));
		}
		Mesh large = makeGut(big, true, 1.0f, 13);
		large.centre();
		items.push_back({"large gut", large});
	}
	{
		// The rocket, wound the Unreal way round: turned back to this file's way.
		const rocketshape::Shape r = rocketshape::makeRocket();
		Mesh m;
		for (size_t i = 0; i < r.points.size(); ++i)
		{
			m.add(V3(r.points[i].x, r.points[i].y, r.points[i].z), C4(r.colours[i].r, r.colours[i].g, r.colours[i].b, 0.6f));
		}
		for (size_t t = 0; t + 2 < r.triangles.size(); t += 3)
		{
			m.tri(r.triangles[t], r.triangles[t + 2], r.triangles[t + 1]);
		}
		m.computeNormals();
		items.push_back({"rocket", m});
	}
	for (const Item& it : items)
	{
		checkMesh(it.name, it.mesh);
	}
	const int cols = 4;
	const int rows = static_cast<int>((items.size() * 2 + cols - 1) / cols);
	Image img(cols * cell, rows * cell);
	for (size_t i = 0; i < items.size(); ++i)
	{
		const float fit = items[i].mesh.span() * 0.5f;
		for (int view = 0; view < 2; ++view)
		{
			const int slot = static_cast<int>(i) * 2 + view;
			draw(img, items[i].mesh, (slot % cols) * cell, (slot / cols) * cell, cell, view == 0 ? 0.35f : 2.6f, view == 0 ? -0.25f : 0.35f, fit);
		}
	}
	FILE* f = std::fopen(out.c_str(), "wb");
	if (!f)
	{
		return 1;
	}
	std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
	for (size_t i = 0; i < img.rgb.size(); ++i)
	{
		// A plain filmic curve and sRGB-ish gamma, so dark tissues read as they would on screen.
		float v = img.rgb[i];
		v = v / (1.0f + v);
		v = std::pow(std::max(0.0f, v * 1.4f), 1.0f / 2.2f);
		std::fputc(static_cast<int>(std::min(1.0f, v) * 255.0f + 0.5f), f);
	}
	std::fclose(f);
	std::printf("wrote %s (%dx%d)\n", out.c_str(), img.w, img.h);
	return 0;
}
