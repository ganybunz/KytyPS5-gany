#include "common/archive.h"
#include "common/exfat.h"
#include "common/file.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ExFatTests: failed: %s\n", text);
    std::abort();
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("kyty_exfat_test_" + std::to_string(unique));
    Check(std::filesystem::create_directories(m_path),
          "create temporary directory");
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  [[nodiscard]] const std::filesystem::path &Path() const { return m_path; }
  KYTY_CLASS_NO_COPY(TempDirectory);

private:
  std::filesystem::path m_path;
};

// ---------------------------------------------------------------------------
// A small exFAT image writer, written from the exFAT specification. Images it
// produces are accepted by fsck.exfat (exfatprogs). It supports exactly what
// the tests need: directories spanning several clusters, contiguous files
// (NoFatChain), interleaved/fragmented files (FAT chains), short valid data
// length, deleted entries and corrupted entry-set checksums.
// ---------------------------------------------------------------------------

constexpr uint32_t SectorSize = 512;
constexpr uint32_t SectorsPerCluster = 2;
constexpr uint32_t ClusterSize = SectorSize * SectorsPerCluster;
constexpr uint32_t ClusterCount = 2048;
constexpr uint32_t FatOffsetSectors = 24;
constexpr uint32_t FatLengthSectors = 24;
constexpr uint32_t HeapOffsetSectors = 64;
constexpr uint32_t UpcaseEntries = 256;

struct Item {
  std::string name; // UTF-8
  std::vector<uint8_t> data;
  uint64_t valid = UINT64_MAX; // default: all of `data`
  int frag_group = 0; // files of one group in one directory interleave
  bool corrupt_checksum = false;
  bool deleted = false;
  std::vector<uint32_t> clusters;
};

struct Dir {
  std::string name;
  std::vector<Dir> dirs;
  std::vector<Item> files;
  std::vector<uint32_t> clusters;
};

char16_t Upcase(char16_t c) {
  if (c >= u'a' && c <= u'z') {
    return static_cast<char16_t>(c - 32);
  }
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
    return static_cast<char16_t>(c - 32);
  }
  if (c == 0xFF) {
    return 0x178;
  }
  return c;
}

std::u16string ToUtf16(const std::string &utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<uint8_t>(utf8[i]);
    uint32_t c;
    size_t len;
    if (lead < 0x80) {
      c = lead;
      len = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      c = lead & 0x1F;
      len = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      c = lead & 0x0F;
      len = 3;
    } else {
      c = lead & 0x07;
      len = 4;
    }
    for (size_t k = 1; k < len; ++k) {
      c = (c << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3F);
    }
    i += len;
    if (c >= 0x10000) {
      c -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (c >> 10));
      out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
    } else {
      out += static_cast<char16_t>(c);
    }
  }
  return out;
}

void Put16(std::vector<uint8_t> &v, size_t at, uint16_t x) {
  v[at] = x & 0xFF;
  v[at + 1] = x >> 8;
}
void Put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) {
    v[at + i] = (x >> (8 * i)) & 0xFF;
  }
}
void Put64(std::vector<uint8_t> &v, size_t at, uint64_t x) {
  for (int i = 0; i < 8; ++i) {
    v[at + i] = (x >> (8 * i)) & 0xFF;
  }
}

class ImageBuilder {
public:
  Dir root;

  std::vector<uint8_t> Build() {
    m_image.assign(
        (uint64_t(HeapOffsetSectors) + uint64_t(ClusterCount) * SectorsPerCluster) * SectorSize, 0);
    m_fat.assign(FatLengthSectors * SectorSize / 4, 0);
    m_fat[0] = 0xFFFFFFF8;
    m_fat[1] = 0xFFFFFFFF;
    m_bitmap.assign((ClusterCount + 7) / 8, 0);

    m_bitmap_clusters = AllocateChain(m_bitmap.size());
    std::vector<uint8_t> upcase(UpcaseEntries * 2);
    for (uint32_t i = 0; i < UpcaseEntries; ++i) {
      Put16(upcase, i * 2, Upcase(static_cast<char16_t>(i)));
    }
    m_upcase_clusters = AllocateChain(upcase.size());
    Allocate(root, true);
    WriteClusters(m_bitmap_clusters, m_bitmap);
    WriteClusters(m_upcase_clusters, upcase);
    m_upcase_checksum = 0;
    for (const auto b : upcase) {
      m_upcase_checksum = ((m_upcase_checksum << 31) | (m_upcase_checksum >> 1)) + b;
    }
    Serialize(root, true);
    WriteClusters(m_bitmap_clusters, m_bitmap); // allocation is final now
    WriteBootRegion();
    std::memcpy(m_image.data() + FatOffsetSectors * SectorSize, m_fat.data(),
                m_fat.size() * 4);
    return m_image;
  }

private:
  std::vector<uint8_t> m_image;
  std::vector<uint32_t> m_fat;
  std::vector<uint8_t> m_bitmap;
  std::vector<uint32_t> m_bitmap_clusters;
  std::vector<uint32_t> m_upcase_clusters;
  uint32_t m_upcase_checksum = 0;
  uint32_t m_next = 2;

  void Mark(uint32_t cluster) {
    Check(cluster - 2 < ClusterCount, "image is large enough");
    m_bitmap[(cluster - 2) / 8] |= static_cast<uint8_t>(1u << ((cluster - 2) % 8));
  }

  // Clusters taken from the allocator are always FAT-chained here; callers
  // that want NoFatChain simply do not publish the chain into the FAT.
  std::vector<uint32_t> AllocateRun(uint64_t bytes) {
    std::vector<uint32_t> clusters;
    for (uint64_t n = (bytes + ClusterSize - 1) / ClusterSize; n > 0; --n) {
      Mark(m_next);
      clusters.push_back(m_next++);
    }
    return clusters;
  }

  void LinkInFat(const std::vector<uint32_t> &clusters) {
    for (size_t i = 0; i < clusters.size(); ++i) {
      m_fat[clusters[i]] = i + 1 < clusters.size() ? clusters[i + 1] : 0xFFFFFFFF;
    }
  }

  std::vector<uint32_t> AllocateChain(uint64_t bytes) {
    auto clusters = AllocateRun(std::max<uint64_t>(bytes, 1));
    LinkInFat(clusters);
    return clusters;
  }

  static size_t NameEntries(const std::string &name) {
    return (ToUtf16(name).size() + 14) / 15;
  }

  static size_t EntryBytes(const Dir &dir, bool is_root) {
    size_t entries = is_root ? 2 : 0; // allocation bitmap + up-case table
    for (const auto &d : dir.dirs) {
      entries += 2 + NameEntries(d.name);
    }
    for (const auto &f : dir.files) {
      entries += 2 + NameEntries(f.name);
    }
    return entries * 32;
  }

  void Allocate(Dir &dir, bool is_root) {
    dir.clusters = AllocateChain(EntryBytes(dir, is_root));
    // Interleaved groups first: every cluster of a group's files is dealt out
    // round-robin from one region, so each file ends up as many short runs.
    std::map<int, std::vector<Item *>> groups;
    for (auto &f : dir.files) {
      if (f.frag_group != 0) {
        groups[f.frag_group].push_back(&f);
      }
    }
    for (auto &[group, files] : groups) {
      std::vector<size_t> need;
      for (auto *f : files) {
        need.push_back((f->data.size() + ClusterSize - 1) / ClusterSize);
      }
      for (bool more = true; more;) {
        more = false;
        for (size_t i = 0; i < files.size(); ++i) {
          if (files[i]->clusters.size() < need[i]) {
            Mark(m_next);
            files[i]->clusters.push_back(m_next++);
            more = true;
          }
        }
      }
      for (auto *f : files) {
        LinkInFat(f->clusters);
      }
    }
    for (auto &f : dir.files) {
      if (f.frag_group == 0 && !f.data.empty()) {
        f.clusters = AllocateRun(f.data.size()); // contiguous, NoFatChain
      }
    }
    for (auto &d : dir.dirs) {
      Allocate(d, false);
    }
  }

  void WriteClusters(const std::vector<uint32_t> &clusters,
                     const std::vector<uint8_t> &data) {
    for (size_t i = 0; i < clusters.size(); ++i) {
      const size_t at = (uint64_t(HeapOffsetSectors) * SectorSize) +
                        uint64_t(clusters[i] - 2) * ClusterSize;
      const size_t from = i * ClusterSize;
      if (from < data.size()) {
        std::memcpy(m_image.data() + at, data.data() + from,
                    std::min<size_t>(ClusterSize, data.size() - from));
      }
    }
  }

  static uint16_t NameHash(const std::u16string &name) {
    uint16_t hash = 0;
    for (const auto c : name) {
      const auto up = Upcase(c);
      for (const uint8_t byte : {static_cast<uint8_t>(up & 0xFF), static_cast<uint8_t>(up >> 8)}) {
        hash = static_cast<uint16_t>(((hash << 15) | (hash >> 1)) + byte);
      }
    }
    return hash;
  }

  static void AppendSet(std::vector<uint8_t> &out, const std::string &utf8,
                        bool is_dir, uint8_t flags, uint64_t valid,
                        uint64_t length, uint32_t first, bool corrupt,
                        bool deleted) {
    const auto name = ToUtf16(utf8);
    const size_t name_entries = (name.size() + 14) / 15;
    std::vector<uint8_t> set((2 + name_entries) * 32, 0);
    set[0] = 0x85;
    set[1] = static_cast<uint8_t>(1 + name_entries);
    Put16(set, 4, is_dir ? 0x10 : 0x20);
    for (const size_t at : {8, 12, 16}) { // create, modify, access: 2024-01-01
      Put32(set, at, (44u << 25) | (1u << 21) | (1u << 16));
    }
    set[32] = 0xC0;
    set[33] = flags;
    set[35] = static_cast<uint8_t>(name.size());
    Put16(set, 36, NameHash(name));
    Put64(set, 40, valid);
    Put32(set, 52, first);
    Put64(set, 56, length);
    for (size_t k = 0; k < name_entries; ++k) {
      set[64 + k * 32] = 0xC1;
      for (size_t c = 0; c < 15 && k * 15 + c < name.size(); ++c) {
        Put16(set, 64 + k * 32 + 2 + c * 2, name[k * 15 + c]);
      }
    }
    uint16_t checksum = 0;
    for (size_t i = 0; i < set.size(); ++i) {
      if (i != 2 && i != 3) {
        checksum = static_cast<uint16_t>(((checksum << 15) | (checksum >> 1)) + set[i]);
      }
    }
    Put16(set, 2, corrupt ? static_cast<uint16_t>(checksum ^ 0x5A5A) : checksum);
    if (deleted) {
      for (size_t i = 0; i < set.size(); i += 32) {
        set[i] &= 0x7F; // clear the in-use bit
      }
    }
    out.insert(out.end(), set.begin(), set.end());
  }

  void Serialize(Dir &dir, bool is_root) {
    std::vector<uint8_t> bytes;
    if (is_root) {
      std::vector<uint8_t> e(32, 0);
      e[0] = 0x81;
      Put32(e, 20, m_bitmap_clusters.front());
      Put64(e, 24, m_bitmap.size());
      bytes.insert(bytes.end(), e.begin(), e.end());
      std::vector<uint8_t> u(32, 0);
      u[0] = 0x82;
      Put32(u, 4, m_upcase_checksum);
      Put32(u, 20, m_upcase_clusters.front());
      Put64(u, 24, UpcaseEntries * 2);
      bytes.insert(bytes.end(), u.begin(), u.end());
    }
    for (auto &d : dir.dirs) {
      const uint64_t size = d.clusters.size() * ClusterSize;
      AppendSet(bytes, d.name, true, 0x01, size, size, d.clusters.front(), false, false);
    }
    for (auto &f : dir.files) {
      const uint64_t length = f.data.size();
      const uint64_t valid = std::min<uint64_t>(f.valid, length);
      const bool no_chain = f.frag_group == 0 && length != 0;
      const uint8_t flags = no_chain ? 0x03 : 0x01;
      AppendSet(bytes, f.name, false, flags, valid, length,
                f.clusters.empty() ? 0 : f.clusters.front(), f.corrupt_checksum,
                f.deleted);
      WriteClusters(f.clusters, f.data);
    }
    WriteClusters(dir.clusters, bytes);
    for (auto &d : dir.dirs) {
      Serialize(d, false);
    }
  }

  void WriteBootRegion() {
    std::vector<uint8_t> boot(SectorSize, 0);
    boot[0] = 0xEB;
    boot[1] = 0x76;
    boot[2] = 0x90;
    std::memcpy(boot.data() + 3, "EXFAT   ", 8);
    Put64(boot, 72, uint64_t(HeapOffsetSectors) + uint64_t(ClusterCount) * SectorsPerCluster);
    Put32(boot, 80, FatOffsetSectors);
    Put32(boot, 84, FatLengthSectors);
    Put32(boot, 88, HeapOffsetSectors);
    Put32(boot, 92, ClusterCount);
    Put32(boot, 96, root.clusters.front());
    Put32(boot, 100, 0x12345678);
    Put16(boot, 104, 0x0100);
    boot[108] = 9;
    boot[109] = 1;
    boot[110] = 1;
    boot[111] = 0x80;
    boot[112] = 0xFF;
    boot[510] = 0x55;
    boot[511] = 0xAA;

    std::vector<uint8_t> region(12 * SectorSize, 0);
    std::memcpy(region.data(), boot.data(), SectorSize);
    for (size_t s = 1; s <= 8; ++s) {
      Put32(region, s * SectorSize + 508, 0xAA550000);
    }
    uint32_t checksum = 0;
    for (size_t i = 0; i < 11 * SectorSize; ++i) {
      if (i != 106 && i != 107 && i != 112) {
        checksum = ((checksum << 31) | (checksum >> 1)) + region[i];
      }
    }
    for (size_t i = 0; i < SectorSize; i += 4) {
      Put32(region, 11 * SectorSize + i, checksum);
    }
    std::memcpy(m_image.data(), region.data(), region.size());
    std::memcpy(m_image.data() + 12 * SectorSize, region.data(), region.size());
  }
};

std::vector<uint8_t> Pattern(size_t size, uint32_t seed) {
  std::vector<uint8_t> data(size);
  uint32_t state = seed * 2654435761u + 1;
  for (auto &b : data) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
  return data;
}

const std::string UnicodeName = "asset-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin";
const std::string EmojiName = "emoji-\xf0\x9f\x98\x80.bin";
const std::string LongName = std::string(255, 'a');

struct Fixture {
  std::map<std::string, std::vector<uint8_t>> files; // member path -> bytes
  std::vector<uint8_t> image;
};

Fixture MakeFixture() {
  Fixture fx;
  ImageBuilder b;
  const auto add = [&](Dir &dir, const std::string &prefix, Item item) {
    fx.files[prefix + item.name] = item.data;
    dir.files.push_back(std::move(item));
  };
  add(b.root, "", {"eboot.bin", Pattern(11, 1)});
  add(b.root, "", {"MiXeD_CaSe.TXT", Pattern(300, 2)});
  add(b.root, "", {UnicodeName, Pattern(1500, 3)});
  add(b.root, "", {EmojiName, Pattern(77, 4)});
  add(b.root, "", {LongName, Pattern(900, 5)});
  add(b.root, "", {"empty.bin", {}});
  add(b.root, "", {"contiguous.bin", Pattern(100 * 1024 + 321, 6)});
  add(b.root, "", {"frag_a.bin", Pattern(40 * 1024 + 5, 7), UINT64_MAX, 1});
  add(b.root, "", {"frag_b.bin", Pattern(33 * 1024, 8), UINT64_MAX, 1});

  Item short_valid{"short_valid.bin", Pattern(3000, 9), 1500};
  fx.files["short_valid.bin"] = short_valid.data;
  std::fill(fx.files["short_valid.bin"].begin() + 1500, fx.files["short_valid.bin"].end(), 0);
  b.root.files.push_back(short_valid);

  // Entries the reader has to ignore.
  b.root.files.push_back({"bad_checksum.bin", Pattern(10, 10), UINT64_MAX, 0, true});
  b.root.files.push_back({"deleted.bin", Pattern(10, 11), UINT64_MAX, 0, false, true});

  Dir sce{"sce_sys"};
  add(sce, "sce_sys/", {"param.json", Pattern(64, 12)});
  b.root.dirs.push_back(std::move(sce));

  Dir many{"many"};
  for (int i = 0; i < 100; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "file_%03d_with_a_reasonably_long_name.dat", i);
    add(many, "many/", {name, Pattern(static_cast<size_t>(i) * 7, 100 + i)});
  }
  b.root.dirs.push_back(std::move(many));

  Dir deep{"assets"};
  Dir sub{"subdir"};
  Dir deeper{"deeper"};
  add(deeper, "assets/subdir/deeper/", {"data.bin", Pattern(5000, 13)});
  sub.dirs.push_back(std::move(deeper));
  deep.dirs.push_back(std::move(sub));
  b.root.dirs.push_back(std::move(deep));
  b.root.dirs.push_back(Dir{"emptydir"});

  fx.image = b.Build();
  return fx;
}

void WriteImage(const std::filesystem::path &path, const std::vector<uint8_t> &image,
                size_t size = SIZE_MAX) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(image.data()),
            static_cast<std::streamsize>(std::min(size, image.size())));
  Check(out.good(), "write image");
}

std::vector<uint8_t> ReadAll(Common::ExFatVolume &volume, const std::string &member,
                             size_t chunk) {
  const auto entry = volume.Find(member);
  Check(entry && entry->is_file, "member exists and is a file");
  std::vector<uint8_t> data(entry->size);
  size_t done = 0;
  while (done < data.size()) {
    const auto want = std::min<uint64_t>(chunk, data.size() - done);
    const auto got = volume.Read(entry->id, done, want, data.data() + done);
    Check(got == want, "read returns the requested bytes");
    done += got;
  }
  Check(volume.Read(entry->id, entry->size, 16, data.data()) == 0,
        "read at end of file returns nothing");
  return data;
}

void TestContents(const Fixture &fx, const std::filesystem::path &path) {
  auto volume = Common::ExFatVolume::Open(path);
  Check(volume != nullptr, "open image");

  // Listing: ignores deleted and corrupt entry sets, keeps everything else.
  std::set<std::string> names;
  for (const auto &item : volume->List("")) {
    Check(names.insert(item.name).second, "no duplicate names");
  }
  Check(!names.count("deleted.bin") && !names.count("bad_checksum.bin"),
        "deleted and corrupt entry sets are skipped");
  for (const auto &name : {"eboot.bin", "MiXeD_CaSe.TXT", "empty.bin", "contiguous.bin",
                           "frag_a.bin", "frag_b.bin", "sce_sys", "many", "assets", "emptydir"}) {
    Check(names.count(name) == 1, name);
  }
  Check(names.count(UnicodeName) && names.count(EmojiName) && names.count(LongName),
        "unicode, surrogate-pair and 255 character names");
  Check(volume->List("many").size() == 100, "directory spanning many clusters");
  Check(volume->List("emptydir").empty(), "empty directory");
  Check(volume->List("eboot.bin").empty() && volume->List("nope").empty(),
        "listing a file or a missing directory is empty");

  // Every file, through several read sizes (smaller and larger than a cluster).
  for (const auto &[member, expected] : fx.files) {
    for (const size_t chunk : {size_t{1000}, size_t{4097}, size_t{1} << 20}) {
      Check(ReadAll(*volume, member, chunk) == expected, member.c_str());
    }
  }

  // Random partial reads crossing cluster and run boundaries, including past EOF.
  for (const auto *member : {"frag_a.bin", "frag_b.bin", "contiguous.bin", "short_valid.bin"}) {
    const auto &expected = fx.files.at(member);
    const auto entry = volume->Find(member);
    Check(entry.has_value() && entry->size == expected.size(), "size");
    uint32_t state = 12345;
    for (int i = 0; i < 400; ++i) {
      state = state * 1664525u + 1013904223u;
      const uint64_t offset = (state >> 8) % (expected.size() + 100);
      state = state * 1664525u + 1013904223u;
      const uint64_t size = (state >> 8) % 3000;
      std::vector<uint8_t> got(size + 1, 0xCC);
      const auto n = volume->Read(entry->id, offset, size, got.data());
      const uint64_t want = offset >= expected.size() ? 0 : std::min<uint64_t>(size, expected.size() - offset);
      Check(n == want, "partial read length");
      Check(std::equal(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n),
                       expected.begin() + static_cast<std::ptrdiff_t>(std::min<uint64_t>(offset, expected.size()))),
            "partial read bytes");
      Check(got[size] == 0xCC, "read does not write past the requested size");
    }
  }

  // Lookups are case-insensitive, also beyond ASCII (via the volume's up-case table).
  Check(volume->Find("mixed_case.txt").has_value(), "ascii case folding");
  Check(volume->Find("SCE_SYS/PARAM.JSON").has_value(), "case folding in directories");
  Check(volume->Find("ASSET-\xc3\x89-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.BIN").has_value(),
        "non-ascii case folding");
  Check(volume->Find("assets//subdir/./deeper/data.bin").has_value(), "redundant separators");
  Check(!volume->Find("nope").has_value() && !volume->Find("eboot.bin/x").has_value() &&
            !volume->Find("sce_sys/missing").has_value() && !volume->Find("bad\xff").has_value(),
        "missing paths are not found");
  const auto root = volume->Find("");
  Check(root && !root->is_file, "empty path is the root directory");
  const auto empty = volume->Find("empty.bin");
  Check(empty && empty->is_file && empty->size == 0, "empty file");
}

void TestConcurrency(const Fixture &fx, const std::filesystem::path &path) {
  auto volume = Common::ExFatVolume::Open(path);
  Check(volume != nullptr, "open image");
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < 20; ++round) {
        for (const auto &[member, expected] : fx.files) {
          if ((std::hash<std::string>{}(member) + static_cast<size_t>(t + round)) % 3 != 0) {
            continue;
          }
          const auto entry = volume->Find(member);
          std::vector<uint8_t> data(expected.size());
          if (!entry || volume->Read(entry->id, 0, data.size(), data.data()) != data.size() ||
              data != expected) {
            ++failures;
          }
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  Check(failures == 0, "concurrent reads are consistent");
}

void TestRejection(const Fixture &fx, const std::filesystem::path &directory) {
  const auto bad = directory / "bad.exfat";
  Check(Common::ExFatVolume::Open(directory / "missing.exfat") == nullptr, "missing file");

  WriteImage(bad, std::vector<uint8_t>(4096, 0));
  Check(Common::ExFatVolume::Open(bad) == nullptr, "zeroed file is not exFAT");

  WriteImage(bad, fx.image, 100);
  Check(Common::ExFatVolume::Open(bad) == nullptr, "file shorter than a boot sector");

  auto damaged = fx.image;
  damaged[510] = 0; // boot signature
  WriteImage(bad, damaged);
  Check(Common::ExFatVolume::Open(bad) == nullptr, "missing boot signature");

  damaged = fx.image;
  std::memcpy(damaged.data() + 3, "NTFS    ", 8);
  WriteImage(bad, damaged);
  Check(Common::ExFatVolume::Open(bad) == nullptr, "wrong file system name");

  // An image cut off in the middle of the data still opens; reads are short and never crash.
  WriteImage(bad, fx.image, fx.image.size() / 3);
  auto truncated = Common::ExFatVolume::Open(bad);
  Check(truncated != nullptr, "truncated image opens");
  for (const auto &[member, expected] : fx.files) {
    const auto entry = truncated->Find(member);
    if (!entry || entry->size == 0) {
      continue;
    }
    std::vector<uint8_t> data(entry->size);
    const auto got = truncated->Read(entry->id, 0, data.size(), data.data());
    Check(got <= data.size(), "short read on truncated image");
  }
}

// The same image through the archive layer used by the launcher and the guest file system.
void TestArchiveIntegration(const Fixture &fx, const std::filesystem::path &path) {
  Check(Common::IsSupportedArchive("game.exfat") && Common::IsSupportedArchive("GAME.EXFAT") &&
            !Common::IsSupportedArchive("game.exfat.bak"),
        "extension routing");
  const auto root = Common::MakeArchivePath(path);
  Check(Common::IsArchivePath(root), "archive path marker");
  Check(Common::File::IsFileExisting(Common::MakeArchivePath(path, "sce_sys/param.json")),
        "file inside the image exists");
  Check(!Common::File::IsFileExisting(Common::MakeArchivePath(path, "sce_sys/nope.json")),
        "missing file inside the image");

  const auto expected = fx.files.at("sce_sys/param.json");
  Common::File file(Common::MakeArchivePath(path, "sce_sys/param.json"),
                    Common::File::Mode::Read);
  Check(!file.IsInvalid() && file.Size() == expected.size(), "open via Common::File");
  std::vector<uint8_t> data(expected.size());
  uint32_t read = 0;
  file.Read(data.data(), static_cast<uint32_t>(data.size()), &read);
  Check(read == data.size() && data == expected, "read via Common::File");

  // A larger, fragmented file read through the shared 1 MiB chunked path.
  const auto &frag = fx.files.at("frag_a.bin");
  Common::File big(Common::MakeArchivePath(path, "frag_a.bin"), Common::File::Mode::Read);
  Check(!big.IsInvalid() && big.Size() == frag.size(), "open fragmented file");
  std::vector<uint8_t> big_data(frag.size());
  read = 0;
  big.Read(big_data.data(), static_cast<uint32_t>(big_data.size()), &read);
  Check(read == big_data.size() && big_data == frag, "read fragmented file via Common::File");

  std::set<std::string> names;
  for (const auto &entry : Common::File::GetDirEntries(root)) {
    names.insert(entry.name);
  }
  Check(names.count("eboot.bin") && names.count("sce_sys") && names.count("many"),
        "directory listing via Common::File");
}

} // namespace

int main(int argc, char **argv) {
  // `exfat_tests --write-image <path>` only dumps the fixture image, so that it can be checked
  // with an independent implementation such as `fsck.exfat`.
  if (argc == 3 && std::strcmp(argv[1], "--write-image") == 0) {
    WriteImage(argv[2], MakeFixture().image);
    return 0;
  }

  TempDirectory temp;
  const auto fixture = MakeFixture();
  const auto image = temp.Path() / "game.exfat";
  WriteImage(image, fixture.image);

  TestContents(fixture, image);
  TestConcurrency(fixture, image);
  TestRejection(fixture, temp.Path());
  TestArchiveIntegration(fixture, image);
  std::puts("ExFatTests: all passed");
  return 0;
}
