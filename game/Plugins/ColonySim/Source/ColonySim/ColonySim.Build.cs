using UnrealBuildTool;

// The sim is plain C++. Only ColonySimModule.cpp touches Unreal; everything else
// must also build standalone with CMake (see CMakeLists.txt in the plugin root).
public class ColonySim : ModuleRules
{
	public ColonySim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bEnableExceptions = false;
		bUseRTTI = false;

		PrivateDependencyModuleNames.Add("Core");
	}
}
