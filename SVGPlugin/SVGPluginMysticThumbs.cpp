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
#include <cstdarg>
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


	// ----------------------------------------------------------------------------
	// Plugin identity / supported extensions
	// ----------------------------------------------------------------------------
static const wchar_t* s_name = L"Voith's CODE SVG Plugin";

//static const LPCWSTR s_extensions[] = { L".svx" };
static const LPCWSTR s_extensions[] = { L".svg", L".svgz" };

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

// Values under the plugin root
static const wchar_t* REG_LEAVE_TEMP = L"LeaveTempFiles";
static const wchar_t* REG_SWAP_RB = L"SwapRB";
static const wchar_t* REG_RETURN_DEBUG_SVG_TN = L"ReturnDebugSVGThumbnail";
static const wchar_t* REG_USE_DESIRED_SIZE_HINT = L"useDesiredSizeHint";

// Subkeys under the plugin root
static const wchar_t* REG_THUMB_SUBKEY = L"Thumbnailer";
static const wchar_t* REG_THUMB_ENABLED = L"ThumbEnable";
static const wchar_t* REG_THUMB_PATH = L"Path";
static const wchar_t* REG_THUMB_PARAMS = L"Params";

static const wchar_t* REG_NORM_SUBKEY = L"Normalize";
static const wchar_t* REG_NORM_ROOTFILLWHITE = L"RootFillWhite";
static const wchar_t* REG_NORM_RGBA_ATTR = L"RgbaAttributes";
static const wchar_t* REG_NORM_RGBA_STYLE = L"RgbaStyle";

static HMODULE g_hModule = nullptr;

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

class CSVGPlugin : public IMysticThumbsPlugin
{
private:
	MysticLogTag  m_logTag;

	// Valid for lifetime through the lifetime of the plugin instance until the end of Destroy
	IMysticThumbsPluginContext* m_context;
	const IMysticThumbsLog* m_log{};

	struct PluginConfig
	{
		IMysticThumbsPluginContext* context{};

		// Main config
		bool leaveTempFiles = false;
		bool swapRB = true;
		bool returnDebugSVGThumbnail = false;
		bool useDesiredSizeHint = false;

		// External thumbnailer
		bool thumbEnabled = false;
		std::wstring thumbPath;
		std::wstring thumbParams;

		// Normalization
		bool normRootFillWhite = true;
		bool normRgbaAttributes = false;
		bool normRgbaStyle = false;

		PluginConfig() = default;
		explicit PluginConfig(CSVGPlugin* p) { context = p ? p->m_context : nullptr; }

		void Load()
		{
			ATLASSUME(context);

			HKEY hRoot = context ? context->GetPluginRegistryRootKey() : nullptr;
			if (!hRoot)
				return;

			// IMPORTANT: Do not close MysticThumbs' HKEY. Use helper that Detach()'es on destruction.
			CRegKeyHelper<false> root(hRoot);

			DWORD d = 0;

			// Main toggles
			if (root.QueryDWORDValue(REG_LEAVE_TEMP, d) == ERROR_SUCCESS) leaveTempFiles = (d != 0);
			if (root.QueryDWORDValue(REG_SWAP_RB, d) == ERROR_SUCCESS) swapRB = (d != 0);
			if (root.QueryDWORDValue(REG_RETURN_DEBUG_SVG_TN, d) == ERROR_SUCCESS) returnDebugSVGThumbnail = (d != 0);
			if (root.QueryDWORDValue(REG_USE_DESIRED_SIZE_HINT, d) == ERROR_SUCCESS) useDesiredSizeHint = (d != 0);

			// External thumbnailer
			thumbEnabled = false;
			thumbPath.clear();
			thumbParams.clear();
			CRegKeyHelper<> sub;
			if (sub.Open(root, REG_THUMB_SUBKEY, KEY_READ) == ERROR_SUCCESS)
			{
				(void)sub.QueryStringValue(REG_THUMB_PATH, thumbPath);
				(void)sub.QueryStringValue(REG_THUMB_PARAMS, thumbParams);

				DWORD enabledVal = 0;
				const LSTATUS hasEnable = sub.QueryDWORDValue(REG_THUMB_ENABLED, enabledVal);

				// Preserve earlier behavior:
				// - If Enabled exists: respect it.
				// - If missing: enable when thumbPath is set.
				thumbEnabled = (hasEnable == ERROR_SUCCESS) ? (enabledVal != 0) : (!thumbPath.empty());
			}
			else
			{
				thumbEnabled = false;
			}
			 
			// Normalization
			CRegKeyHelper<> norm;
			if (norm.Open(root, REG_NORM_SUBKEY, KEY_READ) == ERROR_SUCCESS)
			{
				if (norm.QueryDWORDValue(REG_NORM_ROOTFILLWHITE, d) == ERROR_SUCCESS) normRootFillWhite = (d != 0);
				if (norm.QueryDWORDValue(REG_NORM_RGBA_ATTR, d) == ERROR_SUCCESS) normRgbaAttributes = (d != 0);
				if (norm.QueryDWORDValue(REG_NORM_RGBA_STYLE, d) == ERROR_SUCCESS) normRgbaStyle = (d != 0);
			}
		}

		void Save(HWND hDlg) const
		{
			HKEY hRoot = context ? context->GetPluginRegistryRootKey() : nullptr;
			if (!hRoot)
				return;

			// IMPORTANT: Do not close MysticThumbs' HKEY. 
			CRegKeyHelper<false> root(hRoot);

			root.SetDWORDValue(REG_LEAVE_TEMP, GetCheck(hDlg, IDC_CFG_LEAVE_TEMP) ? 1u : 0u);
			root.SetDWORDValue(REG_SWAP_RB, GetCheck(hDlg, IDC_CFG_SWAP_RB) ? 1u : 0u);
			root.SetDWORDValue(REG_RETURN_DEBUG_SVG_TN, GetCheck(hDlg, IDC_CFG_RETURN_DEBUG) ? 1u : 0u);
			root.SetDWORDValue(REG_USE_DESIRED_SIZE_HINT, GetCheck(hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT) ? 1u : 0u);

			CRegKeyHelper<> thumb;
			if (thumb.Create(root, REG_THUMB_SUBKEY) == ERROR_SUCCESS)
			{
				thumb.SetDWORDValue(REG_THUMB_ENABLED, GetCheck(hDlg, IDC_CFG_THUMB_ENABLE) ? 1u : 0u);
				std::wstring thumbPath = GetText(hDlg, IDC_CFG_THUMB_PATH);
				thumb.SetStringValue(REG_THUMB_PATH, thumbPath.c_str());
				std::wstring thumbParams = GetText(hDlg, IDC_CFG_THUMB_PARAMS);
				thumb.SetStringValue(REG_THUMB_PARAMS, thumbParams.c_str());
			}

			CRegKeyHelper<> norm;
			if (norm.Create(root, REG_NORM_SUBKEY) == ERROR_SUCCESS)
			{
				norm.SetDWORDValue(REG_NORM_ROOTFILLWHITE, GetCheck(hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE) ? 1u : 0u);
				norm.SetDWORDValue(REG_NORM_RGBA_ATTR, GetCheck(hDlg, IDC_CFG_NORM_RGBA_ATTR) ? 1u : 0u);
				norm.SetDWORDValue(REG_NORM_RGBA_STYLE, GetCheck(hDlg, IDC_CFG_NORM_RGBA_STYLE) ? 1u : 0u);
			}
		}
	} config;

	static INT_PTR CALLBACK ConfigureDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

public:
	explicit CSVGPlugin(_In_ IMysticThumbsPluginContext* context)
		: m_context(context), config(this)
	{
		m_log = context->Log();
	}

	const IMysticThumbsPluginContext* GetContext() const
	{
		return m_context;
	}



private:

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
		capabilities |= PluginCapabilities_CanConfigure | PluginCapabilities_IsProcedural;
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
		return L"Voith's CODE\nwww.vcode.no";
	}

	void Destroy() override
	{
		this->~CSVGPlugin();
		CoTaskMemFree(this);
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



	bool Ping(_Inout_ MysticThumbsPluginPing& ping) override
	{
		config.Load();

		const unsigned int pingSizeHint = (ping.requestedWidth && ping.requestedHeight) ? std::max(1u, std::min(ping.requestedWidth, ping.requestedHeight)) : 0u;
		m_logTag.UpdateFromStream(m_context ? m_context->GetStream() : nullptr, pingSizeHint);

		IStream* pStream = m_context ? m_context->GetStream() : nullptr;
		if (!pStream)
			if (m_log) m_log->logf(L"%sPing: context stream is null", m_logTag.Tag());

		if (m_log) m_log->logf(L"%sPing (SVG): File \"%s\"", m_logTag.Tag(), m_logTag.Name());

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
					if (!config.leaveTempFiles) {
						DeleteFileW(svgPath.c_str());
					}
				}
				else {
					if (!svgPath.empty() && !config.leaveTempFiles)
						DeleteFileW(svgPath.c_str());
				}

				// Restore position so Generate sees the stream at the head as expected.
				LARGE_INTEGER restore{};
				restore.QuadPart = (LONGLONG)savedPos.QuadPart;
				pStream->Seek(restore, STREAM_SEEK_SET, nullptr);
			}
		}

		ping.width = outW;
		ping.height = outH;
		ping.bitDepth = 32;

		if (m_log) m_log->logf(L"%sPing called, returning %ux%u @ %u bpp (flags=0x%08X)", m_logTag.Tag(), ping.width, ping.height, ping.bitDepth, (unsigned int)ping.flags);

		return true;
	}

	bool Configure(_In_ HWND hWndParent) override
	{
		INT_PTR result = DialogBoxParamW(
			g_hModule,
			MAKEINTRESOURCEW(IDD_SVG_PLUGIN_CONFIGURE), // your dialog resource ID
			hWndParent,
			ConfigureDialogProc,
			(LPARAM)this);
		return result == IDOK;
	}


	bool RemoveSvgRootFillAttributeInMemory(std::wstring& contents)
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
			if (m_log) m_log->logf(L"%sRemoveSvgRootFillAttributeInMemory: no fill= attribute in <svg> tag", m_logTag.Tag());
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
			if (m_log) m_log->logf(L"%sRemoveSvgRootFillAttributeInMemory: fill= exists but is not \"white\"", m_logTag.Tag());
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

		if (m_log) m_log->logf(L"%sRemoveSvgRootFillAttributeInMemory: removed fill=\"white\" from root <svg>", m_logTag.Tag());
		return true;
	}


	// ----------------------------------------------------------------------------
	// Primary renderer: resvg
	// ----------------------------------------------------------------------------
	unsigned char* RenderSvgWithResvgFromFile(
		const std::wstring& svgPath,
		unsigned int desiredSize,        // MysticThumbs hint (used only if useDesiredSize == true)
		bool useDesiredSize,             // if false: ignore desiredSize, return "full sized" (subject to safety cap)
		bool& hasAlpha,
		unsigned int& width,
		unsigned int& height)
	{
		hasAlpha = false;
		width = height = 0;

		std::string svgPathUtf8 = WideToUtf8(svgPath);
		if (svgPathUtf8.empty())
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: WideToUtf8 failed", m_logTag.Tag());
			return nullptr;
		}

		// IMPORTANT: Create resvg_options per call.
		// Explorer/MysticThumbs may call GenerateImage concurrently; resvg_options is mutable and not thread-safe.
		resvg_options* opt = resvg_options_create();
		if (!opt)
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: resvg_options_create failed", m_logTag.Tag());
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
			if (m_log) m_log->logf(
				L"%sRenderSvgWithResvgFromFile: parse failed, err=%d",
				m_logTag.Tag(),
				(int)err);
			return nullptr;
		}

		if (resvg_is_image_empty(tree))
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: SVG is empty", m_logTag.Tag());
			resvg_tree_destroy(tree);
			return nullptr;
		}

		resvg_size sz = resvg_get_image_size(tree);
		const float svgW = sz.width;
		const float svgH = sz.height;

		if (svgW <= 0.0f || svgH <= 0.0f)
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: invalid SVG size", m_logTag.Tag());
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

			scale = capScale; // never upscale 

			// Use rounded pixel size; clamp to at least 1x1.
			width = (unsigned int)std::max(1, (int)std::lround(svgW * scale));
			height = (unsigned int)std::max(1, (int)std::lround(svgH * scale));

			tr.a = scale;
			tr.d = scale;
			// tr.e/tr.f remain 0 (top-left aligned)
		}

		// Compute stride/bufSize carefully
		const size_t w = (size_t)width;
		const size_t h = (size_t)height;

		// Basic sanity
		if (w == 0 || h == 0)
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: invalid output size", m_logTag.Tag());
			resvg_tree_destroy(tree);
			return nullptr;
		}

		// Overflow-safe stride = w * 4
		if (w > (SIZE_MAX / 4))
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: stride overflow", m_logTag.Tag());
			resvg_tree_destroy(tree);
			return nullptr;
		}
		const size_t stride = w * 4;

		// Overflow-safe bufSize = stride * h
		if (h > 0 && stride > (SIZE_MAX / h))
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: bufSize overflow", m_logTag.Tag());
			resvg_tree_destroy(tree);
			return nullptr;
		}
		const size_t bufSize = stride * h;

		unsigned char* buffer = (unsigned char*)LocalAlloc(LMEM_FIXED, bufSize);
		if (!buffer)
		{
			if (m_log) m_log->logf(L"%sRenderSvgWithResvgFromFile: LocalAlloc failed", m_logTag.Tag());
			resvg_tree_destroy(tree);
			return nullptr;
		}

		memset(buffer, 0, bufSize);

		// Render premultiplied RGBA8888
		resvg_render(tree, tr, (uint32_t)width, (uint32_t)height, (char*)buffer);

		resvg_tree_destroy(tree);

		// If MysticThumbs/Windows interprets pixels as BGRA, swap channels
		if (config.swapRB)
			SwapRedBlueInPlace(buffer, width, height);

		hasAlpha = true;

		return buffer;
	}


	// Optional: rgba normalizers (kept because useful, but disabled by default for resvg)
	bool NormalizeRgbaFillInMemory(std::wstring& /*contents*/)
	{
		// Keep it as a future tool: implement/enable only when needed.
		// Returning false by default preserves behavior unless you expand it.
		return false;
	}

	bool NormalizeSvgFileIfConfigured(const std::wstring& path)
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

		if (config.normRootFillWhite)
			modified |= RemoveSvgRootFillAttributeInMemory(contents);

		if (config.normRgbaAttributes || config.normRgbaStyle)
			modified |= NormalizeRgbaFillInMemory(contents);

		if (!modified)
			return false;

		f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
		if (!f)
			return false;

		fputws(contents.c_str(), f);
		fclose(f);

		if (m_log) m_log->logf(L"%sNormalizeSvgFileIfConfigured: updated SVG", m_logTag.Tag());
		return true;
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

		if (config.returnDebugSVGThumbnail)
		{
			if (m_log) m_log->logf(L"%sGenerateImage: returning synthetic SVG debug image", m_logTag.Tag());
			return MakeDebugImage(desiredSize, hasAlpha, width, height);
		}

		// Reset outputs early
		hasAlpha = false;
		width = height = 0;

		std::wstring tempDir = GetTempDirectory(L"Voith's CODE\\SVG Plugin MysticThumbs\\");

		std::wstring svgPath, pngPath;
		if (!MakeTempFilePair(tempDir, L"svg", svgPath, pngPath))
		{
			if (m_log) m_log->logf(L"%sGenerateImage: MakeTempFilePair failed", m_logTag.Tag());
			return nullptr;
		}

		if (m_log) m_log->logf(L"%sGenerateImage: Generated files=%s and .png", m_logTag.Tag(), svgPath.c_str());

		// Stage 1: stream -> temp SVG
		if (!WriteStreamToFile(pStream, svgPath))
		{
			if (m_log) m_log->logf(L"%sGenerateImage: WriteStreamToFile failed", m_logTag.Tag());
			if (!config.leaveTempFiles)
			{
				DeleteFileW(svgPath.c_str());
				DeleteFileW(pngPath.c_str());
			}
			return nullptr;
		}

		// Stage 2: normalization (optional)
		NormalizeSvgFileIfConfigured(svgPath);

		// Stage 3: resvg render (primary)
		const bool useDesiredSize = config.useDesiredSizeHint; // Should we respect desiredSize hint?

		unsigned char* buffer = RenderSvgWithResvgFromFile(
			svgPath,
			desiredSize,
			useDesiredSize,
			hasAlpha,
			width,
			height);

		if (buffer)
		{
			if (m_log) m_log->logf(L"%sGenerateImage: resvg rendering succeeded", m_logTag.Tag());
			if (!config.leaveTempFiles)
			{
				DeleteFileW(svgPath.c_str());
				DeleteFileW(pngPath.c_str());
			}
			return buffer;
		}

		if (m_log) m_log->logf(L"%sGenerateImage: resvg rendering failed, trying external Thumbnailer", m_logTag.Tag());

		// Stage 4: external thumbnailer fallback -> PNG
		if (!config.thumbPath.empty() && !config.thumbParams.empty())
		{
			std::wstring args = ExpandThumbParams(config.thumbParams, svgPath, pngPath, desiredSize, tempDir);

			std::wstring captured;
			DWORD ec = 0;
			if (RunExternalThumbnailerCapture(config.thumbPath, args, captured, ec))
			{
				if (!captured.empty())
					if (m_log) m_log->logf(L"%sThumbnailer output: exit code %ld", m_logTag.Tag(), ec);
			}
			else
			{
				if (m_log) m_log->logf(L"%sThumbnailer: failed to launch or capture output", m_logTag.Tag());
			}
		}
		else
		{
			if (m_log) m_log->logf(L"%sGenerateImage: no Thumbnailer configured", m_logTag.Tag());
		}

		// Stage 5: load PNG if produced
		if (FileExists(pngPath))
		{
			buffer = LoadPngToRgbaBuffer(pngPath, desiredSize, config.useDesiredSizeHint, hasAlpha, width, height);
			if (buffer)
			{
				if (m_log) m_log->logf(L"%sGenerateImage: success (external thumbnailer)", m_logTag.Tag());
				if (!config.leaveTempFiles)
				{
					DeleteFileW(svgPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return buffer;
			}

			if (m_log) m_log->logf(L"%sGenerateImage: PNG existed but LoadPngToRgbaBuffer failed", m_logTag.Tag());
		}
		else
		{
			if (m_log) m_log->logf(L"%sGenerateImage: external thumbnailer did not produce PNG", m_logTag.Tag());
		}

		if (!config.leaveTempFiles)
		{
			DeleteFileW(svgPath.c_str());
			DeleteFileW(pngPath.c_str());
		}

		return nullptr;
	}

	HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params,
		_COM_Outptr_result_maybenull_ IWICBitmapSource** lplpOutputImage) override
	{
		if (!lplpOutputImage) return E_POINTER;
		*lplpOutputImage = nullptr;

		IStream* pStream = m_context ? m_context->GetStream() : nullptr;
		if (!pStream) return E_FAIL;

		config.context = m_context;
		config.Load();

		const unsigned int desiredSize = std::max(1u, std::min(params.desiredWidth, params.desiredHeight));

		if (m_log) m_log->logf(L"%sGenerate: start: %ux%u", m_logTag.Tag(), (unsigned)params.desiredWidth, (unsigned)params.desiredHeight);

		// Ensure stream is at head
		LARGE_INTEGER zero{};
		pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

		bool hasAlpha = false;
		unsigned int w = 0, h = 0;

		unsigned char* pixels = GenerateImage(
			pStream,
			desiredSize,
			(unsigned int)params.flags,
			hasAlpha,
			w, h);

		if (!pixels || w == 0 || h == 0)
			return E_FAIL;

		// Ensure output ordering is BGRA for GUID_WICPixelFormat32bppPBGRA
		if (!config.swapRB) // buffer is RGBA -> convert to BGRA for WIC PBGRA
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

};

INT_PTR CALLBACK CSVGPlugin::ConfigureDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
	{
		CSVGPlugin* plugin = (CSVGPlugin*)lParam;
		SetWindowLongPtrW(hwndDlg, GWLP_USERDATA, (LONG_PTR)plugin);
		
		plugin->config.context = plugin->m_context;
		plugin->config.Load();

		const auto& c = plugin->config;

		SetCheck(hwndDlg, IDC_CFG_LEAVE_TEMP, c.leaveTempFiles);
		SetCheck(hwndDlg, IDC_CFG_SWAP_RB, c.swapRB);
		SetCheck(hwndDlg, IDC_CFG_RETURN_DEBUG, c.returnDebugSVGThumbnail);
		SetCheck(hwndDlg, IDC_CFG_USE_DESIRED_SIZE_HINT, c.useDesiredSizeHint);

		CheckDlgButton(hwndDlg, IDC_CFG_THUMB_ENABLE, c.thumbEnabled ? BST_CHECKED : BST_UNCHECKED);
		SetText(hwndDlg, IDC_CFG_THUMB_PATH, c.thumbPath);
		SetText(hwndDlg, IDC_CFG_THUMB_PARAMS, c.thumbParams);
		if (c.thumbEnabled == BST_UNCHECKED) {
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PATH), FALSE);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PATH_LBL), FALSE);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_BROWSE), FALSE);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PARAMS), FALSE);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PARAMS_LBL), FALSE);
		}

		SetCheck(hwndDlg, IDC_CFG_NORM_ROOT_FILL_WHITE, c.normRootFillWhite);
		SetCheck(hwndDlg, IDC_CFG_NORM_RGBA_ATTR, c.normRgbaAttributes);
		SetCheck(hwndDlg, IDC_CFG_NORM_RGBA_STYLE, c.normRgbaStyle);

		// Tooltips
		if(plugin->m_context->TooltipsEnabled()) {
			HWND hTip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
										WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
										CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
										hwndDlg, nullptr, g_hModule, nullptr);
			if(hTip) {
				SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, 420);


				AddTooltip(hTip, hwndDlg, IDC_CFG_LEAVE_TEMP, L"Keep temporary extracted/converted files. Useful when debugging. Be aware that this can consume significant disk space.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_SWAP_RB, L"Swap R/B channels (RGBA <-> BGRA) for Windows compatibility.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_RETURN_DEBUG, L"Return a debug thumbnail when rendering fails. Useful for debugging");
				AddTooltip(hTip, hwndDlg, IDC_CFG_USE_DESIRED_SIZE_HINT, L"Let MysticThumbs desired size hint influence render scale.");

				AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_ENABLE, L"Enable external thumbnailer (\"tool that can create thumbnails\") in case internal rendering fails.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_PATH, L"Optional external fallback thumbnailer executable path.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_BROWSE, L"Pick an external thumbnailer executable.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_PARAMS, L"Command-line parameters for the external thumbnailer.");

				AddTooltip(hTip, hwndDlg, IDC_CFG_NORM_ROOT_FILL_WHITE, L"Normalize: force root element fill to white.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_NORM_RGBA_ATTR, L"Normalize: add/adjust RGBA attributes.");
				AddTooltip(hTip, hwndDlg, IDC_CFG_NORM_RGBA_STYLE, L"Normalize: add/adjust RGBA style entries.");
			}
		}

		return TRUE;
	}

	case WM_COMMAND:
	{
		CSVGPlugin* plugin = (CSVGPlugin*)GetWindowLongPtrW(hwndDlg, GWLP_USERDATA);
		ATLASSERT(plugin);

		int wNotifyCode = HIWORD(wParam);
		int wID = LOWORD(wParam);

		switch (wID)
		{

		case IDC_CFG_THUMB_BROWSE:
		{
			HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			std::wstring current = GetText(hwndDlg, IDC_CFG_THUMB_PATH);
			std::wstring picked = PickExe(hwndDlg, current);
			if (!picked.empty())
				SetText(hwndDlg, IDC_CFG_THUMB_PATH, picked);
			if (SUCCEEDED(hr)) CoUninitialize();
			return TRUE;
		}

		case IDC_CFG_THUMB_ENABLE:
		{
			bool en = (IsDlgButtonChecked(hwndDlg, IDC_CFG_THUMB_ENABLE) == BST_CHECKED);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PATH), en);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PATH_LBL), en);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_BROWSE), en);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PARAMS), en);
			EnableWindow(GetDlgItem(hwndDlg, IDC_CFG_THUMB_PARAMS_LBL), en);
			return TRUE;
		}

		case IDOK:
			if (wNotifyCode == BN_CLICKED) {
				// Save the current configuration to the registry
				plugin->config.Save(hwndDlg);
				EndDialog(hwndDlg, IDOK);
				return TRUE;
			}
			break;

		case IDCANCEL:
			if (wNotifyCode == BN_CLICKED) {
				EndDialog(hwndDlg, IDCANCEL);
				return TRUE;
			}
			break;
		}
		break;
	}
	}
	return FALSE;
}



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
	CSVGPlugin* plugin =
		(CSVGPlugin*)CoTaskMemAlloc(sizeof(CSVGPlugin));

	if (!plugin)
	{
		if (context && context->Log()) context->Log()->log(L"CreateInstance: CoTaskMemAlloc failed");
		return nullptr;
	}

	new (plugin) CSVGPlugin(context);
	return plugin;
}

extern "C" SVGPLUGIN_API bool Shutdown()
{
	return true;
}

extern "C" SVGPLUGIN_API bool PreventLoading([[maybe_unused]] bool isDebugProcess)
{
	// Return true to prevent MysticThumbs from loading this plugin.
	//UNREFERENCED_PARAMETER(isDebugProcess); 
	return false;
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
		g_hModule = hInstance;
		break;

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}
