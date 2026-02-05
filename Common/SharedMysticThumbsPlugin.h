#pragma once
// SharedMysticThumbsPlugin.h
// Shared, header-only helpers for MysticThumbs plugins (V4+).
//
// NOTE: Keep this header free of plugin-specific registry paths, resource IDs, or config structs.
// Plugins may wrap these helpers as needed.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincodec.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <stdint.h>
#include <mutex>

#include <string>
#include <vector>

#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <algorithm>


// ----------------------------------------------------------------------------
// Forward decls (so helpers can log without ordering issues)
// ----------------------------------------------------------------------------
struct LogConfigCommon;
static inline void BindLogConfig(LogConfigCommon* cfg);
static inline void LogMessage(const std::wstring& msg);

// ----------------------------------------------------------------------------
// Explicit link libraries
// ----------------------------------------------------------------------------
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

static inline std::mutex& LogMutex()
{
	static std::mutex m;
	return m;
}

static inline void AddTooltip(HWND hTip, HWND hDlg, int ctrlId, const wchar_t* text)
{
	HWND hCtrl = GetDlgItem(hDlg, ctrlId);
	if (!hCtrl)
		return;

	TOOLINFOW ti{};
	ti.cbSize = sizeof(ti);
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
	ti.hwnd = hDlg;
	ti.uId = (UINT_PTR)hCtrl;
	ti.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(hTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
}

static inline uint32_t Crc32(const void* data, size_t len)
{
	static uint32_t table[256];
	static std::once_flag initOnce;

	std::call_once(initOnce, [] {
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int j = 0; j < 8; ++j)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		});

	uint32_t crc = 0xFFFFFFFFu;
	const uint8_t* p = static_cast<const uint8_t*>(data);

	for (size_t i = 0; i < len; ++i)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

	return crc ^ 0xFFFFFFFFu;
}

static inline uint32_t Crc32OfWString(const std::wstring& s)
{
	return Crc32(s.data(), s.size() * sizeof(wchar_t));
}

static inline bool EnsureDirectoryExists(const std::wstring& path)
{
	if (path.empty())
		return false;

	DWORD attr = GetFileAttributesW(path.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
		return true;

	// CreateDirectoryW only creates the leaf. We create parents iteratively.
	std::wstring cur;
	cur.reserve(path.size());

	for (size_t i = 0; i < path.size(); ++i)
	{
		wchar_t c = path[i];
		cur.push_back(c);

		if (c == L'\\' || c == L'/')
		{
			if (cur.size() > 2) // skip "C:\"
				CreateDirectoryW(cur.c_str(), nullptr);
		}
	}

	CreateDirectoryW(path.c_str(), nullptr);

	attr = GetFileAttributesW(path.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static inline std::wstring ExpandThumbParams(std::wstring templ,
	const std::wstring& source,
	const std::wstring& target,
	unsigned int desiredSize,
	const std::wstring& tempDir)
{
	auto repl = [&](const wchar_t* key, const std::wstring& val) {
		size_t pos = 0;
		while ((pos = templ.find(key, pos)) != std::wstring::npos) {
			templ.replace(pos, wcslen(key), val);
			pos += val.size();
		}
		};

	repl(L"$(SourceFile)", source);
	repl(L"$(TargetFile)", target);
	repl(L"$(TempDir)", tempDir);

	wchar_t sz[32];
	StringCchPrintfW(sz, _countof(sz), L"%u", desiredSize);
	repl(L"$(DesiredSize)", sz);

	return templ;
}

static inline bool FileExists(const std::wstring& path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static inline bool GetCheck(HWND hDlg, int id)
{
	return IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
}

static inline std::wstring GetDirectoryOfPath(const std::wstring& fullPath)
{
	size_t slash = fullPath.find_last_of(L"\\/");
	if (slash == std::wstring::npos) return L"";
	return fullPath.substr(0, slash);
}

static inline std::wstring GetModuleFileVersion(HMODULE hMod)
{
	wchar_t path[MAX_PATH]{};
	if (!GetModuleFileNameW(hMod, path, _countof(path)))
		return L"";

	DWORD handle = 0;
	DWORD sz = GetFileVersionInfoSizeW(path, &handle);
	if (sz == 0)
		return L"";

	std::vector<BYTE> buf(sz);
	if (!GetFileVersionInfoW(path, 0, sz, buf.data()))
		return L"";

	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT ffiLen = 0;
	if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) ||
		!ffi || ffiLen < sizeof(VS_FIXEDFILEINFO))
		return L"";

	const WORD major = HIWORD(ffi->dwFileVersionMS);
	const WORD minor = LOWORD(ffi->dwFileVersionMS);
	const WORD build = HIWORD(ffi->dwFileVersionLS);
	const WORD rev = LOWORD(ffi->dwFileVersionLS);

	std::wstringstream ss;
	ss << major << L"." << minor << L"." << build << L"." << rev;
	return ss.str();
}

static inline std::wstring GetModulePathW(HMODULE hMod)
{
	if (!hMod)
		return L"";

	std::wstring buf;
	buf.resize(MAX_PATH);

	for (;;)
	{
		DWORD n = GetModuleFileNameW(
			hMod,
			&buf[0],
			static_cast<DWORD>(buf.size())
		);

		if (n == 0)
			return L"";

		if (n < buf.size() - 1)
		{
			buf.resize(n);
			return buf;
		}

		// buffer too small -> grow  
		buf.resize(buf.size() * 2);
		if (buf.size() > (1u << 20))
			return L"";
	}
}

static inline std::wstring GetProcessBitness()
{
	USHORT processMachine = 0;
	USHORT nativeMachine = 0;

	// Windows 10+
	auto pIsWow64Process2 =
		reinterpret_cast<decltype(&IsWow64Process2)>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

	if (pIsWow64Process2 &&
		pIsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine))
	{
		if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN)
			return L"64-bit";   // native 64-bit
		else
			return L"32-bit (WOW64)";
	}

	// Fallback (older Windows)
	BOOL wow64 = FALSE;
	if (IsWow64Process(GetCurrentProcess(), &wow64))
		return wow64 ? L"32-bit (WOW64)" : L"32-bit";

	return L"(unknown)";
}

static inline std::wstring GetTempDirectory(const std::wstring& path)
{
	wchar_t buffer[MAX_PATH] = { 0 };
	DWORD len = GetTempPathW(MAX_PATH, buffer);

	if (len == 0 || len > MAX_PATH)
	{
		// Fallback: %USERPROFILE%\AppData\Local\Temp
		wchar_t userProfile[MAX_PATH] = { 0 };
		DWORD ul = GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
		if (ul != 0 && ul < MAX_PATH)
			StringCchPrintfW(buffer, MAX_PATH, L"%s\\AppData\\Local\\Temp\\", userProfile);
		else
			StringCchCopyW(buffer, MAX_PATH, L"C:\\Windows\\Temp\\");
	}

	std::wstring base(buffer);

	// Use a subdirectory for easier debugging / cleanup
	std::wstring sub = base;
	if (!sub.empty() && sub.back() != L'\\')
		sub.push_back(L'\\');

	sub += path;
	EnsureDirectoryExists(sub);

	return sub;
}

static inline std::wstring GetText(HWND hDlg, int id)
{
	wchar_t buf[2048]{};
	GetDlgItemTextW(hDlg, id, buf, static_cast<int>(std::size(buf)));
	return std::wstring(buf);
}

static inline DWORD GetUInt(HWND hDlg, int id, DWORD defValue)
{
	BOOL ok = FALSE;
	UINT v = GetDlgItemInt(hDlg, id, &ok, FALSE);
	return ok ? static_cast<DWORD>(v) : defValue;
}

static inline bool MakeTempFilePair(const std::wstring& tempDir, const std::wstring& outFileExt,  std::wstring& outFile, std::wstring& outPng)
{
	wchar_t tmp[MAX_PATH] = { 0 };
	std::wstring prefix = tempDir;
	if (!prefix.empty() && prefix.back() != L'\\')
		prefix.push_back(L'\\');

	// GetTempFileName requires an existing directory.
	EnsureDirectoryExists(prefix);

	// prefix must be directory; we pass it via SetCurrentDirectory? No: GetTempFileName uses path param.
	// Use 3-char prefix for filename.
	if (!GetTempFileNameW(prefix.c_str(), outFileExt.c_str(), 0, tmp))
		return false;

	outFile = tmp;
	outFile += L"." + outFileExt;

	// Use the same base name for PNG to keep pairs obvious.
	outPng = tmp;
	outPng += L".png";

	// Delete the placeholder file GetTempFileName creates (the raw tmp name)
	DeleteFileW(tmp);

	return true;
}



static unsigned char* LoadPngToRgbaBuffer(const std::wstring& pngPath,
	unsigned int desiredSize,
	bool useDesiredSizeHint,
	bool& hasAlpha,
	unsigned int& width,
	unsigned int& height)
{
	hasAlpha = true;
	width = height = 0;

	if (!FileExists(pngPath))
		return nullptr;

	IWICImagingFactory* factory = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory)
		return nullptr;

	IWICBitmapDecoder* decoder = nullptr;
	hr = factory->CreateDecoderFromFilename(pngPath.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr) || !decoder)
	{
		factory->Release();
		return nullptr;
	}

	IWICBitmapFrameDecode* frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr) || !frame)
	{
		decoder->Release();
		factory->Release();
		return nullptr;
	}

	UINT w = 0, h = 0;
	frame->GetSize(&w, &h);

	// Decide target size:
	// - If useDesiredSizeHint=1 and desiredSize!=0: scale to desiredSize x desiredSize
	// - Else: keep original PNG size
	UINT targetW = w;
	UINT targetH = h;

	if (useDesiredSizeHint && desiredSize != 0)
	{
		targetW = desiredSize;
		targetH = desiredSize;
	}

	IWICBitmapSource* source = frame;
	IWICBitmapScaler* scaler = nullptr;

	if (targetW != w || targetH != h)
	{
		hr = factory->CreateBitmapScaler(&scaler);
		if (SUCCEEDED(hr) && scaler)
		{
			hr = scaler->Initialize(frame, targetW, targetH, WICBitmapInterpolationModeFant);
			if (SUCCEEDED(hr))
			{
				source = scaler;
				w = targetW;
				h = targetH;
			}
			else
			{
				// If scaling fails, fall back to original size rather than failing completely
				scaler->Release();
				scaler = nullptr;
				source = frame;
				frame->GetSize(&w, &h);
			}
		}
	}

	IWICFormatConverter* converter = nullptr;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter)
	{
		if (scaler) scaler->Release();
		frame->Release();
		decoder->Release();
		factory->Release();
		return nullptr;
	}

	hr = converter->Initialize(source, GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	if (FAILED(hr))
	{
		converter->Release();
		if (scaler) scaler->Release();
		frame->Release();
		decoder->Release();
		factory->Release();
		return nullptr;
	}

	width = w;
	height = h;

	{
		std::wstringstream ss;
		ss << L"LoadPngToRgbaBuffer: src=\""
			<< pngPath
			<< L"\" out=" << width << L"x" << height
			<< L" desiredSize=" << desiredSize
			<< L" useDesiredSizeHint=" << (useDesiredSizeHint ? 1 : 0);
		LogMessage(ss.str());
	}


	const size_t stride = (size_t)width * 4;
	const size_t bufSize = stride * (size_t)height;

	unsigned char* buffer = (unsigned char*)LocalAlloc(LMEM_FIXED, bufSize);
	if (!buffer)
	{
		converter->Release();
		if (scaler) scaler->Release();
		frame->Release();
		decoder->Release();
		factory->Release();
		return nullptr;
	}

	hr = converter->CopyPixels(nullptr, (UINT)stride, (UINT)bufSize, buffer);

	converter->Release();
	if (scaler) scaler->Release();
	frame->Release();
	decoder->Release();
	factory->Release();

	if (FAILED(hr))
	{
		LocalFree(buffer);
		return nullptr;
	}

	hasAlpha = true;
	return buffer;
}

// ----------------------------------------------------------------------------
// Common logging configuration & helpers
// ----------------------------------------------------------------------------
struct LogConfigCommon
{
	bool enabled = false;
	bool includeCRC = true;
	std::wstring fileName;
};

struct LogContext
{
	bool         valid = false;
	uint32_t     crc32 = 0;
	unsigned int desiredSize = 0; // 0 = unknown/not set
	uint32_t     callSeq = 0;     // per-thread sequence number
	std::wstring name;
};

static inline LogConfigCommon*& BoundLogConfig()
{
	static LogConfigCommon* p = nullptr;
	return p;
}

static inline void BindLogConfig(LogConfigCommon* pCfg)
{
	BoundLogConfig() = pCfg;
}

static inline LogContext& GetLogContext()
{
	static thread_local LogContext ctx;
	return ctx;
}

static inline uint32_t& GetLogSeq()
{
	static thread_local uint32_t seq = 0;
	return seq;
}

static inline void ClearLogContext()
{
	LogContext& c = GetLogContext();
	c.valid = false;
	c.crc32 = 0;
	c.desiredSize = 0;
	c.callSeq = 0;
	c.name.clear();
}

static inline void SetLogContextName(const std::wstring& name)
{
	LogContext& c = GetLogContext();
	c.name = name;
	c.crc32 = Crc32OfWString(name);
	c.valid = true;
}

static inline void SetLogContextCall(unsigned int desiredSize)
{
	LogContext& c = GetLogContext();
	c.desiredSize = desiredSize;
	c.callSeq = ++GetLogSeq();
}

static inline bool TryAdoptStreamNameForLogContext(IStream* pStream)
{
	if (!pStream)
		return false;

	STATSTG st{};
	HRESULT hr = pStream->Stat(&st, STATFLAG_DEFAULT);
	if (FAILED(hr) || !st.pwcsName)
		return false;

	std::wstring name = st.pwcsName;
	CoTaskMemFree(st.pwcsName);

	SetLogContextName(name);

	return true;
}

static inline std::wstring NowTimestamp()
{
	SYSTEMTIME st{};
	GetLocalTime(&st);

	wchar_t buf[64];
	// yyyy-mm-dd hh:mm:ss.mmm
	StringCchPrintfW(buf, _countof(buf), L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

static inline void LogMessage(const std::wstring& msg)
{
	LogConfigCommon* cfg = BoundLogConfig();
	if (!cfg || !cfg->enabled || cfg->fileName.empty())
		return;

	std::lock_guard<std::mutex> lock(LogMutex());

#pragma warning(push)
#pragma warning(disable : 4996)
	FILE* f = _wfopen(cfg->fileName.c_str(), L"a+, ccs=UTF-8");
#pragma warning(pop)
	if (!f)
		return;

	std::wstring clean = msg;
	while (!clean.empty() && (clean.back() == L'\n' || clean.back() == L'\r'))
		clean.pop_back();

	std::wstring line = L"[";
	line += NowTimestamp();
	line += L"] ";

	LogContext& c = GetLogContext();
	if (cfg->includeCRC && c.valid)
	{
		wchar_t tag[80];
		if (c.desiredSize != 0)
			StringCchPrintfW(tag, _countof(tag), L"[ID=%08X:%u#%u] ", c.crc32, c.desiredSize, c.callSeq);
		else
			StringCchPrintfW(tag, _countof(tag), L"[CRC=%08X] ", c.crc32);
		line += tag;
	}

	line += clean;
	line += L"\n";

	fputws(line.c_str(), f);
	fclose(f);
}

void LogMessageF(const wchar_t* fmt, ...)
{
	if (!fmt) return;

	wchar_t buffer[2048];

	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
	va_end(args);

	LogMessage(buffer);
}

void LogMessageUtf8F(const wchar_t* fmt, ...)
{
	if (!fmt) return;

	wchar_t buffer[2048];

	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
	va_end(args);

	LogMessage(buffer);
}


static inline bool NormalizeRgbaFillInMemory(std::wstring& /*contents*/)
{
	// Keep it as a future tool: implement/enable only when needed.
	// Returning false by default preserves behavior unless you expand it.
	return false;
}


static inline std::wstring PickExe(HWND hOwner, const std::wstring& initialPath)
{
	std::wstring result;

	IFileDialog* pfd = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&pfd));
	if (FAILED(hr) || !pfd)
		return result;

	DWORD opts = 0;
	pfd->GetOptions(&opts);
	pfd->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);

	COMDLG_FILTERSPEC spec[] = {
		{ L"Executables (*.exe)", L"*.exe" },
		{ L"All files (*.*)", L"*.*" }
	};
	pfd->SetFileTypes(static_cast<UINT>(std::size(spec)), spec);

	if (!initialPath.empty())
	{
		IShellItem* psi = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr, IID_PPV_ARGS(&psi))) && psi)
		{
			pfd->SetFolder(psi);
			psi->Release();
		}
	}

	if (SUCCEEDED(pfd->Show(hOwner)))
	{
		IShellItem* psi = nullptr;
		if (SUCCEEDED(pfd->GetResult(&psi)) && psi)
		{
			PWSTR psz = nullptr;
			if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
			{
				result = psz;
				CoTaskMemFree(psz);
			}
			psi->Release();
		}
	}

	pfd->Release();
	return result;
}

static inline std::wstring PickFolder(HWND hOwner, const std::wstring& initialFolder)
{
	std::wstring result;

	IFileDialog* pfd = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&pfd));
	if (FAILED(hr) || !pfd)
		return result;

	DWORD opts = 0;
	pfd->GetOptions(&opts);
	pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

	if (!initialFolder.empty())
	{
		IShellItem* psi = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&psi))) && psi)
		{
			pfd->SetFolder(psi);
			psi->Release();
		}
	}

	if (SUCCEEDED(pfd->Show(hOwner)))
	{
		IShellItem* psi = nullptr;
		if (SUCCEEDED(pfd->GetResult(&psi)) && psi)
		{
			PWSTR psz = nullptr;
			if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
			{
				result = psz;
				CoTaskMemFree(psz);
			}
			psi->Release();
		}
	}

	pfd->Release();
	return result;
}

static inline bool RegGetDword(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD& out)
{
	out = 0;

	if (!root || !valueName)
		return false;

	HKEY hKey = nullptr;
	bool closeKey = false;

	// Allow subkey == nullptr or L"" to mean "use root directly".
	if (!subkey || subkey[0] == L'\0')
	{
		hKey = root;
	}
	else
	{
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
			return false;
		closeKey = true;
	}

	DWORD type = 0;
	DWORD cb = sizeof(DWORD);
	DWORD val = 0;
	LONG r = RegQueryValueExW(hKey, valueName, nullptr, &type, (LPBYTE)&val, &cb);

	if (closeKey)
		RegCloseKey(hKey);

	if (r != ERROR_SUCCESS || type != REG_DWORD)
		return false;

	out = val;
	return true;
}

static inline bool RegGetString(HKEY root, const wchar_t* subkey, const wchar_t* valueName, std::wstring& out)
{
	out.clear();

	if (!root || !valueName)
		return false;

	HKEY hKey = nullptr;
	bool closeKey = false;

	// Allow subkey == nullptr or L"" to mean "use root directly".
	if (!subkey || subkey[0] == L'\0')
	{
		hKey = root;
	}
	else
	{
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
			return false;
		closeKey = true;
	}

	DWORD type = 0;
	DWORD cb = 0;
	LONG r = RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &cb);
	if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb == 0)
	{
		if (closeKey)
			RegCloseKey(hKey);
		return false;
	}

	std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, 0);
	r = RegQueryValueExW(hKey, valueName, nullptr, nullptr, (LPBYTE)buf.data(), &cb);

	if (closeKey)
		RegCloseKey(hKey);

	if (r != ERROR_SUCCESS)
		return false;

	out.assign(buf.data());
	return true;
}

static inline bool RegSetDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value)
{
	if (!root || !valueName)
		return false;

	HKEY hKey = nullptr;
	bool closeKey = false;

	// Allow subKey == nullptr or L"" to mean "use root directly".
	if (!subKey || subKey[0] == L'\0')
	{
		hKey = root;
	}
	else
	{
		LONG r = RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
		if (r != ERROR_SUCCESS)
			return false;
		closeKey = true;
	}

	LONG r = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&value), sizeof(value));

	if (closeKey)
		RegCloseKey(hKey);
	 
	return r == ERROR_SUCCESS;
}

static inline bool RegSetString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const std::wstring& value)
{
	if (!root || !valueName)
		return false;

	HKEY hKey = nullptr;
	bool closeKey = false;

	// Allow subKey == nullptr or L"" to mean "use root directly".
	if (!subKey || subKey[0] == L'\0')
	{
		hKey = root;
	}
	else
	{
		LONG r = RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
		if (r != ERROR_SUCCESS)
			return false;
		closeKey = true;
	}

	const DWORD cb = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
	LONG r = RegSetValueExW(hKey, valueName, 0, REG_SZ,
		reinterpret_cast<const BYTE*>(value.c_str()), cb);

	if (closeKey)
		RegCloseKey(hKey);

	return r == ERROR_SUCCESS;
}

static inline bool RunExternalThumbnailerCapture(const std::wstring& exePath,
	const std::wstring& args,
	std::wstring& outText,
	DWORD& exitCode)
{
	outText.clear();
	exitCode = (DWORD)-1;

	if (exePath.empty() || !FileExists(exePath))
		return false;

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE hRead = NULL, hWrite = NULL;
	if (!CreatePipe(&hRead, &hWrite, &sa, 0))
		return false;

	// Don't inherit the read end
	SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = hWrite;
	si.hStdError = hWrite;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};

	std::wstring cmd = L"\"";
	cmd += exePath;
	cmd += L"\" ";
	cmd += args;

	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');

	BOOL ok = CreateProcessW(
		NULL,
		cmdBuf.data(),
		NULL, NULL,
		TRUE,
		CREATE_NO_WINDOW,
		NULL, NULL,
		&si, &pi);

	CloseHandle(hWrite); // allow EOF on read pipe

	if (!ok)
	{
		CloseHandle(hRead);
		return false;
	}

	std::string bytes;
	char buf[4096];
	DWORD read = 0;
	while (ReadFile(hRead, buf, sizeof(buf), &read, NULL) && read > 0)
		bytes.append(buf, buf + read);

	CloseHandle(hRead);

	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &exitCode);

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	if (!bytes.empty())
	{
		UINT cp = CP_UTF8;
		int wlen = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), NULL, 0);
		if (wlen <= 0)
		{
			cp = CP_ACP;
			wlen = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), NULL, 0);
		}

		if (wlen > 0)
		{
			outText.resize(wlen);
			MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &outText[0], wlen);
		}
	}

	return true;
}

static inline void SetCheck(HWND hDlg, int id, bool v)
{
	CheckDlgButton(hDlg, id, v ? BST_CHECKED : BST_UNCHECKED);
}

static inline void SetText(HWND hDlg, int id, const std::wstring& s)
{
	SetDlgItemTextW(hDlg, id, s.c_str());
}

static inline void SetUInt(HWND hDlg, int id, DWORD v)
{
	SetDlgItemInt(hDlg, id, v, FALSE);
}

static inline std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};

	int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
		nullptr, 0, nullptr, nullptr);
	if (needed <= 0) return {};

	std::string s;
	s.resize(needed);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
		&s[0], needed, nullptr, nullptr);
	return s;
}

static inline bool WriteStreamToFile(IStream* pStream, const std::wstring& path)
{
	if (!pStream)
		return false;

	LARGE_INTEGER zero{};
	pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	const ULONG kBufSize = 64 * 1024;
	std::vector<BYTE> buf(kBufSize);

	bool ok = true;
	while (true)
	{
		ULONG read = 0;
		HRESULT hr = pStream->Read(buf.data(), (ULONG)buf.size(), &read);
		if (FAILED(hr))
		{
			ok = false;
			break;
		}
		if (read == 0)
			break;

		DWORD written = 0;
		if (!WriteFile(h, buf.data(), read, &written, nullptr) || written != read)
		{
			ok = false;
			break;
		}
	}

	CloseHandle(h);
	return ok;
}

