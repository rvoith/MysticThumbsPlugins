#pragma once

// Avoid
// warning C4996: '_wfopen': This function or variable may be unsafe. Consider using _wfopen_s instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. See online help for details.
#define _CRT_SECURE_NO_WARNINGS


// The following ifdef block is the standard way of creating macros which make exporting 
// from a DLL simpler. All files within this DLL are compiled with the FFMpegPLUGIN_EXPORTS
// symbol defined on the command line. This symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see 
// FFMpegPLUGIN_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.


#ifdef FFMpegPLUGIN_EXPORTS
#define FFMpegPLUGIN_API __declspec(dllexport)
#else
#define FFMpegPLUGIN_API __declspec(dllimport)
#endif
