#pragma once

#include <memory>
#include <string>

namespace DirectX {
	class AudioEngine;
	class SoundEffect;
}

namespace SoundEngine {
	bool is_compressed_sound(const std::wstring &extension);
	std::unique_ptr<DirectX::SoundEffect> load_compressed_sound(
			DirectX::AudioEngine *audio, const std::wstring &path,
			const std::wstring &extension);
}
