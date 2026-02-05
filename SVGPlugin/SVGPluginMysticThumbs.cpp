// SVGPluginMysticThumbs.cpp

// Avoid the std::max error:
// error C2589: '(': illegal token on right side of '::'
#define NOMINMAX 

#include "SVGPluginMysticThumbs.h"
#include "../Common/MysticThumbsPlugin.h"
#include "../Common/SharedMysticThumbsPlugin.h"
#include "resource.h"

// resvg import library. 
// Specified in Project Properties -> Directories -> Include Directories.
#include <resvg\resvg.h>

#include <Windows.h>
#include <Shlwapi.h>
#include <wincodec.h>
#include <strsafe.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <string>
#include <sstream>
#include <vector>
#include <cmath> 

// ----------------------------------------------------------------------------
// Explicit link libraries
// ----------------------------------------------------------------------------
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// Needed when linking resvg.lib (Rust std dependencies on Windows)
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "ntdll.lib")

// resvg STATIC LINKED import library
// Specified in Project Properties -> Linker -> Additional Library Directories / Dependencies.
#pragma comment(lib, "resvg.lib")

#pragma comment(lib, "Version.lib")

namespace svgthumb {

	// ----------------------------------------------------------------------------
	// Plugin identity / supported extensions
	// ----------------------------------------------------------------------------
	static const wchar_t* s_name = L"Voith's CODE SVG Plugin";

	//static const LPCWSTR s_extensions[] = { L".svx" };
	static const LPCWSTR s_extensions[] = { L".svg", L".svgz" };

	// {D80D880E-7B58-4411-AFCA-A610D04357B8}
	static const GUID s_guid =
	{ 0xd80d880e, 0x7b58, 0x4411, { 0xaf, 0xca, 0xa6, 0x10, 0xd0, 0x43, 0x57, 0xb8 } };

	// Build-time bitness
#if defined(_WIN64)
	constexpr wchar_t* kBitness = L"64-bit";
#else
	constexpr wchar_t* kBitness = L"32-bit";
#endif


	// ----------------------------------------------------------------------------
	// Registry constants (MysticThumbs-managed root key)
	// ----------------------------------------------------------------------------
	// IMPORTANT:
	//   Do NOT hardcode HKCU\Software\... paths.
	//   Always read/write under context->GetPluginRegistryRootKey() (unique per plugin+user).

	// Values under the plugin root
	static const wchar_t* REG_LOG_ENABLED           = L"Log";
	static const wchar_t* REG_LOG_INCLUDE_CRC       = L"LogIncludeCRC";
	static const wchar_t* REG_LOG_FILENAME          = L"LogFileName";
	static const wchar_t* REG_LEAVE_TEMP            = L"LeaveTempFiles";
	static const wchar_t* REG_SWAP_RB               = L"SwapRB";
	static const wchar_t* REG_RETURN_DEBUG_SVG_TN   = L"ReturnDebugSVGThumbnail";
	static const wchar_t* REG_USE_DESIRED_SIZE_HINT = L"useDesiredSizeHint";
	static const wchar_t* REG_MAX_SVG_DIM           = L"maxSvgDim";
	static const wchar_t* REG_MAX_SVG_BYTES         = L"maxSvgBytes";

	// Subkeys under the plugin root
	static const wchar_t* REG_THUMB_SUBKEY          = L"Thumbnailer";
	static const wchar_t* REG_THUMB_ENABLED         = L"ThumbEnable";
	static const wchar_t* REG_THUMB_PATH            = L"Path";
	static const wchar_t* REG_THUMB_PARAMS          = L"Params";

	static const wchar_t* REG_NORM_SUBKEY           = L"Normalize";
	static const wchar_t* REG_NORM_ROOTFILLWHITE    = L"RootFillWhite";
	static const wchar_t* REG_NORM_RGBA_ATTR        = L"RgbaAttributes";
	static const wchar_t* REG_NORM_RGBA_STYLE       = L"RgbaStyle";

	// ----------------------------------------------------------------------------
	// Global config (loaded once per process)
	// ----------------------------------------------------------------------------
	struct SvgPluginConfig
	{
		bool loaded = false;

		LogConfigCommon log;
		bool leaveTempFiles = false;
		bool returnDebugSVGThumbnail = false;
		bool useDesiredSizeHint = false;
		DWORD maxSvgDim = 4096;
		DWORD maxSvgBytes = 256 * 1024 * 1024;

		// External fallback thumbnailer
		bool thumbEnabled = false;
		std::wstring thumbPath;
		std::wstring thumbParams;

		// Normalization toggles
		bool normRootFillWhite = true; 
		bool normRgbaAttributes = false;
		bool normRgbaStyle = false;

		// Pixel format toggle:
		// resvg renders RGBA; Windows often expects BGRA.
		bool swapRB = true;       
	};

	static SvgPluginConfig g_Config;

	static HMODULE g_hModule = nullptr;

	static void LoadConfig(const IMysticThumbsPluginContext* ctx, bool dumpToLog)
	{
		HKEY pluginRootKey = ctx ? ctx->GetPluginRegistryRootKey() : nullptr;

		// Defaults (keep in sync with struct defaults)
		g_Config.log.enabled = false;
		g_Config.log.includeCRC = true;
		g_Config.log.fileName.clear();
		g_Config.leaveTempFiles = false;
		g_Config.swapRB = true;
		g_Config.returnDebugSVGThumbnail = false;
		g_Config.useDesiredSizeHint = false;
		g_Config.maxSvgDim = 4096;
		g_Config.maxSvgBytes = 256 * 1024 * 1024;

		g_Config.thumbPath.clear();
		g_Config.thumbParams.clear();

		g_Config.normRootFillWhite = true;
		g_Config.normRgbaAttributes = false;
		g_Config.normRgbaStyle = false;

		// Read from registry (MysticThumbs-managed root key)

		DWORD d = 0;

		if (RegGetDword(pluginRootKey, L"", REG_LOG_ENABLED, d))
			g_Config.log.enabled = (d != 0);

		if (RegGetDword(pluginRootKey, L"", REG_LOG_INCLUDE_CRC, d))
			g_Config.log.includeCRC = (d != 0);

		std::wstring logName;
		if (RegGetString(pluginRootKey, L"", REG_LOG_FILENAME, logName))
			g_Config.log.fileName = logName;

		if (RegGetDword(pluginRootKey, L"", REG_LEAVE_TEMP, d))
			g_Config.leaveTempFiles = (d != 0);

		if (RegGetDword(pluginRootKey, L"", REG_SWAP_RB, d))
			g_Config.swapRB = (d != 0);

		if (RegGetDword(pluginRootKey, L"", REG_RETURN_DEBUG_SVG_TN, d))
			g_Config.returnDebugSVGThumbnail = (d != 0);

		if (RegGetDword(pluginRootKey, L"", REG_USE_DESIRED_SIZE_HINT, d))
			g_Config.useDesiredSizeHint = (d != 0);

		if (RegGetDword(pluginRootKey, L"", REG_MAX_SVG_DIM, d))
			g_Config.maxSvgDim = d;

		if (RegGetDword(pluginRootKey, L"", REG_MAX_SVG_BYTES, d))
			g_Config.maxSvgBytes = d;

		// External thumbnailer
		// If ThumbEnable is not present, we treat it as disabled unless a Path is configured.
		bool hasEnable = RegGetDword(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_ENABLED, d);
		g_Config.thumbEnabled = hasEnable ? (d != 0) : false; // or default false

		std::wstring tp, tparams;
		if (RegGetString(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_PATH, tp))
			g_Config.thumbPath = tp;
		if (RegGetString(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_PARAMS, tparams))
			g_Config.thumbParams = tparams;

		// Protect external thumbnailer activation
		if (!hasEnable && !g_Config.thumbPath.empty())
			g_Config.thumbEnabled = true;

		// Normalization toggles. Remember, SVGs are text files which can be modified.
		if (RegGetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_ROOTFILLWHITE, d))
			g_Config.normRootFillWhite = (d != 0);
		if (RegGetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_RGBA_ATTR, d))
			g_Config.normRgbaAttributes = (d != 0);
		if (RegGetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_RGBA_STYLE, d))
			g_Config.normRgbaStyle = (d != 0);


		if (dumpToLog && g_Config.log.enabled)
		{
			LogMessage(L"Config:");

			// Host PID and exe name
			DWORD pid = GetCurrentProcessId();
			wchar_t exePath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exePath, _countof(exePath));
			const wchar_t* exeName = wcsrchr(exePath, L'\\');
			exeName = exeName ? exeName + 1 : exePath;

			// This DLL's path and version
			std::wstring dllPath = GetModulePathW(g_hModule);
			std::wstring dllVer = GetModuleFileVersion(g_hModule); // store g_hModule from DllMain

			LogMessage(L"Config:");

			LogMessage(L"  PID=" + std::to_wstring(pid));
			LogMessage(L"  Process=" + std::wstring(exeName));

			LogMessage(L"  DllPath=" +
				(dllPath.empty() ? std::wstring(L"(unknown)") : dllPath));

			LogMessage(L"  DllFileVersion=" +
				(dllVer.empty() ? std::wstring(L"(unknown)") : dllVer));

			LogMessage(L"  DllBitness=" + std::wstring(kBitness));
			LogMessage(L"  ProcessBitness=" + GetProcessBitness());
			LogMessage(L"  LogEnabled=" + std::to_wstring(g_Config.log.enabled ? 1 : 0));
			LogMessage(L"  LogIncludeCRC=" + std::to_wstring(g_Config.log.includeCRC ? 1 : 0));
			LogMessage(L"  LogFileName=" + g_Config.log.fileName);
			LogMessage(L"  LeaveTempFiles=" + std::to_wstring(g_Config.leaveTempFiles ? 1 : 0));
			LogMessage(L"  SwapRB=" + std::to_wstring(g_Config.swapRB ? 1 : 0));
			LogMessage(L"  ReturnDebugSVGThumbnail=" + std::to_wstring(g_Config.returnDebugSVGThumbnail ? 1 : 0));
			LogMessage(L"  UseDesiredSizeHint=" + std::to_wstring(g_Config.useDesiredSizeHint ? 1 : 0));
			LogMessage(L"  MaxSvgDim=" + std::to_wstring(g_Config.maxSvgDim));
			LogMessage(L"  MaxSvgBytes=" + std::to_wstring(g_Config.maxSvgBytes));
			LogMessage(L"  ThumbPath=" + g_Config.thumbPath);
			LogMessage(L"  ThumbParams=" + g_Config.thumbParams);
			LogMessage(L"  NormRootFillWhite=" + std::to_wstring(g_Config.normRootFillWhite ? 1 : 0));
			LogMessage(L"  NormRgbaAttributes=" + std::to_wstring(g_Config.normRgbaAttributes ? 1 : 0));
			LogMessage(L"  NormRgbaStyle=" + std::to_wstring(g_Config.normRgbaStyle ? 1 : 0));
		}
	}

	static void LoadConfigOnce(const IMysticThumbsPluginContext* ctx)
	{
		if (g_Config.loaded)
			return;

		g_Config.loaded = true;
		LoadConfig(ctx, true);
		BindLogConfig(&g_Config.log);
	}

	// ------------------------------------------------------------------------
	// Configure dialog helpers
	// ------------------------------------------------------------------------

	static bool ApplyDialogToRegistry(HWND hDlg, HKEY pluginRootKey, const SvgPluginConfig& oldCfg, SvgPluginConfig& newCfg)
	{
		if (!pluginRootKey)
		{
			MessageBoxW(hDlg,
				L"MysticThumbs did not provide a valid plugin registry root key.\r\n\r\n"
				L"Settings cannot be saved.",
				L"Voith\x27s CODE SVG Plugin", MB_OK | MB_ICONERROR);
			return false;
		}

		newCfg = oldCfg;

		newCfg.log.enabled = GetCheck(hDlg, IDC_CFG_LOG_ENABLED);
		newCfg.log.includeCRC = GetCheck(hDlg, IDC_CFG_LOG_INCLUDE_CRC);
		newCfg.log.fileName = GetText(hDlg, IDC_CFG_LOG_FILENAME);

		newCfg.leaveTempFiles = GetCheck(hDlg, IDC_CFG_LEAVE_TEMP);
		newCfg.swapRB = GetCheck(hDlg, IDC_CFG_SWAP_RB);
		newCfg.returnDebugSVGThumbnail = GetCheck(hDlg, IDC_CFG_RETURN_DEBUG);
		newCfg.useDesiredSizeHint = GetCheck(hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT);

		newCfg.maxSvgDim = GetUInt(hDlg, IDC_CFG_MAX_DIM, oldCfg.maxSvgDim);
		newCfg.maxSvgBytes = GetUInt(hDlg, IDC_CFG_MAX_BYTES, oldCfg.maxSvgBytes);

		newCfg.thumbEnabled = GetCheck(hDlg, IDC_CFG_THUMB_ENABLE);
		newCfg.thumbPath = GetText(hDlg, IDC_CFG_THUMB_PATH);
		newCfg.thumbParams = GetText(hDlg, IDC_CFG_THUMB_PARAMS);

		newCfg.normRootFillWhite = GetCheck(hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE);
		newCfg.normRgbaAttributes = GetCheck(hDlg, IDC_CFG_NORM_RGBA_ATTR);
		newCfg.normRgbaStyle = GetCheck(hDlg, IDC_CFG_NORM_RGBA_STYLE);

		auto different =
			(newCfg.log.enabled != oldCfg.log.enabled) ||
			(newCfg.log.includeCRC != oldCfg.log.includeCRC) ||
			(newCfg.log.fileName != oldCfg.log.fileName) ||
			(newCfg.leaveTempFiles != oldCfg.leaveTempFiles) ||
			(newCfg.swapRB != oldCfg.swapRB) ||
			(newCfg.returnDebugSVGThumbnail != oldCfg.returnDebugSVGThumbnail) ||
			(newCfg.useDesiredSizeHint != oldCfg.useDesiredSizeHint) ||
			(newCfg.maxSvgDim != oldCfg.maxSvgDim) ||
			(newCfg.maxSvgBytes != oldCfg.maxSvgBytes) ||
			(newCfg.thumbEnabled != oldCfg.thumbEnabled) ||
			(newCfg.thumbPath != oldCfg.thumbPath) ||
			(newCfg.thumbParams != oldCfg.thumbParams) ||
			(newCfg.normRootFillWhite != oldCfg.normRootFillWhite) ||
			(newCfg.normRgbaAttributes != oldCfg.normRgbaAttributes) ||
			(newCfg.normRgbaStyle != oldCfg.normRgbaStyle);

		if (!different)
			return false;

		// Write back to registry
		RegSetDword(pluginRootKey, L"", REG_LOG_ENABLED, newCfg.log.enabled ? 1 : 0);
		RegSetDword(pluginRootKey, L"", REG_LOG_INCLUDE_CRC, newCfg.log.includeCRC ? 1 : 0);
		RegSetString(pluginRootKey, L"", REG_LOG_FILENAME, newCfg.log.fileName);

		RegSetDword(pluginRootKey, L"", REG_LEAVE_TEMP, newCfg.leaveTempFiles ? 1 : 0);
		RegSetDword(pluginRootKey, L"", REG_SWAP_RB, newCfg.swapRB ? 1 : 0);
		RegSetDword(pluginRootKey, L"", REG_RETURN_DEBUG_SVG_TN, newCfg.returnDebugSVGThumbnail ? 1 : 0);
		RegSetDword(pluginRootKey, L"", REG_USE_DESIRED_SIZE_HINT, newCfg.useDesiredSizeHint ? 1 : 0);
		RegSetDword(pluginRootKey, L"", REG_MAX_SVG_DIM, newCfg.maxSvgDim);
		RegSetDword(pluginRootKey, L"", REG_MAX_SVG_BYTES, newCfg.maxSvgBytes);

		RegSetDword(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_ENABLED, newCfg.thumbEnabled ? 1 : 0);
		RegSetString(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_PATH, newCfg.thumbPath);
		RegSetString(pluginRootKey, REG_THUMB_SUBKEY, REG_THUMB_PARAMS, newCfg.thumbParams);

		RegSetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_ROOTFILLWHITE, newCfg.normRootFillWhite ? 1 : 0);
		RegSetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_RGBA_ATTR, newCfg.normRgbaAttributes ? 1 : 0);
		RegSetDword(pluginRootKey, REG_NORM_SUBKEY, REG_NORM_RGBA_STYLE, newCfg.normRgbaStyle ? 1 : 0);


		MessageBoxW(hDlg,
			L"SVG plugin settings saved to the registry.\r\n\r\n"
			L"MysticThumbs/Explorer may need to be restarted before all new thumbnails use the updated settings.",
			L"Voith\x27s CODE SVG Plugin", MB_OK | MB_ICONINFORMATION);

		return true;
	}

	static INT_PTR CALLBACK SvgConfigureDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		static SvgPluginConfig s_cfgAtOpen{};

		switch (msg)
		{
		case WM_INITDIALOG:
		{
			// Ensure controls reflect CURRENT registry values
			IMysticThumbsPluginContext* ctx = reinterpret_cast<IMysticThumbsPluginContext*>(lParam);
			HKEY rootKey = ctx ? ctx->GetPluginRegistryRootKey() : nullptr;
			SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)rootKey);
			LoadConfig(ctx, false);
			s_cfgAtOpen = g_Config;

			SetCheck(hDlg, IDC_CFG_LOG_ENABLED, g_Config.log.enabled);
			SetCheck(hDlg, IDC_CFG_LOG_INCLUDE_CRC, g_Config.log.includeCRC);
			SetText(hDlg, IDC_CFG_LOG_FILENAME, g_Config.log.fileName);
			if (g_Config.log.enabled == BST_UNCHECKED) {
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME_LBL), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_BROWSE), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_INCLUDE_CRC), FALSE);
			}

			SetCheck(hDlg, IDC_CFG_LEAVE_TEMP, g_Config.leaveTempFiles);
			SetCheck(hDlg, IDC_CFG_SWAP_RB, g_Config.swapRB);
			SetCheck(hDlg, IDC_CFG_RETURN_DEBUG, g_Config.returnDebugSVGThumbnail);
			SetCheck(hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT, g_Config.useDesiredSizeHint);

			SetUInt(hDlg, IDC_CFG_MAX_DIM, g_Config.maxSvgDim);
			SetUInt(hDlg, IDC_CFG_MAX_BYTES, g_Config.maxSvgBytes);

			CheckDlgButton(hDlg, IDC_CFG_THUMB_ENABLE, g_Config.thumbEnabled ? BST_CHECKED : BST_UNCHECKED);
			SetText(hDlg, IDC_CFG_THUMB_PATH, g_Config.thumbPath);
			SetText(hDlg, IDC_CFG_THUMB_PARAMS, g_Config.thumbParams);
			if (g_Config.thumbEnabled == BST_UNCHECKED) {
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH_LBL), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_BROWSE), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS_LBL), FALSE);
			}

			SetCheck(hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE, g_Config.normRootFillWhite);
			SetCheck(hDlg, IDC_CFG_NORM_RGBA_ATTR, g_Config.normRgbaAttributes);
			SetCheck(hDlg, IDC_CFG_NORM_RGBA_STYLE, g_Config.normRgbaStyle);

			// Tooltips
			HWND hTip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
				WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
				CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
				hDlg, nullptr, g_hModule, nullptr);
			if (hTip)
			{
				SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, 420);

				AddTooltip(hTip, hDlg, IDC_CFG_LOG_ENABLED, L"Enable verbose logging to the log file. Should only be enabled for debugging.");
				AddTooltip(hTip, hDlg, IDC_CFG_LOG_FILENAME, L"Full path to the log file (folder + filename).");
				AddTooltip(hTip, hDlg, IDC_CFG_LOG_BROWSE, L"Pick a folder and a filename for the log file.");
				AddTooltip(hTip, hDlg, IDC_CFG_LOG_INCLUDE_CRC, L"Include CRC/hash info in log entries (useful for debugging).");

				AddTooltip(hTip, hDlg, IDC_CFG_LEAVE_TEMP, L"Keep temporary extracted/converted files. Useful when debugging. Be aware that this can consume significant disk space.");
				AddTooltip(hTip, hDlg, IDC_CFG_SWAP_RB, L"Swap R/B channels (RGBA <-> BGRA) for Windows compatibility.");
				AddTooltip(hTip, hDlg, IDC_CFG_RETURN_DEBUG, L"Return a debug thumbnail when rendering fails. Useful for debugging");
				AddTooltip(hTip, hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT, L"Let MysticThumbs desired size hint influence render scale.");

				AddTooltip(hTip, hDlg, IDC_CFG_MAX_DIM, L"Reject SVGs with width/height larger than this (safety/DoS protection).");
				AddTooltip(hTip, hDlg, IDC_CFG_MAX_BYTES, L"Reject SVGs larger than this many bytes (safety/DoS protection).");

				AddTooltip(hTip, hDlg, IDC_CFG_THUMB_ENABLE, L"Enable external thumbnailer (\"tool that can create thumbnails\") in case internal rendering fails.");
				AddTooltip(hTip, hDlg, IDC_CFG_THUMB_PATH, L"Optional external fallback thumbnailer executable path.");
				AddTooltip(hTip, hDlg, IDC_CFG_THUMB_BROWSE, L"Pick an external thumbnailer executable.");
				AddTooltip(hTip, hDlg, IDC_CFG_THUMB_PARAMS, L"Command-line parameters for the external thumbnailer.");

				AddTooltip(hTip, hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE, L"Normalize: force root element fill to white.");
				AddTooltip(hTip, hDlg, IDC_CFG_NORM_RGBA_ATTR, L"Normalize: add/adjust RGBA attributes.");
				AddTooltip(hTip, hDlg, IDC_CFG_NORM_RGBA_STYLE, L"Normalize: add/adjust RGBA style entries.");
			}

			return TRUE;
		}

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
			case IDC_CFG_LOG_BROWSE:
			{

				HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				std::wstring current = GetText(hDlg, IDC_CFG_LOG_FILENAME);
				std::wstring folder;

				// Try to derive folder from current path
				if (!current.empty())
				{
					wchar_t tmp[2048]{};
					StringCchCopyW(tmp, std::size(tmp), current.c_str());
					PathRemoveFileSpecW(tmp);
					folder = tmp;
				}

				std::wstring picked = PickFolder(hDlg, folder);
				if (!picked.empty())
				{
					// Keep filename if present, otherwise default
					std::wstring fname = L"SVGPlugin.log";
					if (!current.empty())
					{
						wchar_t tmp[2048]{};
						StringCchCopyW(tmp, std::size(tmp), current.c_str());
						fname = PathFindFileNameW(tmp);
					}
					wchar_t combined[2048]{};
					PathCombineW(combined, picked.c_str(), fname.c_str());
					SetText(hDlg, IDC_CFG_LOG_FILENAME, combined);
				}

				if (SUCCEEDED(hr)) CoUninitialize();
				return TRUE;
			}

			case IDC_CFG_LOG_ENABLED:
			{
				bool en = (IsDlgButtonChecked(hDlg, IDC_CFG_LOG_ENABLED) == BST_CHECKED);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME_LBL), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_INCLUDE_CRC), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_BROWSE), en);
				return TRUE;
			}


			case IDC_CFG_THUMB_BROWSE:
			{
				HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				std::wstring current = GetText(hDlg, IDC_CFG_THUMB_PATH);
				std::wstring picked = PickExe(hDlg, current);
				if (!picked.empty())
					SetText(hDlg, IDC_CFG_THUMB_PATH, picked);
				if (SUCCEEDED(hr)) CoUninitialize();
				return TRUE;
			}

			case IDC_CFG_THUMB_ENABLE:
			{
				bool en = (IsDlgButtonChecked(hDlg, IDC_CFG_THUMB_ENABLE) == BST_CHECKED);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH_LBL), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_BROWSE), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS), en);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS_LBL), en);
				return TRUE;
			}

			case IDOK:
			{
				SvgPluginConfig newCfg{};
				bool changed = ApplyDialogToRegistry(hDlg, (HKEY)GetWindowLongPtrW(hDlg, GWLP_USERDATA), s_cfgAtOpen, newCfg);
				if (changed)
				{
					// Refresh in-memory config in this instance immediately
					g_Config = newCfg;
					g_Config.loaded = true;
				}
				EndDialog(hDlg, IDOK);
				return TRUE;
			}

			case IDCANCEL:
				EndDialog(hDlg, IDCANCEL);
				return TRUE;
			}
			break;
		}
		}
		return FALSE;
	}

	// ----------------------------------------------------------------------------
	// SVG normalization helpers
	// ----------------------------------------------------------------------------
	static bool RemoveSvgRootFillAttributeInMemory(std::wstring& contents)
	{
		// Remove fill="white" from the root <svg ...> tag only.
		// This helps with icons that set a global white fill but rely on transparent background.
		std::wstring lower = contents;
		for (auto& ch : lower) ch = towlower(ch);

		size_t svgPos = lower.find(L"<svg");
		if (svgPos == std::wstring::npos)
			return false;

		size_t tagEnd = lower.find(L'>', svgPos);
		if (tagEnd == std::wstring::npos)
			return false;

		std::wstring tag = contents.substr(svgPos, tagEnd - svgPos + 1);
		std::wstring tagLower = lower.substr(svgPos, tagEnd - svgPos + 1);

		// Look for: fill="white" or fill='white'
		size_t fillPos = tagLower.find(L"fill=");
		if (fillPos == std::wstring::npos)
		{
			LogMessage(L"RemoveSvgRootFillAttributeInMemory: no fill= attribute in <svg> tag");
			return false;
		}

		// Only remove if it is fill="white" (case-insensitive).
		// This is intentionally conservative.
		// Parse quote
		size_t qPos = fillPos + 5;
		if (qPos >= tag.size())
			return false;

		wchar_t q = tag[qPos];
		if (q != L'"' && q != L'\'')
			return false;

		size_t valStart = qPos + 1;
		size_t valEnd = tag.find(q, valStart);
		if (valEnd == std::wstring::npos)
			return false;

		std::wstring val = tag.substr(valStart, valEnd - valStart);
		std::wstring vLower = val;
		for (auto& ch : vLower) ch = towlower(ch);

		if (vLower != L"white")
		{
			LogMessage(L"RemoveSvgRootFillAttributeInMemory: fill= exists but is not \"white\"");
			return false;
		}

		// Remove the attribute segment (including surrounding whitespace if possible)
		size_t attrStart = fillPos;
		// Expand left by one if there's a space before
		if (attrStart > 0 && iswspace(tag[attrStart - 1]))
			attrStart--;

		size_t attrEnd = valEnd + 1; // include closing quote

		tag.erase(attrStart, attrEnd - attrStart);

		// Replace in original contents
		contents.replace(svgPos, tagEnd - svgPos + 1, tag);

		LogMessage(L"RemoveSvgRootFillAttributeInMemory: removed fill=\"white\" from root <svg>");
		return true;
	}

	// Optional: rgba normalizers (kept because useful, but disabled by default for resvg)
	static bool NormalizeRgbaFillInMemory(std::wstring& /*contents*/)
	{
		// Keep it as a future tool: implement/enable only when needed.
		// Returning false by default preserves behavior unless you expand it.
		return false;
	}

	static bool NormalizeSvgFileIfConfigured(const std::wstring& path)
	{
		// Read file as UTF-8 wide stream
		FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8");
		if (!f)
			return false;

		std::wstring contents;
		wchar_t buf[1024];
		while (fgetws(buf, _countof(buf), f))
			contents.append(buf);
		fclose(f);

		if (contents.empty())
			return false;

		bool modified = false;

		if (g_Config.normRootFillWhite)
			modified |= RemoveSvgRootFillAttributeInMemory(contents);

		if (g_Config.normRgbaAttributes || g_Config.normRgbaStyle)
			modified |= NormalizeRgbaFillInMemory(contents);

		if (!modified)
			return false;

		f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
		if (!f)
			return false;

		fputws(contents.c_str(), f);
		fclose(f);

		LogMessage(L"NormalizeSvgFileIfConfigured: updated SVG");
		return true;
	}

	// ----------------------------------------------------------------------------
	// Pixel format helpers
	// ----------------------------------------------------------------------------
	static void SwapRedBlueInPlace(unsigned char* buf, unsigned int w, unsigned int h)
	{
		const size_t stride = (size_t)w * 4;
		for (unsigned int y = 0; y < h; ++y)
		{
			unsigned char* row = buf + y * stride;
			for (unsigned int x = 0; x < w; ++x)
			{
				unsigned char* p = row + x * 4;
				// RGBA -> BGRA
				std::swap(p[0], p[2]);
			}
		}
	}

	// ----------------------------------------------------------------------------
	// Primary renderer: resvg
	// ----------------------------------------------------------------------------
	static unsigned char* RenderSvgWithResvgFromFile(
		const std::wstring& svgPath,
		unsigned int desiredSize,        // MysticThumbs hint (used only if useDesiredSize == true)
		bool useDesiredSize,             // if false: ignore desiredSize, return "full sized" (subject to safety cap)
		unsigned int maxDim,             // 0 = no dimension cap; otherwise downscale so max(width,height) <= maxDim
		size_t maxBytes,                 // 0 = no memory cap; otherwise refuse allocations larger than this
		bool& hasAlpha,
		unsigned int& width,
		unsigned int& height)
	{
		hasAlpha = false;
		width = height = 0;

		std::string svgPathUtf8 = WideToUtf8(svgPath);
		if (svgPathUtf8.empty())
		{
			LogMessage(L"RenderSvgWithResvgFromFile: WideToUtf8 failed");
			return nullptr;
		}

		// IMPORTANT: Create resvg_options per call.
		// Explorer/MysticThumbs may call GenerateImage concurrently; resvg_options is mutable and not thread-safe.
		resvg_options* opt = resvg_options_create();
		if (!opt)
		{
			LogMessage(L"RenderSvgWithResvgFromFile: resvg_options_create failed");
			return nullptr;
		}

		// Note: loading system fonts can be slow, but is safe here.
		resvg_options_load_system_fonts(opt);

		// Set resources dir so relative references resolve (images, CSS, etc.)
		std::wstring dirW = GetDirectoryOfPath(svgPath);
		std::string dirUtf8 = WideToUtf8(dirW);
		if (!dirUtf8.empty())
			resvg_options_set_resources_dir(opt, dirUtf8.c_str());

		resvg_render_tree* tree = nullptr;
		int32_t err = resvg_parse_tree_from_file(svgPathUtf8.c_str(), opt, &tree);

		resvg_options_destroy(opt);

		if (err != RESVG_OK || !tree)
		{
			std::wstringstream ss;
			ss << L"RenderSvgWithResvgFromFile: parse failed, err=" << err;
			LogMessage(ss.str());
			return nullptr;
		}

		if (resvg_is_image_empty(tree))
		{
			LogMessage(L"RenderSvgWithResvgFromFile: SVG is empty");
			resvg_tree_destroy(tree);
			return nullptr;
		}

		resvg_size sz = resvg_get_image_size(tree);
		const float svgW = sz.width;
		const float svgH = sz.height;

		if (svgW <= 0.0f || svgH <= 0.0f)
		{
			LogMessage(L"RenderSvgWithResvgFromFile: invalid SVG size");
			resvg_tree_destroy(tree);
			return nullptr;
		}

		// ---- Decide output size + transform ----
		resvg_transform tr = resvg_transform_identity();
		float scale = 1.0f;

		if (useDesiredSize)
		{
			// Thumbnail-mode: square buffer desiredSize x desiredSize and center-fit.
			if (desiredSize == 0)
				desiredSize = 256;

			width = height = desiredSize;

			const float scaleX = (float)desiredSize / svgW;
			const float scaleY = (float)desiredSize / svgH;
			scale = (scaleX < scaleY) ? scaleX : scaleY;

			const float scaledW = svgW * scale;
			const float scaledH = svgH * scale;
			const float offsetX = ((float)desiredSize - scaledW) * 0.5f;
			const float offsetY = ((float)desiredSize - scaledH) * 0.5f;

			tr.a = scale;
			tr.d = scale;
			tr.e = offsetX;
			tr.f = offsetY;
		}
		else
		{
			// Full-size mode: ignore desiredSize completely.
			// Safety cap: if maxDim > 0 and SVG is larger, downscale to fit within maxDim.
			float capScale = 1.0f;

			if (maxDim > 0)
			{
				const float maxSrcDim = (svgW > svgH) ? svgW : svgH;
				if (maxSrcDim > (float)maxDim)
					capScale = (float)maxDim / maxSrcDim; // downscale only
			}

			scale = capScale; // never upscale

			// Use rounded pixel size; clamp to at least 1x1.
			width = (unsigned int)std::max(1, (int)std::lround(svgW * scale));
			height = (unsigned int)std::max(1, (int)std::lround(svgH * scale));

			tr.a = scale;
			tr.d = scale;
			// tr.e/tr.f remain 0 (top-left aligned)
		}

		// ---- Allocate output buffer with overflow + size checks ----
		// Default memory cap if caller passes 0 (tune to taste).
		if (maxBytes == 0)
			maxBytes = 256ull * 1024ull * 1024ull; // 256 MB

		// Compute stride/bufSize carefully
		const size_t w = (size_t)width;
		const size_t h = (size_t)height;

		// Basic sanity
		if (w == 0 || h == 0)
		{
			LogMessage(L"RenderSvgWithResvgFromFile: invalid output size");
			resvg_tree_destroy(tree);
			return nullptr;
		}

		// Overflow-safe stride = w * 4
		if (w > (SIZE_MAX / 4))
		{
			LogMessage(L"RenderSvgWithResvgFromFile: stride overflow");
			resvg_tree_destroy(tree);
			return nullptr;
		}
		const size_t stride = w * 4;

		// Overflow-safe bufSize = stride * h
		if (h > 0 && stride > (SIZE_MAX / h))
		{
			LogMessage(L"RenderSvgWithResvgFromFile: bufSize overflow");
			resvg_tree_destroy(tree);
			return nullptr;
		}
		const size_t bufSize = stride * h;

		if (bufSize > maxBytes)
		{
			std::wstringstream ss;
			ss << L"RenderSvgWithResvgFromFile: refusing allocation bufSize=" << (unsigned long long)bufSize
				<< L" > maxBytes=" << (unsigned long long)maxBytes
				<< L" (out=" << width << L"x" << height << L")";
			LogMessage(ss.str());
			resvg_tree_destroy(tree);
			return nullptr;
		}

		unsigned char* buffer = (unsigned char*)LocalAlloc(LMEM_FIXED, bufSize);
		if (!buffer)
		{
			LogMessage(L"RenderSvgWithResvgFromFile: LocalAlloc failed");
			resvg_tree_destroy(tree);
			return nullptr;
		}

		memset(buffer, 0, bufSize);

		// Render premultiplied RGBA8888
		resvg_render(tree, tr, (uint32_t)width, (uint32_t)height, (char*)buffer);

		resvg_tree_destroy(tree);

		// If MysticThumbs/Windows interprets pixels as BGRA, swap channels
		if (g_Config.swapRB)
			SwapRedBlueInPlace(buffer, width, height);

		hasAlpha = true;

		std::wstringstream ss;
		ss << L"RenderSvgWithResvgFromFile: success, out=" << width << L"x" << height
			<< L", svg=" << (int)svgW << L"x" << (int)svgH
			<< L", useDesiredSize=" << (useDesiredSize ? 1 : 0)
			<< L", maxDim=" << maxDim
			<< L", scale=" << scale
			<< L", bufSize=" << (unsigned long long)bufSize;
		LogMessage(ss.str());

		return buffer;
	}

	// ----------------------------------------------------------------------------
	// Optional: synthetic debug image (useful to prove GenerateImage is called)
	// ----------------------------------------------------------------------------
	static unsigned char* MakeDebugImage(unsigned int desiredSize,
		bool& hasAlpha,
		unsigned int& width,
		unsigned int& height)
	{
		if (desiredSize == 0) desiredSize = 256;
		width = height = desiredSize;
		hasAlpha = true;

		const size_t stride = (size_t)width * 4;
		const size_t size = stride * (size_t)height;

		unsigned char* buf = (unsigned char*)LocalAlloc(LMEM_FIXED, size);
		if (!buf) return nullptr;

		for (unsigned int y = 0; y < height; ++y)
		{
			for (unsigned int x = 0; x < width; ++x)
			{
				unsigned char* p = buf + (y * stride) + (x * 4);
				bool diag = (x == y) || (x + 1 == y) || (x == y + 1);

				if (diag)
				{
					p[0] = 0;   p[1] = 255; p[2] = 0;   p[3] = 255; // green
				}
				else
				{
					p[0] = 255; p[1] = 0;   p[2] = 255; p[3] = 255; // magenta
				}
			}
		}

		return buf;
	}


	class CSVGPluginMysticThumbs : public IMysticThumbsPlugin
	{
	public:
		explicit CSVGPluginMysticThumbs(_In_ IMysticThumbsPluginContext* ctx) : context(ctx)
		{
			// Avoid heavy work here; plugin may be created only for capability checks.
			// Also, do not attempt to call any methods on context until GenerateImage or Ping, as they may not be valid yet.
		}

	private:
		IMysticThumbsPluginContext* context = nullptr; // Valid for lifetime of this plugin instance

		_Notnull_ LPCWSTR GetName() const override
		{
			// VERSION 4: Caller copies strings. Do NOT allocate.
			return s_name;
		}

		unsigned int GetExtensionCount() const override
		{
			return ARRAYSIZE(s_extensions);
		}

		_Notnull_ LPCWSTR GetExtension(_In_ unsigned int index) const override
		{
			if (index >= ARRAYSIZE(s_extensions))
				return L"";
			return s_extensions[index];
		}

		_Notnull_ LPCGUID GetGuid() const override
		{
			return &s_guid;
		}

		bool GetCapabilities(_Out_ MysticThumbsPluginCapabilities& capabilities) override
		{
			capabilities = {};
			capabilities |= PluginCapabilities_CanConfigure;
			// NOTE: We do not set PluginCapabilities_CanNonUniformSize because this plugin currently renders square thumbnails (desiredSize x desiredSize) with aspect-preserving fit.
			return true;
		}


		_Notnull_ LPCWSTR GetDescription() const override
		{
			// Use /n for new lines (per header comment).
			return L"Plugin creates a SVG/SVGZ thumbnail using resvg (with optional external fallback thumbnailer)";
		}

		_Notnull_ LPCWSTR GetAuthor() const override
		{
			return L"Voith's CODE";
		}

		void Destroy() override
		{
			this->~CSVGPluginMysticThumbs();
			CoTaskMemFree(this);
		}

		// --- V4: Ping() uses context->GetStream(), and should return actual image dimensions when possible.
		bool Ping(_Inout_ MysticThumbsPluginPing& ping) override
		{
			LoadConfigOnce(context);

			// Establish per-thread log context (CRC32 based on stream name) so logs remain readable under parallel calls.
			ClearLogContext();

			IStream* pStream = context ? context->GetStream() : nullptr;
			if (pStream) {
				// Best-effort log tagging by stream name (STATSTG::pwcsName).
				TryAdoptStreamNameForLogContext(pStream);

				LogContext& c = GetLogContext();
				if (c.valid) {
					LogMessageF(L"Ping: Stream name=\"%s\", CRC32=%08X", c.name.c_str(), c.crc32);
				}
			}
			else
			{
				LogMessage(L"Ping: context stream is null");
			}

			// Default/fallback behavior (old V3 behavior)
			const unsigned int fallback = 256;
			bool isQuickView = !!(ping.flags & MysticThumbsPluginPingFlags_QuickView);

			unsigned int outW = (isQuickView && ping.requestedWidth) ? ping.requestedWidth : fallback;
			unsigned int outH = (isQuickView && ping.requestedHeight) ? ping.requestedHeight : fallback;

			// Try to determine actual SVG canvas size quickly (no rasterization).
			// If this fails, I keep the fallback dimensions.
			if (pStream) {
				LARGE_INTEGER zero{};
				ULARGE_INTEGER savedPos{};
				// Save current position
				if (SUCCEEDED(pStream->Seek(zero, STREAM_SEEK_CUR, &savedPos))) {
					// Seek to start
					pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

					std::wstring tempDir = GetTempDirectory(L"Voith's CODE\\SVG Plugin MysticThumbs\\");
					std::wstring svgPath = MakeTempSvgFile(tempDir);

					if (!svgPath.empty() && WriteStreamToFile(pStream, svgPath)) {
						// Parse with resvg to obtain intrinsic size
						float svgW = 0.0f, svgH = 0.0f;
						if (TryGetSvgSizeWithResvg(svgPath, svgW, svgH)) {
							// Round to nearest pixel, clamp to at least 1.
							outW = (unsigned int)std::max(1, (int)std::lround(svgW));
							outH = (unsigned int)std::max(1, (int)std::lround(svgH));
						}
						if (!g_Config.leaveTempFiles) {
							DeleteFileW(svgPath.c_str());
						}
					}
					else {
						if (!svgPath.empty() && !g_Config.leaveTempFiles)
							DeleteFileW(svgPath.c_str());
					}

					// Restore position so Generate() sees the stream at the head as expected.
					LARGE_INTEGER restore{};
					restore.QuadPart = (LONGLONG)savedPos.QuadPart;
					pStream->Seek(restore, STREAM_SEEK_SET, nullptr);
				}
			}

			ping.width = outW;
			ping.height = outH;
			ping.bitDepth = 32;

			LogMessageF(L"Ping called, returning %ux%u @ %u bpp (flags=0x%08X)",
				ping.width, ping.height, ping.bitDepth, (unsigned int)ping.flags);

			if (context) {
				context->logf(L"Ping called, returning %ux%u @ %u bpp (flags=0x%08X)",
					ping.width, ping.height, ping.bitDepth, (unsigned int)ping.flags);
			}

			return true;
		}

		bool Configure(_In_ HWND hWndParent) override
		{
			// Always load fresh values from registry right before opening the dialog
			svgthumb::LoadConfig(context, false);

			INT_PTR result = DialogBoxParamW(svgthumb::g_hModule,
				MAKEINTRESOURCE(IDD_SVG_PLUGIN_CONFIGURE),
				hWndParent,
				svgthumb::SvgConfigureDialogProc,
				(LPARAM)context);

			return result == IDOK;
		}

		HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params,
			_COM_Outptr_result_maybenull_ IWICBitmapSource** lplpOutputImage) override
		{
			if (!lplpOutputImage) return E_POINTER;
			*lplpOutputImage = nullptr;

			IStream* pStream = context ? context->GetStream() : nullptr;
			if (!pStream) return E_FAIL;

			// Ensure stream is at head
			LARGE_INTEGER zero{};
			pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

			// Choose a size hint for existing square renderer.
			const unsigned int desiredSize =
				(params.desiredWidth > params.desiredHeight) ? params.desiredWidth : params.desiredHeight;

			bool hasAlpha = false;
			unsigned int w = 0, h = 0;

			LogMessage(L"Generate: Chaining GenerateImage");

			unsigned char* pixels = GenerateImage(
				pStream,
				desiredSize,
				(unsigned int)params.flags,
				hasAlpha,
				w, h);

			if (!pixels || w == 0 || h == 0)
				return E_FAIL;

			// Ensure output ordering is BGRA for GUID_WICPixelFormat32bppPBGRA
			if (!g_Config.swapRB) // buffer is RGBA -> convert to BGRA for WIC PBGRA
				SwapRedBlueInPlace(pixels, w, h);

			const UINT srcStride = w * 4;

			IWICImagingFactory* factory = nullptr;
			HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&factory));
			if (FAILED(hr)) {
				LocalFree(pixels);
				return hr;
			}

			IWICBitmap* bmp = nullptr;
			hr = factory->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapCacheOnLoad, &bmp);
			factory->Release();

			if (FAILED(hr)) {
				LocalFree(pixels);
				return hr;
			}

			WICRect rc{ 0, 0, (INT)w, (INT)h };
			IWICBitmapLock* lock = nullptr;
			hr = bmp->Lock(&rc, WICBitmapLockWrite, &lock);
			if (FAILED(hr)) {
				LocalFree(pixels);
				bmp->Release();
				return hr;
			}

			UINT dstStride = 0, dstSize = 0;
			BYTE* dst = nullptr;
			hr = lock->GetStride(&dstStride);
			if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&dstSize, &dst);

			if (FAILED(hr) || !dst) {
				lock->Release();
				LocalFree(pixels);
				bmp->Release();
				return FAILED(hr) ? hr : E_FAIL;
			}

			const UINT copyStride = (dstStride < srcStride) ? dstStride : srcStride;
			for (unsigned int y = 0; y < h; ++y)
				memcpy(dst + (size_t)y * dstStride, pixels + (size_t)y * srcStride, copyStride);

			lock->Release();
			LocalFree(pixels);

			if (hasAlpha)
				params.flags |= MT_HasAlpha;

			*lplpOutputImage = bmp; // refcount passed to caller
			return S_OK;
		}

		// ---- Helper: create a temp .svg file path (no paired .png needed for Ping)
		static std::wstring MakeTempSvgFile(const std::wstring& tempDir)
		{
			// Reuse the existing temp naming scheme, but only create the SVG file name.
			// MakeTempFilePair creates both svgPath and pngPath; Ping only needs svgPath.
			// We'll generate a unique name ourselves.
			wchar_t fileName[MAX_PATH]{};
			if (!GetTempFileNameW(tempDir.c_str(), L"svg", 0, fileName))
				return L"";
			// Ensure it ends with .svg so resvg parser has a meaningful extension when needed.
			// GetTempFileName creates a real file; rename it.
			std::wstring path = fileName;
			std::wstring svgPath = path + L".svg";
			MoveFileExW(path.c_str(), svgPath.c_str(), MOVEFILE_REPLACE_EXISTING);
			return svgPath;
		}

		static bool TryGetSvgSizeWithResvg(const std::wstring& svgPath, float& outW, float& outH)
		{
			outW = outH = 0.0f;

			std::string svgPathUtf8 = WideToUtf8(svgPath);
			if (svgPathUtf8.empty())
				return false;

			resvg_options* opt = resvg_options_create();
			if (!opt)
				return false;

			resvg_options_load_system_fonts(opt);

			std::wstring dirW = GetDirectoryOfPath(svgPath);
			std::string dirUtf8 = WideToUtf8(dirW);
			if (!dirUtf8.empty())
				resvg_options_set_resources_dir(opt, dirUtf8.c_str());

			resvg_render_tree* tree = nullptr;
			int32_t err = resvg_parse_tree_from_file(svgPathUtf8.c_str(), opt, &tree);

			resvg_options_destroy(opt);

			if (err != RESVG_OK || !tree)
				return false;

			if (resvg_is_image_empty(tree)) {
				resvg_tree_destroy(tree);
				return false;
			}

			resvg_size sz = resvg_get_image_size(tree);
			resvg_tree_destroy(tree);

			outW = sz.width;
			outH = sz.height;
			return (outW > 0.0f && outH > 0.0f);
		}

		// Existing implementation kept as-is (now a private helper, not an interface method).
		unsigned char* GenerateImage(
			IN IStream* pStream,
			IN unsigned int desiredSize,
			IN unsigned int /*flags*/,
			OUT bool& hasAlpha,
			OUT unsigned int& width,
			OUT unsigned int& height)
		{
			LoadConfigOnce(context);

			// Update per-thread call context (size + sequence). If Ping did not run on this thread,
			// we still try to adopt a stream name for CRC tagging.
			SetLogContextCall(desiredSize);
			if (!GetLogContext().valid)
			{
				if (TryAdoptStreamNameForLogContext(pStream))
				{
					std::wstringstream ss;
					ss << L"GenerateImage: adopted stream name=\"" << GetLogContext().name
						<< L"\", CRC32=0x" << std::hex << std::uppercase << GetLogContext().crc32;
					LogMessage(ss.str());
				}
			}

			if (g_Config.returnDebugSVGThumbnail)
			{
				LogMessage(L"GenerateImage: returning synthetic SVG debug image");
				return MakeDebugImage(desiredSize, hasAlpha, width, height);
			}

			// Reset outputs early
			hasAlpha = false;
			width = height = 0;

			std::wstring tempDir = GetTempDirectory(L"Voith's CODE\\SVG Plugin MysticThumbs\\");

			std::wstring svgPath, pngPath;
			if (!MakeTempFilePair(tempDir, L"svg", svgPath, pngPath))
			{
				LogMessage(L"GenerateImage: MakeTempFilePair failed");
				return nullptr;
			}

			{
				std::wstringstream ss;
				ss << L"GenerateImage: Generated files=" << svgPath << L" and .png";
				LogMessage(ss.str());
			}

			// Stage 1: stream -> temp SVG
			if (!WriteStreamToFile(pStream, svgPath))
			{
				LogMessage(L"GenerateImage: WriteStreamToFile failed");
				if (!g_Config.leaveTempFiles)
				{
					DeleteFileW(svgPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return nullptr;
			}

			// Stage 2: normalization (optional)
			NormalizeSvgFileIfConfigured(svgPath);

			// Stage 3: resvg render (primary)
			const bool useDesiredSize = g_Config.useDesiredSizeHint; // Should we respect desiredSize hint?
			const unsigned int maxDim = g_Config.maxSvgDim;          // Max SVG dimension
			const size_t maxBytes = g_Config.maxSvgBytes;            // Max SVG memory consumption, MAX CAP!

			unsigned char* buffer = RenderSvgWithResvgFromFile(
				svgPath,
				desiredSize,
				useDesiredSize,
				maxDim,
				maxBytes,
				hasAlpha,
				width,
				height);

			if (buffer)
			{
				LogMessage(L"GenerateImage: resvg rendering succeeded");
				if (!g_Config.leaveTempFiles)
				{
					DeleteFileW(svgPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return buffer;
			}

			LogMessage(L"GenerateImage: resvg rendering failed, trying external Thumbnailer");

			// Stage 4: external thumbnailer fallback -> PNG
			if (!g_Config.thumbPath.empty() && !g_Config.thumbParams.empty())
			{
				std::wstring args = ExpandThumbParams(g_Config.thumbParams, svgPath, pngPath, desiredSize, tempDir);

				{
					std::wstringstream ss;
					ss << L"Thumbnailer: \"" << g_Config.thumbPath << L"\" " << args;
					LogMessage(ss.str());
				}

				std::wstring captured;
				DWORD ec = 0;
				if (RunExternalThumbnailerCapture(g_Config.thumbPath, args, captured, ec))
				{
					std::wstringstream s2;
					s2 << L"Thumbnailer: exit code " << ec;
					LogMessage(s2.str());

					if (!captured.empty())
						LogMessage(L"Thumbnailer output: " + captured);
				}
				else
				{
					LogMessage(L"Thumbnailer: failed to launch or capture output");
				}
			}
			else
			{
				LogMessage(L"GenerateImage: no Thumbnailer configured (Settings\\Thumbnailer)");
			}

			// Stage 5: load PNG if produced
			if (FileExists(pngPath))
			{
				buffer = LoadPngToRgbaBuffer(pngPath, desiredSize, g_Config.useDesiredSizeHint, hasAlpha, width, height);
				if (buffer)
				{
					LogMessage(L"GenerateImage: success (external thumbnailer)");
					if (!g_Config.leaveTempFiles)
					{
						DeleteFileW(svgPath.c_str());
						DeleteFileW(pngPath.c_str());
					}
					return buffer;
				}

				LogMessage(L"GenerateImage: PNG existed but LoadPngToRgbaBuffer failed");
			}
			else
			{
				LogMessage(L"GenerateImage: external thumbnailer did not produce PNG");
			}

			if (!g_Config.leaveTempFiles)
			{
				DeleteFileW(svgPath.c_str());
				DeleteFileW(pngPath.c_str());
			}

			return nullptr;
		} 
	};

} // namespace svgthumb

// ----------------------------------------------------------------------------
// DLL exports required by MysticThumbs
// ----------------------------------------------------------------------------

// Return the version of the plugin compiled.
extern "C" SVGPLUGIN_API int Version()
{
	return MYSTICTHUMBS_PLUGIN_VERSION;
}

extern "C" SVGPLUGIN_API bool Initialize()
{
	// No context available here. Defer registry loading until CreateInstance/Ping/Generate.
	return true;
}

extern "C" SVGPLUGIN_API IMysticThumbsPlugin* CreateInstance(_In_ IMysticThumbsPluginContext* context)
{
	using namespace svgthumb;
	 
	CSVGPluginMysticThumbs* plugin =
		(CSVGPluginMysticThumbs*)CoTaskMemAlloc(sizeof(CSVGPluginMysticThumbs));

	if (!plugin)
	{
		LogMessage(L"CreateInstance: CoTaskMemAlloc failed");
		return nullptr;
	}

	new (plugin) CSVGPluginMysticThumbs(context);
	return plugin;
}

extern "C" SVGPLUGIN_API bool Shutdown()
{
	return true;
}

// ----------------------------------------------------------------------------
// DllMain (init logging lock)
// ----------------------------------------------------------------------------
SVGPLUGIN_API BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD reason, LPVOID /*reserved*/)
{
	(void)hInstance;

	switch (reason)
	{ 
	case DLL_PROCESS_ATTACH:
		svgthumb::g_hModule = hInstance;
		break;

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}
