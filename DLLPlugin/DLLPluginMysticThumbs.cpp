// DLLPluginMysticThumbs.cpp

// Avoid trouble with std::min and std::max
#define NOMINMAX

#include "DLLPluginMysticThumbs.h"
#include "..\\Common\\MysticThumbsPlugin.h"
#include "..\\Common\\SharedMysticThumbsPlugin.h"
#include "resource.h"

#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <atlbase.h>
#include <vector>
#include <string>
#include <sstream>
#include <map>

#include <winver.h>
#pragma comment(lib, "Version.lib")

#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")



// -----------------------------------------------------------------------------
// Registry settings (under MysticThumbs per-plugin registry root)
// -----------------------------------------------------------------------------
static const wchar_t* REG_SETTINGS_KEY = L"";

static const wchar_t* REG_TEMPLATE      = L"Template";
static const wchar_t* REG_PLATE_OPACITY = L"PlateOpacity";   // DWORD 0..100
static const wchar_t* REG_PLATE_OPAQUE  = L"PlateOpaque";    // DWORD 0/1
static const wchar_t* REG_LABEL_SCALE   = L"LabelScalePct";  // DWORD e.g. 75 means 75%

// Defaults
static const wchar_t* DEFAULT_TEMPLATE =
   L"<center>$(DllFileType)</center>\r\n"
   L"<center><strong>$(DLLFileVersionAsText)</strong></center>\r\n"
   L"<tiny>$(VI_FileDescriptionFirstSentence)</tiny>\r\n";

// -----------------------------------------------------------------------------
// Direct2D / DirectWrite factories (process-wide)
// -----------------------------------------------------------------------------
static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;

static HMODULE g_hModule = nullptr;

// Plugin metadata
static const wchar_t* s_name = L"Voith's CODE DLL Plugin";
static const wchar_t* s_description = L"Plugin creates thumbnails for DLLs showing version info, 32- or 64-bit badge";
static const wchar_t* s_author = L"Voith's CODE\nwww.vcode.no";

// NOTE: Replace with your own GUID if you want uniqueness.
static const GUID s_guid =
{ 0x4f0b2d7a, 0xa3f3, 0x4c3a, { 0x8e, 0x7e, 0x1a, 0x92, 0x8e, 0x19, 0x7b, 0x31 } };

// Build-time bitness
#if defined(_WIN64)
static constexpr const wchar_t* kBitness = L"64-bit";
#else
static constexpr const wchar_t* kBitness = L"32-bit";
#endif

static const wchar_t* s_extensions[] = { L".dll", L".mtp", L".ocx" };

// -----------------------------------------------------------------------------
// Small utilities
// -----------------------------------------------------------------------------
static float Clamp(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

// Get file path from IStream via IPersistFile (MysticCoder recommended method)
static std::wstring GetPathFromStream(IStream* pStream)
{
    if (!pStream) return L"";
    CComQIPtr<IPersistFile> pf(pStream);
    if (!pf) return L"";
    CComHeapPtr<OLECHAR> psz;
    if (FAILED(pf->GetCurFile(&psz)) || !psz) return L"";
    return std::wstring(psz);
}

static std::wstring GetDllFileTypeFromPath(const std::wstring& path)
{
	// Returns "DLL", "OCX", "MTP" etc (uppercase, no dot)
	if (path.empty()) return L"";

	size_t dot = path.find_last_of(L'.');
	if (dot == std::wstring::npos || dot + 1 >= path.size()) return L"";

	std::wstring ext = path.substr(dot + 1);
	for (auto& ch : ext) ch = (wchar_t)towupper(ch);
	return ext;
}

enum class SigKind
{
	None = 0,
	Embedded,
	Catalog
};

static bool HasEmbeddedSignatureBlob(const std::wstring& path)
{
	if (path.empty()) return false;

	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	HANDLE map = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!map) { CloseHandle(h); return false; }

	BYTE* base = (BYTE*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
	if (!base) { CloseHandle(map); CloseHandle(h); return false; }

	bool has = false;
	
	__try
	{
		auto* dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			__leave;

		BYTE* ntp = base + dos->e_lfanew;
		DWORD sig = *(DWORD*)ntp;
		if (sig != IMAGE_NT_SIGNATURE)
			__leave;

		// FILE_HEADER sits right after signature
		auto* fh = (IMAGE_FILE_HEADER*)(ntp + sizeof(DWORD));

		// Optional header begins after FILE_HEADER
		BYTE* opt = (BYTE*)fh + sizeof(IMAGE_FILE_HEADER);

		// Read OptionalHeader.Magic to decide PE32 vs PE32+
		WORD magic = *(WORD*)opt;

		// DataDirectory offset differs between PE32 and PE32+
		size_t ddOff = 0;
		if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)       // 0x10B
			ddOff = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
		else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)  // 0x20B
			ddOff = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
		else
			__leave;

		// Security directory is entry index 4
		const size_t secOff = ddOff + IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY);

		IMAGE_DATA_DIRECTORY sec{};
		memcpy(&sec, opt + secOff, sizeof(sec));

		// Note: for SECURITY directory, "VirtualAddress" is a FILE OFFSET, not RVA.
		has = (sec.VirtualAddress != 0 && sec.Size != 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		has = false;
	}

	UnmapViewOfFile(base);
	CloseHandle(map);
	CloseHandle(h);
	return has;
}

static bool HasCatalogSignature(const std::wstring& path)
{
	HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	HCATADMIN hCatAdmin = nullptr;
	if (!CryptCATAdminAcquireContext(&hCatAdmin, nullptr, 0))
	{
		CloseHandle(hFile);
		return false;
	}

	DWORD hashLen = 0;
	CryptCATAdminCalcHashFromFileHandle(hFile, &hashLen, nullptr, 0);
	if (!hashLen)
	{
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	std::vector<BYTE> hash(hashLen);
	if (!CryptCATAdminCalcHashFromFileHandle(hFile, &hashLen, hash.data(), 0))
	{
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	HCATINFO hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, hash.data(), hashLen, 0, nullptr);

	CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
	CryptCATAdminReleaseContext(hCatAdmin, 0);
	CloseHandle(hFile);

	return hCatInfo != nullptr;
}

static SigKind DetectSigKind(const std::wstring& path)
{
	// Prefer embedded if both exist (rare, but can happen)
	if (HasEmbeddedSignatureBlob(path))
		return SigKind::Embedded;

	if (HasCatalogSignature(path))
		return SigKind::Catalog;

	return SigKind::None;
}

static bool IsTrustedEmbeddedSignature(const std::wstring& path)
{
	WINTRUST_FILE_INFO fi{};
	fi.cbStruct = sizeof(fi);
	fi.pcwszFilePath = path.c_str();

	GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;

	WINTRUST_DATA wtd{};
	wtd.cbStruct = sizeof(wtd);
	wtd.dwUIChoice = WTD_UI_NONE;
	wtd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wtd.dwUnionChoice = WTD_CHOICE_FILE;
	wtd.pFile = &fi;

	// Avoid network delays for thumbnails
	wtd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

	// Do a proper verify/close cycle (more reliable than IGNORE in edge cases)
	wtd.dwStateAction = WTD_STATEACTION_VERIFY;
	LONG st = WinVerifyTrust(nullptr, &policy, &wtd);
	wtd.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(nullptr, &policy, &wtd);

	return st == ERROR_SUCCESS;
}

static bool IsTrustedCatalogSignature(const std::wstring& path)
{
	// Open file
	HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	// Acquire catalog admin context (generic)
	HCATADMIN hCatAdmin = nullptr;
	if (!CryptCATAdminAcquireContext(&hCatAdmin, nullptr, 0))
	{
		CloseHandle(hFile);
		return false;
	}

	// Compute hash used for catalog lookup
	DWORD hashLen = 0;
	CryptCATAdminCalcHashFromFileHandle(hFile, &hashLen, nullptr, 0);
	if (hashLen == 0)
	{
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	std::vector<BYTE> hash(hashLen);
	if (!CryptCATAdminCalcHashFromFileHandle(hFile, &hashLen, hash.data(), 0))
	{
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	// Find a catalog containing this hash
	HCATINFO hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, hash.data(), hashLen, 0, nullptr);
	if (!hCatInfo)
	{
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	// Get catalog file path
	CATALOG_INFO ci{};
	ci.cbStruct = sizeof(ci);
	if (!CryptCATCatalogInfoFromContext(hCatInfo, &ci, 0))
	{
		CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
		CryptCATAdminReleaseContext(hCatAdmin, 0);
		CloseHandle(hFile);
		return false;
	}

	// Verify the catalog itself (signed), and that it contains our member hash
	WINTRUST_CATALOG_INFO cat{};
	cat.cbStruct = sizeof(cat);
	cat.pcwszCatalogFilePath = ci.wszCatalogFile;
	cat.pbCalculatedFileHash = hash.data();
	cat.cbCalculatedFileHash = hashLen;
	cat.pcwszMemberTag = nullptr;
	cat.pcwszMemberFilePath = path.c_str();

	GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;

	WINTRUST_DATA wtd{};
	wtd.cbStruct = sizeof(wtd);
	wtd.dwUIChoice = WTD_UI_NONE;
	wtd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wtd.dwUnionChoice = WTD_CHOICE_CATALOG;
	wtd.pCatalog = &cat;
	wtd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

	wtd.dwStateAction = WTD_STATEACTION_VERIFY;
	LONG st = WinVerifyTrust(nullptr, &policy, &wtd);
	wtd.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(nullptr, &policy, &wtd);

	CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
	CryptCATAdminReleaseContext(hCatAdmin, 0);
	CloseHandle(hFile);

	return st == ERROR_SUCCESS;
}

static bool IsFileTrustedSigned(const std::wstring& path)
{
	if (IsTrustedEmbeddedSignature(path))
		return true;

	// Important: catalog-only files (like many in System32) land here
	return IsTrustedCatalogSignature(path);
}


static bool IsFileAuthenticodeSigned(const std::wstring& path)
{
	if (path.empty()) return false;

	WINTRUST_FILE_INFO fileInfo{};
	fileInfo.cbStruct = sizeof(fileInfo);
	fileInfo.pcwszFilePath = path.c_str(); 

	GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;

	WINTRUST_DATA wtd{};
	wtd.cbStruct = sizeof(wtd);
	wtd.dwUIChoice = WTD_UI_NONE;
	wtd.fdwRevocationChecks = WTD_REVOKE_NONE;  // fast/offline
	wtd.dwUnionChoice = WTD_CHOICE_FILE;
	wtd.pFile = &fileInfo;

	// Avoid network stalls during thumbnailing
	wtd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

	// More reliable than IGNORE for catalog scenarios
	wtd.dwStateAction = WTD_STATEACTION_VERIFY;
	LONG status = WinVerifyTrust(nullptr, &policyGUID, &wtd);

	wtd.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(nullptr, &policyGUID, &wtd);

	return status == ERROR_SUCCESS;
}




// -----------------------------------------------------------------------------
// PE bitness from file (target DLL/exe), without loading it
// -----------------------------------------------------------------------------
static std::wstring GetPEBitnessFromFile(const std::wstring& path)
{
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return L"Unknown";

    BYTE buffer[4096]{};
    DWORD bytesRead = 0;

    if (!ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) ||
        bytesRead < sizeof(IMAGE_DOS_HEADER))
    {
        CloseHandle(hFile);
        return L"Unknown";
    }

    CloseHandle(hFile);

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return L"Unknown";

    LONG peOffset = dos->e_lfanew;
    if (peOffset <= 0 || peOffset > (LONG)(bytesRead - sizeof(IMAGE_NT_HEADERS)))
        return L"Unknown";

    const DWORD peSignature = *reinterpret_cast<const DWORD*>(buffer + peOffset);
    if (peSignature != IMAGE_NT_SIGNATURE)
        return L"Unknown";

    const IMAGE_FILE_HEADER* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(
        buffer + peOffset + sizeof(DWORD));

    switch (fileHeader->Machine)
    {
    case IMAGE_FILE_MACHINE_I386:  return L"32-bit";
    case IMAGE_FILE_MACHINE_AMD64: return L"64-bit";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case IMAGE_FILE_MACHINE_ARM:   return L"ARM";
    default:                       return L"Unknown";
    }
}

// -----------------------------------------------------------------------------
// Version resource helpers
// -----------------------------------------------------------------------------
struct VersionInfoBlob
{
    std::vector<BYTE> buf;
    std::vector<std::pair<WORD, WORD>> translations; // (lang, codepage)
};

static bool LoadVersionInfoBlob(const std::wstring& path, VersionInfoBlob& out)
{
    out = {};
    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (sz == 0) return false;

    out.buf.resize(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, out.buf.data()))
        return false;

    struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; };
    LANGANDCODEPAGE* lpTranslate = nullptr;
    UINT cbTranslate = 0;

    if (VerQueryValueW(out.buf.data(), L"\\VarFileInfo\\Translation",
        reinterpret_cast<void**>(&lpTranslate), &cbTranslate) && lpTranslate && cbTranslate >= sizeof(LANGANDCODEPAGE))
    {
        const int count = (int)(cbTranslate / sizeof(LANGANDCODEPAGE));
        for (int i = 0; i < count; i++)
            out.translations.push_back({ lpTranslate[i].wLanguage, lpTranslate[i].wCodePage });
    }

    if (out.translations.empty())
        out.translations.push_back({ 0x0409, 1200 });

    return true;
}

static std::wstring QueryVersionString(const VersionInfoBlob& vi, const wchar_t* key)
{
    if (vi.buf.empty() || !key) return L"";

    for (const auto& tr : vi.translations)
    {
        wchar_t subblock[128];
        swprintf_s(subblock, L"\\StringFileInfo\\%04x%04x\\%s", tr.first, tr.second, key);

        LPWSTR value = nullptr;
        UINT len = 0;
        if (VerQueryValueW((void*)vi.buf.data(), subblock, reinterpret_cast<void**>(&value), &len) && value && len > 0)
            return std::wstring(value);
    }

    // common explicit fallback 040904B0
    {
        wchar_t subblock[128];
        swprintf_s(subblock, L"\\StringFileInfo\\040904B0\\%s", key);
        LPWSTR value = nullptr;
        UINT len = 0;
        if (VerQueryValueW((void*)vi.buf.data(), subblock, reinterpret_cast<void**>(&value), &len) && value && len > 0)
            return std::wstring(value);
    }

    return L"";
}

static std::wstring QueryFixedFileVersion(const VersionInfoBlob& vi)
{
    if (vi.buf.empty()) return L"";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueW((void*)vi.buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) ||
        !ffi || ffiLen < sizeof(VS_FIXEDFILEINFO))
        return L"";

    std::wstringstream ss;
    ss << HIWORD(ffi->dwFileVersionMS) << L"."
        << LOWORD(ffi->dwFileVersionMS) << L"."
        << HIWORD(ffi->dwFileVersionLS) << L"."
        << LOWORD(ffi->dwFileVersionLS);
    return ss.str();
}

static std::wstring FirstSentenceOrTruncate(const std::wstring& s, size_t hardMax = 140)
{
	// take until first . ! ? (common sentence ends), else truncate at word boundary
	auto isEnd = [](wchar_t c) { return c == L'.' || c == L'!' || c == L'?'; };

	for (size_t i = 0; i < s.size(); ++i)
	{
		if (isEnd(s[i]))
		{
			// include punctuation
			size_t end = i + 1;
			// skip trailing quotes/spaces
			while (end < s.size() && (s[end] == L'"' || s[end] == L'\'' || s[end] == L' ')) end++;
			return s.substr(0, i + 1);
		}
	}

	if (s.size() <= hardMax) return s;

	size_t cut = hardMax;
	while (cut > 0 && s[cut - 1] != L' ' && s[cut - 1] != L'\t') cut--;
	if (cut < 20) cut = hardMax; // avoid over-short
	return s.substr(0, cut) + L"…";
}

static std::wstring HtmlTrim(std::wstring s)
{
	auto isSpace = [](wchar_t c)
		{
			return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
		};

	while (!s.empty() && isSpace(s.front())) s.erase(s.begin());
	while (!s.empty() && isSpace(s.back()))  s.pop_back();
	return s;
}

static bool StripTagPair(std::wstring& s,
	const wchar_t* open,
	const wchar_t* close)
{
	std::wstring o(open), c(close);

	size_t p0 = s.find(o);
	size_t p1 = s.rfind(c);

	if (p0 != std::wstring::npos &&
		p1 != std::wstring::npos &&
		p1 > p0)
	{
		std::wstring left = HtmlTrim(s.substr(0, p0));
		std::wstring right = HtmlTrim(s.substr(p1 + c.size()));

		if (left.empty() && right.empty())
		{
			s = s.substr(p0 + o.size(), p1 - (p0 + o.size()));
			return true;
		}
	}

	return false;
}


static D2D1_COLOR_F ColorFromRGB(unsigned rgb, float a = 1.0f)
{
	float r = ((rgb >> 16) & 0xFF) / 255.0f;
	float g = ((rgb >> 8) & 0xFF) / 255.0f;
	float b = ((rgb) & 0xFF) / 255.0f;
	return D2D1::ColorF(r, g, b, a);
}

static D2D1_COLOR_F GetBitnessAccentColor(const std::wstring& bitness, float a = 1.0f)
{
	if (bitness == L"64-bit") return ColorFromRGB(0xEAA61F, a);
	if (bitness == L"32-bit") return ColorFromRGB(0x1ABDB8, a);
	return D2D1::ColorF(1, 1, 1, a); // unknown
}


// -----------------------------------------------------------------------------
// Template expansion + <small> tag parsing to font size ranges
// -----------------------------------------------------------------------------
struct SmallRange { UINT32 start; UINT32 length; };

static std::wstring ExpandTokens(std::wstring templ, const std::map<std::wstring, std::wstring>& kv)
{
    for (const auto& it : kv)
    {
        std::wstring token = L"$(" + it.first + L")";
        size_t pos = 0;
        while ((pos = templ.find(token, pos)) != std::wstring::npos)
        {
            templ.replace(pos, token.size(), it.second);
            pos += it.second.size();
        }
    }
    return templ;
}

static void NormalizeNewlinesInPlace(std::wstring& s)
{
    for (;;)
    {
        size_t pos = s.find(L"\r\n\r\n\r\n");
        if (pos == std::wstring::npos) break;
        s.erase(pos, 2);
    }
}


// -----------------------------------------------------------------------------
// DLL Type (Debug, Test, Release) helpers
// -----------------------------------------------------------------------------

enum class BuildFlavor
{
	Unknown = 0,
	Release,
	Test,
	Debug
};

static const wchar_t* BuildFlavorLabel(BuildFlavor f)
{
	switch (f)
	{
	case BuildFlavor::Debug:   return L"Debug";
	case BuildFlavor::Test:    return L"Test";
	case BuildFlavor::Release: return L"Release";
	default:                   return L"";
	}
}


struct MappedFile
{
	HANDLE hFile = INVALID_HANDLE_VALUE;
	HANDLE hMap = nullptr;
	BYTE* base = nullptr;
	size_t size = 0;

	~MappedFile()
	{
		if (base) UnmapViewOfFile(base);
		if (hMap) CloseHandle(hMap);
		if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
	}

	bool Open(const std::wstring& path)
	{
		hFile = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER li{};
		if (!GetFileSizeEx(hFile, &li) || li.QuadPart <= 0) return false;
		size = (size_t)li.QuadPart;

		hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (!hMap) return false;

		base = (BYTE*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
		return base != nullptr;
	}
};

static bool GetNtHeaders(BYTE* base, IMAGE_NT_HEADERS** outNt)
{
	if (!base || !outNt) return false;
	auto* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

	auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	if ((BYTE*)nt < base) return false;
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

	*outNt = nt;
	return true;
}

static DWORD RvaToFileOffset(BYTE* base, IMAGE_NT_HEADERS* nt, DWORD rva)
{
	// Convert RVA -> file offset using section headers
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
	{
		DWORD va = sec->VirtualAddress;
		DWORD sz = std::max(sec->Misc.VirtualSize, sec->SizeOfRawData);
		if (rva >= va && rva < va + sz)
		{
			DWORD delta = rva - va;
			return sec->PointerToRawData + delta;
		}
	}
	return 0;
}

static bool HasDebugCRTImports(const std::wstring& path)
{
	MappedFile mf;
	if (!mf.Open(path)) return false;

	IMAGE_NT_HEADERS* nt = nullptr;
	if (!GetNtHeaders(mf.base, &nt)) return false;

	const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

	DWORD off = RvaToFileOffset(mf.base, nt, dir.VirtualAddress);
	if (!off) return false;

	auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(mf.base + off);

	auto ToUpper = [](std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)toupper(c); });
			return s;
		};

	// Common debug CRT DLLs
	const char* dbgDlls[] = {
		 "UCRTBASD.DLL",
		 "VCRUNTIMED.DLL",
		 "MSVCRTD.DLL",
		 "MSVCP140D.DLL",
		 "VCRUNTIME140D.DLL"
	};

	for (; imp->Name != 0; imp++)
	{
		DWORD nameOff = RvaToFileOffset(mf.base, nt, imp->Name);
		if (!nameOff) continue;

		const char* dllName = (const char*)(mf.base + nameOff);
		if (!dllName || !*dllName) continue;

		std::string up = ToUpper(dllName);
		for (auto* d : dbgDlls)
		{
			if (up == d) return true;
		}
	}
	return false;
}


struct VersionInfoMini
{
	std::vector<BYTE> buf;
};

static bool LoadVersionInfoMini(const std::wstring& path, VersionInfoMini& out)
{
	out.buf.clear();
	DWORD handle = 0;
	DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &handle);
	if (!sz) return false;

	out.buf.resize(sz);
	return !!GetFileVersionInfoW(path.c_str(), 0, sz, out.buf.data());
}

static bool HasVSFFDebugFlag(const VersionInfoMini& vi)
{
	if (vi.buf.empty()) return false;
	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT len = 0;
	if (!VerQueryValueW((void*)vi.buf.data(), L"\\", (void**)&ffi, &len) || !ffi) return false;
	return (ffi->dwFileFlags & VS_FF_DEBUG) != 0;
}

static std::wstring QueryVersionString0409(const VersionInfoMini& vi, const wchar_t* key)
{
	if (vi.buf.empty()) return L"";
	wchar_t sub[128];
	swprintf_s(sub, L"\\StringFileInfo\\040904B0\\%s", key);

	LPWSTR val = nullptr;
	UINT len = 0;
	if (VerQueryValueW((void*)vi.buf.data(), sub, (void**)&val, &len) && val && len)
		return std::wstring(val);
	return L"";
}

static bool StringContainsAnyCI(const std::wstring& s, const std::vector<std::wstring>& needles)
{
	std::wstring low = s;
	std::transform(low.begin(), low.end(), low.begin(), ::towlower);
	for (const auto& n : needles)
	{
		std::wstring nl = n;
		std::transform(nl.begin(), nl.end(), nl.begin(), ::towlower);
		if (low.find(nl) != std::wstring::npos) return true;
	}
	return false;
}

static std::wstring GetPdbPathFromDebugDirectory(const std::wstring& path)
{
	MappedFile mf;
	if (!mf.Open(path)) return L"";

	IMAGE_NT_HEADERS* nt = nullptr;
	if (!GetNtHeaders(mf.base, &nt)) return L"";

	const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
	if (dir.VirtualAddress == 0 || dir.Size == 0) return L"";

	DWORD off = RvaToFileOffset(mf.base, nt, dir.VirtualAddress);
	if (!off) return L"";

	auto* dbg = (IMAGE_DEBUG_DIRECTORY*)(mf.base + off);
	size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

	for (size_t i = 0; i < count; i++)
	{
		if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;

		DWORD dataOff = dbg[i].PointerToRawData; // file offset
		if (!dataOff) continue;

		BYTE* cv = mf.base + dataOff;
		// CodeView signature: "RSDS"
		if (cv[0] == 'R' && cv[1] == 'S' && cv[2] == 'D' && cv[3] == 'S')
		{
			// Layout: RSDS(4) + GUID(16) + Age(4) + PDB path (null-terminated char*)
			const char* pdb = (const char*)(cv + 4 + 16 + 4);
			if (!pdb || !*pdb) return L"";

			// Convert to wide (best-effort; PDB path is typically ASCII/UTF-8-ish)
			int n = MultiByteToWideChar(CP_UTF8, 0, pdb, -1, nullptr, 0);
			if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, pdb, -1, nullptr, 0);
			if (n <= 0) return L"";

			std::wstring w(n - 1, L'\0');
			if (MultiByteToWideChar(CP_UTF8, 0, pdb, -1, &w[0], n) <= 0)
				MultiByteToWideChar(CP_ACP, 0, pdb, -1, &w[0], n);

			return w;
		}
	}

	return L"";
}

static BuildFlavor DetectBuildFlavor(const std::wstring& path)
{
	if (path.empty()) return BuildFlavor::Unknown;

	int scoreDebug = 0;
	int scoreTest = 0;
	int scoreRel = 0;

	// 1) Debug CRT imports: huge signal
	const bool dbgCRT = HasDebugCRTImports(path);
	if (dbgCRT) scoreDebug += 100;

	// Version info (optional)
	VersionInfoMini vi;
	const bool hasVI = LoadVersionInfoMini(path, vi);

	// 2) VS_FF_DEBUG flag: strong signal
	if (hasVI && HasVSFFDebugFlag(vi))
		scoreDebug += 60;

	// 3) String markers
	if (hasVI)
	{
		std::wstring special = QueryVersionString0409(vi, L"SpecialBuild");
		std::wstring priv = QueryVersionString0409(vi, L"PrivateBuild");
		std::wstring desc = QueryVersionString0409(vi, L"FileDescription");
		std::wstring prodver = QueryVersionString0409(vi, L"ProductVersion");
		std::wstring comments = QueryVersionString0409(vi, L"Comments");

		if (!special.empty() || !priv.empty())
			scoreTest += 50;

		if (StringContainsAnyCI(desc + L" " + prodver + L" " + comments,
			{ L"test", L"internal", L"nightly", L"beta", L"alpha", L"rc", L"preview" }))
			scoreTest += 25;

		if (StringContainsAnyCI(desc + L" " + prodver + L" " + comments,
			{ L"debug", L"dbg", L"checked" }))
			scoreDebug += 25;
	}

	// 4) PDB path hint
	std::wstring pdb = GetPdbPathFromDebugDirectory(path);
	if (!pdb.empty())
	{
		// Seeing \Debug\ is a meaningful hint
		if (StringContainsAnyCI(pdb, { L"\\debug\\", L"/debug/" })) scoreDebug += 20;

		// These are often "release with symbols" patterns
		if (StringContainsAnyCI(pdb, { L"\\relwithdebinfo\\", L"\\release\\", L"/release/" }))
			scoreRel += 5; // weak
	}

	// Decide outcome
	if (scoreDebug >= 20) return BuildFlavor::Debug;
	if (scoreTest >= 40) return BuildFlavor::Test;

	// If we have some evidence it’s a normal build
	if (hasVI || !pdb.empty())
		return BuildFlavor::Release;

	// Otherwise we know very little
	return BuildFlavor::Unknown;
}


// -----------------------------------------------------------------------------
// Renderer helpers
// -----------------------------------------------------------------------------

static D2D1_COLOR_F FlavorColor(BuildFlavor f)
{
	switch (f)
	{
	case BuildFlavor::Debug:   return ColorFromRGB(0xFF3B30, 1.0f); // red
	case BuildFlavor::Test:    return ColorFromRGB(0xFFD60A, 1.0f); // yellow
	case BuildFlavor::Release: return ColorFromRGB(0x34C759, 1.0f); // green
	default:                   return D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f); // gray
	}
}

static std::wstring FlavorLabel(BuildFlavor f)
{
	switch (f)
	{
	case BuildFlavor::Debug:   return L"Debug";
	case BuildFlavor::Test:    return L"Test";
	case BuildFlavor::Release: return L"Release";
	default:                   return L"";
	}
}

static void PushRoundedClip(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rr)
{
	CComPtr<ID2D1Layer> layer;
	rt->CreateLayer(nullptr, &layer);

	D2D1_LAYER_PARAMETERS lp = {};
	lp.contentBounds = rr.rect;
	lp.opacity = 1.0f;
	lp.geometricMask = nullptr;
	lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;

	// Create rounded-rect geometry as mask
	CComPtr<ID2D1RoundedRectangleGeometry> geo;
	g_d2d->CreateRoundedRectangleGeometry(rr, &geo);
	lp.geometricMask = geo;

	rt->PushLayer(lp, layer);
}

static void PopRoundedClip(ID2D1RenderTarget* rt)
{
	rt->PopLayer();
}

// Rounded-clip build strip (Option A2)
static void DrawBuildStrip(
	ID2D1RenderTarget* rt,
	IDWriteFactory* dw,
	const D2D1_RECT_F& plateRect,
	float stripH,
	const std::wstring& label,
	const D2D1_COLOR_F& stripColor,
	float cornerRadius) 
{
	if (!rt || !dw) return;
	if (label.empty()) return;

	const float plateH = plateRect.bottom - plateRect.top;
	const float plateW = plateRect.right - plateRect.left;
	if (plateH <= 1.0f || plateW <= 1.0f) return;

	stripH = Clamp(stripH, 1.0f, plateH);

	// Limit radius to something sane
	cornerRadius = Clamp(cornerRadius, 0.0f, std::min(plateW, plateH) * 0.5f);

	// Strip area (bottom band)
	D2D1_RECT_F strip = D2D1::RectF(
		plateRect.left,
		plateRect.bottom - stripH,
		plateRect.right,
		plateRect.bottom);

	// --- Create rounded geometry for clipping ---
	CComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
	HRESULT hr = g_d2d->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(plateRect, cornerRadius, cornerRadius),
		&rrGeo);
	if (FAILED(hr) || !rrGeo) return;

	// Layer for geometric clip
	CComPtr<ID2D1Layer> layer;
	hr = rt->CreateLayer(nullptr, &layer);
	if (FAILED(hr) || !layer) return;

	D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
	lp.contentBounds = plateRect;
	lp.geometricMask = rrGeo;
	lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
	lp.opacity = 1.0f;

	// Push rounded clip
	rt->PushLayer(lp, layer);

	// --- Fill strip ---
	CComPtr<ID2D1SolidColorBrush> stripBrush;
	rt->CreateSolidColorBrush(stripColor, &stripBrush);
	if (stripBrush)
		rt->FillRectangle(strip, stripBrush);

	// --- Strip text ---
	// Choose black text; if you ever use a very dark stripColor, switch to white.
	CComPtr<ID2D1SolidColorBrush> textBrush;
	rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &textBrush);

	CComPtr<IDWriteTextFormat> fmt;
	dw->CreateTextFormat(
		L"Segoe UI",
		nullptr,
		DWRITE_FONT_WEIGHT_SEMI_BOLD,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		Clamp(stripH * 0.60f, 9.f, 22.f),
		L"en-us",
		&fmt);

	if (fmt)
	{
		fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		if (textBrush)
			rt->DrawTextW(label.c_str(), (UINT32)label.size(), fmt, strip, textBrush);
	}

	// Pop rounded clip
	rt->PopLayer();
}


// Backward-compatible wrapper (keeps existing 6-arg calls compiling)
// It guesses a reasonable radius from the plate height.
static void DrawBuildStrip(
	ID2D1RenderTarget* rt,
	IDWriteFactory* dw,
	const D2D1_RECT_F& plateRect,
	float stripH,
	const std::wstring& label,
	const D2D1_COLOR_F& stripColor)
{
	const float plateH = plateRect.bottom - plateRect.top;
	float guessedRadius = Clamp(plateH * 0.10f, 4.f, 14.f); // similar feel to your plate radius
	DrawBuildStrip(rt, dw, plateRect, stripH, label, stripColor, guessedRadius);
}


static bool RectIntersects(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
{
	return !(a.right <= b.left || a.left >= b.right || a.bottom <= b.top || a.top >= b.bottom);
}

static void DrawChipPins(
	ID2D1RenderTarget* rt,
	const D2D1_RECT_F& plate,
	float pinThickness,
	float pinLength,
	int pinsPerSide,
	ID2D1Brush* brush,
	const D2D1_RECT_F& keepOut,
	float cornerInset)   
{
	if (!rt || !brush || pinsPerSide <= 0) return;

	// Clamp inset so it can't invert the rectangle
	const float maxInsetX = (plate.right - plate.left) * 0.45f;
	const float maxInsetY = (plate.bottom - plate.top) * 0.45f;
	cornerInset = Clamp(cornerInset, 0.f, std::min(maxInsetX, maxInsetY));

	const float left = plate.left + cornerInset;
	const float right = plate.right - cornerInset;
	const float top = plate.top + cornerInset;
	const float bottom = plate.bottom - cornerInset;

	// Left/right pins along reduced height
	float usableH = (bottom - top) - pinThickness;
	float stepY = (pinsPerSide <= 1) ? 0.0f : (usableH / (pinsPerSide - 1));

	for (int i = 0; i < pinsPerSide; i++)
	{
		float y = top + i * stepY;

		D2D1_RECT_F leftPin = D2D1::RectF(
			plate.left - pinLength, y,
			plate.left, y + pinThickness);

		D2D1_RECT_F rightPin = D2D1::RectF(
			plate.right, y,
			plate.right + pinLength, y + pinThickness);

		if (!RectIntersects(leftPin, keepOut))  rt->FillRectangle(leftPin, brush);
		if (!RectIntersects(rightPin, keepOut)) rt->FillRectangle(rightPin, brush);
	}

	// Top/bottom pins along reduced width
	float usableW = (right - left) - pinThickness;
	float stepX = (pinsPerSide <= 1) ? 0.0f : (usableW / (pinsPerSide - 1));

	for (int i = 0; i < pinsPerSide; i++)
	{
		float x = left + i * stepX;

		D2D1_RECT_F topPin = D2D1::RectF(
			x, plate.top - pinLength,
			x + pinThickness, plate.top);

		D2D1_RECT_F bottomPin = D2D1::RectF(
			x, plate.bottom,
			x + pinThickness, plate.bottom + pinLength);

		if (!RectIntersects(topPin, keepOut))    rt->FillRectangle(topPin, brush);
		if (!RectIntersects(bottomPin, keepOut)) rt->FillRectangle(bottomPin, brush);
	}
}


static void DrawBitnessBadge(
	ID2D1RenderTarget* rt,
	IDWriteFactory* dw,
	IDWriteTextFormat* /*fmtBase*/,
	float W, float /*H*/,
	const std::wstring& badgeText,
	bool diamond,
	float sizePx,
	float marginPx,
	const D2D1_COLOR_F& fillColor)
{
	if (!rt || !dw) return;

	const float s = sizePx;
	const float x0 = W - marginPx - s;
	const float y0 = marginPx;

	CComPtr<ID2D1SolidColorBrush> fill;
	CComPtr<ID2D1SolidColorBrush> stroke;
	CComPtr<ID2D1SolidColorBrush> textBrush;

	// Badge fill uses bitness accent color
	rt->CreateSolidColorBrush(fillColor, &fill);

	// Subtle border + white text
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.25f), &stroke);
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 1.0f), &textBrush);

	if (!diamond)
	{
		D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(x0 + s * 0.5f, y0 + s * 0.5f), s * 0.5f, s * 0.5f);
		rt->FillEllipse(e, fill);
		rt->DrawEllipse(e, stroke, 1.0f);
	}
	else
	{
		// Diamond (rotated square)
		CComPtr<ID2D1PathGeometry> geo;
		CComPtr<ID2D1GeometrySink> sink;
		if (SUCCEEDED(g_d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink)))
		{
			const float cx = x0 + s * 0.5f;
			const float cy = y0 + s * 0.5f;

			sink->BeginFigure(D2D1::Point2F(cx, cy - s * 0.5f), D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine(D2D1::Point2F(cx + s * 0.5f, cy));
			sink->AddLine(D2D1::Point2F(cx, cy + s * 0.5f));
			sink->AddLine(D2D1::Point2F(cx - s * 0.5f, cy));
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			sink->Close();

			rt->FillGeometry(geo, fill);
			rt->DrawGeometry(geo, stroke, 1.0f);
		}
	}

	// Text in badge
	float badgeFontSize = Clamp(s * 0.42f, 9.f, 20.f);

	CComPtr<IDWriteTextFormat> fmt;
	dw->CreateTextFormat(
		L"Segoe UI", nullptr,
		DWRITE_FONT_WEIGHT_BOLD,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		badgeFontSize,
		L"en-us",
		&fmt);

	fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	D2D1_RECT_F r = D2D1::RectF(x0, y0, x0 + s, y0 + s);
	rt->DrawTextW(badgeText.c_str(), (UINT32)badgeText.size(), fmt, r, textBrush);
}

static void DrawCatalogShield(
	ID2D1RenderTarget* rt,
	float x, float y, float s)
{
	if (!rt || !g_d2d) return;

	// Shield geometry (same as before)
	CComPtr<ID2D1PathGeometry> geo;
	if (FAILED(g_d2d->CreatePathGeometry(&geo)) || !geo) return;

	CComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink)) || !sink) return;

	auto P = [&](float px, float py) { return D2D1::Point2F(x + px * s, y + py * s); };

	sink->BeginFigure(P(0.50f, 0.05f), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLine(P(0.86f, 0.18f));
	sink->AddLine(P(0.86f, 0.52f));
	sink->AddBezier(D2D1::BezierSegment(P(0.86f, 0.78f), P(0.70f, 0.92f), P(0.50f, 0.98f)));
	sink->AddBezier(D2D1::BezierSegment(P(0.30f, 0.92f), P(0.14f, 0.78f), P(0.14f, 0.52f)));
	sink->AddLine(P(0.14f, 0.18f));
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();

	// Yellow gradient
	const D2D1_COLOR_F topColor = D2D1::ColorF(0.98f, 0.86f, 0.25f, 1.0f); // light yellow
	const D2D1_COLOR_F bottomColor = D2D1::ColorF(0.86f, 0.66f, 0.10f, 1.0f); // darker yellow

	D2D1_GRADIENT_STOP gs[3]{};
	gs[0] = { 0.0f,  topColor };
	gs[1] = { 0.55f, topColor };
	gs[2] = { 1.0f,  bottomColor };

	CComPtr<ID2D1GradientStopCollection> stops;
	if (FAILED(rt->CreateGradientStopCollection(gs, 3, &stops)) || !stops) return;

	D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgp =
		D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(x, y + s));

	CComPtr<ID2D1LinearGradientBrush> fill;
	if (FAILED(rt->CreateLinearGradientBrush(lgp, stops, &fill)) || !fill) return;

	CComPtr<ID2D1SolidColorBrush> stroke;
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.28f), &stroke);

	rt->FillGeometry(geo, fill);
	if (stroke) rt->DrawGeometry(geo, stroke, 1.0f);

	// Small "folder tab" cue (simple rounded rect) in top-left area of the shield
	CComPtr<ID2D1SolidColorBrush> tab;
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.22f), &tab);
	if (tab)
	{
		D2D1_RECT_F r = D2D1::RectF(x + s * 0.18f, y + s * 0.16f, x + s * 0.55f, y + s * 0.30f);
		rt->FillRoundedRectangle(D2D1::RoundedRect(r, s * 0.06f, s * 0.06f), tab);
	}
}


static void DrawDigitalSignedShield(
	ID2D1RenderTarget* rt,
	float x, float y, float s) // top-left + size
{
	if (!rt || !g_d2d) return;

	CComPtr<ID2D1PathGeometry> geo;
	if (FAILED(g_d2d->CreatePathGeometry(&geo)) || !geo) return;

	CComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink)) || !sink) return;

	// Shield shape in [0..1] scaled to s
	auto P = [&](float px, float py) { return D2D1::Point2F(x + px * s, y + py * s); };

	sink->BeginFigure(P(0.50f, 0.05f), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLine(P(0.86f, 0.18f));
	sink->AddLine(P(0.86f, 0.52f));
	sink->AddBezier(D2D1::BezierSegment(P(0.86f, 0.78f), P(0.70f, 0.92f), P(0.50f, 0.98f)));
	sink->AddBezier(D2D1::BezierSegment(P(0.30f, 0.92f), P(0.14f, 0.78f), P(0.14f, 0.52f)));
	sink->AddLine(P(0.14f, 0.18f));
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();

	// --- Two-tone gradient fill (top lighter, bottom darker) ---
	// Windows accent blue-ish: base #0078D4
	const D2D1_COLOR_F topColor = D2D1::ColorF(0.12f, 0.55f, 0.93f, 1.0f); // slightly lighter
	const D2D1_COLOR_F bottomColor = D2D1::ColorF(0.00f, 0.42f, 0.76f, 1.0f); // slightly darker

	CComPtr<ID2D1GradientStopCollection> stops;
	D2D1_GRADIENT_STOP gs[3]{};
	gs[0].position = 0.0f;  gs[0].color = topColor;
	gs[1].position = 0.55f; gs[1].color = topColor;      // hold the top color a bit
	gs[2].position = 1.0f;  gs[2].color = bottomColor;

	if (FAILED(rt->CreateGradientStopCollection(gs, 3, &stops)) || !stops)
		return;

	// Vertical gradient from top to bottom of the shield
	D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgp =
		D2D1::LinearGradientBrushProperties(
			D2D1::Point2F(x, y),
			D2D1::Point2F(x, y + s));

	CComPtr<ID2D1LinearGradientBrush> fill;
	if (FAILED(rt->CreateLinearGradientBrush(lgp, stops, &fill)) || !fill)
		return;

	// Subtle border
	CComPtr<ID2D1SolidColorBrush> stroke;
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.28f), &stroke);

	// Fill + outline
	rt->FillGeometry(geo, fill);
	if (stroke) rt->DrawGeometry(geo, stroke, 1.0f);

	// Optional: subtle inner highlight line down the center (adds "life")
	// Comment out if you want it flatter.
	CComPtr<ID2D1SolidColorBrush> highlight;
	rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.10f), &highlight);
	if (highlight)
	{
		rt->DrawLine(
			D2D1::Point2F(x + s * 0.50f, y + s * 0.18f),
			D2D1::Point2F(x + s * 0.50f, y + s * 0.82f),
			highlight,
			1.0f);
	}
}


// -----------------------------------------------------------------------------
// Plugin class
// -----------------------------------------------------------------------------
class CVCDLLPlugin : public IMysticThumbsPlugin
{
public:
	// Valid for lifetime through the lifetime of the plugin instance until the end of Destroy
	IMysticThumbsPluginContext* m_context = nullptr;
	const IMysticThumbsLog* m_log{};

	struct PluginConfig
	{
		IMysticThumbsPluginContext* context = nullptr;
		LogConfigCommon log{};

		std::wstring templ = DEFAULT_TEMPLATE;
		DWORD plateOpacity = 55;
		bool  plateOpaque = false;
		DWORD labelScalePct = 75;

		PluginConfig() = default;
		explicit PluginConfig(CVCDLLPlugin* p) { context = p ? p->m_context : nullptr; }

		void Load(bool dumpToLog)
		{
			HKEY root = context ? context->GetPluginRegistryRootKey() : nullptr;
			CSimpleRegistryHelper reg(root);

			log.enabled = (reg.GetDword(REG_SETTINGS_KEY, REG_LOG_ENABLED, 0) != 0);
			log.includeCRC = (reg.GetDword(REG_SETTINGS_KEY, REG_LOG_INCLUDE_CRC, 1) != 0);
			log.fileName = reg.GetString(REG_SETTINGS_KEY, REG_LOG_FILENAME, L"");

			templ = reg.GetString(REG_SETTINGS_KEY, REG_TEMPLATE, DEFAULT_TEMPLATE);
			if (templ.empty()) templ = DEFAULT_TEMPLATE;

			plateOpacity = (DWORD)Clamp((float)reg.GetDword(REG_SETTINGS_KEY, REG_PLATE_OPACITY, 55), 0.f, 100.f);
			plateOpaque = (reg.GetDword(REG_SETTINGS_KEY, REG_PLATE_OPAQUE, 0) != 0);
			labelScalePct = (DWORD)Clamp((float)reg.GetDword(REG_SETTINGS_KEY, REG_LABEL_SCALE, 75), 50.f, 100.f);

			if (dumpToLog && log.enabled)
			{
				LogSessionHeader(g_hModule, &log, kBitness);
				LogMessage(L"  TemplateLen=" + std::to_wstring(templ.size()));
				LogMessage(L"  PlateOpacity=" + std::to_wstring(plateOpacity));
				LogMessage(L"  PlateOpaque=" + std::to_wstring(plateOpaque ? 1 : 0));
				LogMessage(L"  LabelScalePct=" + std::to_wstring(labelScalePct));
			}
		}

		void Save() const
		{
			HKEY root = context ? context->GetPluginRegistryRootKey() : nullptr;
			CSimpleRegistryHelper reg(root);
			reg.SetString(REG_SETTINGS_KEY, REG_TEMPLATE, templ);
			reg.SetDword(REG_SETTINGS_KEY, REG_PLATE_OPACITY, plateOpacity);
			reg.SetDword(REG_SETTINGS_KEY, REG_PLATE_OPAQUE, plateOpaque ? 1u : 0u);
			reg.SetDword(REG_SETTINGS_KEY, REG_LABEL_SCALE, labelScalePct);
		}
	} config;
		
	explicit CVCDLLPlugin(_In_ IMysticThumbsPluginContext* context)
		: m_context(context), config(this)
	{
		m_log = context->Log();
	}

    void Destroy() override
    {
        this->~CVCDLLPlugin();
        CoTaskMemFree(this);
    }

    _Notnull_ LPCWSTR GetName() const override { return s_name; }
    _Notnull_ LPCGUID GetGuid() const override { return &s_guid; }
    _Notnull_ LPCWSTR GetDescription() const override { return s_description; }
    _Notnull_ LPCWSTR GetAuthor() const override { return s_author; }

    unsigned int GetExtensionCount() const override { return (unsigned int)_countof(s_extensions); }
    _Notnull_ LPCWSTR GetExtension(unsigned int index) const override
    {
        if (index >= _countof(s_extensions)) return L"";
        return s_extensions[index];
    }

    bool Ping(_Inout_ MysticThumbsPluginPing& ping) override
    {
        config.context = m_context; 
        config.Load(true);

        BindLogConfig(&config.log);
        ClearLogContext();

        unsigned int w = ping.requestedWidth ? ping.requestedWidth : 256;
        unsigned int h = ping.requestedHeight ? ping.requestedHeight : 256;
        ping.width = w;
        ping.height = h;
        ping.bitDepth = 32;
        return true;
    } 

    bool GetCapabilities(_Out_ MysticThumbsPluginCapabilities& capabilities) override
    {
        capabilities = {};
        capabilities |= PluginCapabilities_CanConfigure;
        return true;
    }

    // ---- Configure dialog ----
    struct DlgState
    {
        CVCDLLPlugin* plugin = nullptr;
        PluginConfig cfgAtOpen;
        explicit DlgState(CVCDLLPlugin* p) : plugin(p), cfgAtOpen(p) {}
    };

    static void ApplyDialogToConfig(HWND hDlg, const PluginConfig& oldCfg, PluginConfig& newCfg)
    {
        newCfg = oldCfg;
        newCfg.templ = GetText(hDlg, IDC_DLL_TEMPLATE_EDIT);
        UINT w = GetUInt(hDlg, IDC_DLL_PLATE_OPACITY_EDIT, oldCfg.plateOpacity);
        newCfg.plateOpacity = (DWORD)Clamp((float)w, 0.f, 100.f);
        newCfg.plateOpaque = GetCheck(hDlg, IDC_DLL_PLATE_OPAQUE);
        w = GetUInt(hDlg, IDC_DLL_LABEL_SCALE_EDIT, oldCfg.labelScalePct);
        newCfg.labelScalePct = (DWORD)Clamp((float)w, 50.f, 100.f);
    }

    static bool ConfigDifferent(const PluginConfig& a, const PluginConfig& b)
    {
        return a.templ != b.templ ||
            a.plateOpacity != b.plateOpacity ||
            a.plateOpaque != b.plateOpaque ||
            a.labelScalePct != b.labelScalePct;
    }

    static INT_PTR CALLBACK DllConfigureDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            auto* plugin = reinterpret_cast<CVCDLLPlugin*>(lParam);
            if (!plugin) return FALSE;

            auto* st = new DlgState(plugin);
            plugin->config.context = plugin->m_context;
            plugin->config.Load(false);
            st->cfgAtOpen = plugin->config;
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

            const auto& c = st->cfgAtOpen;
            SetText(hDlg, IDC_DLL_TEMPLATE_EDIT, c.templ);
            SetUInt(hDlg, IDC_DLL_PLATE_OPACITY_EDIT, c.plateOpacity);
            SetCheck(hDlg, IDC_DLL_PLATE_OPAQUE, c.plateOpaque);
            SetUInt(hDlg, IDC_DLL_LABEL_SCALE_EDIT, c.labelScalePct);
            return TRUE;
        }
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDOK:
            {
                auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
                if (st && st->plugin)
                {
                    PluginConfig newCfg = st->cfgAtOpen;
                    ApplyDialogToConfig(hDlg, st->cfgAtOpen, newCfg);
                    if (ConfigDifferent(st->cfgAtOpen, newCfg)) 
                    {
                        st->plugin->config = newCfg;
                        st->plugin->config.Save();
                        MessageBoxW(hDlg,
                            L"DLL plugin settings saved to the registry.\r\n\r\n"
                            L"MysticThumbs/Explorer may need to be restarted before all new thumbnails use the updated settings.",
                            L"Voith's CODE DLL Plugin", MB_OK | MB_ICONINFORMATION);
                    }
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
        case WM_NCDESTROY:
        {
            auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
            if (st)
            {
                delete st;
                SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
            }
            break;
        }
        }
        return FALSE;
    }

    bool Configure(_In_ HWND hWndParent) override
    {
        config.context = m_context;
        config.Load(false);
        INT_PTR result = DialogBoxParamW(
            g_hModule,
            MAKEINTRESOURCE(IDD_DLL_PLUGIN_CONFIGURE),
            hWndParent,
            DllConfigureDialogProc,
            (LPARAM)this);
        return result == IDOK;
    }

	 // ---- Rendering ----
	 static HRESULT RenderTextThumb(
		 const PluginConfig& cfg,
		 IWICBitmap* bmp,
		 unsigned int width,
		 unsigned int height,
		 const std::wstring& fullText,
		 const std::wstring& bitness,
		 BuildFlavor flavor,
		 const SigKind sigKind)
	 {
		 if (!bmp || !g_d2d || !g_dw) return E_INVALIDARG;

		 const float W = (float)width;
		 const float H = (float)height;

		 struct LineRun
		 {
			 std::wstring text;
			 bool center = false;
			 bool strong = false;
			 enum class SizeTag { Normal, Small, Tiny } size = SizeTag::Normal;
		 };

		 auto ParseLines = [&](const std::wstring& markup) -> std::vector<LineRun>
			 {
				 std::vector<LineRun> out;
				 size_t start = 0;
				 while (start <= markup.size())
				 {
					 size_t end = markup.find(L"\r\n", start);
					 std::wstring line = (end == std::wstring::npos) ? markup.substr(start) : markup.substr(start, end - start);
					 start = (end == std::wstring::npos) ? markup.size() + 1 : end + 2;

					 line = HtmlTrim(line);
					 if (line.empty()) continue;

					 LineRun lr;
					 lr.text = line;

					 bool changed = true;
					 while (changed)
					 {
						 changed = false;
						 if (StripTagPair(lr.text, L"<center>", L"</center>")) { lr.center = true; changed = true; }
						 if (StripTagPair(lr.text, L"<strong>", L"</strong>")) { lr.strong = true; changed = true; }
						 if (StripTagPair(lr.text, L"<small>", L"</small>")) { lr.size = LineRun::SizeTag::Small; changed = true; }
						 if (StripTagPair(lr.text, L"<tiny>", L"</tiny>")) { lr.size = LineRun::SizeTag::Tiny;  changed = true; }
						 lr.text = HtmlTrim(lr.text);
					 }

					 out.push_back(lr);
				 }
				 return out;
			 };

		 // Flavor label + color
		 auto FlavorLabel = [](BuildFlavor f) -> const wchar_t*
			 {
				 switch (f)
				 {
				 case BuildFlavor::Debug:   return L"Debug";
				 case BuildFlavor::Test:    return L"Test";
				 case BuildFlavor::Release: return L"Release";
				 default:                   return L"";
				 }
			 };

		 auto ColorFromRGB = [](unsigned rgb, float a) -> D2D1_COLOR_F
			 {
				 float r = ((rgb >> 16) & 0xFF) / 255.0f;
				 float g = ((rgb >> 8) & 0xFF) / 255.0f;
				 float b = ((rgb) & 0xFF) / 255.0f;
				 return D2D1::ColorF(r, g, b, a);
			 };

		 auto FlavorColor = [&](BuildFlavor f) -> D2D1_COLOR_F
			 {
				 switch (f)
				 {
				 case BuildFlavor::Debug:   return ColorFromRGB(0xFF3B30, 1.0f);
				 case BuildFlavor::Test:    return ColorFromRGB(0xFFD60A, 1.0f);
				 case BuildFlavor::Release: return ColorFromRGB(0x34C759, 1.0f);
				 default:                   return ColorFromRGB(0x9E9E9E, 1.0f);
				 }
			 };

		 const wchar_t* flavorText = FlavorLabel(flavor);
		 const D2D1_COLOR_F flavorColor = FlavorColor(flavor);
		 bool showStrip = (flavor == BuildFlavor::Debug || flavor == BuildFlavor::Test || flavor == BuildFlavor::Release);
		 if (flavor == BuildFlavor::Unknown) showStrip = false;

		 // Parse lines
		 std::vector<LineRun> lines = ParseLines(fullText);
		 if (lines.empty())
		 {
			 LineRun lr;
			 lr.text = L"(no text)";
			 lines.push_back(lr);
		 }

		 // Render target
		 CComPtr<ID2D1RenderTarget> rt;
		 D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
			 D2D1_RENDER_TARGET_TYPE_DEFAULT,
			 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		 HRESULT hr = g_d2d->CreateWicBitmapRenderTarget(bmp, props, &rt);
		 if (FAILED(hr)) return hr;

		 rt->SetTextAntialiasMode(cfg.plateOpaque ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
			 : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

		 // Brushes
		 const float plateAlpha = cfg.plateOpaque ? 1.0f : Clamp((float)cfg.plateOpacity / 100.f, 0.f, 1.f);
		 D2D1_COLOR_F plateColor = D2D1::ColorF(0.16f, 0.16f, 0.16f, plateAlpha);
		 D2D1_COLOR_F accent = GetBitnessAccentColor(bitness, 1.0f);

		 CComPtr<ID2D1SolidColorBrush> plateBrush;
		 CComPtr<ID2D1SolidColorBrush> textBrush;
		 CComPtr<ID2D1SolidColorBrush> accentBrush;

		 rt->CreateSolidColorBrush(plateColor, &plateBrush);
		 rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 1.f), &textBrush);
		 rt->CreateSolidColorBrush(accent, &accentBrush);

		 // Geometry constants
		 const float outerMargin = 6.f;
		 const float padX = 9.f;
		 const float padY = 5.f;

		 float plateW = Clamp(W * 0.84f, 60.f, W - outerMargin * 2.f);
		 float maxPlateH = H - outerMargin * 2.f;

		 float stripReserve = 0.0f;
		 if (showStrip && flavorText && *flavorText)
		 {
			 float pct = (flavor == BuildFlavor::Release) ? 0.10f : 0.12f;
			 stripReserve = Clamp(H * pct, 12.f, 26.f);
		 }

		 float cornerRadius = Clamp(H * 0.06f, 4.f, 14.f);
		 float pinThickness = Clamp(cornerRadius * 0.60f, 6.f, 13.f);
		 float pinLength = Clamp(H * 0.060f, 6.f, 16.f);
		 int   pinsPerSide = 7;

		 // Layout limits
		 const float textW = Clamp(plateW - padX * 2.f, 20.f, W);
		 const float textHMax = Clamp(maxPlateH - padY * 2.f - stripReserve, 20.f, H);

		 float baseFontStart = Clamp(H * 0.145f, 9.f, 40.f);
		 float baseFontMin = 9.f;

		 const float lineSpacingMul = 1.06f;
		 const float smallScale = Clamp((float)cfg.labelScalePct, 50.f, 100.f) / 100.f;
		 const float tinyScale = 0.55f;

		 DWRITE_TRIMMING trim{};
		 trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;

		 struct BuiltLine
		 {
			 CComPtr<IDWriteTextFormat> fmt;
			 CComPtr<IDWriteTextLayout> layout;
			 DWRITE_TEXT_METRICS tm{};
			 float drawH = 0.f;
			 float fontSize = 0.f; // IMPORTANT: store font size for consistent re-layout
		 };

		 std::vector<BuiltLine> bestBuilt;
		 CComPtr<IDWriteTextFormat> bestFormatForBadge;

		 for (float baseFont = baseFontStart; baseFont >= baseFontMin; baseFont -= 1.0f)
		 {
			 std::vector<BuiltLine> built;
			 built.reserve(lines.size());

			 float totalH = 0.f;

			 for (size_t i = 0; i < lines.size(); ++i)
			 {
				 const LineRun& lr = lines[i];

				 float scale = 1.0f;
				 if (lr.size == LineRun::SizeTag::Small) scale = smallScale;
				 else if (lr.size == LineRun::SizeTag::Tiny) scale = tinyScale;

				 float fontSize = Clamp(baseFont * scale, 8.f, 80.f);

				 DWRITE_FONT_WEIGHT weight = lr.strong ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;

				 CComPtr<IDWriteTextFormat> fmt;
				 hr = g_dw->CreateTextFormat(
					 L"Segoe UI", nullptr,
					 weight,
					 DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL,
					 fontSize,
					 L"en-us",
					 &fmt);
				 if (FAILED(hr)) return hr;

				 fmt->SetTextAlignment(lr.center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
				 fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
				 fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

				 CComPtr<IDWriteTextLayout> layout;
				 hr = g_dw->CreateTextLayout(lr.text.c_str(), (UINT32)lr.text.size(), fmt, textW, 10000.f, &layout);
				 if (FAILED(hr)) return hr;

				 layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fontSize * lineSpacingMul, fontSize * 0.80f);

				 if (i == lines.size() - 1)
				 {
					 CComPtr<IDWriteInlineObject> ellipsis;
					 if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(fmt, &ellipsis)))
						 layout->SetTrimming(&trim, ellipsis);
				 }
				 else
				 {
					 DWRITE_TRIMMING noTrim{};
					 noTrim.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
					 layout->SetTrimming(&noTrim, nullptr);
				 }

				 DWRITE_TEXT_METRICS tm{};
				 layout->GetMetrics(&tm);

				 float hLine = tm.height;
				 totalH += hLine;

				 built.push_back({ fmt, layout, tm, hLine, fontSize });

				 if (!bestFormatForBadge)
					 bestFormatForBadge = fmt;
			 }

			 float gap = Clamp(baseFont * 0.10f, 0.f, 3.f);
			 totalH += gap * (float)std::max<int>(0, (int)lines.size() - 1);

			 bestBuilt = std::move(built);

			 if (totalH <= textHMax)
				 break;
		 }

		 if (bestBuilt.empty()) return E_FAIL;

		 // total stacked height
		 float totalTextH = 0.f;
		 float gap = Clamp(bestBuilt[0].fontSize * 0.18f, 0.f, 3.f); // use font size, not tm.height
		 for (size_t i = 0; i < bestBuilt.size(); ++i)
		 {
			 totalTextH += bestBuilt[i].drawH;
			 if (i + 1 < bestBuilt.size()) totalTextH += gap;
		 }

		 float plateH = Clamp(totalTextH + padY * 2.f + stripReserve, 30.f, maxPlateH);
		 float left = Clamp((W - plateW) * 0.5f, outerMargin, W - plateW - outerMargin);

		 // Keep the plate visually "chip-like" even if text is short
		 const float minPlateH = Clamp(H * 0.85f, 90.f, H - outerMargin * 2.f); // The 0.85 can be tweaked to adjust height of plate and pins
		 if (plateH < minPlateH)
			 plateH = minPlateH;

		 float top = Clamp((H - plateH) * 0.5f, outerMargin, H - plateH - outerMargin);

		 D2D1_RECT_F plateRect = D2D1::RectF(left, top, left + plateW, top + plateH);

		 // Badge data
		 std::wstring badgeText = L"?";
		 bool diamond = false;
		 if (bitness == L"32-bit") { badgeText = L"32"; diamond = true; }
		 else if (bitness == L"64-bit") { badgeText = L"64"; diamond = false; }
		 else if (bitness == L"ARM64") { badgeText = L"A64"; diamond = false; }

		 float badgeSize = Clamp(H * 0.22f, 22.f, 44.f);
		 float badgeMargin = Clamp(H * 0.010f, 4.f, 10.f);

		 rt->BeginDraw();
		 rt->Clear(D2D1::ColorF(0, 0, 0, 0));

		 D2D1_RECT_F badgeRect = D2D1::RectF(
			 plateRect.right - badgeMargin - badgeSize,
			 plateRect.top + badgeMargin,
			 plateRect.right - badgeMargin,
			 plateRect.top + badgeMargin + badgeSize);

		 float cornerInset = cornerRadius * 0.55f;
		 DrawChipPins(rt, plateRect, pinThickness, pinLength, pinsPerSide, accentBrush, badgeRect, cornerInset);

		 rt->FillRoundedRectangle(D2D1::RoundedRect(plateRect, cornerRadius, cornerRadius), plateBrush);

		 // Draw stacked lines
		 float x0 = plateRect.left + padX;
		 float y = plateRect.top + padY;

		 for (size_t i = 0; i < bestBuilt.size(); ++i)
		 {
			 float remaining = (plateRect.bottom - stripReserve) - y - padY;
			 if (remaining < 10.f) remaining = 10.f;

			 if (i == bestBuilt.size() - 1)
			 {
				 // Rebuild last line with correct line spacing + height cap
				 const LineRun& lr = lines[i];

				 CComPtr<IDWriteTextLayout> capped;
				 hr = g_dw->CreateTextLayout(
					 lr.text.c_str(), (UINT32)lr.text.size(),
					 bestBuilt[i].fmt, textW, remaining, &capped);

				 if (SUCCEEDED(hr) && capped)
				 {
					 capped->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
						 bestBuilt[i].fontSize * lineSpacingMul,
						 bestBuilt[i].fontSize * 0.80f);

					 CComPtr<IDWriteInlineObject> ellipsis;
					 if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(bestBuilt[i].fmt, &ellipsis)))
						 capped->SetTrimming(&trim, ellipsis);

					 rt->DrawTextLayout(D2D1::Point2F(x0, y), capped, textBrush);
				 }
				 else
				 {
					 rt->DrawTextLayout(D2D1::Point2F(x0, y), bestBuilt[i].layout, textBrush);
				 }
			 }
			 else
			 {
				 rt->DrawTextLayout(D2D1::Point2F(x0, y), bestBuilt[i].layout, textBrush);
			 }

			 y += bestBuilt[i].drawH;
			 if (i + 1 < bestBuilt.size()) y += gap;
		 }

		 // Strip
		 if (stripReserve > 0.0f && showStrip && flavorText && *flavorText)
			 DrawBuildStrip(rt, g_dw, plateRect, stripReserve, std::wstring(flavorText), flavorColor, cornerRadius);

		 // Bitness badge
		 D2D1_COLOR_F accentColor = GetBitnessAccentColor(bitness, 1.0f);
		 DrawBitnessBadge(rt, g_dw,
			 bestFormatForBadge ? bestFormatForBadge : bestBuilt[0].fmt,
			 W, H, badgeText, diamond, badgeSize, badgeMargin, accentColor);

		 // Signature shield
		 float shieldSize = Clamp(H * 0.25f, 16.f, 35.f);
		 float sx = plateRect.right - shieldSize * 0.45f;
		 float sy = plateRect.bottom - shieldSize * 0.75f;
		 sy -= (stripReserve > 0.0f ? stripReserve * 0.15f : 0.0f);

		 if (sigKind == SigKind::Embedded)
			 DrawDigitalSignedShield(rt, sx, sy, shieldSize);
		 else if (sigKind == SigKind::Catalog)
			 DrawCatalogShield(rt, sx, sy, shieldSize);

		 hr = rt->EndDraw();
		 return hr;
	 }


    HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params,
        _COM_Outptr_result_maybenull_ IWICBitmapSource** lplpOutputImage) override
    {
        if (!lplpOutputImage) return E_POINTER;
        *lplpOutputImage = nullptr;

        IStream* pStream = m_context ? m_context->GetStream() : nullptr;
        if (!pStream) return E_FAIL;

        LARGE_INTEGER zero{};
        pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

        config.context = m_context;
        config.Load(false);

        BindLogConfig(&config.log);
        ClearLogContext();
        SetLogContextCall((int)params.desiredWidth);

        std::wstring path = GetPathFromStream(pStream);
        std::wstring bitness = L"Unknown";
        if (!path.empty())
            bitness = GetPEBitnessFromFile(path);

        CComPtr<IWICImagingFactory> wic;
        HRESULT hr = wic.CoCreateInstance(CLSID_WICImagingFactory);
        if (FAILED(hr)) return hr;

        CComPtr<IWICBitmap> bmp;
        hr = wic->CreateBitmap(params.desiredWidth, params.desiredHeight,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp);
        if (FAILED(hr)) return hr;

        std::map<std::wstring, std::wstring> kv;
        kv[L"DLLBitnessAsText"] = bitness;

		  // Build variables to use in template expansion
		  const wchar_t* keys[] =
		  {
				L"CompanyName", L"FileDescription", L"FileVersion",
				L"InternalName", L"LegalCopyright", 
				L"OriginalFilename", L"ProductName", L"ProductVersion",
				L"Comments", L"LegalTrademarks", L"PrivateBuild", L"SpecialBuild"
		  };

        VersionInfoBlob vi;
		  bool hasVersion = (!path.empty() && LoadVersionInfoBlob(path, vi));

		  kv[L"DLLFileVersionAsText"] =
			  hasVersion ?
			  (QueryFixedFileVersion(vi).empty() ? L"(no version)" : QueryFixedFileVersion(vi))
			  : L"(no version info)";

		  for (auto* k : keys)
		  {
			  // VI = This variable comes from VERSIONINFO!
			  std::wstring key = std::wstring(L"VI_") + k;

			  if (hasVersion)
				  kv[key] = QueryVersionString(vi, k);
			  else
				  kv[key] = L"";
		  }

        if (!path.empty()) {
           kv[L"DllFileType"] = GetDllFileTypeFromPath(path);
        }
        else
           kv[L"DllFileType"] = L"UNKNOWN";

		  kv[L"VI_FileDescriptionFirstSentence"] =
			  FirstSentenceOrTruncate(kv[L"VI_FileDescription"]);


        std::wstring expanded = ExpandTokens(config.templ, kv);
        NormalizeNewlinesInPlace(expanded);

		  BuildFlavor flavor = DetectBuildFlavor(path);

		  SigKind sigKind = DetectSigKind(path);

        hr = RenderTextThumb(config, bmp, params.desiredWidth, params.desiredHeight, expanded, bitness, flavor, sigKind);
        if (FAILED(hr)) return hr;

        *lplpOutputImage = bmp.Detach();
        return S_OK;
    }
};

// -----------------------------------------------------------------------------
// Required exports
// -----------------------------------------------------------------------------
extern "C" DLLPLUGIN_API int Version()
{
    return MYSTICTHUMBS_PLUGIN_VERSION;
}

extern "C" DLLPLUGIN_API bool Initialize()
{
    if (!g_d2d)
    {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, &g_d2d);
        if (FAILED(hr)) return false;
    }

    if (!g_dw)
    {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&g_dw));
        if (FAILED(hr)) return false;
    }

    return true;
}

extern "C" DLLPLUGIN_API bool Shutdown()
{
    if (g_dw) { g_dw->Release();  g_dw = nullptr; }
    if (g_d2d) { g_d2d->Release(); g_d2d = nullptr; }
    return true;
}

extern "C" DLLPLUGIN_API IMysticThumbsPlugin* CreateInstance(_In_ IMysticThumbsPluginContext* context)
{
    CVCDLLPlugin* plugin = (CVCDLLPlugin*)CoTaskMemAlloc(sizeof(CVCDLLPlugin));
    if (!plugin) return nullptr;
    return new(plugin) CVCDLLPlugin(context);
}

extern "C" DLLPLUGIN_API bool PreventLoading(bool /*isDebugProcess*/)
{
    return false;
}

// DllMain: keep it minimal
DLLPLUGIN_API BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
        g_hModule = hModule;
    return TRUE;
}