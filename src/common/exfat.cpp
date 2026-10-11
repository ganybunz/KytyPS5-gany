#include "common/exfat.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <ios>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Common {

namespace {

constexpr size_t   EntrySize         = 32;
constexpr uint32_t BootSectorSize    = 512;
constexpr uint32_t FatCacheBlockSize = 4096;
constexpr uint32_t EndOfChainMin     = 0xFFFFFFF8u;
constexpr uint32_t MaxClusterCount   = 0xFFFFFFF5u;
constexpr uint64_t MaxDirectoryBytes = 256ull << 20u; // exFAT limit for one directory
constexpr uint64_t MaxUpcaseBytes    = 2ull * 65536;
constexpr size_t   MaxNodes          = 1u << 24u;

// Directory entry types.
constexpr uint8_t EntryEndOfDirectory = 0x00;
constexpr uint8_t EntryUpcaseTable    = 0x82;
constexpr uint8_t EntryFile           = 0x85;
constexpr uint8_t EntryStreamExt      = 0xC0;
constexpr uint8_t EntryFileName       = 0xC1;

constexpr uint16_t AttributeDirectory = 0x0010;
constexpr uint8_t  FlagNoFatChain     = 0x02;

uint16_t Le16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8u));
}

uint32_t Le32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
	       (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t Le64(const uint8_t* p) {
	return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32u);
}

void AppendUtf8(std::string& out, uint32_t c) {
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xC0 | (c >> 6u));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else if (c < 0x10000) {
		out += static_cast<char>(0xE0 | (c >> 12u));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else {
		out += static_cast<char>(0xF0 | (c >> 18u));
		out += static_cast<char>(0x80 | ((c >> 12u) & 0x3Fu));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	}
}

std::string Utf16ToUtf8(std::u16string_view in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		uint32_t c = in[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 &&
		    in[i + 1] < 0xE000) {
			c = 0x10000u + ((c - 0xD800u) << 10u) + (static_cast<uint32_t>(in[i + 1]) - 0xDC00u);
			++i;
		} else if (c >= 0xD800 && c < 0xE000) {
			c = 0xFFFD; // unpaired surrogate
		}
		AppendUtf8(out, c);
	}
	return out;
}

std::optional<std::u16string> Utf8ToUtf16(std::string_view in) {
	std::u16string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size();) {
		const auto lead = static_cast<uint8_t>(in[i]);
		uint32_t   c    = 0;
		size_t     len  = 0;
		if (lead < 0x80) {
			c   = lead;
			len = 1;
		} else if ((lead & 0xE0u) == 0xC0u) {
			c   = lead & 0x1Fu;
			len = 2;
		} else if ((lead & 0xF0u) == 0xE0u) {
			c   = lead & 0x0Fu;
			len = 3;
		} else if ((lead & 0xF8u) == 0xF0u) {
			c   = lead & 0x07u;
			len = 4;
		} else {
			return std::nullopt;
		}
		if (i + len > in.size()) {
			return std::nullopt;
		}
		for (size_t k = 1; k < len; ++k) {
			const auto next = static_cast<uint8_t>(in[i + k]);
			if ((next & 0xC0u) != 0x80u) {
				return std::nullopt;
			}
			c = (c << 6u) | (next & 0x3Fu);
		}
		static constexpr uint32_t MinForLength[] = {0, 0, 0x80, 0x800, 0x10000};
		if (c < MinForLength[len] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
			return std::nullopt;
		}
		if (c >= 0x10000) {
			c -= 0x10000;
			out += static_cast<char16_t>(0xD800 + (c >> 10u));
			out += static_cast<char16_t>(0xDC00 + (c & 0x3FFu));
		} else {
			out += static_cast<char16_t>(c);
		}
		i += len;
	}
	return out;
}

// A run of consecutive clusters. `first_index` is the position of the run's first cluster within
// the file, so a file offset maps to a run with a binary search.
struct Run {
	uint64_t first_index;
	uint32_t start;
	uint32_t count;
};

struct DirData {
	bool                                         loaded = false;
	std::vector<uint32_t>                        children;
	std::unordered_map<std::u16string, uint32_t> index; // case-folded name -> node
};

struct Node {
	std::string              name;
	uint64_t                 data_length   = 0;
	uint64_t                 valid_length  = 0;
	uint32_t                 first_cluster = 0;
	bool                     is_dir        = false;
	bool                     no_fat_chain  = false;
	bool                     runs_loaded   = false;
	std::vector<Run>         runs;
	std::unique_ptr<DirData> dir;
};

} // namespace

struct ExFatVolume::Private {
	std::mutex    mutex;
	std::ifstream file;
	uint64_t      image_size        = 0;
	uint64_t      bytes_per_cluster = 0;
	uint64_t      fat_offset        = 0; // bytes
	uint64_t      heap_offset       = 0; // bytes
	uint32_t      cluster_count     = 0;
	uint32_t      root_cluster      = 0;

	std::vector<char16_t> upcase; // empty: ASCII-only folding
	std::deque<Node>      nodes;  // stable references; index 0 is the root directory

	uint64_t             fat_cache_block = UINT64_MAX;
	std::vector<uint8_t> fat_cache;

	uint64_t ReadAt(uint64_t offset, void* data, uint64_t size) {
		if (offset >= image_size || size == 0) {
			return 0;
		}
		size = std::min(size, image_size - offset);
		file.clear();
		file.seekg(static_cast<std::streamoff>(offset));
		if (!file) {
			return 0;
		}
		file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
		return static_cast<uint64_t>(file.gcount());
	}

	std::optional<uint32_t> FatNext(uint32_t cluster) {
		const uint64_t offset = fat_offset + 4ull * cluster;
		const uint64_t block  = offset / FatCacheBlockSize;
		if (block != fat_cache_block) {
			fat_cache.assign(FatCacheBlockSize, 0);
			if (ReadAt(block * FatCacheBlockSize, fat_cache.data(), FatCacheBlockSize) == 0) {
				return std::nullopt;
			}
			fat_cache_block = block;
		}
		return Le32(fat_cache.data() + (offset % FatCacheBlockSize));
	}

	bool ValidCluster(uint32_t cluster) const {
		return cluster >= 2 && cluster - 2 < cluster_count;
	}

	// Builds the cluster runs of a chain. With a known length the chain must supply every
	// cluster of it; with an unknown length (the root directory) it is followed to its end.
	bool BuildRuns(uint32_t first, bool no_fat_chain, bool length_known, uint64_t length,
	               std::vector<Run>& runs) {
		runs.clear();
		if (length_known && length == 0) {
			return true;
		}
		if (!ValidCluster(first)) {
			return false;
		}
		const uint64_t needed =
		    length_known ? (length + bytes_per_cluster - 1) / bytes_per_cluster : UINT64_MAX;
		if (no_fat_chain) {
			if (!length_known || needed > cluster_count ||
			    uint64_t(first) + needed - 1 > uint64_t(cluster_count) + 1) {
				return false;
			}
			runs.push_back({0, first, static_cast<uint32_t>(needed)});
			return true;
		}
		uint64_t total   = 0;
		uint32_t current = first;
		while (true) {
			if (!ValidCluster(current) || total >= cluster_count) {
				return false; // bad cluster, out of range, or a loop
			}
			if (!runs.empty() && runs.back().start + runs.back().count == current) {
				++runs.back().count;
			} else {
				runs.push_back({total, current, 1});
			}
			++total;
			if (total >= needed) {
				break;
			}
			const auto next = FatNext(current);
			if (!next) {
				return false;
			}
			if (*next >= EndOfChainMin) {
				break;
			}
			current = *next;
		}
		return !length_known || total >= needed;
	}

	uint64_t ReadRuns(const std::vector<Run>& runs, uint64_t offset, uint8_t* out, uint64_t size) {
		uint64_t done = 0;
		while (done < size) {
			const uint64_t position = offset + done;
			const uint64_t index    = position / bytes_per_cluster;
			auto           it       = std::upper_bound(runs.begin(), runs.end(), index,
			                                           [](uint64_t v, const Run& r) { return v < r.first_index; });
			if (it == runs.begin()) {
				break;
			}
			--it;
			const uint64_t in_run = index - it->first_index;
			if (in_run >= it->count) {
				break;
			}
			const uint64_t run_end = (it->first_index + it->count) * bytes_per_cluster;
			const uint64_t chunk   = std::min(size - done, run_end - position);
			const uint64_t disk    = heap_offset +
			                      (uint64_t(it->start - 2) + in_run) * bytes_per_cluster +
			                      position % bytes_per_cluster;
			const uint64_t got = ReadAt(disk, out + done, chunk);
			done += got;
			if (got < chunk) {
				break;
			}
		}
		return done;
	}

	std::u16string Fold(std::u16string_view name) const {
		std::u16string out(name);
		for (auto& c: out) {
			if (!upcase.empty()) {
				c = upcase[static_cast<uint16_t>(c)];
			} else if (c >= u'a' && c <= u'z') {
				c = static_cast<char16_t>(c - (u'a' - u'A'));
			}
		}
		return out;
	}

	// The up-case table is stored compressed: 0xFFFF followed by N means "the next N characters
	// map to themselves".
	void LoadUpcase(const std::vector<uint8_t>& directory) {
		for (size_t pos = 0; pos + EntrySize <= directory.size(); pos += EntrySize) {
			const uint8_t* e = &directory[pos];
			if (e[0] == EntryEndOfDirectory) {
				return;
			}
			if (e[0] != EntryUpcaseTable) {
				continue;
			}
			const uint32_t first  = Le32(e + 20);
			const uint64_t length = Le64(e + 24);
			if (length == 0 || length > MaxUpcaseBytes || length % 2 != 0) {
				return;
			}
			std::vector<Run>     runs;
			std::vector<uint8_t> raw(length);
			if (!BuildRuns(first, false, true, length, runs) ||
			    ReadRuns(runs, 0, raw.data(), length) != length) {
				return;
			}
			std::vector<char16_t> table(65536);
			for (size_t i = 0; i < table.size(); ++i) {
				table[i] = static_cast<char16_t>(i);
			}
			size_t character = 0;
			for (size_t i = 0; i + 2 <= raw.size() && character < table.size();) {
				const uint16_t value = Le16(&raw[i]);
				i += 2;
				if (value == 0xFFFF && i + 2 <= raw.size()) {
					character += Le16(&raw[i]);
					i += 2;
				} else {
					table[character++] = static_cast<char16_t>(value);
				}
			}
			if (table[u'a'] == u'A' && table[u'z'] == u'Z') { // sanity check
				upcase = std::move(table);
			}
			return;
		}
	}

	void AddChild(uint32_t parent, Node&& node, std::u16string_view name16) {
		if (nodes.size() >= MaxNodes) {
			return;
		}
		const auto index  = static_cast<uint32_t>(nodes.size());
		auto       folded = Fold(name16);
		auto&      dir    = *nodes[parent].dir;
		if (dir.index.emplace(std::move(folded), index).second) {
			dir.children.push_back(index);
			nodes.push_back(std::move(node));
		}
	}

	void ParseDirectory(uint32_t parent, const std::vector<uint8_t>& buf) {
		for (size_t pos = 0; pos + EntrySize <= buf.size(); pos += EntrySize) {
			const uint8_t* e = &buf[pos];
			if (e[0] == EntryEndOfDirectory) {
				break;
			}
			if (e[0] != EntryFile) {
				continue;
			}
			const size_t secondary = e[1];
			if (secondary < 2) {
				continue;
			}
			const size_t set_bytes = (1 + secondary) * EntrySize;
			if (pos + set_bytes > buf.size()) {
				break;
			}
			// Whatever happens below, the whole set is consumed.
			const size_t last = pos + set_bytes - EntrySize;

			uint16_t checksum = 0;
			for (size_t i = 0; i < set_bytes; ++i) {
				if (i == 2 || i == 3) {
					continue;
				}
				checksum = static_cast<uint16_t>(((checksum << 15u) | (checksum >> 1u)) + e[i]);
			}
			const uint8_t* s = e + EntrySize;
			if (checksum != Le16(e + 2) || s[0] != EntryStreamExt) {
				pos = last;
				continue;
			}

			const uint8_t  flags       = s[1];
			const size_t   name_length = s[3];
			const uint64_t valid       = Le64(s + 8);
			const uint32_t first       = Le32(s + 20);
			const uint64_t length      = Le64(s + 24);
			const bool     is_dir      = (Le16(e + 4) & AttributeDirectory) != 0;

			const size_t name_entries = (name_length + 14) / 15;
			if (name_length == 0 || name_entries > secondary - 1) {
				pos = last;
				continue;
			}
			std::u16string name;
			bool           name_ok = true;
			for (size_t k = 0; k < name_entries && name_ok; ++k) {
				const uint8_t* ne = s + EntrySize * (1 + k);
				if (ne[0] != EntryFileName) {
					name_ok = false;
					break;
				}
				for (size_t c = 0; c < 15 && name.size() < name_length; ++c) {
					name += static_cast<char16_t>(Le16(ne + 2 + 2 * c));
				}
			}
			pos = last;
			if (!name_ok || name.find(u'/') != std::u16string::npos ||
			    name.find(u'\0') != std::u16string::npos) {
				continue;
			}
			if (length > 0 && !ValidCluster(first)) {
				continue;
			}
			if (is_dir && length > MaxDirectoryBytes) {
				continue;
			}

			Node node;
			node.name          = Utf16ToUtf8(name);
			node.data_length   = length;
			node.valid_length  = std::min(valid, length);
			node.first_cluster = first;
			node.is_dir        = is_dir;
			node.no_fat_chain  = (flags & FlagNoFatChain) != 0;
			if (is_dir) {
				node.dir = std::make_unique<DirData>();
			}
			AddChild(parent, std::move(node), name);
		}
	}

	bool LoadDirectory(uint32_t index) {
		Node& node = nodes[index];
		if (node.dir->loaded) {
			return true;
		}
		const bool       root = index == 0;
		std::vector<Run> runs;
		if (!BuildRuns(node.first_cluster, node.no_fat_chain, !root, node.data_length, runs)) {
			return false;
		}
		uint64_t bytes = node.data_length;
		if (root) {
			bytes = runs.empty()
			            ? 0
			            : (runs.back().first_index + runs.back().count) * bytes_per_cluster;
		}
		if (bytes > MaxDirectoryBytes) {
			return false;
		}
		std::vector<uint8_t> buf(bytes);
		const uint64_t       got = ReadRuns(runs, 0, buf.data(), bytes);
		if (got == 0 && bytes != 0) {
			return false;
		}
		buf.resize(got); // a truncated image still yields the entries that are present
		if (root) {
			LoadUpcase(buf);
		}
		ParseDirectory(index, buf);
		node.dir->loaded = true;
		return true;
	}

	std::optional<uint32_t> Lookup(std::string_view member) {
		uint32_t current = 0;
		size_t   pos     = 0;
		while (pos < member.size()) {
			const auto separator = member.find('/', pos);
			const auto component =
			    member.substr(pos, separator == std::string_view::npos ? std::string_view::npos
			                                                           : separator - pos);
			pos = separator == std::string_view::npos ? member.size() : separator + 1;
			if (component.empty() || component == ".") {
				continue;
			}
			if (!nodes[current].is_dir || !LoadDirectory(current)) {
				return std::nullopt;
			}
			const auto name = Utf8ToUtf16(component);
			if (!name) {
				return std::nullopt;
			}
			const auto& index = nodes[current].dir->index;
			const auto  it    = index.find(Fold(*name));
			if (it == index.end()) {
				return std::nullopt;
			}
			current = it->second;
		}
		return current;
	}
};

ExFatVolume::ExFatVolume(std::unique_ptr<Private> p): m_p(std::move(p)) {}
ExFatVolume::~ExFatVolume() = default;

std::unique_ptr<ExFatVolume> ExFatVolume::Open(const std::filesystem::path& path) {
	auto p = std::make_unique<Private>();
	p->file.open(path, std::ios::binary);
	if (!p->file) {
		return nullptr;
	}
	p->file.seekg(0, std::ios::end);
	const auto end = p->file.tellg();
	if (end < static_cast<std::streamoff>(BootSectorSize)) {
		return nullptr;
	}
	p->image_size = static_cast<uint64_t>(end);

	uint8_t boot[BootSectorSize];
	if (p->ReadAt(0, boot, sizeof(boot)) != sizeof(boot)) {
		return nullptr;
	}
	static constexpr uint8_t Signature[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
	if (std::memcmp(boot + 3, Signature, sizeof(Signature)) != 0 || boot[510] != 0x55 ||
	    boot[511] != 0xAA) {
		return nullptr;
	}
	for (size_t i = 11; i < 64; ++i) {
		if (boot[i] != 0) {
			return nullptr; // MustBeZero
		}
	}

	const uint32_t fat_offset_sectors  = Le32(boot + 80);
	const uint32_t fat_length_sectors  = Le32(boot + 84);
	const uint32_t heap_offset_sectors = Le32(boot + 88);
	const uint32_t cluster_count       = Le32(boot + 92);
	const uint32_t root_cluster        = Le32(boot + 96);
	const uint16_t volume_flags        = Le16(boot + 106);
	const uint8_t  sector_shift        = boot[108];
	const uint8_t  cluster_shift       = boot[109];
	const uint8_t  fat_count           = boot[110];

	if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 - sector_shift ||
	    (fat_count != 1 && fat_count != 2) || cluster_count == 0 ||
	    cluster_count > MaxClusterCount) {
		return nullptr;
	}
	const uint64_t sector_size = 1ull << sector_shift;
	const uint64_t fat_bytes   = fat_length_sectors * sector_size;
	if (fat_offset_sectors == 0 || fat_bytes < (uint64_t(cluster_count) + 2) * 4 ||
	    heap_offset_sectors <
	        uint64_t(fat_offset_sectors) + uint64_t(fat_length_sectors) * fat_count) {
		return nullptr;
	}

	p->bytes_per_cluster = 1ull << (sector_shift + cluster_shift);
	p->fat_offset        = fat_offset_sectors * sector_size;
	p->heap_offset       = heap_offset_sectors * sector_size;
	p->cluster_count     = cluster_count;
	p->root_cluster      = root_cluster;
	if (fat_count == 2 && (volume_flags & 1u) != 0) {
		p->fat_offset += fat_bytes; // TexFAT: the second FAT is the active one
	}
	if (p->heap_offset >= p->image_size || !p->ValidCluster(root_cluster)) {
		return nullptr;
	}

	Node root;
	root.is_dir        = true;
	root.first_cluster = root_cluster;
	root.dir           = std::make_unique<DirData>();
	p->nodes.push_back(std::move(root));
	if (!p->LoadDirectory(0)) {
		return nullptr;
	}
	return std::unique_ptr<ExFatVolume>(new ExFatVolume(std::move(p)));
}

std::optional<ExFatVolume::Entry> ExFatVolume::Find(std::string_view member) {
	std::lock_guard lock(m_p->mutex);
	const auto      index = m_p->Lookup(member);
	if (!index) {
		return std::nullopt;
	}
	const Node& node = m_p->nodes[*index];
	return Entry {*index, node.is_dir ? 0 : node.data_length, !node.is_dir};
}

std::vector<ExFatVolume::DirItem> ExFatVolume::List(std::string_view member) {
	std::lock_guard lock(m_p->mutex);
	const auto      index = m_p->Lookup(member);
	if (!index || !m_p->nodes[*index].is_dir || !m_p->LoadDirectory(*index)) {
		return {};
	}
	std::vector<DirItem> result;
	const auto&          children = m_p->nodes[*index].dir->children;
	result.reserve(children.size());
	for (const auto child: children) {
		const Node& node = m_p->nodes[child];
		result.push_back({node.name, !node.is_dir});
	}
	return result;
}

uint64_t ExFatVolume::Read(uint64_t id, uint64_t offset, uint64_t size, void* data) {
	std::lock_guard lock(m_p->mutex);
	auto&           p = *m_p;
	if (id >= p.nodes.size() || data == nullptr) {
		return 0;
	}
	Node& node = p.nodes[id];
	if (node.is_dir || offset >= node.data_length || size == 0) {
		return 0;
	}
	size = std::min(size, node.data_length - offset);
	if (!node.runs_loaded) {
		if (!p.BuildRuns(node.first_cluster, node.no_fat_chain, true, node.data_length,
		                 node.runs)) {
			return 0;
		}
		node.runs_loaded = true;
	}
	auto*    out  = static_cast<uint8_t*>(data);
	uint64_t done = 0;
	if (offset < node.valid_length) {
		const uint64_t wanted = std::min(size, node.valid_length - offset);
		done                  = p.ReadRuns(node.runs, offset, out, wanted);
		if (done < wanted) {
			return done;
		}
	}
	// Bytes between ValidDataLength and DataLength read as zero.
	if (done < size) {
		std::memset(out + done, 0, size - done);
	}
	return size;
}

} // namespace Common
