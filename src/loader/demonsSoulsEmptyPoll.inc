// Included inside DemonsSoulsIdle's private namespace, after Readable().
// Exact USA 01.004 experiment. No patch is installed unless explicitly enabled.
#if defined(__x86_64__) || defined(_M_X64)
uint64_t empty_poll_cave = 0, empty_poll_site = 0;
const Program* empty_poll_owner = nullptr;
std::array<uint8_t, 5> empty_poll_original {}, empty_poll_installed {};

void KYTY_SYSV_ABI BackoffEmptyPoll() {
	Common::Thread::SleepMicroWithoutSpinning(5);
}

void InstallEmptyPoll(Program* program) {
	const char* enabled = std::getenv("KYTY_DS_EMPTY_POLL_BACKOFF");
	if (enabled == nullptr || std::strcmp(enabled, "1") != 0 || empty_poll_cave != 0) return;
	std::string title, version;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title) || title != "PPSA01342" ||
	    !Loader::SystemContentParamSfoGetString("APP_VER", &version) || version != "01.004.000") return;
	const Xbyak::util::Cpu cpu;
	// Xbyak's AVX flag also checks OSXSAVE and XCR0's XMM/YMM enablement.
	if (!cpu.has(Xbyak::util::Cpu::tAVX)) return;
	constexpr uint64_t GuardOffset = 0x86eb1d, SiteOffset = 0x86eb39, EmptyCaveOffset = 0x8004000;
	constexpr uint64_t FunctionOffset = 0x86ea30, FunctionSize = 658;
	// Both entries to the pause have tested r15b bit0 clear. Ready-work branches
	// bypass it. Includes the subsequent timestamp call and deadline branches.
	constexpr uint8_t guard[] {
	    0x41,0xf6,0xc7,0x01,0x75,0xa0,0xeb,0x14,
	    0x66,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
	    0x41,0xb7,0x01,0x41,0xf6,0xc7,0x01,0x75,0x8a,
	    0xf3,0x90,0x4d,0x89,0xfd,0x45,0x89,0xe7,
	    0x4e,0x8b,0xa4,0xf3,0x00,0x0f,0x00,0x00,
	    0xe8,0xd2,0x4e,0xff,0xff,0x4c,0x29,0xe0,0x48,0x85,0xc0,
	    0x0f,0x8e,0xf5,0x00,0x00,0x00,0x45,0x89,0xfc,0x48,0x3b,0x45,0xa8,
	    0x0f,0x86,0xfd,0x00,0x00,0x00};
	if (program->mapped_size < FunctionOffset + FunctionSize || program->mapped_size > EmptyCaveOffset ||
	    program->base_vaddr > UINT64_MAX - EmptyCaveOffset - PageSize) return;
	const auto guard_address = program->base_vaddr + GuardOffset;
	const auto function_address = program->base_vaddr + FunctionOffset;
	if (!Readable(function_address, FunctionSize)) return;
	uint64_t hash = 14695981039346656037ull;
	for (uint64_t i = 0; i < FunctionSize; ++i)
		hash = (hash ^ reinterpret_cast<const uint8_t*>(function_address)[i]) * 1099511628211ull;
	if (!Readable(guard_address, sizeof(guard)) ||
	    hash != 0xc85cde9513ef7e84ull ||
	    std::memcmp(reinterpret_cast<const void*>(guard_address), guard, sizeof(guard)) != 0) {
		std::printf("Demon's Souls empty poll: code guard differs; retaining guest code\n");
		return;
	}
	const auto requested = program->base_vaddr + EmptyCaveOffset;
	const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "demons_souls_empty_poll", true);
	if (allocated != requested) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	Xbyak::ClearError();
	Xbyak::CodeGenerator c(PageSize, reinterpret_cast<void*>(allocated));
	using namespace Xbyak::util;
	// A call pushes the return slot that the following RDTSC call already uses;
	// no intervening instruction accesses that slot. Protect the rest of the
	// guest red zone before spilling. LEA leaves original flags intact.
	c.lea(rsp, ptr[rsp - 128]);
	c.pushfq();
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11}) c.push(reg);
	// Entry RSP 8 mod16, minus128 and80 remains8; minus1032 aligns FXSAVE.
	c.sub(rsp, 1032);
	constexpr uint8_t save_fp[] {0x48,0x0f,0xae,0x04,0x24};
	c.db(save_fp, sizeof(save_fp));
	// A host callback may use VZEROUPPER. FXSAVE alone does not preserve YMM
	// upper halves. Save all guest AVX registers explicitly as well as x87/MXCSR.
	for (int i = 0; i < 16; ++i) c.vmovdqu(ptr[rsp + 512 + i * 32], Xbyak::Ymm(i));
	c.mov(rax, reinterpret_cast<uint64_t>(&BackoffEmptyPoll));
	c.call(rax);
	c.fxrstor64(ptr[rsp]);
	for (int i = 0; i < 16; ++i) c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 512 + i * 32]);
	c.add(rsp, 1032);
	for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax}) c.pop(reg);
	c.popfq();
	c.lea(rsp, ptr[rsp + 128]);
	c.mov(r13, r15); // original PAUSE; MOV R13,R15, exactly the five replaced bytes
	c.ret();
	c.ready();
	if (Xbyak::GetError() || !Common::VirtualMemory::FlushInstructionCache(allocated, c.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	const auto patch_site = program->base_vaddr + SiteOffset;
	const int32_t displacement = static_cast<int32_t>(allocated - (patch_site + 5));
	empty_poll_installed[0] = 0xe8;
	std::memcpy(empty_poll_installed.data()+1, &displacement, sizeof(displacement));
	std::memcpy(empty_poll_original.data(), reinterpret_cast<const void*>(patch_site), 5);
	std::memcpy(reinterpret_cast<void*>(patch_site), empty_poll_installed.data(), 5);
	if (!Common::VirtualMemory::FlushInstructionCache(patch_site, 5)) {
		std::memcpy(reinterpret_cast<void*>(patch_site), empty_poll_original.data(), 5);
		Common::VirtualMemory::FlushInstructionCache(patch_site, 5);
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	empty_poll_owner = program;
	empty_poll_cave = allocated;
	empty_poll_site = patch_site;
	std::printf("Demon's Souls empty poll: opt-in 5 us backoff at eboot+0x86eb39 (unverified timing experiment)\n");
}

// Keep the installed patch and lease valid until process teardown. They cannot
// be restored/freed safely without a guest-thread quiescence protocol.
#endif
