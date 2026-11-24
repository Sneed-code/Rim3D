#include "HudIcons.h"

#include "ColonyHudStyle.h"

#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"

namespace
{
	TAutoConsoleVariable<int32> GHudIconsOn(TEXT("UI.HudIcons"), 1, TEXT("1: painted icons on the bar tabs, Build menu and order tools; 0: text only."));

	/** One icon: its picture at 256, 128, 64 and 32 pixels (each made from the one above, alpha-weighted so edges stay clean). */
	struct FHudIconSet
	{
		TArray<UTexture2D*, TInlineAllocator<4>> Levels;  // rooted, kept for the run
	};

	TMap<FString, FHudIconSet>& HudIconCache()
	{
		static TMap<FString, FHudIconSet> Cache;
		return Cache;
	}

	int32 GHudIconsLoaded = 0;

	/** Halves a BGRA picture: each pixel is the average of four, colour weighted by alpha so see-through pixels add no dark rim. */
	TArray<uint8> HudIconHalve(const TArray<uint8>& Src, int32 Width, int32 Height)
	{
		const int32 OutW = FMath::Max(1, Width / 2);
		const int32 OutH = FMath::Max(1, Height / 2);
		TArray<uint8> Out;
		Out.SetNumUninitialized(OutW * OutH * 4);
		for (int32 Y = 0; Y < OutH; ++Y)
		{
			for (int32 X = 0; X < OutW; ++X)
			{
				float Sum[3] = {0.0f, 0.0f, 0.0f};
				float AlphaSum = 0.0f;
				for (int32 DY = 0; DY < 2; ++DY)
				{
					for (int32 DX = 0; DX < 2; ++DX)
					{
						const int32 SX = FMath::Min(X * 2 + DX, Width - 1);
						const int32 SY = FMath::Min(Y * 2 + DY, Height - 1);
						const uint8* P = &Src[(SY * Width + SX) * 4];
						const float A = P[3] / 255.0f;
						Sum[0] += P[0] * A;
						Sum[1] += P[1] * A;
						Sum[2] += P[2] * A;
						AlphaSum += A;
					}
				}
				uint8* Q = &Out[(Y * OutW + X) * 4];
				for (int32 C = 0; C < 3; ++C)
				{
					Q[C] = AlphaSum > 0.0f ? static_cast<uint8>(FMath::Clamp(Sum[C] / AlphaSum, 0.0f, 255.0f)) : 0;
				}
				Q[3] = static_cast<uint8>(FMath::Clamp(AlphaSum / 4.0f * 255.0f + 0.5f, 0.0f, 255.0f));
			}
		}
		return Out;
	}

	UTexture2D* HudIconTexture(const TArray<uint8>& Bgra, int32 Width, int32 Height)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		FTexturePlatformData* Platform = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Platform || Platform->Mips.IsEmpty())
		{
			return nullptr;
		}
		FTexture2DMipMap& Mip = Platform->Mips[0];
		void* Pixels = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Pixels, Bgra.GetData(), FMath::Min<int64>(Bgra.Num(), Mip.BulkData.GetBulkDataSize()));
		Mip.BulkData.Unlock();
		Texture->SRGB = true;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		Texture->UpdateResource();
		Texture->AddToRoot();
		return Texture;
	}

	FHudIconSet HudIconLoad(const FString& Name)
	{
		FHudIconSet Set;
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/ui/icons") / (Name + TEXT(".png")));
		TArray<uint8> Compressed;
		if (!FFileHelper::LoadFileToArray(Compressed, *File, FILEREAD_Silent))
		{
			return Set;
		}
		IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
		const TSharedPtr<IImageWrapper> Png = Wrappers.CreateImageWrapper(EImageFormat::PNG);
		TArray64<uint8> Decoded;
		if (!Png.IsValid() || !Png->SetCompressed(Compressed.GetData(), Compressed.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Decoded))
		{
			UE_LOG(LogTemp, Warning, TEXT("HudIcons: could not read %s"), *File);
			return Set;
		}
		TArray<uint8> Raw(Decoded.GetData(), static_cast<int32>(Decoded.Num()));
		int32 Width = static_cast<int32>(Png->GetWidth());
		int32 Height = static_cast<int32>(Png->GetHeight());
		for (int32 Level = 0; Level < 4 && Width >= 16 && Height >= 16; ++Level)
		{
			if (UTexture2D* Texture = HudIconTexture(Raw, Width, Height))
			{
				Set.Levels.Add(Texture);
			}
			Raw = HudIconHalve(Raw, Width, Height);
			Width = FMath::Max(1, Width / 2);
			Height = FMath::Max(1, Height / 2);
		}
		++GHudIconsLoaded;
		return Set;
	}
}

namespace HudIcons
{
	UTexture* Get(const FString& Name, float Pixels)
	{
		if (GHudIconsOn.GetValueOnGameThread() == 0 || Name.IsEmpty())
		{
			return nullptr;
		}
		TMap<FString, FHudIconSet>& Cache = HudIconCache();
		const FHudIconSet* Set = Cache.Find(Name);
		if (!Set)
		{
			Set = &Cache.Add(Name, HudIconLoad(Name));
		}
		if (Set->Levels.IsEmpty())
		{
			return nullptr;
		}
		// The smallest picture still at least as big as it is drawn: bilinear filtering from no more than twice the size
		// stays sharp without shimmering.
		int32 Level = 0;
		while (Level + 1 < Set->Levels.Num() && Set->Levels[Level + 1]->GetSizeX() >= Pixels)
		{
			++Level;
		}
		return Set->Levels[Level];
	}

	void Draw(ImDrawList* DrawList, UTexture* Icon, const ImVec2& Centre, float Size, bool bDim)
	{
		if (!DrawList || !Icon)
		{
			return;
		}
		const float Half = Size * 0.5f;
		DrawList->AddImage(ImTextureRef(Icon), ImVec2(Centre.x - Half, Centre.y - Half), ImVec2(Centre.x + Half, Centre.y + Half),
			ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), bDim ? IM_COL32(255, 255, 255, 90) : IM_COL32(255, 255, 255, 255));
	}

	bool Button(UTexture* Icon, const FString& Label, bool bActive, const ImVec2& Size)
	{
		if (!Icon)
		{
			return ColonyHud::Button(Label, bActive, Size);
		}
		const int32 Hash = Label.Find(TEXT("##"));
		const FString Shown = Hash == INDEX_NONE ? Label : Label.Left(Hash);
		const FString Id = Hash == INDEX_NONE ? TEXT("##") + Label : Label.Mid(Hash);
		const FTCHARToUTF8 Text(*Shown);
		const ImGuiStyle& Style = ImGui::GetStyle();
		const ImVec2 TextSize = Shown.IsEmpty() ? ImVec2(0.0f, 0.0f) : ImGui::CalcTextSize(Text.Get());
		const float Height = Size.y > 0.0f ? Size.y : ImGui::GetFrameHeight();
		const float IconSide = FMath::Max(TextSize.y * 1.25f, Height * 0.80f);
		const float Gap = Shown.IsEmpty() ? 0.0f : Style.ItemInnerSpacing.x;
		const float Width = Size.x > 0.0f ? Size.x : IconSide + Gap + TextSize.x + 2.0f * Style.FramePadding.x;
		const bool bClicked = ColonyHud::Button(Id, bActive, ImVec2(Width, Height));
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		// Icon and label centred together; a label too long for the button is drawn smaller so it never spills out.
		const float Room = FMath::Max(1.0f, Max.x - Min.x - 2.0f * Style.FramePadding.x - IconSide - Gap);
		const float FontSize = TextSize.x > Room ? ImGui::GetFontSize() * FMath::Max(0.55f, Room / TextSize.x) : ImGui::GetFontSize();
		const float TextWidth = TextSize.x > Room ? TextSize.x * FontSize / ImGui::GetFontSize() : TextSize.x;
		const float Total = IconSide + Gap + TextWidth;
		const float Left = (Min.x + Max.x - Total) * 0.5f;
		const float MidY = (Min.y + Max.y) * 0.5f;
		Draw(DrawList, Icon, ImVec2(Left + IconSide * 0.5f, MidY), IconSide);
		if (!Shown.IsEmpty())
		{
			const ImU32 Color = ImGui::GetColorU32(ImGuiCol_Text);
			DrawList->AddText(ImGui::GetFont(), FontSize, ImVec2(Left + IconSide + Gap, MidY - FontSize * 0.5f), Color, Text.Get());
		}
		return bClicked;
	}

	int32 LoadedCount()
	{
		return GHudIconsLoaded;
	}
}
