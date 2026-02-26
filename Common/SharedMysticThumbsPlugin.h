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
#include <atlbase.h>

#include <string>
#include <vector>

#include <sstream>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <type_traits>

// ----------------------------------------------------------------------------
// Explicit link libraries
// ----------------------------------------------------------------------------
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")


// ----------------------------------------------------------------------------
// Simple Registry Helper (header-only)
// Add helper methods to CRegKey without modifying the original class
// Idea and concept from MysticCoder, MysticCoder.net 
// ----------------------------------------------------------------------------
template <bool TCloseOnDestruct = true>
class CRegKeyHelper final : public CRegKey
{
public:
    CRegKeyHelper(HKEY hKey) : CRegKey(hKey) {}
    CRegKeyHelper() = default;
    // Safety
    CRegKeyHelper(const CRegKeyHelper&) = delete;
    CRegKeyHelper(CRegKeyHelper&&) = delete;
    CRegKeyHelper& operator=(const CRegKeyHelper&) = delete;

    ~CRegKeyHelper() {
        if(!TCloseOnDestruct)
            Detach();
    }

    /// <summary>
    /// Query for a wstring value from a REG_SZ, with optional default value.
    /// On success, the output string will contain the value (without the null terminator).
    /// On failure, the output string will be left empty.
    /// </summary>
    /// <param name="pszValueName">Value name to query</param>
    /// <param name="value">Output value if success</param>
    /// <returns>ERROR_SUCCESS if the string existed and was returned or if the value didn't exist and a default string was supplied.</returns>
    LSTATUS QueryStringValue(_In_ LPCWSTR pszValueName, _Out_ std::wstring& value, _In_opt_z_ LPCWSTR pszDefaultValue = nullptr)
    {
        value.clear();
        DWORD nChars{}; // on success nChars will include the null terminator, on failure it will be 0
        LSTATUS lRet = __super::QueryStringValue(pszValueName, nullptr, &nChars);
        if(lRet == ERROR_SUCCESS && nChars > 0) {
            value.resize(size_t(nChars - 1));
            lRet = __super::QueryStringValue(pszValueName, &value[0], &nChars);
            if(lRet == ERROR_SUCCESS) {
                return lRet;
            }
        }
        if(pszDefaultValue) {
            value = pszDefaultValue;
            return ERROR_SUCCESS;
        }
        return lRet;
    }
};


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

	std::wstring sub = base;
	if (!sub.empty() && sub.back() != L'\\')
		sub.push_back(L'\\');

	sub += path;
	EnsureDirectoryExists(sub);

	return sub;
}

static inline std::wstring GetText(HWND hDlg, int id)
{
	std::wstring buf;
	HWND item = ::GetDlgItem(hDlg, id);
	if(!item) return buf;
	buf.resize(::GetWindowTextLengthW(item));
	if(buf.length()) GetWindowTextW(item, &buf[0], static_cast<int>(buf.length() + 1));
	return buf;
}

template<typename T = DWORD>
static inline T GetUInt(HWND hDlg, const int nIDDlgItem, const T defValue)
{
	BOOL ok = FALSE;
	UINT v = GetDlgItemInt(hDlg, nIDDlgItem, &ok, (BOOL)std::is_signed<T>());
	return ok ? static_cast<T>(v) : defValue;
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

class MysticLogTag final
{
private:
	uint32_t     m_seq = 0;
	uint32_t     m_crc = 0;
	std::wstring m_name;
	std::wstring m_tag;

	static bool TryGetStreamName(_In_opt_ IStream* stream, _Out_ std::wstring& outName)
	{
		outName.clear();
		if (!stream) return false;

		STATSTG st{};
		if (FAILED(stream->Stat(&st, STATFLAG_DEFAULT)) || !st.pwcsName)
			return false;

		outName = st.pwcsName;
		CoTaskMemFree(st.pwcsName);
		return !outName.empty();
	}

	void BuildTag(unsigned int desiredSize)
	{
		// Keep format aligned with your current plugin:
		//  - With size: [ID=%08X:%u#%u]
		//  - Without : [CRC=%08X]
		wchar_t buf[96]{};

		if (desiredSize != 0)
			StringCchPrintfW(buf, _countof(buf), L"[ID=%08X:%u#%u] ", m_crc, desiredSize, m_seq);
		else
			StringCchPrintfW(buf, _countof(buf), L"[CRC=%08X] ", m_crc);

		m_tag = buf;
	}

public:
	MysticLogTag() = default;

	void Reset() noexcept
	{
		m_seq = 0;
		m_crc = 0;
		m_name.clear();
		m_tag.clear();
	}

	// Update using IStream name (best-effort).
	// If stream has no name, the tag will become empty and crc/name will reset to 0/empty.
	void UpdateFromStream(_In_opt_ IStream* stream, _In_ unsigned int desiredSize)
	{
		m_tag.clear();

		std::wstring name;
		if (!TryGetStreamName(stream, name))
		{
			m_name.clear();
			m_crc = 0;
			return;
		}

		m_name = std::move(name);
		m_crc = Crc32OfWString(m_name);
		++m_seq;

		BuildTag(desiredSize);
	}

	// Optional: update directly from a known name/path (useful if a plugin has a path already).
	void UpdateFromName(_In_ const std::wstring& name, _In_ unsigned int desiredSize)
	{
		m_tag.clear();

		if (name.empty())
		{
			m_name.clear();
			m_crc = 0;
			return;
		}

		m_name = name;
		m_crc = Crc32OfWString(m_name);
		++m_seq;

		BuildTag(desiredSize);
	}

	const wchar_t* c_str() const noexcept { return m_tag.c_str(); }
	const wchar_t* Tag() const noexcept { return m_tag.c_str(); }
	const wchar_t* Name() const noexcept { return m_name.c_str(); }
	const std::wstring& TagW()  const noexcept { return m_tag; }
	const std::wstring& NameW() const noexcept { return m_name; }
	uint32_t Crc() const noexcept { return m_crc; }
	uint32_t Seq() const noexcept { return m_seq; }

	// Convenience: “do I have a usable tag right now?”
	bool HasTag() const noexcept { return !m_tag.empty(); }
};



