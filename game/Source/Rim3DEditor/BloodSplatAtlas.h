#pragma once

// The blood splat atlas (docs/design/blood.md): 4 x 4 pictures drawn from shapes, in plain C++ so the cloud can build
// and look at it (tools/blood_atlas_preview.cpp) without Unreal. BloodArtCommandlet turns it into T_BloodAtlas (R
// coverage, G thickness, B wound core) and T_BloodAtlasN (a normal map from the thickness).
//
// Cells: 0-3 a drop that hit square on (an uneven blot with droplets); 4-7 one that hit at a slant travelling to +U
// (a body, a short tail and drops thrown ahead); 8-9 fine spatter; 10-11 pools; 12 a run, top to bottom; 13 a smear along U;
// 14 a bullet hole or stab; 15 a torn exit wound or a cut along U.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bloodatlas
{
	constexpr int kCells = 4;

	struct Pixel
	{
		float Coverage = 0.0f;
		float Thickness = 0.0f;
		float Core = 0.0f;
	};

	struct Image
	{
		int Size = 0;  // the whole atlas, Size x Size
		std::vector<Pixel> Pixels;
		Pixel& At(int X, int Y) { return Pixels[static_cast<size_t>(Y) * Size + X]; }
		const Pixel& At(int X, int Y) const { return Pixels[static_cast<size_t>(Y) * Size + X]; }
	};

	// A small deterministic random source (the same atlas on every machine).
	struct Random
	{
		uint64_t State;
		explicit Random(uint64_t Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
		uint32_t Next()
		{
			State ^= State << 13;
			State ^= State >> 7;
			State ^= State << 17;
			return static_cast<uint32_t>(State >> 16);
		}
		float Unit() { return static_cast<float>(Next() & 0xFFFFFF) / 16777216.0f; }
		float Range(float A, float B) { return A + (B - A) * Unit(); }
	};

	inline float Hash2(int X, int Y, uint32_t Seed)
	{
		uint32_t H = static_cast<uint32_t>(X) * 374761393u + static_cast<uint32_t>(Y) * 668265263u + Seed * 2246822519u;
		H = (H ^ (H >> 13)) * 1274126177u;
		H ^= H >> 16;
		return static_cast<float>(H & 0xFFFFFF) / 16777216.0f;
	}

	inline float ValueNoise(float X, float Y, uint32_t Seed)
	{
		const int X0 = static_cast<int>(std::floor(X));
		const int Y0 = static_cast<int>(std::floor(Y));
		const float Fx = X - X0;
		const float Fy = Y - Y0;
		const float Sx = Fx * Fx * (3.0f - 2.0f * Fx);
		const float Sy = Fy * Fy * (3.0f - 2.0f * Fy);
		const float A = Hash2(X0, Y0, Seed);
		const float B = Hash2(X0 + 1, Y0, Seed);
		const float C = Hash2(X0, Y0 + 1, Seed);
		const float D = Hash2(X0 + 1, Y0 + 1, Seed);
		return (A + (B - A) * Sx) + ((C + (D - C) * Sx) - (A + (B - A) * Sx)) * Sy;
	}

	inline float Fbm(float X, float Y, uint32_t Seed, int Octaves = 4)
	{
		float Sum = 0.0f;
		float Amplitude = 0.5f;
		float Norm = 0.0f;
		for (int I = 0; I < Octaves; ++I)
		{
			Sum += ValueNoise(X, Y, Seed + static_cast<uint32_t>(I) * 101u) * Amplitude;
			Norm += Amplitude;
			X *= 2.03f;
			Y *= 2.03f;
			Amplitude *= 0.5f;
		}
		return Sum / Norm;
	}

	// A capsule whose radius goes from Ra at A to Rb at B (a circle when A == B). Units: the cell, 0 to 1.
	struct Shape
	{
		float Ax, Ay, Bx, By, Ra, Rb;
		float Thick = 1.0f;   // how thick its middle is (satellite drops are thin)
		float Core = 0.0f;    // 1: wound flesh (darker)
	};

	inline float ShapeDistance(const Shape& S, float X, float Y, float& OutRadius)
	{
		const float Dx = S.Bx - S.Ax;
		const float Dy = S.By - S.Ay;
		const float L2 = Dx * Dx + Dy * Dy;
		float T = 0.0f;
		if (L2 > 1e-10f)
		{
			T = std::clamp(((X - S.Ax) * Dx + (Y - S.Ay) * Dy) / L2, 0.0f, 1.0f);
		}
		const float Px = S.Ax + Dx * T - X;
		const float Py = S.Ay + Dy * T - Y;
		OutRadius = S.Ra + (S.Rb - S.Ra) * T;
		return std::sqrt(Px * Px + Py * Py) - OutRadius;
	}

	inline float SmoothMin(float A, float B, float K)
	{
		const float H = std::max(K - std::fabs(A - B), 0.0f) / K;
		return std::min(A, B) - H * H * K * 0.25f;
	}

	struct Cell
	{
		std::vector<Shape> Shapes;
		float Smooth = 0.012f;     // how much shapes melt into each other
		float EdgeNoise = 0.006f;  // ragged edge
		float EdgeNoiseScale = 28.0f;
		float ThickScale = 0.09f;  // distance inside the edge where the blood is full thickness
		uint32_t Seed = 1;
	};

	inline void Add(Cell& C, float Ax, float Ay, float Bx, float By, float Ra, float Rb, float Thick = 1.0f, float Core = 0.0f)
	{
		C.Shapes.push_back(Shape{Ax, Ay, Bx, By, Ra, Rb, Thick, Core});
	}

	inline void AddCircle(Cell& C, float X, float Y, float R, float Thick = 1.0f, float Core = 0.0f) { Add(C, X, Y, X, Y, R, R, Thick, Core); }

	// A drop that hit square on: a soft, uneven blot (lobes melted together, a few short rounded bulges where the crown
	// spilled over) with droplets nearby, more of them close in. No spines: thin rays with beads read as a cartoon
	// starburst or a virus (PC look, 2026-11-19).
	inline Cell MakeRound(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		const float Main = R.Range(0.14f, 0.18f);
		AddCircle(C, 0.5f, 0.5f, Main);
		const int Lobes = static_cast<int>(R.Range(4.0f, 7.0f));
		for (int I = 0; I < Lobes; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = Main * R.Range(0.3f, 0.6f);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, Main * R.Range(0.55f, 0.8f));
		}
		// Short, round-ended bulges past the edge.
		const int Bulges = static_cast<int>(R.Range(2.0f, 5.0f));
		for (int I = 0; I < Bulges; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float To = Main * R.Range(1.05f, 1.3f);
			const float W = Main * R.Range(0.18f, 0.3f);
			Add(C, 0.5f + std::cos(A) * Main * 0.7f, 0.5f + std::sin(A) * Main * 0.7f, 0.5f + std::cos(A) * To, 0.5f + std::sin(A) * To, W, W * 0.8f, 0.8f);
		}
		// Droplets: a few bigger ones close by, small ones further out, all round.
		const int Droplets = static_cast<int>(R.Range(6.0f, 14.0f));
		for (int I = 0; I < Droplets; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = Main * (1.25f + 1.6f * std::pow(R.Unit(), 1.5f));
			const float Size = R.Range(0.004f, 0.016f) * std::clamp(1.6f - D / Main * 0.4f, 0.3f, 1.0f);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, Size, 0.6f);
		}
		C.Smooth = 0.03f;
		C.EdgeNoise = 0.012f;
		C.EdgeNoiseScale = 14.0f;
		C.ThickScale = 0.12f;
		return C;
	}

	// A drop that hit at a slant, travelling to +U (+X): a long rounded body narrowing to a short tail, and a few
	// small drops thrown ahead along its line.
	inline Cell MakeDirectional(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		const float Y = 0.5f + R.Range(-0.03f, 0.03f);
		const float W = R.Range(0.07f, 0.09f);
		AddCircle(C, 0.28f, Y, W * 0.95f);
		Add(C, 0.28f, Y, 0.55f, Y + R.Range(-0.01f, 0.01f), W, W * 0.8f);
		Add(C, 0.55f, Y, 0.74f, Y + R.Range(-0.015f, 0.015f), W * 0.8f, W * 0.2f, 0.8f);
		// A lobe or two on the sides of the body.
		for (int I = 0; I < 2; ++I)
		{
			const float X = R.Range(0.3f, 0.5f);
			const float Side = R.Unit() < 0.5f ? -1.0f : 1.0f;
			AddCircle(C, X, Y + Side * W * R.Range(0.45f, 0.65f), W * R.Range(0.35f, 0.5f));
		}
		// Drops thrown ahead, elongated along the travel, smaller further out.
		const int Ahead = static_cast<int>(R.Range(3.0f, 7.0f));
		for (int I = 0; I < Ahead; ++I)
		{
			const float X = R.Range(0.78f, 0.95f);
			const float Rr = R.Range(0.006f, 0.014f) * (1.2f - X);
			const float Dy = R.Range(-0.04f, 0.04f);
			Add(C, X - Rr * 1.8f, Y + Dy, X, Y + Dy, Rr * 0.7f, Rr, 0.5f);
		}
		C.Smooth = 0.025f;
		C.EdgeNoise = 0.008f;
		C.EdgeNoiseScale = 14.0f;
		return C;
	}

	// Fine spatter: many small drops, more in the middle.
	inline Cell MakeSpecks(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		const int Count = static_cast<int>(R.Range(60.0f, 140.0f));
		for (int I = 0; I < Count; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = 0.45f * std::pow(R.Unit(), 0.7f);
			const float Rr = 0.002f + 0.016f * std::pow(R.Unit(), 3.0f) * (1.0f - D);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, Rr, 0.6f);
		}
		C.Smooth = 0.004f;
		C.EdgeNoise = 0.002f;
		return C;
	}

	// A pool: many lobes of different sizes melted together, so the outline is uneven all round (no single tongue),
	// thick in the middle, with a few drops on the rim.
	inline Cell MakePool(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		AddCircle(C, 0.5f, 0.5f, R.Range(0.16f, 0.2f));
		const int Lobes = static_cast<int>(R.Range(9.0f, 14.0f));
		for (int I = 0; I < Lobes; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = R.Range(0.08f, 0.24f);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, R.Range(0.05f, 0.13f) * (1.25f - D * 1.5f));
		}
		const int Rim = static_cast<int>(R.Range(5.0f, 10.0f));
		for (int I = 0; I < Rim; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = R.Range(0.33f, 0.44f);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, R.Range(0.006f, 0.02f), 0.6f);
		}
		C.Smooth = 0.06f;
		C.EdgeNoise = 0.014f;
		C.EdgeNoiseScale = 11.0f;
		C.ThickScale = 0.2f;
		return C;
	}

	// A run from the top down (+V, image rows down): it wanders a little, thins, and ends in a bead.
	inline Cell MakeRun(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		for (int Strand = 0; Strand < 3; ++Strand)
		{
			float X = 0.5f + (Strand == 0 ? 0.0f : R.Range(-0.12f, 0.12f));
			float Y = 0.06f + (Strand == 0 ? 0.0f : R.Range(0.05f, 0.25f));
			const float End = Strand == 0 ? 0.88f : R.Range(0.45f, 0.75f);
			float W = Strand == 0 ? 0.045f : R.Range(0.015f, 0.028f);
			const int Steps = 14;
			const float Step = (End - Y) / Steps;
			for (int I = 0; I < Steps; ++I)
			{
				const float Nx = X + R.Range(-0.012f, 0.012f);
				const float Nw = W * R.Range(0.86f, 0.98f);
				Add(C, X, Y, Nx, Y + Step, W, Nw, 0.9f);
				X = Nx;
				Y += Step;
				W = Nw;
			}
			AddCircle(C, X, Y + W * 0.6f, W * 1.7f, 1.0f);
		}
		AddCircle(C, 0.5f, 0.07f, 0.07f);
		C.Smooth = 0.02f;
		C.EdgeNoise = 0.003f;
		return C;
	}

	// A smear along +U: a wide band that streaks and thins out.
	inline Cell MakeSmear(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		Add(C, 0.12f, 0.5f, 0.86f, 0.5f + R.Range(-0.05f, 0.05f), 0.15f, 0.07f, 0.6f);
		AddCircle(C, 0.16f, 0.5f, 0.16f);
		C.EdgeNoise = 0.02f;
		C.EdgeNoiseScale = 12.0f;
		C.ThickScale = 0.08f;
		return C;
	}

	// A bullet hole or stab: a dark wet hole, a ring of torn skin, a little spatter.
	inline Cell MakeHole(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		AddCircle(C, 0.5f, 0.5f, 0.11f, 1.0f, 1.0f);
		AddCircle(C, 0.5f, 0.5f, 0.2f, 0.55f, 0.0f);
		for (int I = 0; I < 14; ++I)
		{
			const float A = R.Range(0.0f, 6.2832f);
			const float D = R.Range(0.22f, 0.42f);
			AddCircle(C, 0.5f + std::cos(A) * D, 0.5f + std::sin(A) * D, R.Range(0.006f, 0.02f), 0.5f);
		}
		C.EdgeNoise = 0.01f;
		C.EdgeNoiseScale = 18.0f;
		return C;
	}

	// A torn exit wound or a cut along +U: a lens of open flesh with ragged lips.
	inline Cell MakeGash(uint32_t Seed)
	{
		Random R(Seed);
		Cell C;
		C.Seed = Seed;
		const float Y = 0.5f;
		Add(C, 0.10f, Y, 0.5f, Y + R.Range(-0.02f, 0.02f), 0.01f, 0.10f, 1.0f, 0.0f);
		Add(C, 0.5f, Y, 0.90f, Y + R.Range(-0.03f, 0.03f), 0.10f, 0.01f, 1.0f, 0.0f);
		Add(C, 0.22f, Y, 0.5f, Y, 0.005f, 0.05f, 1.0f, 1.0f);
		Add(C, 0.5f, Y, 0.78f, Y, 0.05f, 0.005f, 1.0f, 1.0f);
		for (int I = 0; I < 12; ++I)
		{
			const float X = R.Range(0.2f, 0.8f);
			const float Side = R.Unit() < 0.5f ? -1.0f : 1.0f;
			AddCircle(C, X, Y + Side * R.Range(0.12f, 0.3f), R.Range(0.006f, 0.02f), 0.5f);
		}
		C.EdgeNoise = 0.014f;
		C.EdgeNoiseScale = 22.0f;
		return C;
	}

	inline Cell MakeCell(int Index)
	{
		const uint32_t Seed = 1000u + static_cast<uint32_t>(Index) * 7919u;
		if (Index < 4) return MakeRound(Seed);
		if (Index < 8) return MakeDirectional(Seed);
		if (Index < 10) return MakeSpecks(Seed);
		if (Index < 12) return MakePool(Seed);
		if (Index == 12) return MakeRun(Seed);
		if (Index == 13) return MakeSmear(Seed);
		if (Index == 14) return MakeHole(Seed);
		return MakeGash(Seed);
	}

	// Draws one cell into the atlas at its place.
	inline void DrawCell(Image& Out, int Index)
	{
		const Cell C = MakeCell(Index);
		const int CellSize = Out.Size / kCells;
		const int Ox = (Index % kCells) * CellSize;
		const int Oy = (Index / kCells) * CellSize;
		const float PixelSize = 1.0f / CellSize;
		for (int Y = 0; Y < CellSize; ++Y)
		{
			for (int X = 0; X < CellSize; ++X)
			{
				const float U = (X + 0.5f) * PixelSize;
				const float V = (Y + 0.5f) * PixelSize;
				float D = 1.0f;
				float Thick = 0.0f;
				float Core = 0.0f;
				float CoreD = 1.0f;
				for (const Shape& S : C.Shapes)
				{
					float Radius = 0.0f;
					const float Sd = ShapeDistance(S, U, V, Radius);
					if (Sd < D + C.Smooth)
					{
						D = SmoothMin(D, Sd, C.Smooth);
					}
					// Thickness by the nearest shapes, blended (no seams where a thin shape meets a thick one).
					Thick = std::max(Thick, S.Thick * std::clamp(0.5f - Sd / (C.Smooth + 0.03f), 0.0f, 1.0f));
					if (S.Core > 0.0f)
					{
						CoreD = std::min(CoreD, Sd);
					}
				}
				D += (Fbm(U * C.EdgeNoiseScale, V * C.EdgeNoiseScale, C.Seed) - 0.5f) * 2.0f * C.EdgeNoise;
				const float Coverage = std::clamp(0.5f - D / (1.5f * PixelSize), 0.0f, 1.0f);
				// Thickness rises from the edge (surface tension rim a little thicker), with slow lumps.
				const float Inside = std::clamp(-D / C.ThickScale, 0.0f, 1.0f);
				const float Rim = std::exp(-std::pow((-D) / (4.0f * PixelSize) - 1.0f, 2.0f)) * 0.12f;
				const float Lumps = 0.85f + 0.3f * Fbm(U * 7.0f, V * 7.0f, C.Seed + 77u, 3);
				float T = (std::pow(Inside, 0.6f) * Lumps + Rim) * (0.35f + 0.65f * Thick);
				Core = std::clamp(0.5f - CoreD / (2.0f * PixelSize), 0.0f, 1.0f);
				Pixel& P = Out.At(Ox + X, Oy + Y);
				P.Coverage = Coverage;
				P.Thickness = std::clamp(T, 0.0f, 1.0f) * Coverage;
				P.Core = Core * Coverage;
			}
		}
	}

	inline Image MakeAtlas(int Size)
	{
		Image Out;
		Out.Size = Size;
		Out.Pixels.assign(static_cast<size_t>(Size) * Size, Pixel());
		for (int Index = 0; Index < kCells * kCells; ++Index)
		{
			DrawCell(Out, Index);
		}
		return Out;
	}

	// Normal map from the thickness (+X right, +Y up in the picture), as 0..1 RGB.
	inline void NormalAt(const Image& In, int X, int Y, float Strength, float& OutX, float& OutY, float& OutZ)
	{
		const auto H = [&In](int Px, int Py)
		{
			Px = std::clamp(Px, 0, In.Size - 1);
			Py = std::clamp(Py, 0, In.Size - 1);
			return In.At(Px, Py).Thickness;
		};
		const float Dx = (H(X + 1, Y) - H(X - 1, Y)) * Strength;
		const float Dy = (H(X, Y + 1) - H(X, Y - 1)) * Strength;
		float Nx = -Dx;
		float Ny = Dy;  // image rows go down, the normal's Y goes up
		float Nz = 1.0f;
		const float L = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz);
		OutX = Nx / L * 0.5f + 0.5f;
		OutY = Ny / L * 0.5f + 0.5f;
		OutZ = Nz / L * 0.5f + 0.5f;
	}
}
