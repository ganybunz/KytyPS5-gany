#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Libs::Graphics::TextureQuality {

// Read-only, filter-sampled textures may omit their top mip levels to reduce GPU and upload memory.
// Any lookup that needs a different view promotes the image back to full quality from guest memory.
// "reduced" skips one level; "very_reduced" skips two, for tighter-memory Demon's Souls runs.
// The default stays full quality.
[[nodiscard]] inline uint32_t MipSkip() {
	static const uint32_t mip_skip = [] {
		const char* value = std::getenv("KYTY_TEXTURE_QUALITY");
		if (value == nullptr) return 0u;
		const std::string_view quality {value};
		if (quality == "reduced") return 1u;
		if (quality == "very_reduced") return 2u;
		return 0u;
	}();
	return mip_skip;
}

} // namespace Libs::Graphics::TextureQuality

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_ */
