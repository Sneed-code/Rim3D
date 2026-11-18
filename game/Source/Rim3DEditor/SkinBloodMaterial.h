#pragma once

#include "CoreMinimal.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"

// Blood in the skin (docs/design/blood.md): the MakeHuman skin material reads a per-pawn blood mask in its own UV
// space (USkinBlood paints it). R: how much blood (fades 3% every 10 s), G: how wet (dries over Blood.DryMinutes),
// B: a wound's dark open core. Fresh blood is bright, glossy and a little raised; dry blood dark brown and matte. With
// BloodOn 0 (every pawn not bleeding) the skin is as it was. Used by BloodArt (adds it to the existing skin material)
// and MakeHumanArt (when it makes the skin material again).

namespace SkinBloodMaterial
{
	inline const TCHAR* const Tag = TEXT("SkinBlood");

	inline bool IsTagged(const UMaterialExpression* Expression)
	{
		return Expression && Expression->Desc.StartsWith(Tag);
	}

	/** True if the material already reads a blood mask. */
	inline bool Has(UMaterial* Material)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (IsTagged(Expression))
			{
				return true;
			}
		}
		return false;
	}

	/** Takes the blood out again (to make it anew): each property goes back to what fed it before. */
	inline void Strip(UMaterial* Material)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		FExpressionInput* Properties[] = {&Data->BaseColor, &Data->Roughness, &Data->Normal};
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (FExpressionInput* Property : Properties)
			{
				auto* Custom = Cast<UMaterialExpressionCustom>(Property->Expression);
				if (IsTagged(Custom) && Custom->Inputs.Num() > 0)
				{
					const FExpressionInput Before = Custom->Inputs[0].Input;
					Property->Connect(Before.OutputIndex, Before.Expression);
					bChanged = true;
				}
			}
		}
		TArray<UMaterialExpression*> Gone;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (IsTagged(Expression))
			{
				Gone.Add(Expression);
			}
		}
		for (UMaterialExpression* Expression : Gone)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
	}

	template <typename T>
	T* Node(UMaterial* Material, int32 X, int32 Y, const TCHAR* Desc)
	{
		T* Expression = Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
		Expression->Desc = FString(Tag) + Desc;
		return Expression;
	}

	inline UMaterialExpressionCustom* Custom(UMaterial* Material, int32 Y, const TCHAR* Desc, ECustomMaterialOutputType Out, const TCHAR* Code,
		std::initializer_list<const TCHAR*> InputNames)
	{
		auto* Expression = Node<UMaterialExpressionCustom>(Material, 200, Y, Desc);
		Expression->OutputType = Out;
		Expression->Description = FString(Tag) + Desc;
		Expression->Code = Code;
		Expression->Inputs.SetNum(InputNames.size());
		int32 Index = 0;
		for (const TCHAR* Name : InputNames)
		{
			Expression->Inputs[Index++].InputName = Name;
		}
		return Expression;
	}

	/** Adds the blood to a skin material whose base colour, roughness and normal are connected. False if one is not. */
	inline bool Add(UMaterial* Material)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		if (!Data->BaseColor.Expression || !Data->Roughness.Expression || !Data->Normal.Expression)
		{
			return false;
		}
		auto* Mask = Node<UMaterialExpressionTextureObjectParameter>(Material, -200, 3200, TEXT("Mask"));
		Mask->ParameterName = TEXT("BloodMask");
		Mask->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
		Mask->SamplerType = SAMPLERTYPE_Color;
		auto* On = Node<UMaterialExpressionScalarParameter>(Material, -200, 3350, TEXT("On"));
		On->ParameterName = TEXT("BloodOn");
		On->DefaultValue = 0.0f;
		auto* Res = Node<UMaterialExpressionScalarParameter>(Material, -200, 3450, TEXT("Res"));
		Res->ParameterName = TEXT("BloodMaskRes");
		Res->DefaultValue = 1024.0f;
		auto* Bump = Node<UMaterialExpressionScalarParameter>(Material, -200, 3550, TEXT("Bump"));
		Bump->ParameterName = TEXT("BloodBump");
		Bump->DefaultValue = 1.5f;
		auto* Wet = Node<UMaterialExpressionVectorParameter>(Material, -200, 3650, TEXT("Wet"));
		Wet->ParameterName = TEXT("BloodWetColor");
		Wet->DefaultValue = FLinearColor(0.32f, 0.02f, 0.016f);
		auto* Dry = Node<UMaterialExpressionVectorParameter>(Material, -200, 3800, TEXT("Dry"));
		Dry->ParameterName = TEXT("BloodDryColor");
		Dry->DefaultValue = FLinearColor(0.07f, 0.018f, 0.012f);
		auto* WetRough = Node<UMaterialExpressionScalarParameter>(Material, -200, 3950, TEXT("WetRough"));
		WetRough->ParameterName = TEXT("BloodWetRoughness");
		WetRough->DefaultValue = 0.3f;
		auto* UV = Node<UMaterialExpressionTextureCoordinate>(Material, -200, 4050, TEXT("UV"));

		// Colour: a thin pink-brown stain where there is a little, the blood itself where there is more; the open core
		// of a hole or gash much darker.
		auto* Colour = Custom(Material, -700, TEXT("Colour"), CMOT_Float3,
			TEXT("float4 m = Texture2DSample(Mask, MaskSampler, UV);\n"
				 "float a = saturate(m.r * 1.25) * On;\n"
				 "if (a <= 0.001) return Base;\n"
				 "float3 blood = lerp(Dry, Wet, saturate(m.g));\n"
				 "blood *= 1.0 - 0.8 * saturate(m.b * On);\n"
				 "float3 stained = Base * lerp(float3(1, 1, 1), float3(0.72, 0.4, 0.38), saturate(m.r * 4.0) * On);\n"
				 "return lerp(stained, blood, a * 0.96);"),
			{TEXT("Base"), TEXT("Mask"), TEXT("UV"), TEXT("On"), TEXT("Wet"), TEXT("Dry")});
		Colour->Inputs[0].Input.Connect(Data->BaseColor.OutputIndex, Data->BaseColor.Expression);
		Colour->Inputs[1].Input.Connect(0, Mask);
		Colour->Inputs[2].Input.Connect(0, UV);
		Colour->Inputs[3].Input.Connect(0, On);
		Colour->Inputs[4].Input.Connect(0, Wet);
		Colour->Inputs[5].Input.Connect(0, Dry);
		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);

		// Roughness: wet blood glossy, dry matte.
		auto* Rough = Custom(Material, 1500, TEXT("Roughness"), CMOT_Float1,
			TEXT("float4 m = Texture2DSample(Mask, MaskSampler, UV);\n"
				 "float a = saturate(m.r * 1.25) * On;\n"
				 "return lerp(Base, lerp(0.68, WetRough, saturate(m.g)), a);"),
			{TEXT("Base"), TEXT("Mask"), TEXT("UV"), TEXT("On"), TEXT("WetRough")});
		Rough->Inputs[0].Input.Connect(Data->Roughness.OutputIndex, Data->Roughness.Expression);
		Rough->Inputs[1].Input.Connect(0, Mask);
		Rough->Inputs[2].Input.Connect(0, UV);
		Rough->Inputs[3].Input.Connect(0, On);
		Rough->Inputs[4].Input.Connect(0, WetRough);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);

		// Normal: blood a little raised, a wound's core sunk in.
		auto* Normal = Custom(Material, 2600, TEXT("Normal"), CMOT_Float3,
			TEXT("if (On < 0.5) return Base;\n"
				 "float4 c = Texture2DSample(Mask, MaskSampler, UV);\n"
				 "if (c.r <= 0.001) return Base;\n"
				 "float px = 1.0 / max(Res, 1.0);\n"
				 "float4 sx = Texture2DSample(Mask, MaskSampler, UV + float2(px, 0));\n"
				 "float4 sy = Texture2DSample(Mask, MaskSampler, UV + float2(0, px));\n"
				 "float h = saturate(c.r) * 0.5 - c.b;\n"
				 "float2 g = float2(saturate(sx.r) * 0.5 - sx.b - h, saturate(sy.r) * 0.5 - sy.b - h) * Bump;\n"
				 "return normalize(float3(Base.xy - g, Base.z));"),
			{TEXT("Base"), TEXT("Mask"), TEXT("UV"), TEXT("On"), TEXT("Res"), TEXT("Bump")});
		Normal->Inputs[0].Input.Connect(Data->Normal.OutputIndex, Data->Normal.Expression);
		Normal->Inputs[1].Input.Connect(0, Mask);
		Normal->Inputs[2].Input.Connect(0, UV);
		Normal->Inputs[3].Input.Connect(0, On);
		Normal->Inputs[4].Input.Connect(0, Res);
		Normal->Inputs[5].Input.Connect(0, Bump);
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);
		return true;
	}
}
