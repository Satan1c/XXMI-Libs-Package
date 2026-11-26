#include "SoundFile.h"

#include <windows.h>
#include <Audio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../ThirdPartyLibs/dr_libs/dr_mp3.h"

#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace SoundEngine {
	using Microsoft::WRL::ComPtr;

	static void append_pcm(std::vector<uint8_t> &pcm, const void *data, size_t bytes)
	{
		if (bytes > XAUDIO2_MAX_BUFFER_BYTES - pcm.size())
			throw std::runtime_error("decoded audio exceeds the XAudio2 buffer limit");

		if (bytes) {
			const auto *start = static_cast<const uint8_t *>(data);
			pcm.insert(pcm.end(), start, start + bytes);
		}
	}

	static std::unique_ptr<DirectX::SoundEffect> make_sound(
			DirectX::AudioEngine *audio, const WAVEFORMATEX &format, size_t format_bytes,
			const std::vector<uint8_t> &pcm)
	{
		if (pcm.empty() || !format.nBlockAlign || pcm.size() % format.nBlockAlign)
			throw std::runtime_error("decoder returned no complete audio buffer");

		// SoundEffect retains pointers into the allocation it takes ownership of.
		// Align the first PCM sample and keep its format in the same allocation.
		const size_t offset = (format_bytes + 15) & ~size_t(15);
		std::unique_ptr<uint8_t[]> storage(new uint8_t[offset + pcm.size()]);
		std::memcpy(storage.get(), &format, format_bytes);
		uint8_t *samples = storage.get() + offset;
		std::memcpy(samples, pcm.data(), pcm.size());

		return std::make_unique<DirectX::SoundEffect>(audio, storage,
				reinterpret_cast<const WAVEFORMATEX *>(storage.get()), samples, pcm.size());
	}

	static std::unique_ptr<DirectX::SoundEffect> load_mp3(
			DirectX::AudioEngine *audio, const std::wstring &path)
	{
		drmp3 decoder;
		if (!drmp3_init_file_w(&decoder, path.c_str(), nullptr))
			throw std::runtime_error("cannot open or decode MP3");

		const std::unique_ptr<drmp3, decltype(&drmp3_uninit)> cleanup(&decoder, drmp3_uninit);
		if ((decoder.channels != 1 && decoder.channels != 2) || !decoder.sampleRate)
			throw std::runtime_error("invalid MP3 channel count or sample rate");

		WAVEFORMATEX format{};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = static_cast<WORD>(decoder.channels);
		format.nSamplesPerSec = decoder.sampleRate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = format.nChannels * sizeof(drmp3_int16);
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

		std::vector<uint8_t> pcm;
		drmp3_int16 samples[4096 * 2];
		while (const auto frames = drmp3_read_pcm_frames_s16(&decoder, 4096, samples))
			append_pcm(pcm, samples, static_cast<size_t>(frames) * format.nBlockAlign);

		return make_sound(audio, format, sizeof(format), pcm);
	}

	static void check_media(HRESULT hr, const char *operation)
	{
		if (FAILED(hr)) {
			char message[128];
			std::snprintf(message, sizeof(message), "Media Foundation %s failed (0x%08lx)",
					operation, static_cast<unsigned long>(hr));
			throw std::runtime_error(message);
		}
	}

	class MediaModule {
	public:
		explicit MediaModule(const wchar_t *name)
			: module(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
		{
			if (!module)
				check_media(HRESULT_FROM_WIN32(GetLastError()), "DLL load");
		}

		~MediaModule() { FreeLibrary(module); }

		template<typename Function>
		Function resolve(const char *name) const
		{
			FARPROC proc = GetProcAddress(module, name);
			if (!proc)
				check_media(HRESULT_FROM_WIN32(GetLastError()), name);

			return reinterpret_cast<Function>(proc);
		}

		MediaModule(const MediaModule &) = delete;
		MediaModule &operator=(const MediaModule &) = delete;

	private:
		HMODULE module;
	};

	static std::unique_ptr<DirectX::SoundEffect> load_media(
			DirectX::AudioEngine *audio, const std::wstring &path)
	{
		MediaModule platform(L"mfplat.dll");
		MediaModule readwrite(L"mfreadwrite.dll");
		const auto startup = platform.resolve<decltype(&MFStartup)>("MFStartup");
		const auto shutdown = platform.resolve<decltype(&MFShutdown)>("MFShutdown");
		const auto create_type = platform.resolve<decltype(&MFCreateMediaType)>("MFCreateMediaType");
		const auto create_format = platform.resolve<decltype(&MFCreateWaveFormatExFromMFMediaType)>(
				"MFCreateWaveFormatExFromMFMediaType");
		const auto open_reader = readwrite.resolve<decltype(&MFCreateSourceReaderFromURL)>(
				"MFCreateSourceReaderFromURL");

		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		// The host may already have initialized this thread as an STA.
		if (com != RPC_E_CHANGED_MODE)
			check_media(com, "COM initialization");

		int lifetime = 0;
		const auto uninitialize = [com](int *) {
			if (SUCCEEDED(com))
				CoUninitialize();
		};
		const std::unique_ptr<int, decltype(uninitialize)> com_scope(&lifetime, uninitialize);

		check_media(startup(MF_VERSION, MFSTARTUP_NOSOCKET), "startup");
		const auto stop = [shutdown](int *) { shutdown(); };
		const std::unique_ptr<int, decltype(stop)> media_scope(&lifetime, stop);

		ComPtr<IMFSourceReader> reader;
		check_media(open_reader(path.c_str(), nullptr, reader.GetAddressOf()), "open");
		constexpr DWORD stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
		check_media(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "disable streams");
		check_media(reader->SetStreamSelection(stream, TRUE), "select audio");

		ComPtr<IMFMediaType> type;
		check_media(create_type(type.GetAddressOf()), "create audio type");
		check_media(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "set audio type");
		check_media(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "set PCM subtype");
		check_media(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "select PCM output");
		check_media(reader->GetCurrentMediaType(stream, type.ReleaseAndGetAddressOf()), "get PCM output");

		WAVEFORMATEX *raw_format = nullptr;
		UINT32 format_bytes = 0;
		check_media(create_format(type.Get(), &raw_format, &format_bytes, 0), "get wave format");
		const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw_format, CoTaskMemFree);

		std::vector<uint8_t> pcm;
		for (;;) {
			DWORD flags = 0;
			ComPtr<IMFSample> sample;
			check_media(reader->ReadSample(stream, 0, nullptr, &flags, nullptr,
					sample.GetAddressOf()), "decode");
			if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
				throw std::runtime_error("Media Foundation failed or changed the PCM format during decode");

			if (sample) {
				ComPtr<IMFMediaBuffer> buffer;
				check_media(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()), "get PCM samples");
				BYTE *bytes = nullptr;
				DWORD length = 0;
				check_media(buffer->Lock(&bytes, nullptr, &length), "lock PCM samples");

				const auto unlock = [](IMFMediaBuffer *locked) { locked->Unlock(); };
				const std::unique_ptr<IMFMediaBuffer, decltype(unlock)> buffer_lock(buffer.Get(), unlock);
				append_pcm(pcm, bytes, length);
			}

			if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
				break;
		}

		return make_sound(audio, *format, format_bytes, pcm);
	}

	bool is_compressed_sound(const std::wstring &extension)
	{
		return extension == L".mp3" || extension == L".aac" ||
				extension == L".m4a" || extension == L".wma";
	}

	std::unique_ptr<DirectX::SoundEffect> load_compressed_sound(
			DirectX::AudioEngine *audio, const std::wstring &path,
			const std::wstring &extension)
	{
		if (extension == L".mp3")
			return load_mp3(audio, path);

		return load_media(audio, path);
	}
}
