#pragma once

// Unreal defines COLONYSIM_API to export the sim's classes from the plugin DLL.
// Standalone CMake builds leave it empty.
#if defined(COLONYSIM_API)
#define COLONY_API COLONYSIM_API
#else
#define COLONY_API
#endif
