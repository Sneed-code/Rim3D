#pragma once

// Unreal defines COLONYSIM_API to export the sim's classes from the plugin DLL.
// Standalone CMake builds leave it empty.
#if defined(COLONYSIM_API)
// COLONYSIM_API expands to DLLEXPORT/DLLIMPORT from Unreal's platform headers,
// which sim files don't include. Define them when a sim file is compiled on its
// own (Unreal's definitions are identical, so there's no clash).
#if defined(_MSC_VER) && !defined(DLLEXPORT)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#endif
#define COLONY_API COLONYSIM_API
#else
#define COLONY_API
#endif
