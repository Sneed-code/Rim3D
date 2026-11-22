#pragma once

// Blast flipbooks (docs/design/blast-draw.md): animated pictures of a fireball, a smoke cloud and a scorch mark, drawn by
// marching rays through a noise volume, in plain C++ so the cloud can build and look at them
// (tools/blast_flipbook_preview.cpp) without Unreal. BlastArtCommandlet turns them into textures.
//
// A flipbook is 8 x 8 frames, left to right and top to bottom, frame 0 the first moment. Each pixel:
//   R  heat: how much glowing fire the ray saw (0 none, 1 white hot); the material colours it as a black body.
//   G  light: how lit the smoke the ray saw is, from above and a little in front (baked self-shadowing), 0 to 1.
//   B  depth: a little variation for the material to break up flat colour (the noise at the front of the cloud).
//   A  opacity: 1 - what light gets through.
//
// The scorch is one picture: R soot (0 to 1), G a lighter dusty rim, A coverage.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace blastfx
{
	constexpr int kFrames = 8;  // per side
	// The frame's -1..1 shows the volume's -kFill..kFill. The biggest cloud reaches about 1.0, so it fits inside with a soft
	// edge (sneed 2026-11-22: at 0.74 the clouds were cut off square at the frame's edges and read as low quality). The game
	// draws sprites kSpriteScale times bigger than before to keep the same size on screen.
	constexpr float kFill = 1.05f;
	constexpr float kOldFill = 0.74f;
	// How many octaves of noise: enough for detail at 512 px a frame.
	constexpr int kOctaves = 6;

	struct Pixel
	{
		float Heat = 0.0f;
		float Light = 0.0f;
		float Depth = 0.0f;
		float Opacity = 0.0f;
	};

	struct Image
	{
		int Size = 0;  // the whole sheet, Size x Size
		std::vector<Pixel> Pixels;
		Pixel& At(int X, int Y) { return Pixels[static_cast<size_t>(Y) * Size + X]; }
		const Pixel& At(int X, int Y) const { return Pixels[static_cast<size_t>(Y) * Size + X]; }
	};

	struct Vec3
	{
		float X = 0.0f, Y = 0.0f, Z = 0.0f;
	};
	inline Vec3 operator+(Vec3 A, Vec3 B) { return {A.X + B.X, A.Y + B.Y, A.Z + B.Z}; }
	inline Vec3 operator-(Vec3 A, Vec3 B) { return {A.X - B.X, A.Y - B.Y, A.Z - B.Z}; }
	inline Vec3 operator*(Vec3 A, float S) { return {A.X * S, A.Y * S, A.Z * S}; }
	inline float Length(Vec3 A) { return std::sqrt(A.X * A.X + A.Y * A.Y + A.Z * A.Z); }

	inline float Clamp01(float V) { return V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }
	inline float SmoothStep(float A, float B, float V)
	{
		const float T = Clamp01((V - A) / (B - A));
		return T * T * (3.0f - 2.0f * T);
	}

	inline float Hash3(int X, int Y, int Z, uint32_t Seed)
	{
		uint32_t H = static_cast<uint32_t>(X) * 374761393u + static_cast<uint32_t>(Y) * 668265263u + static_cast<uint32_t>(Z) * 2147483647u + Seed * 2246822519u;
		H = (H ^ (H >> 13)) * 1274126177u;
		H ^= H >> 16;
		return static_cast<float>(H & 0xFFFFFF) / 16777216.0f;
	}

	/** Smooth value noise, 0 to 1. */
	inline float Noise3(Vec3 P, uint32_t Seed)
	{
		const float Fx0 = std::floor(P.X), Fy0 = std::floor(P.Y), Fz0 = std::floor(P.Z);
		const int X0 = static_cast<int>(Fx0), Y0 = static_cast<int>(Fy0), Z0 = static_cast<int>(Fz0);
		float Fx = P.X - Fx0, Fy = P.Y - Fy0, Fz = P.Z - Fz0;
		Fx = Fx * Fx * (3.0f - 2.0f * Fx);
		Fy = Fy * Fy * (3.0f - 2.0f * Fy);
		Fz = Fz * Fz * (3.0f - 2.0f * Fz);
		auto L = [](float A, float B, float T) { return A + (B - A) * T; };
		const float C000 = Hash3(X0, Y0, Z0, Seed), C100 = Hash3(X0 + 1, Y0, Z0, Seed);
		const float C010 = Hash3(X0, Y0 + 1, Z0, Seed), C110 = Hash3(X0 + 1, Y0 + 1, Z0, Seed);
		const float C001 = Hash3(X0, Y0, Z0 + 1, Seed), C101 = Hash3(X0 + 1, Y0, Z0 + 1, Seed);
		const float C011 = Hash3(X0, Y0 + 1, Z0 + 1, Seed), C111 = Hash3(X0 + 1, Y0 + 1, Z0 + 1, Seed);
		return L(L(L(C000, C100, Fx), L(C010, C110, Fx), Fy), L(L(C001, C101, Fx), L(C011, C111, Fx), Fy), Fz);
	}

	/** A pseudo-random unit-ish gradient at a lattice corner (one of 12 cube edge directions). */
	inline float GradDot(int X, int Y, int Z, uint32_t Seed, float Dx, float Dy, float Dz)
	{
		const int H = static_cast<int>(Hash3(X, Y, Z, Seed) * 12.0f) % 12;
		const float U = H < 8 ? Dx : Dy;
		const float V = H < 4 ? Dy : Dz;
		return ((H & 1) ? -U : U) + ((H & 2) ? -V : V);
	}

	/** Smooth gradient (Perlin) noise, about 0 to 1: rounder lumps than value noise, which drew the clouds boxy. */
	inline float GNoise3(Vec3 P, uint32_t Seed)
	{
		const float Fx0 = std::floor(P.X), Fy0 = std::floor(P.Y), Fz0 = std::floor(P.Z);
		const int X0 = static_cast<int>(Fx0), Y0 = static_cast<int>(Fy0), Z0 = static_cast<int>(Fz0);
		const float Dx = P.X - Fx0, Dy = P.Y - Fy0, Dz = P.Z - Fz0;
		auto Fade = [](float T) { return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f); };
		const float U = Fade(Dx), V = Fade(Dy), W = Fade(Dz);
		auto L = [](float A, float B, float T) { return A + (B - A) * T; };
		const float N000 = GradDot(X0, Y0, Z0, Seed, Dx, Dy, Dz);
		const float N100 = GradDot(X0 + 1, Y0, Z0, Seed, Dx - 1.0f, Dy, Dz);
		const float N010 = GradDot(X0, Y0 + 1, Z0, Seed, Dx, Dy - 1.0f, Dz);
		const float N110 = GradDot(X0 + 1, Y0 + 1, Z0, Seed, Dx - 1.0f, Dy - 1.0f, Dz);
		const float N001 = GradDot(X0, Y0, Z0 + 1, Seed, Dx, Dy, Dz - 1.0f);
		const float N101 = GradDot(X0 + 1, Y0, Z0 + 1, Seed, Dx - 1.0f, Dy, Dz - 1.0f);
		const float N011 = GradDot(X0, Y0 + 1, Z0 + 1, Seed, Dx, Dy - 1.0f, Dz - 1.0f);
		const float N111 = GradDot(X0 + 1, Y0 + 1, Z0 + 1, Seed, Dx - 1.0f, Dy - 1.0f, Dz - 1.0f);
		const float N = L(L(L(N000, N100, U), L(N010, N110, U), V), L(L(N001, N101, U), L(N011, N111, U), V), W);
		return Clamp01(0.5f + 0.63f * N);  // 0.63: the same spread as the value noise the shapes were tuned with
	}

	/** Turns a point so value noise's grid does not show as square edges (a fixed rotation, applied every octave). */
	inline Vec3 Turn(Vec3 P)
	{
		return {0.00f * P.X + 0.80f * P.Y + 0.60f * P.Z, -0.80f * P.X + 0.36f * P.Y - 0.48f * P.Z, -0.60f * P.X - 0.48f * P.Y + 0.64f * P.Z};
	}

	/** Billowy turbulence: sum of |2n - 1|, round lumps like rolling smoke. About 0.34 to 1.3, 0.62 on average. */
	inline float Billow(Vec3 P, uint32_t Seed, int Octaves)
	{
		float Sum = 0.0f, Amp = 0.5f, Norm = 0.0f;
		P = Turn(P);
		for (int I = 0; I < Octaves; ++I)
		{
			Sum += std::fabs(2.0f * GNoise3(P, Seed + static_cast<uint32_t>(I) * 131u) - 1.0f) * Amp;
			Norm += Amp;
			P = Turn(P) * 2.03f + Vec3{17.1f, 3.7f, 9.3f};
			Amp *= 0.5f;
		}
		// Turbulence (|2n - 1|) bulges out in round lumps with creases between them, like the cauliflower of a real blast;
		// the 1 - |2n - 1| it used to be drew thin ridges once the noise was gradient noise. Shifted to the old average (0.62).
		return 0.34f + Sum / Norm;
	}

	inline float Fbm(Vec3 P, uint32_t Seed, int Octaves, bool bGradient = false)
	{
		float Sum = 0.0f, Amp = 0.5f, Norm = 0.0f;
		P = Turn(P);
		for (int I = 0; I < Octaves; ++I)
		{
			Sum += (bGradient ? GNoise3(P, Seed + static_cast<uint32_t>(I) * 97u) : Noise3(P, Seed + static_cast<uint32_t>(I) * 97u)) * Amp;
			Norm += Amp;
			P = Turn(P) * 2.01f + Vec3{5.3f, 11.9f, 2.1f};
			Amp *= 0.5f;
		}
		return Sum / Norm;
	}

	/** What a point in a volume holds: smoke density (per unit length) and heat 0 to 1. */
	struct Sample
	{
		float Density = 0.0f;
		float Heat = 0.0f;
	};

	/**
	 * The fireball at time T (0 the flash, 1 the end of the flipbook): a cluster of lumps thrown out from the middle that
	 * swell and lift; white-yellow hot inside at first, cooling from the outside in to dark smoke, which thins at the end.
	 * Coordinates: the sprite is x, y in -1..1 (y up), the ray goes along z.
	 */
	inline Sample FireballAt(Vec3 P, float T, uint32_t Seed)
	{
		Sample S;
		const float Grow = 1.0f - (1.0f - T) * (1.0f - T) * (1.0f - T);
		const float Radius = 0.3f + 0.3f * Grow;
		// Lift: hot gas rises, more later.
		const Vec3 Q = P - Vec3{0.0f, -0.14f + 0.16f * T * T, 0.0f};
		// Lumps: six seeded centres pushed out with the growth.
		float Field = -10.0f;
		for (int I = 0; I < 7; ++I)
		{
			const float A = Hash3(I, 1, 0, Seed) * 6.2832f;
			const float B = (Hash3(I, 2, 0, Seed) - 0.5f) * 2.4f;
			const float Out = I == 0 ? 0.0f : (0.3f + 0.3f * Hash3(I, 3, 0, Seed)) * Radius;
			const Vec3 C{std::cos(A) * std::cos(B) * Out, std::sin(B) * Out * 0.8f + (I == 0 ? 0.0f : 0.05f), std::sin(A) * std::cos(B) * Out};
			const float R = Radius * (I == 0 ? 0.75f : 0.45f + 0.2f * Hash3(I, 4, 0, Seed));
			Field = std::max(Field, 1.0f - Length(Q - C) / R);
		}
		if (Field < -0.6f)
		{
			return S;
		}
		// Rolling surface: billow noise in the ball's own scale, drifting up as it grows.
		const Vec3 N = Q * (2.2f / Radius) + Vec3{0.0f, -1.6f * T, 0.7f * T};
		const float Lumps = Billow(N, Seed + 7u, kOctaves);
		const float Shape = Field + (Lumps - 0.62f) * 0.95f;
		const float Fade = 1.0f - SmoothStep(0.55f, 1.0f, T);  // thins out at the end
		S.Density = SmoothStep(0.0f, 0.3f, Shape) * (10.0f + 14.0f * (1.0f - T)) * (0.35f + 0.65f * Fade);
		// Heat: high in the middle of the field, cooling with time from the outside in; noise makes tongues of flame.
		const float Core = SmoothStep(-0.25f, 0.6f, Field + (Fbm(N * 0.7f + Vec3{3.0f, 1.0f, 0.0f}, Seed + 3u, 4, true) - 0.5f) * 0.7f);
		const float Cool = std::pow(Clamp01(1.0f - T * 1.5f), 1.3f);
		S.Heat = Clamp01(Core * (0.15f + 0.95f * Cool) - T * 0.3f) * SmoothStep(0.0f, 0.12f, Shape);
		return S;
	}

	/**
	 * A smoke puff at time T: one billowing cloud that swells, rises a little and thins away. `Grime` 0 is grey-white dust,
	 * 1 dark sooty smoke (only changes how lumpy it is; the colour is the material's).
	 */
	inline Sample SmokeAt(Vec3 P, float T, uint32_t Seed)
	{
		Sample S;
		const float Grow = 1.0f - (1.0f - T) * (1.0f - T);
		const float Radius = 0.34f + 0.34f * Grow;
		const Vec3 Q = P - Vec3{0.0f, -0.08f + 0.1f * T, 0.0f};
		float Field = -10.0f;
		for (int I = 0; I < 6; ++I)
		{
			const float A = Hash3(I, 5, 0, Seed) * 6.2832f;
			const float Out = I == 0 ? 0.0f : (0.25f + 0.3f * Hash3(I, 6, 0, Seed)) * Radius;
			const Vec3 C{std::cos(A) * Out, (Hash3(I, 7, 0, Seed) - 0.45f) * Out * 1.2f, std::sin(A) * Out};
			const float R = Radius * (I == 0 ? 0.72f : 0.42f + 0.25f * Hash3(I, 8, 0, Seed));
			Field = std::max(Field, 1.0f - Length(Q - C) / R);
		}
		if (Field < -0.6f)
		{
			return S;
		}
		const Vec3 N = Q * (1.9f / Radius) + Vec3{0.3f * T, -0.9f * T, 0.5f * T};
		const float Lumps = Billow(N, Seed + 11u, kOctaves);
		const float Shape = Field + (Lumps - 0.62f) * (0.7f + 0.3f * T);
		const float Thin = 1.0f - SmoothStep(0.35f, 1.0f, T);
		S.Density = SmoothStep(0.0f, 0.25f + 0.2f * T, Shape) * (6.0f + 6.0f * (1.0f - T)) * (0.15f + 0.85f * Thin);
		return S;
	}

	enum class Kind
	{
		Fireball,
		Smoke,
	};

	/** Renders one frame (index 0 .. kFrames^2 - 1) into its cell of `Out`. */
	inline void RenderFrame(Image& Out, Kind What, int Frame, uint32_t Seed)
	{
		const int Cell = Out.Size / kFrames;
		const int CX = (Frame % kFrames) * Cell;
		const int CY = (Frame / kFrames) * Cell;
		const float T = static_cast<float>(Frame) / static_cast<float>(kFrames * kFrames - 1);
		const int Steps = 128;
		const float Dz = 2.0f / Steps;
		const Vec3 LightDir{-0.35f, 0.85f, -0.4f};  // up, a little left and toward the viewer (z is away from the viewer)
		const float LightLen = Length(LightDir);
		const Vec3 L = LightDir * (1.0f / LightLen);
		auto At = [&](Vec3 P) { return What == Kind::Fireball ? FireballAt(P, T, Seed) : SmokeAt(P, T, Seed); };
		for (int Y = 0; Y < Cell; ++Y)
		{
			for (int X = 0; X < Cell; ++X)
			{
				// A little margin so a frame never touches its neighbour (mips).
				const float U = ((X + 0.5f) / Cell * 2.0f - 1.0f) * 1.06f;
				const float V = -((Y + 0.5f) / Cell * 2.0f - 1.0f) * 1.06f;
				Pixel& Px = Out.At(CX + X, CY + Y);
				// A per-pixel jitter of where the march starts hides the steps' banding.
				const float Jitter = Hash3(CX + X, CY + Y, Frame, Seed + 99u);
				float Trans = 1.0f;
				float Heat = 0.0f;
				float Light = 0.0f;
				float Depth = 0.0f;
				bool bFront = false;
				for (int I = 0; I < Steps && Trans > 0.01f; ++I)
				{
					const Vec3 P{U * kFill, V * kFill, (-1.0f + (I + Jitter) * Dz) * kFill};
					Sample S = At(P);
					// Nothing may touch the frame's edge: fade the volume out softly before it (a square cut reads as cheap).
					const float Edge = std::sqrt(P.X * P.X + P.Y * P.Y + P.Z * P.Z) / kFill;
					S.Density *= 1.0f - SmoothStep(0.86f, 1.0f, Edge);
					if (S.Density <= 0.0f)
					{
						continue;
					}
					const float Absorb = 1.0f - std::exp(-S.Density * Dz * (kFill / kOldFill));
					// Light: a few steps toward the light.
					float Shadow = 0.0f;
					for (int J = 1; J <= 6; ++J)
					{
						Shadow += At(P + L * (0.04f * J * J * 0.5f + 0.03f)).Density * 0.04f * J;
					}
					const float Lit = std::exp(-Shadow * 0.9f);
					const float Ambient = 0.18f;
					Light += Trans * Absorb * (Ambient + (1.0f - Ambient) * Lit);
					Heat += Trans * Absorb * std::pow(S.Heat, 1.5f) * 2.0f;
					if (!bFront && Absorb > 0.05f)
					{
						Depth = Clamp01(0.5f + (P.Z * -0.5f));
						bFront = true;
					}
					Trans *= 1.0f - Absorb;
				}
				const float Opacity = 1.0f - Trans;
				Px.Opacity = Clamp01((Opacity - 0.01f) / 0.99f);
				// Light and heat are kept per unit of opacity, so a thin edge is not darker than the middle.
				Px.Light = Opacity > 0.001f ? Clamp01(Light / Opacity) : 0.0f;
				Px.Heat = Clamp01(Heat / std::max(Opacity, 0.3f));
				Px.Depth = Depth;
			}
		}
	}

	inline Image MakeFlipbook(int Size, Kind What, uint32_t Seed)
	{
		Image Out;
		Out.Size = Size;
		Out.Pixels.assign(static_cast<size_t>(Size) * Size, Pixel{});
		for (int F = 0; F < kFrames * kFrames; ++F)
		{
			RenderFrame(Out, What, F, Seed);
		}
		return Out;
	}

	/** The scorch: soot thickest in a ragged middle, streaks thrown out in rays, a paler dusty rim. One picture. */
	inline Image MakeScorch(int Size, uint32_t Seed)
	{
		// A frag blast's mark seen from above: a ragged core of black soot, streaks of soot thrown out in rays past it, a
		// ring of pale dust and soil blown out between the rays, a small pit of churned lighter soil in the middle and
		// dark pocks where fragments struck. R = soot (how black), G = pale dust, A = how much of it covers the ground.
		// PC round 8: the old soft round blotch read as a shadow.
		Image Out;
		Out.Size = Size;
		Out.Pixels.assign(static_cast<size_t>(Size) * Size, Pixel{});
		for (int Y = 0; Y < Size; ++Y)
		{
			for (int X = 0; X < Size; ++X)
			{
				const float U = (X + 0.5f) / Size * 2.0f - 1.0f;
				const float V = (Y + 0.5f) / Size * 2.0f - 1.0f;
				const float R = std::sqrt(U * U + V * V);
				const float A = std::atan2(V, U);
				const float Ca = std::cos(A);
				const float Sa = std::sin(A);
				// Ragged edge of the core: slow lumps round the circle plus fine fraying.
				const float Lumps = Fbm(Vec3{Ca * 2.5f, Sa * 2.5f, 0.3f}, Seed + 31u, 3) - 0.5f;
				const float Fray = Fbm(Vec3{U * 9.0f, V * 9.0f, 0.7f}, Seed + 32u, 4) - 0.5f;
				const float Rc = 0.4f + 0.2f * Lumps + 0.07f * Fray;
				const float Speck = Fbm(Vec3{U * 26.0f, V * 26.0f, 2.0f}, Seed + 33u, 3);
				const float Core = 1.0f - SmoothStep(Rc - 0.04f, Rc + 0.03f, R);
				// Rays: narrow spokes of soot of random length, a little wavy and broken along their length.
				const float Wave = (Fbm(Vec3{R * 3.0f, 0.5f, 4.0f}, Seed + 34u, 2) - 0.5f) * 0.12f;
				const float Spoke = Fbm(Vec3{std::cos(A + Wave) * 16.0f, std::sin(A + Wave) * 16.0f, 1.3f}, Seed + 35u, 3);
				// Wider by the core, thinning out to their tips.
				const float Taper = Clamp01((R - Rc) / 0.45f);
				const float SpokeAmt = SmoothStep(0.46f + 0.12f * Taper, 0.6f + 0.08f * Taper, Spoke);
				const float Reach = Rc + 0.18f + 0.5f * SmoothStep(0.52f, 0.8f, Spoke);
				const float Broken = SmoothStep(0.3f, 0.55f, Fbm(Vec3{U * 14.0f, V * 14.0f, 5.0f}, Seed + 36u, 3));
				const float Rays = SpokeAmt * (1.0f - SmoothStep(Rc, Reach, R)) * Broken * (R > Rc - 0.05f ? 1.0f : 0.0f);
				// Pale dust thrown out in a ring just past the core, mostly between the rays.
				const float Ring = SmoothStep(Rc - 0.02f, Rc + 0.06f, R) * (1.0f - SmoothStep(Rc + 0.12f, Rc + 0.34f, R));
				const float Dust = Ring * (1.0f - 0.8f * SpokeAmt) * (0.3f + 0.4f * Speck);
				// The pit: churned soil, lighter, in the middle.
				const float Pit = 1.0f - SmoothStep(0.03f, 0.08f, R + (Speck - 0.5f) * 0.05f);
				// Fragment pocks: small dark dots scattered past the core.
				const float Dots = SmoothStep(0.74f, 0.8f, Fbm(Vec3{U * 40.0f, V * 40.0f, 3.0f}, Seed + 37u, 2)) * (1.0f - SmoothStep(0.55f, 0.92f, R));
				// The core is charred, not flat black (PC round 10): mottled soot, darker along thin cracks, patches of grey ash
				// between them and a few embers still glowing (B: ash 0 to 0.8, an ember 0.9 at its edge to 1 in its heart).
				const float Mottle = Fbm(Vec3{U * 7.0f, V * 7.0f, 6.0f}, Seed + 38u, 4);
				const float Crack = 1.0f - SmoothStep(0.0f, 0.04f, std::fabs(Fbm(Vec3{U * 11.0f, V * 11.0f, 7.0f}, Seed + 39u, 3) - 0.5f));
				const float CharDark = Clamp01(0.7f + 0.3f * (Mottle - 0.5f) + 0.12f * Crack + 0.1f * (Speck - 0.5f));
				const float AshAmt = Core * SmoothStep(0.6f, 0.78f, Fbm(Vec3{U * 5.0f, V * 5.0f, 8.0f}, Seed + 40u, 4)) * (1.0f - Crack) * (0.25f + 0.35f * Speck);
				// Embers a few centimetres across (PC round 11: pinpricks were lost in the texture's smaller mips at 6 m).
				const float EmberN = Fbm(Vec3{U * 14.0f, V * 14.0f, 9.0f}, Seed + 41u, 2);
				const bool bEmber = R < Rc * 0.85f && EmberN > 0.74f;
				float Soot = std::max({Core * CharDark, Rays * 0.95f, Dots * 0.85f});
				Soot *= 1.0f - 0.4f * Pit;
				const float Pale = Clamp01(std::max(Dust, Pit * 0.4f));
				Pixel& Px = Out.At(X, Y);
				Px.Heat = Clamp01(Soot);
				Px.Light = Pale * (1.0f - Clamp01(Soot));
				Px.Depth = bEmber ? 0.9f + 0.1f * SmoothStep(0.74f, 0.8f, EmberN) : 0.8f * Clamp01(AshAmt);
				Px.Opacity = Clamp01(std::max({Core * 0.96f, Soot, Pale * 0.7f}) * (1.0f - SmoothStep(0.88f, 0.99f, R)));
			}
		}
		return Out;
	}
}
