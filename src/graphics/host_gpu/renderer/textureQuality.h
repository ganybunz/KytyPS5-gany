#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_

#include <cstdlib>
#include <string_view>

namespace Libs::Graphics::TextureQuality {

// KYTY_TEXTURE_QUALITY=reduced: large read-only textures that shaders only filter-sample are kept on
// the GPU without their largest mip level, a quarter of their video memory. Anything that needs the
// exact texels promotes the texture back to full quality from guest memory, so no guest data is
// lost; close-up detail is softer. For 8 GB cards (Demon's Souls filled one; floppa-ps5 46e2a7df).
[[nodiscard]] inline bool Reduced() {
	static const bool reduced = [] {
		const char* value = std::getenv("KYTY_TEXTURE_QUALITY");
		return value != nullptr && std::string_view(value) == "reduced";
	}();
	return reduced;
}

} // namespace Libs::Graphics::TextureQuality

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_ */
