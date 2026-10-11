#ifndef KYTY_COMMON_EXFAT_H_
#define KYTY_COMMON_EXFAT_H_

#include "common/common.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

// Read-only reader for a raw exFAT volume image (the file starts at the boot sector; there is no
// partition table in front of it). Directories are parsed lazily, so opening an image and
// probing for a single file only reads the boot sector, the root directory and the directories on
// the path to that file.
//
// Member paths are UTF-8 and use '/' separators without a leading slash; an empty path is the
// root directory. Lookups are case-insensitive, using the volume's own up-case table.
//
// Every method is safe to call from multiple threads.
class ExFatVolume {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	struct DirItem {
		std::string name;
		bool        is_file;
	};

	~ExFatVolume();

	// Returns null if the file cannot be opened or is not a plausible exFAT volume.
	[[nodiscard]] static std::unique_ptr<ExFatVolume> Open(const std::filesystem::path& path);

	[[nodiscard]] std::optional<Entry> Find(std::string_view member);
	[[nodiscard]] std::vector<DirItem> List(std::string_view member);

	// Reads from a file entry returned by Find(). Returns the number of bytes copied, which is
	// smaller than `size` only at the end of the file or if the image is truncated or damaged.
	[[nodiscard]] uint64_t Read(uint64_t id, uint64_t offset, uint64_t size, void* data);

	KYTY_CLASS_NO_COPY(ExFatVolume);

private:
	struct Private;
	explicit ExFatVolume(std::unique_ptr<Private> p);
	std::unique_ptr<Private> m_p;
};

} // namespace Common

#endif /* KYTY_COMMON_EXFAT_H_ */
