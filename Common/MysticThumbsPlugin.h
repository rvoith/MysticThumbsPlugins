/////////////////////////////////////////////////////////////////////////////
// 
// Header for MysticThumbs plugins
// 
// Copyright 2011 MysticCoder
// http://mysticcoder.net/mysticthumbs
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

struct IMysticThumbsPluginContext : public IMysticThumbsLog
{
    /// <summary>
    /// Get the stream we are reading from.
    /// </summary>
    /// <returns></returns>
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
    /// The returned handle should be closed after immediate use with RegCloseKey() to avoid open handles.
    /// An easier way is to use ATL CRegKey or similar RAII wrapper to manage the handle lifetime in a local context. See the example plugin.
    /// WARNING: This can not be called until after CreateInstance() has been completed. Do not call from a plugin constructor. It will return NULL there.
    /// </summary>
    /// <returns>A registry HKEY root key where your plugin config is stored. NULL if error.</returns>
    virtual _Check_return_ HKEY GetPluginRegistryRootKey() const = 0;
};



// This needs to be filled out with relevant capabilities of the plugin that may be useful to MysticThumbs.
enum MysticThumbsPluginCapabilities : unsigned int
{
    PluginCapabilities_CanConfigure = 0x00000001, // Supports Configure() to allow configuration of the plugin
    PluginCapabilities_CanNonUniformSize = 0x00000010, // Allows resizing to in QuickView without locking aspect ratio. should only be set if the plugin can handle non-square thumbnails for example computation-generated images. For normal images this should not be set.
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

enum MysticThumbsPluginFlags : unsigned int
{
    // NOTE: these have changed since version 2. They are still hints but are now in different orders. Recompile may be needed.
    // Transparency flags, one of the following:
    MT_Transparency_Disable = 0x00000001, // opaque
    MT_Transparency_Canvas = 0x00000002, // Used to be MT_Transparency_Checkerboard2.
    MT_Transparency_Transparent = 0x00000004,
    MT_Transparency_Checkerboard = 0x00000008,
    //MT_Transparency_Mask = 0x0000000f, // all bits

    // NOTE: these have changed since version 2. They are still hints but are now in different orders. Recompile may be needed.
    // Embedded thumbnail flags, one of the following:
    MT_EmbeddedThumb_Never = 0x00000010,
    MT_EmbeddedThumb_Always = 0x00000020,
    MT_EmbeddedThumb_IfLarger = 0x00000040,

    // Scale to fit is requested. This is handled by MysticThumbs, but the hint is here.
    MT_ScaleUp = 0x00000100,


    // Output flags up in the high bits.

    // Hint about alpha. Output flag hint from plugin.
    MT_HasAlpha = 0x00010000,
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

    // New in VERSION 4

    /// <summary>
    /// Get a description of this plugin including what it does and any copyright information.
    /// Use /n for new lines as needed.
    /// </summary>
    /// <returns>The string, can be, but ideally not be, nullptr</returns>
    virtual _Notnull_ LPCWSTR GetDescription() const = 0;

    /// <summary>
    /// Get the author of this plugin.
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
    /// Allows the plugin to be configured if needed by opening a modal dialog box or similar and storing settings in the registry or a file etc.
    /// </summary>
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
typedef bool (*MTP_Initialize)();

// Shutdown the plugin. Clean up everything when the plugin is being unloaded.
typedef bool (*MTP_Shutdown)();

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
