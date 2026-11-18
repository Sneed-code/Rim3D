// The only Unreal-specific file in the sim. CMake builds leave it out.
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ColonySim)

// Unreal's unity build pastes the sim's .cpp files after this one in the same chunk. Unreal's
// assert macros would then rewrite sim names like Narrator::check and DrugSystem::ensure, so
// drop them here; nothing after this point in the module uses Unreal.
#undef check
#undef checkf
#undef checkSlow
#undef checkfSlow
#undef checkCode
#undef checkNoEntry
#undef checkNoReentry
#undef checkNoRecursion
#undef verify
#undef verifyf
#undef verifySlow
#undef ensure
#undef ensureMsgf
#undef ensureAlways
#undef ensureAlwaysMsgf
#undef TEXT
