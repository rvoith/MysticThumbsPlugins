//	 FFMpegPluginMysticThumbs.cpp

// Avoid the std::max error:
// error C2589: '(': illegal token on right side of '::'
#define NOMINMAX 

#include "FFMpegPluginMysticThumbs.h" 
#include "..\\Common\\MysticThumbsPlugin.h"
#include "..\\Common\\SharedMysticThumbsPlugin.h"
#include "resource.h"

// FFmpeg headers (installed via vcpkg like this:
// vcpkg install "ffmpeg[dav1d]:x64-windows"
// vcpkg install "ffmpeg[dav1d]:x86-windows" --allow-unsupported
// We use the in-process libraries: avformat + avcodec + swscale.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <Windows.h>
#include <Shlwapi.h>
#include <wincodec.h>
#include <strsafe.h>
#include <CommCtrl.h>
#include <Shobjidl.h>

#include <string>
#include <sstream>
#include <cstdarg>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>



// ----------------------------------------------------------------------------
// Explicit link libraries
// ----------------------------------------------------------------------------
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// FFmpeg import libraries (the DLLs must be shipped alongside the plugin).
// With vcpkg integration enabled, these are usually resolved automatically.
// Tips: Use LoadLibraryEx like this
// HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
// 	LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
// 	LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
// 	LOAD_LIBRARY_SEARCH_USER_DIRS);
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

// Optional (may be needed depending on your build/features)
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "avdevice.lib")

#pragma comment(lib, "Version.lib")


	// ----------------------------------------------------------------------------
	// Plugin identity / supported extensions
	// ----------------------------------------------------------------------------
	static const wchar_t* s_name = L"Voith's CODE FFMpeg Plugin";

	// MysticThumbs matches files to plugins by extension. 
	static const LPCWSTR s_extensions[] = {
		// Video containers
		L".mp4", L".m4v", L".mov", L".mkv", L".avi", L".wmv", L".flv", L".webm", L".mpg", L".mpeg",
		L".ts", L".m2ts", L".mts",
		// Image / animation
		L".webp",
		// Audio-only (will currently return nullptr unless you add a waveform renderer)
		L".mp3", L".aac", L".m4a", L".flac", L".ogg", L".wav"
	};

	static const GUID s_guid =
	{ 0xeca56b2f, 0xbb26, 0x4507, { 0x8d, 0x3c, 0x7f, 0x25, 0x96, 0x8d, 0x34, 0xf8 } };

	// Build-time bitness
#if defined(_WIN64) 
	constexpr wchar_t* kBitness = L"64-bit";
#else
	constexpr wchar_t* kBitness = L"32-bit";
#endif


	// ----------------------------------------------------------------------------
	// Registry constants
	// ----------------------------------------------------------------------------
	static const wchar_t* REG_SETTINGS_KEY = L""; // Use MysticThumbs per-plugin registry root

	static const wchar_t* REG_LEAVE_TEMP = L"LeaveTempFiles";
	static const wchar_t* REG_SWAP_RB = L"SwapRB";
	static const wchar_t* REG_RETURN_DEBUG_FFMpeg_TN = L"ReturnDebugFFMpegThumbnail";
	static const wchar_t* REG_USE_DESIRED_SIZE_HINT = L"useDesiredSizeHint";
	static const wchar_t* REG_MAX_FFMpeg_DIM = L"maxFFMpegDim";
	static const wchar_t* REG_MAX_FFMpeg_BYTES = L"maxFFMpegBytes";

	static const wchar_t* REG_THUMB_ENABLED = L"ThumbEnable";
	static const wchar_t* REG_THUMB_SUBKEY = L"Thumbnailer";
	static const wchar_t* REG_THUMB_PATH = L"Path";
	static const wchar_t* REG_THUMB_PARAMS = L"Params";

	static const wchar_t* REG_COLLAGE_ENABLE = L"Collage4";
	static const wchar_t* REG_COLLAGE_MIN_SECONDS = L"CollageMinS";


	static HMODULE g_hModule = nullptr;


	class CFFMpegPlugin : public IMysticThumbsPlugin
	{
	private:
		MysticLogTag  m_logTag;
	
		// Valid for lifetime through the lifetime of the plugin instance until the end of Destroy
		IMysticThumbsPluginContext* m_context{};
		const IMysticThumbsLog* m_log{};

		struct PluginConfig
		{
			IMysticThumbsPluginContext* context;

			// Main config
			bool leaveTempFiles = false;
			bool returnDebugFFMpegThumbnail = false;
			bool useDesiredSizeHint = false;

			// Pixel format toggle:
			bool swapRB = true;

			// Collage settings
			bool collage4 = false;
			DWORD collageMinSeconds = 8;

			// External thumbnailer
			bool thumbEnabled = false;
			std::wstring thumbPath;
			std::wstring thumbParams;

			PluginConfig() = default;
			explicit PluginConfig(CFFMpegPlugin* p) { context = p ? p->m_context : nullptr; }

			void Load()
			{
				ATLASSUME(context);

				HKEY hRoot = context ? context->GetPluginRegistryRootKey() : nullptr;
				if (!hRoot)
					return;

				// IMPORTANT: Do not close MysticThumbs' HKEY.
				CRegKeyHelper<false> root(hRoot);

				// Defaults
				leaveTempFiles = false;
				swapRB = true;
				returnDebugFFMpegThumbnail = false;
				useDesiredSizeHint = false;

				collage4 = false;
				collageMinSeconds = 8;


				DWORD d = 0;
				if (root.QueryDWORDValue(REG_LEAVE_TEMP, d) == ERROR_SUCCESS) leaveTempFiles = (d != 0);
				if (root.QueryDWORDValue(REG_SWAP_RB, d) == ERROR_SUCCESS) swapRB = (d != 0);
				if (root.QueryDWORDValue(REG_RETURN_DEBUG_FFMpeg_TN, d) == ERROR_SUCCESS) returnDebugFFMpegThumbnail = (d != 0);
				if (root.QueryDWORDValue(REG_USE_DESIRED_SIZE_HINT, d) == ERROR_SUCCESS) useDesiredSizeHint = (d != 0);

				if (root.QueryDWORDValue(REG_COLLAGE_ENABLE, d) == ERROR_SUCCESS) collage4 = (d != 0);
				if (root.QueryDWORDValue(REG_COLLAGE_MIN_SECONDS, d) == ERROR_SUCCESS) collageMinSeconds = d;

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
			}

			void Save(HWND hDlg) const
			{
				HKEY hRoot = context ? context->GetPluginRegistryRootKey() : nullptr;
				if (!hRoot)
					return;

				CRegKeyHelper<false> root(hRoot);

				(void)root.SetDWORDValue(REG_LEAVE_TEMP, leaveTempFiles ? 1u : 0u);
				(void)root.SetDWORDValue(REG_SWAP_RB, swapRB ? 1u : 0u);
				(void)root.SetDWORDValue(REG_RETURN_DEBUG_FFMpeg_TN, returnDebugFFMpegThumbnail ? 1u : 0u);
				(void)root.SetDWORDValue(REG_USE_DESIRED_SIZE_HINT, useDesiredSizeHint ? 1u : 0u);

				(void)root.SetDWORDValue(REG_COLLAGE_ENABLE, collage4 ? 1u : 0u);
				(void)root.SetDWORDValue(REG_COLLAGE_MIN_SECONDS, collageMinSeconds);

				CRegKeyHelper<> sub;
				if (sub.Create(root, REG_THUMB_SUBKEY) == ERROR_SUCCESS)
				{
					(void)sub.SetDWORDValue(REG_THUMB_ENABLED, thumbEnabled ? 1u : 0u);
					(void)sub.SetStringValue(REG_THUMB_PATH, thumbPath.c_str());
					(void)sub.SetStringValue(REG_THUMB_PARAMS, thumbParams.c_str());
				}
			}
		} config;

		static INT_PTR CALLBACK ConfigureDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

public:
		explicit CFFMpegPlugin(_In_ IMysticThumbsPluginContext* context)
			: m_context(context), config(this)
		{
			m_log = context->Log();
		}

		const IMysticThumbsPluginContext* GetContext() const
		{
			return m_context;
		}

		virtual LPCWSTR GetName() const override
		{
			return s_name;
		}

		virtual LPCGUID GetGuid() const override
		{
			return &s_guid;
		}

		virtual LPCWSTR GetDescription() const override
		{
			return L"Plugin creates thumbnails for video/media files using FFmpeg (and optional external thumbnailer fallback).";
		}

		virtual LPCWSTR GetAuthor() const override
		{
			return L"Voith's CODE\nwww.vcode.no";
		}

		virtual unsigned int GetExtensionCount() const override
		{
			return ARRAYSIZE(s_extensions);
		}
		 
		virtual LPCWSTR GetExtension(_In_ unsigned int index) const		override
		{
			if (index >= ARRAYSIZE(s_extensions))
				return L"";
			return s_extensions[index];
		}

		virtual void Destroy() override
		{
			this->~CFFMpegPlugin();
			CoTaskMemFree(this);
		}

		virtual bool Ping(_Inout_ MysticThumbsPluginPing& ping) override
		{

			config.Load();			

			const unsigned int pingSizeHint = (ping.requestedWidth && ping.requestedHeight) ? std::max(1u, std::min(ping.requestedWidth, ping.requestedHeight)) : 0u;
			m_logTag.UpdateFromStream(m_context ? m_context->GetStream() : nullptr, pingSizeHint);

			IStream* pStream = m_context ? m_context->GetStream() : nullptr;
			if (!pStream)
				if (m_log) m_log->logf(L"%sPing: context stream is null", m_logTag.Tag());

			if (m_log) m_log->logf(L"%sPing (FFMpeg): File \"%s\"", m_logTag.Tag(), m_logTag.Name());

			// Probe real media dimensions (preferred in V4) without generating a thumbnail.
			unsigned int w = 0, h = 0;

			LARGE_INTEGER cur{};
			ULARGE_INTEGER curPos{};
			if (pStream)
			{
				// Save current stream position and rewind for file extraction
				pStream->Seek(cur, STREAM_SEEK_CUR, &curPos);
			}

			std::wstring tempDir = GetTempDirectory(L"Voith's CODE\\FFMpeg Plugin MysticThumbs\\");
			std::wstring mediaPath, pngPath;
			bool wroteTemp = false;

			if (pStream && MakeTempFilePair(tempDir, L"tmpv", mediaPath, pngPath))
			{
				wroteTemp = WriteStreamToFile(pStream, mediaPath);
				if (!wroteTemp)
				{
					if (m_log) m_log->logf(L"%sPing: WriteStreamToFile failed", m_logTag.Tag());
				}
			}

			// Restore stream position so Generate() can read from the beginning again if needed.
			if (pStream)
			{
				LARGE_INTEGER back{};
				back.QuadPart = (LONGLONG)curPos.QuadPart;
				pStream->Seek(back, STREAM_SEEK_SET, nullptr);
			}

			if (wroteTemp)
			{
				// Open with FFmpeg and read codec parameters (fast).
				AVDictionary* opts = nullptr;
				av_dict_set(&opts, "protocol_whitelist", "file", 0);

				std::string pathUtf8 = WideToUtf8(mediaPath);
				AVFormatContext* fmt = nullptr;
				int rc = avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, &opts);
				av_dict_free(&opts);

				if (rc >= 0 && fmt)
				{
					rc = avformat_find_stream_info(fmt, nullptr);
					if (rc >= 0)
					{
						const AVCodec* dec = nullptr;
						const int vIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
						if (vIndex >= 0 && vIndex < (int)fmt->nb_streams)
						{
							AVStream* st = fmt->streams[vIndex];
							if (st && st->codecpar)
							{
								w = (unsigned int)std::max(0, st->codecpar->width);
								h = (unsigned int)std::max(0, st->codecpar->height);
							}
						}
					}
					avformat_close_input(&fmt);
				}

				if (!config.leaveTempFiles)
				{
					DeleteFileW(mediaPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
			}

			// If probing failed, fall back to requested size hint (or 256).
			if (w == 0 || h == 0)
			{
				unsigned int fallback = 256;
				if (ping.requestedWidth && ping.requestedHeight)
					fallback = std::max(1u, std::min(ping.requestedWidth, ping.requestedHeight));
				w = h = fallback;
			}

			ping.width = w;
			ping.height = h;
			ping.bitDepth = 32;
			return true;
		}

		virtual bool GetCapabilities(_Out_ MysticThumbsPluginCapabilities& capabilities) override
		{
			capabilities = {};
			capabilities |= PluginCapabilities_CanConfigure | PluginCapabilities_IsProcedural;
			return true;
		}

		bool Configure(_In_ HWND hWndParent) override
		{
			INT_PTR result = DialogBoxParamW(g_hModule,
				MAKEINTRESOURCE(IDD_FFMPEG_PLUGIN_CONFIGURE),
				hWndParent,
				ConfigureDialogProc,
				(LPARAM)this);

			return result == IDOK;
		}

		bool NormalizeRgbaFillInMemory(std::wstring& /*contents*/)
		{
			// Keep it as a future tool: implement/enable only when needed.
			// Returning false by default preserves behavior unless you expand it.
			return false;
		}

		bool ComputeFit(unsigned int srcW, unsigned int srcH, unsigned int maxSide,
			unsigned int& outW, unsigned int& outH)
		{
			if (srcW == 0 || srcH == 0 || maxSide == 0)
				return false;

			const double scaleW = (double)maxSide / (double)srcW;
			const double scaleH = (double)maxSide / (double)srcH;
			const double scale = (scaleW < scaleH) ? scaleW : scaleH;

			outW = (unsigned int)std::max(1.0, std::floor(srcW * scale + 0.5));
			outH = (unsigned int)std::max(1.0, std::floor(srcH * scale + 0.5));
			return true;
		}

		bool DecodeFrameNearTimestamp(
			AVFormatContext* fmt,
			AVCodecContext* cc,
			int vIndex,
			AVFrame* outFrame,
			int64_t targetTs,              // stream time_base units
			int maxReadPackets = 2000,
			int maxDecodedFrames = 200
		)
		{
			if (!fmt || !cc || !outFrame || vIndex < 0) return false;

			// Prefer avformat_seek_file for MP4; fall back to av_seek_frame.
			int seekRc = avformat_seek_file(fmt, vIndex, INT64_MIN, targetTs, INT64_MAX, AVSEEK_FLAG_BACKWARD);
			if (seekRc < 0)
				seekRc = av_seek_frame(fmt, vIndex, targetTs, AVSEEK_FLAG_BACKWARD);

			if (seekRc < 0)
				return false;

			// Reset demuxer + decoder state after seek
			avformat_flush(fmt);
			avcodec_flush_buffers(cc);

			AVPacket* pkt = av_packet_alloc();
			if (!pkt) return false;

			bool got = false;
			int decodedCount = 0;
			bool sentEof = false;

			auto Drain = [&]() -> int
				{
					while (true)
					{
						int rc = avcodec_receive_frame(cc, outFrame);
						if (rc == 0)
						{
							++decodedCount;
							got = true;
							return 0;
						}
						if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
							return rc;
						return rc; // hard error
					}
				};

			for (int i = 0; i < maxReadPackets && !got; ++i)
			{
				int rcRead = av_read_frame(fmt, pkt);

				if (rcRead == AVERROR_EOF)
				{
					if (!sentEof)
					{
						// Signal end-of-stream to decoder once
						(void)avcodec_send_packet(cc, nullptr);
						sentEof = true;
					}

					// Drain whatever is left
					(void)Drain();
					break;
				}
				else if (rcRead < 0)
				{
					// Hard demux error: try draining once, then bail
					(void)Drain();
					break;
				}

				// Non-video packet? Skip it.
				if (pkt->stream_index != vIndex)
				{
					av_packet_unref(pkt);
					continue;
				}

				// Send packet with proper EAGAIN handling
				while (true)
				{
					int rcSend = avcodec_send_packet(cc, pkt);
					if (rcSend == 0)
					{
						// Packet accepted; we can unref it and then drain frames
						av_packet_unref(pkt);
						break;
					}
					if (rcSend == AVERROR(EAGAIN))
					{
						// Need to drain then retry sending the SAME packet
						int rcDrain = Drain();
						if (got) break;
						if (rcDrain < 0 && rcDrain != AVERROR(EAGAIN) && rcDrain != AVERROR_EOF)
						{
							av_packet_unref(pkt);
							av_packet_free(&pkt);
							return false;
						}
						continue;
					}

					// Hard send error
					av_packet_unref(pkt);
					av_packet_free(&pkt);
					return false;
				}

				if (got) break;

				// Drain after sending packet
				int rcDrain = Drain();
				if (got) break;

				if (decodedCount >= maxDecodedFrames)
					break;

				if (rcDrain < 0 && rcDrain != AVERROR(EAGAIN) && rcDrain != AVERROR_EOF)
					break;
			}

			av_packet_free(&pkt);
			return got;
		}

		bool LooksBlankOrNearBlackRGBA(const unsigned char* rgba, unsigned int w, unsigned int h)
		{
			if (!rgba || w == 0 || h == 0) return true;

			const unsigned int stepX = (w > 128) ? (w / 128) : 1;
			const unsigned int stepY = (h > 128) ? (h / 128) : 1;

			uint64_t sumY = 0, sumYSq = 0, sumA = 0, n = 0;

			for (unsigned int y = 0; y < h; y += stepY)
			{
				const unsigned char* row = rgba + (size_t)y * (size_t)w * 4;
				for (unsigned int x = 0; x < w; x += stepX)
				{
					const unsigned char* p = row + (size_t)x * 4;

					const int R = p[0], G = p[1], B = p[2], A = p[3];

					// Luma approximation
					const int Y = (int)(R * 54 + G * 183 + B * 19) >> 8;

					sumY += (uint64_t)Y;
					sumYSq += (uint64_t)Y * (uint64_t)Y;
					sumA += (uint64_t)A;
					++n;
				}
			}

			if (n == 0) return true;

			const double meanY = (double)sumY / (double)n;
			const double varY = (double)sumYSq / (double)n - meanY * meanY;
			const double meanA = (double)sumA / (double)n;

			// If it's mostly transparent, Explorer will show it as black -> treat as blank.
			if (meanA < 10.0) return true;

			// Near-black or too-flat
			return (meanY < 10.0) || (varY < 5.0);
		}

		void ComputeAvgColorRGBA(
			const unsigned char* rgba, unsigned int w, unsigned int h,
			unsigned char& outR, unsigned char& outG, unsigned char& outB)
		{
			if (!rgba || w == 0 || h == 0)
			{
				outR = outG = outB = 24; // fallback dark gray
				return;
			}

			// Sample grid for speed
			const unsigned int stepX = (w > 64) ? (w / 64) : 1;
			const unsigned int stepY = (h > 64) ? (h / 64) : 1;

			uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0, n = 0;

			for (unsigned int y = 0; y < h; y += stepY)
			{
				const unsigned char* row = rgba + (size_t)y * (size_t)w * 4;
				for (unsigned int x = 0; x < w; x += stepX)
				{
					const unsigned char* p = row + (size_t)x * 4;
					sumR += p[0];
					sumG += p[1];
					sumB += p[2];
					sumA += p[3];
					++n;
				}
			}

			if (n == 0)
			{
				outR = outG = outB = 24;
				return;
			}

			// If mostly transparent, fallback
			const double meanA = (double)sumA / (double)n;
			if (meanA < 10.0)
			{
				outR = outG = outB = 24;
				return;
			}

			outR = (unsigned char)(sumR / n);
			outG = (unsigned char)(sumG / n);
			outB = (unsigned char)(sumB / n);

			// Slightly darken so the content "pops"
			auto darken = [](unsigned char c) -> unsigned char
				{
					int v = (int)(c * 80 / 100); // 80%
					if (v < 0) v = 0;
					if (v > 255) v = 255;
					return (unsigned char)v;
				};
			outR = darken(outR);
			outG = darken(outG);
			outB = darken(outB);
		}

		void FillTileRGBA(unsigned char* dstTile, int tileW, int tileH, int strideBytes,
			unsigned char r, unsigned char g, unsigned char b, unsigned char a)
		{
			for (int y = 0; y < tileH; ++y)
			{
				unsigned char* row = dstTile + (size_t)y * (size_t)strideBytes;
				for (int x = 0; x < tileW; ++x)
				{
					row[x * 4 + 0] = r;
					row[x * 4 + 1] = g;
					row[x * 4 + 2] = b;
					row[x * 4 + 3] = a;
				}
			}
		}

		bool ScaleFrameToRgba(
			AVFrame* src,
			unsigned char* dstRgba,
			int dstW, int dstH,
			int dstStrideBytes
		)
		{
			SwsContext* sws = sws_getContext(
				src->width, src->height, (AVPixelFormat)src->format,
				dstW, dstH, AV_PIX_FMT_RGBA,
				SWS_BILINEAR, nullptr, nullptr, nullptr);

			if (!sws) return false;

			uint8_t* dstData[4] = { dstRgba, nullptr, nullptr, nullptr };
			int dstLinesize[4] = { dstStrideBytes, 0, 0, 0 };

			sws_scale(sws, src->data, src->linesize, 0, src->height, dstData, dstLinesize);
			sws_freeContext(sws);
			return true;
		}

		bool ScaleFrameToRgbaLetterboxed(
			AVFrame* frame,
			unsigned char* dstTile, int tileW, int tileH, int tileStrideBytes,
			unsigned char bgR, unsigned char bgG, unsigned char bgB, unsigned char bgA) // bgA=set 0 if you want transparent bars
		{
			if (!frame || !dstTile || tileW <= 0 || tileH <= 0 || tileStrideBytes < tileW * 4)
				return false;

			// Fill tile (black bars by default)
			for (int y = 0; y < tileH; ++y)
			{
				unsigned char* row = dstTile + (size_t)y * (size_t)tileStrideBytes;
				for (int x = 0; x < tileW; ++x)
				{
					row[x * 4 + 0] = bgR;
					row[x * 4 + 1] = bgG;
					row[x * 4 + 2] = bgB;
					row[x * 4 + 3] = bgA;
				}
			}

			const unsigned int srcW = (unsigned int)frame->width;
			const unsigned int srcH = (unsigned int)frame->height;
			if (srcW == 0 || srcH == 0) return false;

			unsigned int fitW = (unsigned int)tileW;
			unsigned int fitH = (unsigned int)tileH;
			(void)ComputeFit(srcW, srcH, (unsigned int)std::min(tileW, tileH), fitW, fitH);

			// MAY BE CHANGED - fits using a single max side
			{
				const double sx = (double)tileW / (double)srcW;
				const double sy = (double)tileH / (double)srcH;
				const double s = (sx < sy) ? sx : sy;
				fitW = (unsigned int)std::max(1.0, std::floor(srcW * s + 0.5));
				fitH = (unsigned int)std::max(1.0, std::floor(srcH * s + 0.5));
			}

			const int offX = (tileW - (int)fitW) / 2;
			const int offY = (tileH - (int)fitH) / 2;

			std::vector<unsigned char> fitBuf((size_t)fitW * (size_t)fitH * 4);
			if (!ScaleFrameToRgba(frame, fitBuf.data(), (int)fitW, (int)fitH, (int)fitW * 4))
				return false;

			// Blit fitBuf into centered location
			for (unsigned int y = 0; y < fitH; ++y)
			{
				unsigned char* dstRow = dstTile + (size_t)(offY + (int)y) * (size_t)tileStrideBytes + (size_t)offX * 4;
				unsigned char* srcRow = fitBuf.data() + (size_t)y * (size_t)fitW * 4;
				memcpy(dstRow, srcRow, (size_t)fitW * 4);
			}

			return true;
		}

		bool ScaleFrameToRgbaLetterboxedAutoBg(
			AVFrame* frame,
			unsigned char* dstTile, int tileW, int tileH, int tileStrideBytes)
		{
			if (!frame || !dstTile || tileW <= 0 || tileH <= 0 || tileStrideBytes < tileW * 4)
				return false;

			const unsigned int srcW = (unsigned int)frame->width;
			const unsigned int srcH = (unsigned int)frame->height;
			if (srcW == 0 || srcH == 0) return false;

			// --- 1) Probe scale for average color ---
			unsigned int probeW = 96, probeH = 96;
			// fit probe to maintain aspect
			{
				const double sx = (double)probeW / (double)srcW;
				const double sy = (double)probeH / (double)srcH;
				const double s = (sx < sy) ? sx : sy;
				probeW = (unsigned int)std::max(1.0, std::floor(srcW * s + 0.5));
				probeH = (unsigned int)std::max(1.0, std::floor(srcH * s + 0.5));
			}

			std::vector<unsigned char> probe((size_t)probeW * (size_t)probeH * 4);
			unsigned char bgR = 24, bgG = 24, bgB = 24;

			if (ScaleFrameToRgba(frame, probe.data(), (int)probeW, (int)probeH, (int)probeW * 4))
				ComputeAvgColorRGBA(probe.data(), probeW, probeH, bgR, bgG, bgB);

			// --- 2) Fill tile with auto background ---
			FillTileRGBA(dstTile, tileW, tileH, tileStrideBytes, bgR, bgG, bgB, 255);

			// --- 3) Fit content into tile (preserve aspect) ---
			unsigned int fitW, fitH;
			{
				const double sx = (double)tileW / (double)srcW;
				const double sy = (double)tileH / (double)srcH;
				const double s = (sx < sy) ? sx : sy;
				fitW = (unsigned int)std::max(1.0, std::floor(srcW * s + 0.5));
				fitH = (unsigned int)std::max(1.0, std::floor(srcH * s + 0.5));
			}

			const int offX = (tileW - (int)fitW) / 2;
			const int offY = (tileH - (int)fitH) / 2;

			std::vector<unsigned char> fitBuf((size_t)fitW * (size_t)fitH * 4);
			if (!ScaleFrameToRgba(frame, fitBuf.data(), (int)fitW, (int)fitH, (int)fitW * 4))
				return false;

			// --- 4) Blit centered ---
			for (unsigned int y = 0; y < fitH; ++y)
			{
				unsigned char* dstRow = dstTile + (size_t)(offY + (int)y) * (size_t)tileStrideBytes + (size_t)offX * 4;
				unsigned char* srcRow = fitBuf.data() + (size_t)y * (size_t)fitW * 4;
				memcpy(dstRow, srcRow, (size_t)fitW * 4);
			}

			return true;
		}


		virtual HRESULT Generate(_Inout_ MysticThumbsPluginGenerateParams& params,
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

			bool hasAlpha = false;
			unsigned int w = 0, h = 0;

			// Reuse your existing pipeline, which returns LocalAlloc RGBA.
			unsigned char* rgba = GenerateImage(
				pStream,
				desiredSize,
				(unsigned int)params.flags,
				hasAlpha,
				w,
				h);

			if (!rgba || w == 0 || h == 0)
				return E_FAIL;

			// Wrap raw RGBA into an IWICBitmap
			CComPtr<IWICImagingFactory> factory;
			HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&factory));
			if (FAILED(hr) || !factory)
			{
				LocalFree(rgba);
				return E_FAIL;
			}

			const UINT stride = (UINT)(w * 4);
			const UINT bufSize = (UINT)(stride * h);

			CComPtr<IWICBitmap> bmp;
			hr = factory->CreateBitmapFromMemory(
				w, h,
				GUID_WICPixelFormat32bppRGBA,
				stride,
				bufSize,
				rgba,
				&bmp);

			// MysticThumbs owns the IWICBitmapSource; BUT *I* own rgba buffer.
			// CreateBitmapFromMemory does NOT copy by default; it references the buffer.
			// So I cannot free rgba here unless I force a copy.
			//
			// Solution: create a real WIC bitmap and CopyPixels into it.

			if (SUCCEEDED(hr) && bmp)
			{
				CComPtr<IWICBitmap> owned;
				hr = factory->CreateBitmap(
					w,
					h,
					config.swapRB ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat32bppBGRA,
					WICBitmapCacheOnLoad,
					&owned);

				if (FAILED(hr) || !owned)
				{
					LocalFree(rgba);
					return E_FAIL;
				}

				WICRect rc{ 0, 0, (INT)w, (INT)h };

				CComPtr<IWICBitmapLock> lock;
				hr = owned->Lock(&rc, WICBitmapLockWrite, &lock);
				if (FAILED(hr) || !lock)
				{
					LocalFree(rgba);
					return E_FAIL;
				}

				UINT stride = 0;
				UINT bufferSize = 0;
				BYTE* dst = nullptr;

				hr = lock->GetStride(&stride);
				if (FAILED(hr)) { LocalFree(rgba); return E_FAIL; }

				hr = lock->GetDataPointer(&bufferSize, &dst);
				if (FAILED(hr)) { LocalFree(rgba); return E_FAIL; }

				// Copy row by row (stride-safe)
				const UINT srcStride = w * 4;
				for (UINT y = 0; y < h; ++y)
				{
					memcpy(dst + y * stride, rgba + y * srcStride, srcStride);
				}

				// Now WIC owns the pixels
				LocalFree(rgba);

				*lplpOutputImage = owned.Detach();
				return S_OK;

			}

			LocalFree(rgba);
			return E_FAIL;
		}


		unsigned char* RenderWithFFmpegCollage4FromFile(
			const std::wstring& mediaPath,
			unsigned int desiredSize,       // overall output size (square)
			bool useDesiredSize,
			bool& hasAlpha,
			unsigned int& width,
			unsigned int& height)
		{
			hasAlpha = false;
			width = height = 0;

			if (desiredSize == 0) desiredSize = 256;

			// Decide final output size (square is ideal for collage)
			unsigned int outW = desiredSize;
			unsigned int outH = desiredSize;

			if (!useDesiredSize)
				outW = outH = desiredSize;

			// Tile size (2x2)
			const unsigned int tileW = std::max(1u, outW / 2);
			const unsigned int tileH = std::max(1u, outH / 2);

			// Memory cap check
			const size_t bufSize = (size_t)outW * (size_t)outH * 4;

			// FFmpeg open (file only)
			AVDictionary* opts = nullptr;
			av_dict_set(&opts, "protocol_whitelist", "file", 0);

			std::string pathUtf8 = WideToUtf8(mediaPath);
			if (pathUtf8.empty())
			{
				av_dict_free(&opts);
				return nullptr;
			}

			AVFormatContext* fmt = nullptr;
			int rc = avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, &opts);
			av_dict_free(&opts);
			if (rc < 0 || !fmt)
				return nullptr;

			rc = avformat_find_stream_info(fmt, nullptr);
			if (rc < 0)
			{
				avformat_close_input(&fmt);
				return nullptr;
			}

			const AVCodec* dec = nullptr;
			const int vIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
			if (vIndex < 0 || !dec)
			{
				avformat_close_input(&fmt);
				return nullptr;
			}

			AVCodecContext* cc = avcodec_alloc_context3(dec);
			if (!cc)
			{
				avformat_close_input(&fmt);
				return nullptr;
			}

			rc = avcodec_parameters_to_context(cc, fmt->streams[vIndex]->codecpar);
			if (rc < 0)
			{
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Keep it predictable in Explorer
			cc->thread_count = 1;
			cc->thread_type = 0;

			rc = avcodec_open2(cc, dec, nullptr);
			if (rc < 0)
			{
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Require valid duration for % seeking
			if (fmt->duration <= 0 || fmt->duration == AV_NOPTS_VALUE)
			{
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			const double durationSeconds = (double)fmt->duration / (double)AV_TIME_BASE;
			if (durationSeconds < (double)config.collageMinSeconds)
			{
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Allocate final output
			unsigned char* out = (unsigned char*)LocalAlloc(LMEM_FIXED, bufSize);
			if (!out)
			{
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}
			memset(out, 0, bufSize);

			AVFrame* frame = av_frame_alloc();
			if (!frame)
			{
				LocalFree(out);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			struct Tile { int x; int y; double pct; };
			Tile tiles[4] = {
				{ 0,          0,          -1.0 }, // special: avoid 0% blink (use max(1s, 2%))
				{ (int)tileW, 0,           0.25 },
				{ 0,          (int)tileH,  0.50 },
				{ (int)tileW, (int)tileH,  0.75 },
			};

			AVStream* st = fmt->streams[vIndex];

			auto ProbeLooksBlankFrame = [&](AVFrame* fr) -> bool
				{
					if (!fr || fr->width <= 0 || fr->height <= 0) return true;

					unsigned int srcW = (unsigned int)fr->width;
					unsigned int srcH = (unsigned int)fr->height;

					unsigned int probeW = 160, probeH = 160;
					(void)ComputeFit(srcW, srcH, 160, probeW, probeH);
					if (probeW == 0 || probeH == 0) return true;

					// Note that I have multiple options for tiles:
					// ScaleFrameToRgba: direct scale to probe size
					// ScaleFrameToRgbaLetterboxed: scale with bars to probe size
					// ScaleFrameToRgbaLetterboxedAutoBg: scale with auto-bg to probe size
					std::vector<unsigned char> probe((size_t)probeW * (size_t)probeH * 4);
					if (!ScaleFrameToRgbaLetterboxedAutoBg(fr, probe.data(), (int)probeW, (int)probeH, (int)probeW * 4))
					{
						// If we can't probe-convert, don't reject
						return false;
					}

					return LooksBlankOrNearBlackRGBA(probe.data(), probeW, probeH);
				};

			auto DecodeAtTsWithBlankRetries = [&](int64_t tsBase) -> bool
				{
					// Try base ts
					av_frame_unref(frame);
					if (DecodeFrameNearTimestamp(fmt, cc, vIndex, frame, tsBase) && !ProbeLooksBlankFrame(frame))
						return true;

					// Retry at +0.5s and +1.0s (converted to stream time_base)
					const int64_t halfSec = av_rescale_q((int64_t)(AV_TIME_BASE / 2), AV_TIME_BASE_Q, st->time_base);
					const int64_t oneSec = av_rescale_q((int64_t)(AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);

					av_frame_unref(frame);
					if (DecodeFrameNearTimestamp(fmt, cc, vIndex, frame, tsBase + halfSec) && !ProbeLooksBlankFrame(frame))
						return true;

					av_frame_unref(frame);
					if (DecodeFrameNearTimestamp(fmt, cc, vIndex, frame, tsBase + oneSec) && !ProbeLooksBlankFrame(frame))
						return true;

					return false;
				};

			for (int i = 0; i < 4; ++i)
			{
				// Compute t_us in AV_TIME_BASE units
				int64_t t_us = 0;

				if (tiles[i].pct < 0.0)
				{
					// Avoid start blink: max(1s, 2% duration)
					const int64_t t1s = 1 * AV_TIME_BASE;
					const int64_t t2pct = (int64_t)((double)fmt->duration * 0.02);
					t_us = std::max(t1s, t2pct);
				}
				else
				{
					t_us = (int64_t)((double)fmt->duration * tiles[i].pct);
				}

				// AV_TIME_BASE -> stream time_base, and add start_time offset
				int64_t ts = av_rescale_q(t_us, AV_TIME_BASE_Q, st->time_base);
				if (st->start_time != AV_NOPTS_VALUE)
					ts += st->start_time;

				if (!DecodeAtTsWithBlankRetries(ts))
				{
					// Leave tile black
					continue;
				}

				// Destination pointer for tile (top-left pixel)
				unsigned char* tilePtr =
					out + (size_t)tiles[i].y * (size_t)outW * 4 + (size_t)tiles[i].x * 4;

				// Scale decoded frame into a contiguous tile buffer then blit row-wise
				std::vector<unsigned char> tileBuf((size_t)tileW * (size_t)tileH * 4);

				if (!ScaleFrameToRgbaLetterboxedAutoBg(frame, tileBuf.data(), (int)tileW, (int)tileH, (int)tileW * 4))
					continue;

				for (unsigned int y = 0; y < tileH; ++y)
				{
					unsigned char* dstRow = tilePtr + (size_t)y * (size_t)outW * 4;
					unsigned char* srcRow = tileBuf.data() + (size_t)y * (size_t)tileW * 4;
					memcpy(dstRow, srcRow, (size_t)tileW * 4);
				}
			}

			av_frame_free(&frame);
			avcodec_free_context(&cc);
			avformat_close_input(&fmt);

			hasAlpha = true;
			width = outW;
			height = outH;
			return out;
		}

		unsigned char* RenderWithFFmpegFromFile(
			const std::wstring& mediaPath,
			unsigned int desiredSize,
			bool useDesiredSize,
			bool& hasAlpha,
			unsigned int& width,
			unsigned int& height)
		{
			hasAlpha = false;
			width = height = 0;

			if (desiredSize == 0)
				desiredSize = 256;

			// I open a local temp file, so restrict protocols to file only.
			AVDictionary* opts = nullptr;
			av_dict_set(&opts, "protocol_whitelist", "file", 0);

			std::string pathUtf8 = WideToUtf8(mediaPath);
			if (pathUtf8.empty())
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: WideToUtf8 failed", m_logTag.Tag());
				av_dict_free(&opts);
				return nullptr;
			}

			AVFormatContext* fmt = nullptr;
			int rc = avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, &opts);
			av_dict_free(&opts);
			if (rc < 0 || !fmt)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: avformat_open_input failed", m_logTag.Tag());
				return nullptr;
			}

			rc = avformat_find_stream_info(fmt, nullptr);
			if (rc < 0)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: avformat_find_stream_info failed", m_logTag.Tag());
				avformat_close_input(&fmt);
				return nullptr;
			}

			const AVCodec* dec = nullptr;
			const int vIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
			if (vIndex < 0 || !dec)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: no video stream (audio-only not supported yet", m_logTag.Tag());
				avformat_close_input(&fmt);
				return nullptr;
			}

			AVCodecContext* cc = avcodec_alloc_context3(dec);
			if (!cc)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: avcodec_alloc_context3 failed", m_logTag.Tag());
				avformat_close_input(&fmt);
				return nullptr;
			}

			rc = avcodec_parameters_to_context(cc, fmt->streams[vIndex]->codecpar);
			if (rc < 0)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: avcodec_parameters_to_context failed", m_logTag.Tag());
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Keep it predictable in Explorer.
			cc->thread_count = 1;
			cc->thread_type = 0;

			rc = avcodec_open2(cc, dec, nullptr);
			if (rc < 0)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: avcodec_open2 failed", m_logTag.Tag());
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Report decoder info
			if (m_log) m_log->logf(L"%sOpened decoder: %S", m_logTag.Tag(), dec->name);
			if (m_log) m_log->logf(L"%sDecoder long name: %S", m_logTag.Tag(), dec->long_name ? dec->long_name : "(null)");
			if (m_log) m_log->logf(L"%sDecoder pix_fmts: %p", m_logTag.Tag(), dec->pix_fmts);

			// Once per process is enough, but here is OK for now
			if (m_log) m_log->logf(L"%sFFmpeg version: %S", m_logTag.Tag(), av_version_info());
			if (m_log) m_log->logf(L"%sFFmpeg config: %S", m_logTag.Tag(), avcodec_configuration());

			// Decode a representative frame by trying a few timestamps.
			// If the decoded frame looks blank/near-black, try another timestamp.
			AVPacket* pkt = av_packet_alloc();
			AVFrame* frame = av_frame_alloc();
			if (!pkt || !frame)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: av_packet_alloc/av_frame_alloc failed", m_logTag.Tag());
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			auto ProbeLooksBlank = [&](AVFrame* fr) -> bool
				{
					if (!fr || fr->width <= 0 || fr->height <= 0) return true;

					const unsigned int srcW = (unsigned int)fr->width;
					const unsigned int srcH = (unsigned int)fr->height;

					// Probe at a small size for speed (keeps aspect).
					unsigned int probeW = 160;
					unsigned int probeH = 160;
					(void)ComputeFit(srcW, srcH, 160, probeW, probeH);
					if (probeW == 0 || probeH == 0) return true;

					std::vector<unsigned char> probe((size_t)probeW * (size_t)probeH * 4, 0);

					SwsContext* swsProbe = sws_getContext(
						(int)srcW, (int)srcH, (AVPixelFormat)fr->format,
						(int)probeW, (int)probeH, AV_PIX_FMT_RGBA,
						SWS_BILINEAR, nullptr, nullptr, nullptr);

					if (!swsProbe)
					{
						// If I can't probe-convert, don't reject the frame as blank.
						return false;
					}

					AVFrame* dstProbe = av_frame_alloc();
					if (!dstProbe)
					{
						sws_freeContext(swsProbe);
						return false;
					}

					dstProbe->format = AV_PIX_FMT_RGBA;
					dstProbe->width = (int)probeW;
					dstProbe->height = (int)probeH;

					av_image_fill_arrays(dstProbe->data, dstProbe->linesize,
						probe.data(), AV_PIX_FMT_RGBA,
						(int)probeW, (int)probeH, 1);

					sws_scale(swsProbe, fr->data, fr->linesize, 0, (int)srcH,
						dstProbe->data, dstProbe->linesize);

					sws_freeContext(swsProbe);
					av_frame_free(&dstProbe);

					bool blank = LooksBlankOrNearBlackRGBA(probe.data(), probeW, probeH);
					if (blank) if (m_log) m_log->logf(L"%sProbeLooksBlank: rejected frame", m_logTag.Tag());
					return blank;
				};

			auto TryDecodeAtUs = [&](int64_t t_us) -> bool
				{
					// Ensure clean decoder state between tries
					avcodec_flush_buffers(cc);

					AVStream* st = fmt->streams[vIndex];
					int64_t ts = av_rescale_q(t_us, AV_TIME_BASE_Q, st->time_base);
					if (st->start_time != AV_NOPTS_VALUE)
						ts += st->start_time;

					if (!DecodeFrameNearTimestamp(fmt, cc, vIndex, frame, ts))
						return false;

					if (ProbeLooksBlank(frame))
						return false;

					return true;
				};

			bool gotFrame = false;

			// Duration is typically in microseconds (AV_TIME_BASE units) for AVFormatContext.
			const int64_t dur_us =
				(fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) ? fmt->duration : 0;

			const int64_t t1s = 1 * AV_TIME_BASE;
			int64_t t10 = (dur_us > 0) ? (dur_us / 10) : t1s;
			int64_t t50 = (dur_us > 0) ? (dur_us / 2) : t1s;
			int64_t t80 = (dur_us > 0) ? (dur_us * 8 / 10) : t1s;

			if (t10 < t1s) t10 = t1s;
			if (t50 < t1s) t50 = t1s;
			if (t80 < t1s) t80 = t1s;

			// Try: 10% -> middle -> later -> 1s
			const int64_t candidates[] = { t10, t50, t80, t1s };

			for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
			{
				if (TryDecodeAtUs(candidates[i]))
				{
					gotFrame = true;
					break;
				}
			}

			// Final fallback: if timestamp seeking fails (or only yields blank frames), try from beginning
			// and still reject blank/near-black.
			if (!gotFrame)
			{
				(void)av_seek_frame(fmt, vIndex, 0, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(cc);

				// Helper: drain decoder until it would block (or we got a frame)
				auto DrainOne = [&]() -> int
					{
						while (true)
						{
							int rr = avcodec_receive_frame(cc, frame);
							if (rr == 0)
							{
								// Reject blank frames if you want
								if (!ProbeLooksBlank(frame))
									gotFrame = true;
								return 0;
							}
							if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF)
								return rr;
							return rr; // hard error
						}
					};

				// Helper: send a packet (or nullptr) handling EAGAIN correctly
				auto SendWithEagain = [&](AVPacket* p) -> bool
					{
						while (true)
						{
							int sr = avcodec_send_packet(cc, p);
							if (sr == 0)
								return true;

							if (sr == AVERROR(EAGAIN))
							{
								// Need to drain, then retry sending the SAME packet
								int dr = DrainOne();
								if (gotFrame) return true; // we already have what we need
								if (dr < 0 && dr != AVERROR(EAGAIN) && dr != AVERROR_EOF)
									return false;
								continue;
							}

							// hard send error
							return false;
						}
					};

				bool sentEof = false;

				for (int readIters = 0; readIters < 2000 && !gotFrame; ++readIters)
				{
					rc = av_read_frame(fmt, pkt);

					if (rc == AVERROR_EOF)
					{
						if (!sentEof)
						{
							if (!SendWithEagain(nullptr))
								break;
							sentEof = true;
						}

						// Drain whatever remains
						(void)DrainOne();
						break;
					}
					else if (rc < 0)
					{
						// hard demux error: try draining once and stop
						(void)DrainOne();
						break;
					}

					if (pkt->stream_index != vIndex)
					{
						av_packet_unref(pkt);
						continue;
					}

					// Send packet safely (don’t drop on EAGAIN)
					bool ok = SendWithEagain(pkt);
					av_packet_unref(pkt);
					if (!ok)
						break;

					// Drain after sending
					(void)DrainOne();
				}

			}


			if (!gotFrame)
			{
				if (m_log) m_log->logf(
					L"%sRenderWithFFmpegFromFile: codec=%S, pix_fmt=%d, failed to decode a frame",
					m_logTag.Tag(),
					dec->name,
					(frame ? frame->format : -1));
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			const unsigned int srcW = (unsigned int)frame->width;
			const unsigned int srcH = (unsigned int)frame->height;
			if (srcW == 0 || srcH == 0)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: decoded frame has invalid dimensions", m_logTag.Tag());
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			// Decide output dimensions.
			unsigned int dstW = srcW;
			unsigned int dstH = srcH;

			if (useDesiredSize)
			{
				(void)ComputeFit(srcW, srcH, desiredSize, dstW, dstH);
			}

			const size_t w = (size_t)dstW;
			const size_t h = (size_t)dstH;
			if (w == 0 || h == 0)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: invalid output size", m_logTag.Tag());
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}
			if (w > (SIZE_MAX / 4) || (w * 4) > (SIZE_MAX / h))
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: size overflow", m_logTag.Tag());
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}
			const size_t bufSize = (w * 4) * h;

			unsigned char* out = (unsigned char*)LocalAlloc(LMEM_FIXED, bufSize);
			if (!out)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: LocalAlloc failed", m_logTag.Tag());
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}
			memset(out, 0, bufSize);

			SwsContext* sws = sws_getContext(
				(int)srcW, (int)srcH, (AVPixelFormat)frame->format,
				(int)dstW, (int)dstH, AV_PIX_FMT_RGBA,
				SWS_BILINEAR, nullptr, nullptr, nullptr);

			if (!sws)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: sws_getContext failed", m_logTag.Tag());
				LocalFree(out);
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			AVFrame* dst = av_frame_alloc();
			if (!dst)
			{
				if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: av_frame_alloc failed", m_logTag.Tag());
				sws_freeContext(sws);
				LocalFree(out);
				av_packet_free(&pkt);
				av_frame_free(&frame);
				avcodec_free_context(&cc);
				avformat_close_input(&fmt);
				return nullptr;
			}

			dst->format = AV_PIX_FMT_RGBA;
			dst->width = (int)dstW;
			dst->height = (int)dstH;

			// Point the destination frame at our LocalAlloc buffer.
			av_image_fill_arrays(dst->data, dst->linesize, out, AV_PIX_FMT_RGBA, (int)dstW, (int)dstH, 1);

			sws_scale(sws, frame->data, frame->linesize, 0, (int)srcH, dst->data, dst->linesize);

			sws_freeContext(sws);
			av_frame_free(&dst);

			// Cleanup decoder
			av_packet_free(&pkt);
			av_frame_free(&frame);
			avcodec_free_context(&cc);
			avformat_close_input(&fmt);

			hasAlpha = true; // RGBA output
			width = dstW;
			height = dstH;

			if (m_log) m_log->logf(L"%sRenderWithFFmpegFromFile: success, out=%dx%d", m_logTag.Tag(), width, height);

			return out;
		}


		unsigned char* GenerateImage(
			IN IStream* pStream,
			IN unsigned int desiredSize,
			IN unsigned int /*flags*/,
			OUT bool& hasAlpha,
			OUT unsigned int& width,
			OUT unsigned int& height)
		{

			if (config.returnDebugFFMpegThumbnail)
			{
				if (m_log) m_log->logf(L"%sGenerateImage: returning synthetic FFMpeg debug image", m_logTag.Tag());
				return MakeDebugImage(desiredSize, hasAlpha, width, height);
			}

			// Reset outputs early
			hasAlpha = false;
			width = height = 0;

			std::wstring tempDir = GetTempDirectory(L"Voith's CODE\\FFMpeg Plugin MysticThumbs\\");

			std::wstring mediaPath, pngPath;
			if (!MakeTempFilePair(tempDir, L".tmpvid", mediaPath, pngPath))
			{
				if (m_log) m_log->logf(L"%sGenerateImage: MakeTempFilePair failed", m_logTag.Tag());
				return nullptr;
			}

			{
				if (m_log) m_log->logf(L"%sGenerateImage: Generated files=%s and .png", m_logTag.Tag(), mediaPath.c_str());
			}

			// Stage 1: stream -> temp media file (FFmpeg reads from a local file)
			if (!WriteStreamToFile(pStream, mediaPath))
			{
				if (m_log) m_log->logf(L"%sGenerateImage: WriteStreamToFile failed", m_logTag.Tag());
				if (!config.leaveTempFiles)
				{
					DeleteFileW(mediaPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return nullptr;
			}

			// Stage 2: FFmpeg decode (primary)
			const bool useDesiredSize = config.useDesiredSizeHint; // Should we respect desiredSize hint?

			unsigned char* buffer = nullptr;

			if (config.collage4)
			{
				buffer = RenderWithFFmpegCollage4FromFile(
					mediaPath,
					desiredSize,
					config.useDesiredSizeHint,
					hasAlpha,
					width,
					height);
			}

			if (!buffer)
			{
				buffer = RenderWithFFmpegFromFile(
					mediaPath,
					desiredSize,
					config.useDesiredSizeHint,
					hasAlpha,
					width,
					height);
			}

			if (buffer)
			{
				if (m_log) m_log->logf(L"%sGenerateImage: FFmpeg decode succeeded", m_logTag.Tag());
				if (!config.leaveTempFiles)
				{
					DeleteFileW(mediaPath.c_str());
					DeleteFileW(pngPath.c_str());
				}
				return buffer;
			}

			if (m_log) m_log->logf(L"%sGenerateImage: FFmpeg decode failed, trying external Thumbnailer", m_logTag.Tag());

			// Stage 4: external thumbnailer fallback -> PNG
			if (!config.thumbPath.empty() && !config.thumbParams.empty())
			{
				std::wstring args = ExpandThumbParams(config.thumbParams, mediaPath, pngPath, desiredSize, tempDir);

				if (m_log) m_log->logf(
					L"%sThumbnailer: \"%s\" args=%s",
					m_logTag.Tag(),
					config.thumbPath.c_str(),
					args.c_str());

				std::wstring captured;
				DWORD ec = 0;
				if (RunExternalThumbnailerCapture(config.thumbPath, args, captured, ec))
				{
					if (m_log) m_log->logf(L"%sThumbnailer: exit code %ld", m_logTag.Tag(), ec);

					if (!captured.empty())
						if (m_log) m_log->logf(L"%sThumbnailer output: %s", m_logTag.Tag(), captured.c_str());
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
					if (m_log) m_log->logf(L"%sGenerateImage: success (external thumbnailer", m_logTag.Tag());
					if (!config.leaveTempFiles)
					{
						DeleteFileW(mediaPath.c_str());
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
				DeleteFileW(mediaPath.c_str());
				DeleteFileW(pngPath.c_str());
			}

			return nullptr;
		}
	};

	INT_PTR CALLBACK CFFMpegPlugin::ConfigureDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		switch (uMsg) 
		{
		case WM_INITDIALOG:
		{
			CFFMpegPlugin* plugin = (CFFMpegPlugin*)lParam;
			SetWindowLongPtrW(hwndDlg, GWLP_USERDATA, (LONG_PTR)plugin);

			plugin->config.context = plugin->m_context;
			plugin->config.Load();

			const auto& c = plugin->config;

			SetCheck(hwndDlg, IDC_CFG_MISC_LEAVE_TEMP, c.leaveTempFiles);
			SetCheck(hwndDlg, IDC_CFG_MISC_SWAP_RB, c.swapRB);
			SetCheck(hwndDlg, IDC_CFG_MISC_RETURN_DEBUG, c.returnDebugFFMpegThumbnail);
			SetCheck(hwndDlg, IDC_CFG_MISC_USE_DESIRED_SIZE_HINT, c.useDesiredSizeHint);

			SetText(hwndDlg, IDC_CFG_THUMB_PATH, c.thumbPath);
			SetText(hwndDlg, IDC_CFG_THUMB_PARAMS, c.thumbParams);

			SetCheck(hwndDlg, IDC_CFG_COLLAGE_ENABLE, c.collage4);
			SetUInt(hwndDlg, IDC_CFG_COLLAGE_MIN_SECONDS, c.collageMinSeconds);

			// Tooltips
			if(plugin->m_context->TooltipsEnabled()) {
				HWND hTip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
											WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
											CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
											hwndDlg, nullptr, g_hModule, nullptr);
				if(hTip) {
					SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, 420);


					AddTooltip(hTip, hwndDlg, IDC_CFG_MISC_LEAVE_TEMP, L"Keep temporary extracted/converted files. Useful when debugging. Be aware that this can consume significant disk space.");
					AddTooltip(hTip, hwndDlg, IDC_CFG_MISC_SWAP_RB, L"Swap R/B channels (RGBA <-> BGRA) for Windows compatibility.");
					AddTooltip(hTip, hwndDlg, IDC_CFG_MISC_RETURN_DEBUG, L"Return a debug thumbnail when rendering fails. Useful for debugging");
					AddTooltip(hTip, hwndDlg, IDC_CFG_MISC_USE_DESIRED_SIZE_HINT, L"Let MysticThumbs desired size hint influence render scale.");

					//AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_ENABLE, L"Enable external thumbnailer (\"tool that can create thumbnails\") in case internal rendering fails.");
					AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_PATH, L"Optional external fallback thumbnailer executable path.");
					AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_BROWSE, L"Pick an external thumbnailer executable.");
					AddTooltip(hTip, hwndDlg, IDC_CFG_THUMB_PARAMS, L"Command-line parameters for the external thumbnailer.");

					AddTooltip(hTip, hwndDlg, IDC_CFG_COLLAGE_ENABLE, L"Render thumbnail with 4 snapshots instead of one");
					AddTooltip(hTip, hwndDlg, IDC_CFG_COLLAGE_MIN_SECONDS, L"Minimum length of video before attempting to create collage");
				}
			}

			return TRUE;
		}

		case WM_COMMAND:
		{
			CFFMpegPlugin* plugin = (CFFMpegPlugin*)GetWindowLongPtrW(hwndDlg, GWLP_USERDATA);
			ATLASSERT(plugin);

			int wNotifyCode = HIWORD(wParam);
			int wID = LOWORD(wParam);

			switch (wID)
			{
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
extern "C" FFMpegPLUGIN_API int Version()
{
	return MYSTICTHUMBS_PLUGIN_VERSION;
}

extern "C" FFMpegPLUGIN_API bool Initialize()
{
	// Keep Explorer quiet: no stderr spam.
	av_log_set_level(AV_LOG_QUIET);
	return true;
}

extern "C" FFMpegPLUGIN_API bool Shutdown()
{
	return true;
}

extern "C" FFMpegPLUGIN_API IMysticThumbsPlugin* CreateInstance(_In_ IMysticThumbsPluginContext* context)
{
	CFFMpegPlugin* plugin =
		(CFFMpegPlugin*)CoTaskMemAlloc(sizeof(CFFMpegPlugin));

	if (!plugin)
	{
		if (context && context->Log()) context->Log()->log(L"CreateInstance: CoTaskMemAlloc failed");
		return nullptr;
	}

	new (plugin) CFFMpegPlugin(context);
	return plugin;
}

extern "C" FFMpegPLUGIN_API bool PreventLoading([[maybe_unused]] bool isDebugProcess)
{
	// Return true to prevent MysticThumbs from loading this plugin.
	//UNREFERENCED_PARAMETER(isDebugProcess);
	return false;
}

// ----------------------------------------------------------------------------
// DllMain (init logging lock)
// ----------------------------------------------------------------------------
FFMpegPLUGIN_API BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD reason, LPVOID /*reserved*/)
{
	(void)hInstance;

	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_hModule = hInstance;
		break;
	}

	return TRUE;
}
