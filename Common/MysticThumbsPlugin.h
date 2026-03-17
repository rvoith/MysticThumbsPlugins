/////////////////////////////////////////////////////////////////////////////
// 
// Header for MysticThumbs plugins
// 
// Copyright 2011-2026 MysticCoder
// http://mysticcoder.net/mysticthumbs
// 
// This file and the associated example plugin project can be found at:
// https://github.com/ianmasters/MysticThumbsExamplePlugin
// Please read the LICENSE file found there for the terms under which this code is licensed.
//
// 
// This code is provided as an example and template for creating MysticThumbs plugins.
// 
// 
// This file is the primary interface header for MysticThumbs plugins.
// It should not be modified or redistributed in any way.
// Please see the MysticThumbs license.
// 
// 2024-12-27 - Bumped MYSTICTHUMBS_PLUGIN_VERSION to 2.
// Added Version() DLL function for plugins to report their version.
// Work on flags to support single checkerboard and canvas.
// Updated to MysticThumbsPluginPing2 for Ping().
// 
// Header version MYSTICTHUMBS_PLUGIN_VERSION
// When implementing version 2 or newer plusins, use this to return from
// the DLL Version() function.
// Version 2: Rearranged MysticThumbsPluginFlags.
// Version 3: 2026-01-10 added Generate() method for more control and various
//              support structs.
//            Added GetCapabilities() method.
//		      Added IWICBitmapSource support for output images using Generate().
//			  Added ability to query IPersistFile from the IStream to get the
//              full file path if it is required. See example for an example.
//            Added Configure method to allow configuration of the plugin if
//              needed. See PluginCapabilities_CanConfigure to support this.
// Version 4: Overhauled how a lot of functions work.
//            LocalAlloc / StrDup no longer needed for GetName, GetExtension
//              etc. calls. Strings are copied by the caller.
//            Removed Initialize and Shutdown calls because they may have been
//              called out of order if multiple processes are holding references.
//              Consider using an atomoc instance count in your plugin to handle
//              global initialization and cleanup if needed.
// 
// 
// Visual Studio / C++ / ATL and implementation notes:
// - Try to use CComPtr smart pointers for COM interfaces such as IStream, IWICBitmapSource, etc. to manage lifetimes and avoid leaks.
// - Use CRegKey to manage registry keys and values for configuration storage. This is a simple wrapper around the Windows registry API that provides some convenience functions and RAII semantics.
// -- Don't cache registry values because they may change between calls to Generate() or Ping(). Instead, read from the registry as needed in these functions to ensure you have the latest values. CRegKey makes this easy and efficient.
// - Use the logging interface provided by MysticThumbs via the plugin context for logging. This ensures that your logs are integrated with MysticThumbs' logging system and appear in the correct order relative to other log messages.
// - Generate() can return a bitmap not in BGRA or PBGRA format, so you can avoid manual swizzling and format conversions. MysticThumbs will take care of it.
// 
// !!! A NOTE ON DEPLOYMENT OF YOUR .mtp AND ASSOCIATED DLLs !!!
// 
// This is a suggestion, not a hard rule but it will help your plugins load when they may be dependent on other DLLs.
// 
// In the Plugins folder, either for the current user or all users, create two folders:
// One named "32" and one named "64".
// Put your respective 32 bit and 64 bit plugin DLLs in these folders ALONG SIDE any dependencies they may need.
// This will keep the 32 and 64 bit plugins separated and ensure that loading by higher security processes such as StartMenuExperienceHost.exe works correctly.
// The dependent DLLs should all be in the same folder as the .mtp plugin DLL to ensure loading in high security processes
// such as sandboxed / Windows Store or system applications (such as StartMenuExperienceHost.exe).
// 
/////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef _MysticThumbsPlugin_h_
#define _MysticThumbsPlugin_h_

// Use this for your Version() DLL function
// As of MysticThumbs 2026 versions only version 4 or newer plugins are supported.
#define MYSTICTHUMBS_PLUGIN_VERSION 4

// Include Windows headers that we need
#include <Windows.h>
#include <WinCodec.h> // required for VERSION 3

/////////////////////////////////////////////////////////////////////////////
// 
// Logging interface
// Use this instead of creating your own logging system. This ensures that
// your log calls are inserted into the MysticThumbs log file at
// the correct position relative to caller code.
//
// Do not derive classes from this interface. It is managed by MysticThumbs.
// 
/////////////////////////////////////////////////////////////////////////////

struct IMysticThumbsLog
{
    virtual void LoggingEnable(_In_ bool enable) = 0;                   // enable or disable logging for this instance. Logging is enabled by default.
    virtual bool IsLoggingEnabled() const = 0;                          // check if logging is enabled for this instance

    virtual void log(_In_ const wchar_t* message) const = 0;            // simple log message with no formatting or you have already formatted the message
    virtual void logf(_In_ const wchar_t* message, _In_ ...) const = 0; // so you can do printf style logging. Make sure to use wide strings (wchar_t) and correct format specifiers for your data types.
};




/////////////////////////////////////////////////////////////////////////////
// 
// structSize members
// 
// Used for versioning and future expansion of structs.
// 
// For _In_ or _Inout_ structs, check this member to see which version
// you are dealing with against your sizeof(the struct) header. This should
// not strictly be necessary since the plugin is compiled against your
// specific header file / version, but is useful for future-proofing.
// 
// For _Out_ structs, set this member to sizeof(the struct) and
// fill in required members.
//  
/////////////////////////////////////////////////////////////////////////////



/////////////////////////////////////////////////////////////////////////////
// 
// Ping information, used when MysticThumbs is requesting image information
// 
/////////////////////////////////////////////////////////////////////////////

enum MysticThumbsPluginPingFlags : unsigned int
{
    MysticThumbsPluginPingFlags_None = 0x00000000,
    MysticThumbsPluginPingFlags_QuickView = 0x00000001,    // Ping is for QuickView
    MysticThumbsPluginPingFlags_Properties = 0x00000002,   // Ping wants properties only. always true if QuickView, otherwise no image generation is likely to follow (no guarantee)
};
DEFINE_ENUM_FLAG_OPERATORS(MysticThumbsPluginPingFlags)

struct MysticThumbsPluginPing
{
    unsigned int structSize;            // (input) sizeof this struct for versioning and future expansion

    MysticThumbsPluginPingFlags flags;  // (input) Ping flags that determine the operation mode and required results

    // Important!
    // These required sizes are hints and usually only used for procedural generation.
    // For normal ping operation return width/height as the actual image size if it is known.
    // These can be zero - 0 - which usually means "I just want the image size"
    unsigned int requestedWidth;		// (input, hint) either the thumbnail width being requested or if isQuickView then the window width
    unsigned int requestedHeight;		// (input, hint) either the thumbnail height being requested or if isQuickView then the window height

    unsigned int width;                 // (output) actual width in pixels of the image
    unsigned int height;                // (output) actual height in pixels of the image
    unsigned int bitDepth;              // (output) bit depth
};




/////////////////////////////////////////////////////////////////////////////
//
// Plugin context passed to plugin initialization and valid for
// the life of plugin instance until after Destroy().
// 
// Contains useful information for use throughout the lifetime of the plugin.
// 
// IMysticThumbsLog is inherited so logging functions are available
// directly from this interface as well. The implementation of which is
// provided by MysticThumbs.
// 
// Do not derive classes from this interface. It is managed by MysticThumbs.
//
/////////////////////////////////////////////////////////////////////////////

struct IMysticThumbsPluginContext
{
    /// <summary>
    /// Get the stream we are reading from. The stream is valid for the lifetime of the plugin instance.
    /// Note, this is not AddRef'ed so there is no need to Release() it. You could however if you wish
    /// use a smart pointer such as ATL CComPtr if you wish to manage your usage lifetime of it.
    /// </summary>
    /// <returns>The COM IStream handle.</returns>
    virtual _Check_return_ IStream* GetStream() const = 0;

    /// <summary>
    /// Reference the original ping that is populated in PingImage().
    /// Only valid during or after PingImage() has been called on the plugin instance.
    /// </summary>
    /// <returns>The ping struct / interface</returns>
    virtual _Check_return_ const MysticThumbsPluginPing* GetPing() const = 0;

    /// <summary>
    /// Get the root key the plugin should use to save/load configuration settings to the registry.
    /// Each user gets their own registry settings on a multi-user machine.
    /// The returned handle is live for the lifetime of the instance and should NOT be closed with RegCloseKey().
    /// WARNING: This can not be called until after CreateInstance() has been completed. Do not call from a plugin constructor. It will return NULL there.
    /// NOTE: Highly recommended you use atlbase.h / CRegKey 'smart objects' to manage your registry values and sub-key / values from this base.
    ///       See the ATL documentation for more details.
    /// </summary>
    /// <returns>A registry HKEY root key where your plugin config is stored. NULL if error.</returns>
    virtual _Check_return_ HKEY GetPluginRegistryRootKey() const = 0;

    /// <summary>
    /// Determines if tooltips are enabled.
    /// </summary>
    /// <returns>true if tooltips are enabled in the control panel.</returns>
    virtual _Check_return_ bool TooltipsEnabled() const = 0;

    /// <summary>
    /// Determines if dark mode is enabled for the current process.
    /// This may be useful for configuration dialogs or possibly even thumbnail generation if you want to match the system theme.
    /// </summary>
    /// <returns>true if the process is detected as being in dark mode.</returns>
    virtual _Check_return_ bool IsDarkMode() const = 0;

    /// <summary>
    /// Get the logging interface for this plugin instance.
    /// </summary>
    /// <returns>The logging interface </returns>
    virtual _Check_return_ const IMysticThumbsLog* Log() const = 0;

    /// <summary>
    /// Check if this is a default instance used by MysticThumbs control panel or plugin helper functions to gather plugin specific information.
    /// A default instance is not used for image generation.
    /// This could possibly be checked to setup DLL module global settings that *ABSOLUTELY WILL NOT change* during the lifetime of the plugin DLL.
    /// </summary>
    /// <returns>true if this is a default instance.</returns>
    virtual _Check_return_ bool IsDefaultInstance() const = 0;
};



// This needs to be filled out with relevant capabilities of the plugin that may be useful to MysticThumbs.
enum MysticThumbsPluginCapabilities : unsigned int
{
    PluginCapabilities_CanConfigure = 0x00000001, // Supports Configure() to allow configuration of the plugin
    PluginCapabilities_CanNonUniformSize = 0x00000010, // Allows resizing to in QuickView without locking aspect ratio. Should only be set if the plugin can handle non-square thumbnails for example computation-generated images or resizable paper-like documents. For normal images this should not be set.
    PluginCapabilities_IsProcedural = 0x00000020, // Indicates this plugin generates procedural images. Useful for QuickView to automatically refresh when resized and some other things. For normal images this should not be set.
};
DEFINE_ENUM_FLAG_OPERATORS(MysticThumbsPluginCapabilities)

/////////////////////////////////////////////////////////////////////////////
// 
// Flags passed to IMysticThumbsPlugin::Generate()
// 
// These are only hints. Generally you should create a transparent 32 bit
// image since MysticThumbs does the heavy lifting on appearance based
// on the settings for the plugin extensions.
// 
/////////////////////////////////////////////////////////////////////////////

// Gerneration flags. Note we use bit flags here for easier reading in the debugger due to DEFINE_ENUM_FLAG_OPERATORS,
// even though it doesn't make sense to have two transparency flags set at once etc.
enum MysticThumbsPluginFlags : unsigned int
{
    // NOTE: these have changed since version 2. They are still hints but are now in different orders. Recompile may be needed.
    // Transparency flags, one of the following:
    MT_Transparency_Disable = 0x00000001, // opaque
    MT_Transparency_Canvas = 0x00000002, // Used to be MT_Transparency_Checkerboard2.
    MT_Transparency_Transparent = 0x00000004,
    MT_Transparency_Checkerboard = 0x00000008,
    MT_Transparency_Mask = 0x0000000f, // all transparency bits

    // NOTE: these have changed since version 2. They are still hints but are now in different orders. Recompile may be needed.
    // Embedded thumbnail flags, one of the following:
    MT_EmbeddedThumb_Never = 0x00000010,
    MT_EmbeddedThumb_Always = 0x00000020,
    MT_EmbeddedThumb_IfLarger = 0x00000040,
    MT_EmbeddedThumb_Mask = 0x00000070, // all embedded thumbnail bits

    // Scale to fit is requested. This is handled by MysticThumbs, but the hint is here.
    MT_ScaleUp = 0x00000100,
    MT_Scale_Mask = 0x00000100, // all scale bits

    // Added in 2026.1.2 - not passed in previous versions.
    // Sampling methods should be considered carefully.
    // MysticThumbs will scale your output to the destination size regardless of these flags.
    // If you are compositing an image, think carefully about if you want to be scaling the composite images with or without scaling since the output image will be likely the source image size.
    // Ideally, use nearest sampling when COMPOSITING to the target size. Any sub sampling or resizing using another method can introduce artifacts.
    // Thus - multiple filters can reduce image quality, so:
    // - Composite at a "canvas size without filters" (MT_Sample_Nearest) and pass the result to MysticThumbs since it will do final scaling with your result.
    // - This depends on how you are composing if that is what you are going, but try to stay with nearest sampling where possible.
    MT_Sample_Nearest = 0x00000200,
    MT_Sample_Linear = 0x00000400,
    MT_Sample_Cubic = 0x00000800,
    MT_Sample_Mask = 0x00000e00, // all sample bits

    // Output flags up in the high bits.

    // Hint about alpha. Output flag hint from plugin.
    MT_HasAlpha = 0x10000000,
};
DEFINE_ENUM_FLAG_OPERATORS(MysticThumbsPluginFlags)

// This is an in/out struct for MYSTICTHUMBS_PLUGIN_VERSION >= 3 Generate()
struct MysticThumbsPluginGenerateParams {
    unsigned int structSize = sizeof(MysticThumbsPluginGenerateParams);	// sizeof this struct for versioning and future expansion

    // input parameters
    unsigned int desiredWidth;		// desired width in pixels
    unsigned int desiredHeight;		// desired height in pixels (usually same as width)
    MysticThumbsPluginFlags flags;	// flags
};


/////////////////////////////////////////////////////////////////////////////
// 
// Derive your plugin class from this interface and implement all methods
// 
/////////////////////////////////////////////////////////////////////////////

struct IMysticThumbsPlugin
{
public:

    /// Destroy this plugin instance
    virtual void Destroy() = 0;

    /// Identify this plugin by name
    virtual _Notnull_ LPCWSTR GetName() const = 0;

    // Identify plugin by GUID for uniqueness
    virtual _Notnull_ LPCGUID GetGuid() const = 0;

    /// <summary>
    /// New in VERSION 4: Get a description of this plugin including what it does and any copyright information.
    /// Use /n for new lines as needed.
    /// </summary>
    /// <returns>The string, can be, but ideally not be, nullptr</returns>
    virtual _Notnull_ LPCWSTR GetDescription() const = 0;

    /// <summary>
    /// New in VERSION 4: Get the author of this plugin.
    /// Who wrote it, company information, email, GitHub links etc.
    /// Use /n for new lines as needed.
    /// </summary>
    /// <returns>The string, can be, but ideally not be, nullptr</returns>
    virtual _Notnull_ LPCWSTR GetAuthor() const = 0;

    // List extensions supported
    virtual unsigned int GetExtensionCount() const = 0;

    /// Get an extension by index.
    virtual _Notnull_ LPCWSTR GetExtension(_In_ unsigned int index) const = 0;

    /// Ping an image to report it's information such as dimensions and bit depth.
    /// Fill in fields that are relevant. If no ping information can be determined do nothing and return false.
    /// Cast MysticThumbsPluginPing to MysticThumbsPluginPing2 or whatever is relevant for your plugin MYSTICTHUMBS_PLUGIN_VERSION.
    /// WARNING: Do not assume that a PingImage will be followed by a Generate call. MysticThumbs may call PingImage multiple times with different threads without successive Generate calls. They are not necessarily tightly coupled.
    virtual bool Ping(_Inout_ MysticThumbsPluginPing& ping) = 0;

    /////////////////////////////////////////////////////////////////////////////
    // VERSION 3 additions. These methods are optional to implement.
    // Must implement if compiling against MYSTICTHUMBS_PLUGIN_VERSION >= 3.
    // If you are recompiling with latest headers you should implement all
    // the extra methods even if only stubs.
    /////////////////////////////////////////////////////////////////////////////

    /// <summary>
    /// Get the capabilities of this plugin. If it is not implemented return false.
    /// <param name="capabilities">Output parameter to fill in the capabilities of this plugin.</param>
    /// <returns>true if the capabilities struct is populated.</returns>
    virtual bool GetCapabilities(_Out_ MysticThumbsPluginCapabilities& capabilities) = 0;

    /// <summary>
    /// Allows the plugin to be configured if needed by opening a modal dialog box and storing settings in the registry.
    /// </summary>
    /// <param name="hWndParent">Handle to the parent window for the configuration dialog. It's important to use this for correct Windows behaviour.</param>
    /// <returns>false if nothing was done or configuration is not supported. true if something was done.</returns>
    virtual bool Configure(_In_ HWND hWndParent) = 0;

    /// Prior to VERSION 3 this method did not exist so expect for it not to be found.
    /// In VERSION 3 it is the only option.
    /// Parameters
    /// lplpOutputImage - the IWICBitmapSource is owned by the caller and should have a reference of 1. Do not release it yourself.
    /// params - input parameters to aid in generating the image, and output parameters to aid MysticThumbs with knowledge of the output image.
    /// returns E_NOTIMPL if the function is not supported. Nothing will be done, otherwise the result of the image generation. Return any WIC or other Win32 errors ( using HRESULT_FROM_WIN32(GetLastError() ) as needed.
    virtual HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params, _COM_Outptr_result_maybenull_ IWICBitmapSource** lplpOutputImage) = 0;
};


/////////////////////////////////////////////////////////////////////////////
// 
// DLL functions required to be implemented by each plugin
// 
// Note: These these functions should defined extern "C" so they can be found
//       See ExamplePlugin.cpp
// 
/////////////////////////////////////////////////////////////////////////////


// Identify version of plugin used. Will not exist in a version 1 plugin.
// For MYSTICTHUMBS_PLUGIN_VERSION >= 2 implement this and return MYSTICTHUMBS_PLUGIN_VERSION.
typedef int (*MTP_Version)();

// Initialize the plugin. Any initialization required for all instances can be done here.
// Removed in version 4. Use the plugin's DllMain instead (but only for Kernel32.dll calls.)
// If wanting to initialize cached "global" items, consider tracking instances and
// initializing on first instance creation and cleaning up on final instance deletion.
//typedef bool (*MTP_Initialize)();

// Shutdown the plugin. Clean up everything when the plugin is being unloaded.
// Removed in version 4.
//typedef bool (*MTP_Shutdown)();

// MTP_CreateInstance changed removed since VERSION 3.
// Should be fine since MTP_Version exists to identify version and creation will early exit if not version 3 or newer.

// Create an instance of a thumbnail provider
// The context is valid for the lifetime of the plugin instance.
typedef IMysticThumbsPlugin* (*MTP_CreateInstance)(_In_ IMysticThumbsPluginContext* context);

/// <summary>
/// Optional function to prevent loading this plugin entirely.
/// If this function exists and returns true, the plugin will not be loaded.
/// This can be used to check for example if the plugin is a debug build or if certain dependencies are missing.
/// If the function does not exist, the plugin will be loaded as normal.
/// <param name="isDebugProcess">Indicates if MysticThumbs or the loader process is a debug build. Can be used to prevent loading debug plugins in release builds and vice versa.</param>
/// </summary>
typedef bool (*MTP_PreventLoading)(bool isDebugProcess);

#endif // _MysticThumbsPlugin_h_
