#pragma once

// Export macro for this DLL
#ifdef DLLPLUGIN_EXPORTS
#define DLLPLUGIN_API __declspec(dllexport)
#else
#define DLLPLUGIN_API __declspec(dllimport)
#endif
