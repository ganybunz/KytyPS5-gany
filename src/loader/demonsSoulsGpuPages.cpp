#include "loader/demonsSoulsGpuPages.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/demonsSouls.h"
#include "kernel/memory.h"
#include "loader/runtimeLinker.h"
#include <cstdio>
#include <cstdlib>

namespace Loader::DemonsSoulsGpuPages {
namespace {
uint64_t base = 0, cave = 0;
size_t written = 0;
constexpr std::array<uint64_t, 3> AllSites {Sites[0], Sites[1], ContextSite};
std::array<std::array<uint8_t, 5>, AllSites.size()> original {}, installed {};
bool PatchAuditEnabled() {
    const auto* value = std::getenv("KYTY_DS_PATCH_AUDIT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}
}
void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
    // KYTY_GPU_BUFFER_PAGES=1: the two CBuffer allocation calls and the render-context call.
    const auto* enabled = std::getenv("KYTY_GPU_BUFFER_PAGES");
    if (!enabled || std::strcmp(enabled, "1") != 0 || cave || !program ||
        program->file_name.filename() != "eboot.bin" ||
        !Libs::Graphics::DemonsSouls::IsSupportedGame() || program->mapped_size >= CaveOffset ||
        program->base_vaddr > UINT64_MAX - CaveOffset - ImageSize ||
        program->tls.handler_vaddr < program->base_vaddr ||
        program->tls.handler_vaddr - program->base_vaddr > INT32_MAX) return;
    if (PatchAuditEnabled()) {
        const auto* guest = reinterpret_cast<const uint8_t*>(program->base_vaddr);
        std::printf("Demon's Souls patch audit: module=eboot base=0x%016llx mapped=0x%llx tls=0x%016llx\n",
            static_cast<unsigned long long>(program->base_vaddr),
            static_cast<unsigned long long>(program->mapped_size),
            static_cast<unsigned long long>(program->tls.handler_vaddr));
        for (const auto& function : AuditedFunctions) {
            const bool readable = function.rva + function.size <= program->mapped_size &&
                                  Libs::Graphics::HostMemoryRangeIsReadable(
                                      program->base_vaddr + function.rva, function.size);
            const auto raw_hash = readable
                                      ? Fingerprint(guest + function.rva, function.size)
                                      : 0;
            std::printf("Demon's Souls patch audit: eboot+0x%llx size=0x%llx raw=0x%016llx "
                        "expected=0x%016llx readable=%s match=%s\n",
                static_cast<unsigned long long>(function.rva),
                static_cast<unsigned long long>(function.size),
                static_cast<unsigned long long>(raw_hash),
                static_cast<unsigned long long>(function.hash),
                readable ? "yes" : "no",
                readable && raw_hash == function.hash ? "yes" : "no");
        }
    }
    const auto* guest = reinterpret_cast<const uint8_t*>(program->base_vaddr);
    for (const auto& function : AuditedFunctions) {
        if (function.rva + function.size > program->mapped_size ||
            !Libs::Graphics::HostMemoryRangeIsReadable(program->base_vaddr + function.rva, function.size) ||
            !VerifyLoadedFunction(guest, function, program->tls.handler_vaddr - program->base_vaddr)) {
            std::printf("GPU buffer pages: audited function differs at %llx; retaining original allocations\n",
                 static_cast<unsigned long long>(function.rva));
            return;
        }
    }
    std::array<uint8_t, ImageSize> image;
    if (!BuildImage(image.data())) return;
    const size_t site_count = AllSites.size();
    for (size_t i = 0; i < site_count; ++i) {
        if (!BuildCall(installed[i], guest, AllSites[i])) return;
        std::memcpy(original[i].data(), guest + AllSites[i], 5);
    }
    const auto requested = program->base_vaddr + CaveOffset;
    const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(requested, ImageSize,
        Common::VirtualMemory::Mode::ReadWrite, "gpu_buffer_pages", true);
    if (allocated != requested) {
        if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, ImageSize);
        return;
    }
    base = program->base_vaddr;
    cave = allocated;
    std::memcpy(reinterpret_cast<void*>(cave), image.data(), image.size());
    if (!Common::VirtualMemory::FlushInstructionCache(cave, StatsOffset) ||
        !Libs::LibKernel::Memory::ProtectGuestMemory(cave, StatsOffset,
            Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
        Clear();
        return;
    }
    for (size_t i = 0; i < site_count; ++i) {
        const auto address = base + AllSites[i];
        std::memcpy(reinterpret_cast<void*>(address), installed[i].data(), 5);
        ++written;
        if (!Common::VirtualMemory::FlushInstructionCache(address, 5)) {
            Clear();
            return;
        }
    }
    std::printf("GPU buffer pages: %zu verified allocation calls installed, 4096-byte pages\n", site_count);
#endif
}
void Clear() {
    if (!cave) return;
    for (size_t i = 0; i < written; ++i) {
        auto* address = reinterpret_cast<void*>(base + AllSites[i]);
        if (std::memcmp(address, installed[i].data(), 5) == 0) {
            std::memcpy(address, original[i].data(), 5);
            Common::VirtualMemory::FlushInstructionCache(base + AllSites[i], 5);
        }
    }
    const auto* stats = reinterpret_cast<const uint64_t*>(cave + StatsOffset);
    LOGF("GPU buffer pages: cumulative requests=%llu logical=%llu physical=%llu\n",
         static_cast<unsigned long long>(stats[0]), static_cast<unsigned long long>(stats[1]),
         static_cast<unsigned long long>(stats[2]));
    Libs::LibKernel::Memory::FreeGuestMemory(cave, ImageSize);
    base = cave = written = 0;
}
} // namespace Loader::DemonsSoulsGpuPages
