using UnrealBuildTool;

public class Rim3DEditorTarget : TargetRules
{
	public Rim3DEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Rim3D", "Rim3DEditor" });
	}
}
