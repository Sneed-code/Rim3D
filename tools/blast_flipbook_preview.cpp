// Renders the blast flipbooks (game/Source/Rim3DEditor/BlastFlipbooks.h) without Unreal, so the cloud can look at them.
//   g++ -O2 -std=c++20 -pthread -I game/Source/Rim3DEditor tools/blast_flipbook_preview.cpp -o build/blast_preview
//   ./build/blast_preview out_dir [size]
// Writes fire.rgba, smoke.rgba and scorch.rgba (raw 8-bit RGBA, Heat Light Depth Opacity) for tools/blast_preview.py.

#include "BlastFlipbooks.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void writeRaw(const std::string& Path, const blastfx::Image& Image)
	{
		std::vector<unsigned char> Bytes(Image.Pixels.size() * 4);
		auto Q = [](float V) { return static_cast<unsigned char>(blastfx::Clamp01(V) * 255.0f + 0.5f); };
		for (size_t I = 0; I < Image.Pixels.size(); ++I)
		{
			Bytes[I * 4 + 0] = Q(Image.Pixels[I].Heat);
			Bytes[I * 4 + 1] = Q(Image.Pixels[I].Light);
			Bytes[I * 4 + 2] = Q(Image.Pixels[I].Depth);
			Bytes[I * 4 + 3] = Q(Image.Pixels[I].Opacity);
		}
		FILE* File = std::fopen(Path.c_str(), "wb");
		if (!File)
		{
			std::fprintf(stderr, "cannot write %s\n", Path.c_str());
			std::exit(1);
		}
		std::fwrite(Bytes.data(), 1, Bytes.size(), File);
		std::fclose(File);
		std::printf("%s %d\n", Path.c_str(), Image.Size);
	}

	blastfx::Image renderParallel(int Size, blastfx::Kind What, uint32_t Seed)
	{
		blastfx::Image Out;
		Out.Size = Size;
		Out.Pixels.assign(static_cast<size_t>(Size) * Size, blastfx::Pixel{});
		const int Count = blastfx::kFrames * blastfx::kFrames;
		const unsigned Threads = std::max(1u, std::thread::hardware_concurrency());
		std::vector<std::thread> Pool;
		for (unsigned T = 0; T < Threads; ++T)
		{
			Pool.emplace_back([&, T] {
				for (int F = static_cast<int>(T); F < Count; F += static_cast<int>(Threads))
				{
					blastfx::RenderFrame(Out, What, F, Seed);
				}
			});
		}
		for (std::thread& Thread : Pool)
		{
			Thread.join();
		}
		return Out;
	}
}

int main(int Argc, char** Argv)
{
	const std::string Dir = Argc > 1 ? Argv[1] : ".";
	const int Size = Argc > 2 ? std::atoi(Argv[2]) : 1024;
	writeRaw(Dir + "/fire.rgba", renderParallel(Size, blastfx::Kind::Fireball, 1));
	writeRaw(Dir + "/smoke.rgba", renderParallel(Size, blastfx::Kind::Smoke, 2));
	writeRaw(Dir + "/scorch.rgba", blastfx::MakeScorch(Size / 2, 3));
	return 0;
}
