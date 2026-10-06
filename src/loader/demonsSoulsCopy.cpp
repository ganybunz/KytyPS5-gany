#include "loader/demonsSoulsCopy.h"

#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/demonsSouls.h"
#include "kernel/memory.h"
#include "loader/runtimeLinker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xxhash.h>

namespace Loader::DemonsSoulsCopy {
namespace {
uint64_t                site = 0;
std::array<uint8_t, 13> original {}, patch {};
void* KYTY_SYSV_ABI     CoherentMove(void* destination, const void* source, size_t size) {
    return Move(destination, source, size, &Libs::LibKernel::Memory::TryPrepareHostWrite);
}
} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	constexpr uint64_t offset = 0x3c36, verify_size = 2048;
	if (site || !program || !Libs::Graphics::DemonsSouls::IsSupportedGame() ||
	    program->file_name.filename() != "libc.prx" ||
	    program->mapped_size < offset + verify_size ||
	    program->base_vaddr > UINT64_MAX - offset - verify_size)
		return;
	const auto address = program->base_vaddr + offset;
	if (!Libs::Graphics::HostMemoryRangeIsReadable(address, verify_size)) return;
	const auto* bytes = reinterpret_cast<const uint8_t*>(address);
	// Hash the complete audited shared memmove/bcopy body, not just the prologue.
	// This entry follows the guest's argument/stack checks and has one saved RBP.
	// Changed module versions, relocations, or platform loader patches fail closed.
	const auto actual_hash = XXH3_64bits(bytes, verify_size);
	const auto* audit = std::getenv("KYTY_DS_PATCH_AUDIT");
	if (audit != nullptr && std::strcmp(audit, "1") == 0) {
		std::printf("Demon's Souls patch audit: module=libc base=0x%016llx memmove=0x%016llx "
		            "size=0x%llx actual=0x%016llx expected=0xe654325b8be848a9 match=%s\n",
		            static_cast<unsigned long long>(program->base_vaddr),
		            static_cast<unsigned long long>(address),
		            static_cast<unsigned long long>(verify_size),
		            static_cast<unsigned long long>(actual_hash),
		            actual_hash == 0xe654325b8be848a9ull ? "yes" : "no");
	}
	if (actual_hash != 0xe654325b8be848a9ull) {
		std::printf("Demon's Souls copy: libc memmove differs; retaining guest code\n");
		return;
	}
	std::memcpy(original.data(), bytes, original.size());
	patch = Tailcall(reinterpret_cast<uint64_t>(&CoherentMove));
	std::memcpy(reinterpret_cast<void*>(address), patch.data(), patch.size());
	if (!Common::VirtualMemory::FlushInstructionCache(address, patch.size())) {
		std::memcpy(reinterpret_cast<void*>(address), original.data(), original.size());
		Common::VirtualMemory::FlushInstructionCache(address, original.size());
		return;
	}
	site = address;
	std::printf("Demon's Souls copy: installed verified coherent memmove\n");
#endif
}

void Clear() {
	if (!site) return;
	if (std::memcmp(reinterpret_cast<const void*>(site), patch.data(), patch.size()) == 0) {
		std::memcpy(reinterpret_cast<void*>(site), original.data(), original.size());
		Common::VirtualMemory::FlushInstructionCache(site, original.size());
	}
	site = 0;
}
} // namespace Loader::DemonsSoulsCopy
