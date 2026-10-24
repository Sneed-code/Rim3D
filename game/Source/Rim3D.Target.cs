using UnrealBuildTool;

public class Rim3DTarget : TargetRules
{
	public Rim3DTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Rim3D");
	}
}
