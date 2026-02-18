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
	//   Always read/write under m_context->GetPluginRegistryRootKey() (unique per plugin+user).

	// Values under the plugin root
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

	static HMODULE g_hModule = nullptr;

	// ------------------------------------------------------------------------
	// Configure dialog forward decl (implementation is below class definition)
	// ------------------------------------------------------------------------
	static INT_PTR CALLBACK SvgConfigureDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
	
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

	class CSVGPluginMysticThumbs : public IMysticThumbsPlugin
	{
	public:
		// Valid for lifetime through the lifetime of the plugin instance until the end of Destroy
		IMysticThumbsPluginContext* m_context = nullptr; 
		const IMysticThumbsLog* m_log{};

		struct PluginConfig
		{
			IMysticThumbsPluginContext* context = nullptr;

			// Logging
			LogConfigCommon log{};

			// Main config
			bool leaveTempFiles = false;
			bool swapRB = true;
			bool returnDebugSVGThumbnail = false;
			bool useDesiredSizeHint = false;
			DWORD maxSvgDim = 4096;
			DWORD maxSvgBytes = 256u * 1024u * 1024u;

			// External thumbnailer
			bool thumbEnabled = false;
			std::wstring thumbPath;
			std::wstring thumbParams;

			// Normalization
			bool normRootFillWhite = true;
			bool normRgbaAttributes = false;
			bool normRgbaStyle = false;

			PluginConfig() = default;
			explicit PluginConfig(CSVGPluginMysticThumbs* p) { context = p ? p->m_context : nullptr; }

			void Load()
			{
				HKEY root = context ? context->GetPluginRegistryRootKey() : nullptr;
				CSimpleRegistryHelper reg(root);

				// Log
				log.enabled = (reg.GetDword(L"", REG_LOG_ENABLED, 0) != 0);
				log.includeCRC = (reg.GetDword(L"", REG_LOG_INCLUDE_CRC, 1) != 0);
				log.fileName = reg.GetString(L"", REG_LOG_FILENAME, L"");

				// Main toggles
				leaveTempFiles = (reg.GetDword(L"", REG_LEAVE_TEMP, 0) != 0);
				swapRB = (reg.GetDword(L"", REG_SWAP_RB, 1) != 0);
				returnDebugSVGThumbnail = (reg.GetDword(L"", REG_RETURN_DEBUG_SVG_TN, 0) != 0);
				useDesiredSizeHint = (reg.GetDword(L"", REG_USE_DESIRED_SIZE_HINT, 0) != 0);

				// Limits
				maxSvgDim = reg.GetDword(L"", REG_MAX_SVG_DIM, 4096);
				maxSvgBytes = reg.GetDword(L"", REG_MAX_SVG_BYTES, 256u * 1024u * 1024u);

				// External thumbnailer
				thumbPath = reg.GetString(REG_THUMB_SUBKEY, REG_THUMB_PATH, L"");
				thumbParams = reg.GetString(REG_THUMB_SUBKEY, REG_THUMB_PARAMS, L"");

				// Preserve your earlier “tri-state” behavior:
				// - If Enabled exists: respect it.
				// - If missing: enable when thumbPath is set.
				DWORD enabledVal = 0;
				const bool hasEnable = reg.HasDword(REG_THUMB_SUBKEY, REG_THUMB_ENABLED, enabledVal);
				thumbEnabled = hasEnable ? (enabledVal != 0) : (!thumbPath.empty());

				// Normalization
				normRootFillWhite = (reg.GetDword(REG_NORM_SUBKEY, REG_NORM_ROOTFILLWHITE, 1) != 0);
				normRgbaAttributes = (reg.GetDword(REG_NORM_SUBKEY, REG_NORM_RGBA_ATTR, 0) != 0);
				normRgbaStyle = (reg.GetDword(REG_NORM_SUBKEY, REG_NORM_RGBA_STYLE, 0) != 0);
			}

			void Save() const
			{
				HKEY root = context ? context->GetPluginRegistryRootKey() : nullptr;
				CSimpleRegistryHelper reg(root);

				reg.SetDword(L"", REG_LOG_ENABLED, log.enabled ? 1u : 0u);
				reg.SetDword(L"", REG_LOG_INCLUDE_CRC, log.includeCRC ? 1u : 0u);
				reg.SetString(L"", REG_LOG_FILENAME, log.fileName);

				reg.SetDword(L"", REG_LEAVE_TEMP, leaveTempFiles ? 1u : 0u);
				reg.SetDword(L"", REG_SWAP_RB, swapRB ? 1u : 0u);
				reg.SetDword(L"", REG_RETURN_DEBUG_SVG_TN, returnDebugSVGThumbnail ? 1u : 0u);
				reg.SetDword(L"", REG_USE_DESIRED_SIZE_HINT, useDesiredSizeHint ? 1u : 0u);

				reg.SetDword(L"", REG_MAX_SVG_DIM, maxSvgDim);
				reg.SetDword(L"", REG_MAX_SVG_BYTES, maxSvgBytes);

				reg.SetDword(REG_THUMB_SUBKEY, REG_THUMB_ENABLED, thumbEnabled ? 1u : 0u);
				reg.SetString(REG_THUMB_SUBKEY, REG_THUMB_PATH, thumbPath);
				reg.SetString(REG_THUMB_SUBKEY, REG_THUMB_PARAMS, thumbParams);

				reg.SetDword(REG_NORM_SUBKEY, REG_NORM_ROOTFILLWHITE, normRootFillWhite ? 1u : 0u);
				reg.SetDword(REG_NORM_SUBKEY, REG_NORM_RGBA_ATTR, normRgbaAttributes ? 1u : 0u);
				reg.SetDword(REG_NORM_SUBKEY, REG_NORM_RGBA_STYLE, normRgbaStyle ? 1u : 0u);
			}

			void DumpToLog(HMODULE hMod) const
			{
				if (!log.enabled)
					return;

				LogSessionHeader(hMod, &log, kBitness);

				LogMessage(L"  LeaveTempFiles=" + std::to_wstring(leaveTempFiles ? 1 : 0));
				LogMessage(L"  SwapRB=" + std::to_wstring(swapRB ? 1 : 0));
				LogMessage(L"  ReturnDebugSVGThumbnail=" + std::to_wstring(returnDebugSVGThumbnail ? 1 : 0));
				LogMessage(L"  UseDesiredSizeHint=" + std::to_wstring(useDesiredSizeHint ? 1 : 0));
				LogMessage(L"  MaxSvgDim=" + std::to_wstring(maxSvgDim));
				LogMessage(L"  MaxSvgBytes=" + std::to_wstring(maxSvgBytes));
				LogMessage(L"  ThumbEnabled=" + std::to_wstring(thumbEnabled ? 1 : 0));
				LogMessage(L"  ThumbPath=" + thumbPath);
				LogMessage(L"  ThumbParams=" + thumbParams);
				LogMessage(L"  NormRootFillWhite=" + std::to_wstring(normRootFillWhite ? 1 : 0));
				LogMessage(L"  NormRgbaAttributes=" + std::to_wstring(normRgbaAttributes ? 1 : 0));
				LogMessage(L"  NormRgbaStyle=" + std::to_wstring(normRgbaStyle ? 1 : 0));
			}
		} config;

		explicit CSVGPluginMysticThumbs(_In_ IMysticThumbsPluginContext* context)
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
			return L"Voith's CODE\nwww.vcode.no";
		}

		void Destroy() override
		{
			this->~CSVGPluginMysticThumbs();
			CoTaskMemFree(this);
		}

		// Ping() uses m_context->GetStream(), and should return actual image dimensions when possible.
		bool Ping(_Inout_ MysticThumbsPluginPing& ping) override
		{
			config.Load();
			BindLogConfig(&config.log); 
			ClearLogContext();

			static std::once_flag s_dumpOnce;
			std::call_once(s_dumpOnce, [&] { config.DumpToLog(g_hModule); });

			IStream* pStream = m_context ? m_context->GetStream() : nullptr;
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
						if (!config.leaveTempFiles) {
							DeleteFileW(svgPath.c_str());
						}
					}
					else {
						if (!svgPath.empty() && !config.leaveTempFiles)
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

			return true;
		}

		bool Configure(_In_ HWND hWndParent) override
		{
			return (INT_PTR)DialogBoxParamW(
				g_hModule,
				MAKEINTRESOURCEW(IDD_SVG_PLUGIN_CONFIGURE), // your dialog resource ID
				hWndParent,
				&SvgConfigureDialogProc,
				(LPARAM)this) == IDOK;
		}

		HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params,
			_COM_Outptr_result_maybenull_ IWICBitmapSource** lplpOutputImage) override
		{
			config.Load();
			BindLogConfig(&config.log);
			ClearLogContext();
			SetLogContextCall(params.desiredWidth);

			if (!lplpOutputImage) return E_POINTER;
			*lplpOutputImage = nullptr;

			IStream* pStream = m_context ? m_context->GetStream() : nullptr;
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


		// ----------------------------------------------------------------------------
		// Primary renderer: resvg
		// ----------------------------------------------------------------------------
		unsigned char* RenderSvgWithResvgFromFile(
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
			if (config.swapRB)
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

			LogMessage(L"NormalizeSvgFileIfConfigured: updated SVG");
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

			if (config.returnDebugSVGThumbnail)
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
			const unsigned int maxDim = config.maxSvgDim;          // Max SVG dimension
			const size_t maxBytes = config.maxSvgBytes;            // Max SVG memory consumption, MAX CAP!

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
				if (!config.leaveTempFiles)
				{
					DeleteFileW(svgPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return buffer;
			}

			LogMessage(L"GenerateImage: resvg rendering failed, trying external Thumbnailer");

			// Stage 4: external thumbnailer fallback -> PNG
			if (!config.thumbPath.empty() && !config.thumbParams.empty())
			{
				std::wstring args = ExpandThumbParams(config.thumbParams, svgPath, pngPath, desiredSize, tempDir);

				{
					std::wstringstream ss;
					ss << L"Thumbnailer: \"" << config.thumbPath << L"\" " << args;
					LogMessage(ss.str());
				}

				std::wstring captured;
				DWORD ec = 0;
				if (RunExternalThumbnailerCapture(config.thumbPath, args, captured, ec))
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
				buffer = LoadPngToRgbaBuffer(pngPath, desiredSize, config.useDesiredSizeHint, hasAlpha, width, height);
				if (buffer)
				{
					LogMessage(L"GenerateImage: success (external thumbnailer)");
					if (!config.leaveTempFiles)
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

			if (!config.leaveTempFiles)
			{
				DeleteFileW(svgPath.c_str());
				DeleteFileW(pngPath.c_str());
			}

			return nullptr;
		} 
	};

	// ------------------------------------------------------------------------
	// Configure dialog helpers (must be after CSVGPluginMysticThumbs definition)
	// ------------------------------------------------------------------------

	static void ApplyDialogToConfig(HWND hDlg,
		const svgthumb::CSVGPluginMysticThumbs::PluginConfig& oldCfg,
		svgthumb::CSVGPluginMysticThumbs::PluginConfig& newCfg)
	{
		newCfg = oldCfg;

		// Logging
		newCfg.log.enabled = GetCheck(hDlg, IDC_CFG_LOG_ENABLED);
		newCfg.log.includeCRC = GetCheck(hDlg, IDC_CFG_LOG_INCLUDE_CRC);
		newCfg.log.fileName = GetText(hDlg, IDC_CFG_LOG_FILENAME);

		// Main
		newCfg.leaveTempFiles = GetCheck(hDlg, IDC_CFG_LEAVE_TEMP);
		newCfg.swapRB = GetCheck(hDlg, IDC_CFG_SWAP_RB);
		newCfg.returnDebugSVGThumbnail = GetCheck(hDlg, IDC_CFG_RETURN_DEBUG);
		newCfg.useDesiredSizeHint = GetCheck(hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT);

		newCfg.maxSvgDim = GetUInt(hDlg, IDC_CFG_MAX_DIM, oldCfg.maxSvgDim);
		newCfg.maxSvgBytes = GetUInt(hDlg, IDC_CFG_MAX_BYTES, oldCfg.maxSvgBytes);

		// External thumb
		newCfg.thumbEnabled = GetCheck(hDlg, IDC_CFG_THUMB_ENABLE);
		newCfg.thumbPath = GetText(hDlg, IDC_CFG_THUMB_PATH);
		newCfg.thumbParams = GetText(hDlg, IDC_CFG_THUMB_PARAMS);

		// Normalization
		newCfg.normRootFillWhite = GetCheck(hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE);
		newCfg.normRgbaAttributes = GetCheck(hDlg, IDC_CFG_NORM_RGBA_ATTR);
		newCfg.normRgbaStyle = GetCheck(hDlg, IDC_CFG_NORM_RGBA_STYLE);
	}

	static bool ConfigDifferent(const svgthumb::CSVGPluginMysticThumbs::PluginConfig& a,
		const svgthumb::CSVGPluginMysticThumbs::PluginConfig& b)
	{
		return
			a.log.enabled != b.log.enabled ||
			a.log.includeCRC != b.log.includeCRC ||
			a.log.fileName != b.log.fileName ||

			a.leaveTempFiles != b.leaveTempFiles ||
			a.swapRB != b.swapRB ||
			a.returnDebugSVGThumbnail != b.returnDebugSVGThumbnail ||
			a.useDesiredSizeHint != b.useDesiredSizeHint ||
			a.maxSvgDim != b.maxSvgDim ||
			a.maxSvgBytes != b.maxSvgBytes ||

			a.thumbEnabled != b.thumbEnabled ||
			a.thumbPath != b.thumbPath ||
			a.thumbParams != b.thumbParams ||

			a.normRootFillWhite != b.normRootFillWhite ||
			a.normRgbaAttributes != b.normRgbaAttributes ||
			a.normRgbaStyle != b.normRgbaStyle;
	}

	struct SvgCfgDlgState
	{
		svgthumb::CSVGPluginMysticThumbs* plugin = nullptr;
		svgthumb::CSVGPluginMysticThumbs::PluginConfig cfgAtOpen;
	};

	static INT_PTR CALLBACK SvgConfigureDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_INITDIALOG:
		{
			auto* plugin = reinterpret_cast<CSVGPluginMysticThumbs*>(lParam);
			if (!plugin)
				return FALSE;

			auto* st = new SvgCfgDlgState{};
			st->plugin = plugin;

			plugin->config.Load();
			st->cfgAtOpen = plugin->config;

			SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

			const auto& c = st->cfgAtOpen;

			SetCheck(hDlg, IDC_CFG_LOG_ENABLED, c.log.enabled);
			SetCheck(hDlg, IDC_CFG_LOG_INCLUDE_CRC, c.log.includeCRC);
			SetText(hDlg, IDC_CFG_LOG_FILENAME, c.log.fileName);
			if (c.log.enabled == BST_UNCHECKED) {
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_FILENAME_LBL), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_BROWSE), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_LOG_INCLUDE_CRC), FALSE);
			}

			SetCheck(hDlg, IDC_CFG_LEAVE_TEMP, c.leaveTempFiles);
			SetCheck(hDlg, IDC_CFG_SWAP_RB, c.swapRB);
			SetCheck(hDlg, IDC_CFG_RETURN_DEBUG, c.returnDebugSVGThumbnail);
			SetCheck(hDlg, IDC_CFG_USE_DESIRED_SIZE_HINT, c.useDesiredSizeHint);

			SetUInt(hDlg, IDC_CFG_MAX_DIM, c.maxSvgDim);
			SetUInt(hDlg, IDC_CFG_MAX_BYTES, c.maxSvgBytes);

			CheckDlgButton(hDlg, IDC_CFG_THUMB_ENABLE, c.thumbEnabled ? BST_CHECKED : BST_UNCHECKED);
			SetText(hDlg, IDC_CFG_THUMB_PATH, c.thumbPath);
			SetText(hDlg, IDC_CFG_THUMB_PARAMS, c.thumbParams);
			if (c.thumbEnabled == BST_UNCHECKED) {
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PATH_LBL), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_BROWSE), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_CFG_THUMB_PARAMS_LBL), FALSE);
			}

			SetCheck(hDlg, IDC_CFG_NORM_ROOT_FILL_WHITE, c.normRootFillWhite);
			SetCheck(hDlg, IDC_CFG_NORM_RGBA_ATTR, c.normRgbaAttributes);
			SetCheck(hDlg, IDC_CFG_NORM_RGBA_STYLE, c.normRgbaStyle);

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

		case WM_NCDESTROY:
		{
			auto* st = reinterpret_cast<SvgCfgDlgState*>(
				GetWindowLongPtrW(hDlg, GWLP_USERDATA));

			if (st)
			{
				delete st;
				SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
			}

			break;
		}

		case WM_COMMAND:
		{

			CSVGPluginMysticThumbs* plugin = (CSVGPluginMysticThumbs*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

			int wNotifyCode = HIWORD(wParam);
			int wID = LOWORD(wParam);

			switch (wID)
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
				// Any changes? Save!
				auto* st = reinterpret_cast<SvgCfgDlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
				if (!st || !st->plugin)
				{
					EndDialog(hDlg, IDCANCEL);
					return TRUE;
				}

				CSVGPluginMysticThumbs::PluginConfig newCfg = st->cfgAtOpen;
				ApplyDialogToConfig(hDlg, st->cfgAtOpen, newCfg);

				const bool changed = ConfigDifferent(st->cfgAtOpen, newCfg);
				if (changed)
				{
					// Apply to plugin instance and save
					st->plugin->config = newCfg;
					st->plugin->config.Save();

					MessageBoxW(
						hDlg,
						L"SVG plugin settings saved to the registry.\r\n\r\n"
						L"MysticThumbs/Explorer may need to be restarted before all new thumbnails use the updated settings.",
						L"Voith's CODE SVG Plugin",
						MB_OK | MB_ICONINFORMATION);
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
