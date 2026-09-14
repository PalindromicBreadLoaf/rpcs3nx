#include "exception_handler.h"

#include "exception_entry.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

static_assert(sizeof(CpuRegister) == 8);
static_assert(sizeof(FpuRegister) == 16);
static_assert(offsetof(ThreadExceptionDump, error_desc) == RPCS3_DUMP_ERROR_DESC);
static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == RPCS3_DUMP_GPRS);
static_assert(offsetof(ThreadExceptionDump, fp) == RPCS3_DUMP_FP);
static_assert(offsetof(ThreadExceptionDump, lr) == RPCS3_DUMP_LR);
static_assert(offsetof(ThreadExceptionDump, sp) == RPCS3_DUMP_SP);
static_assert(offsetof(ThreadExceptionDump, pc) == RPCS3_DUMP_PC);
static_assert(offsetof(ThreadExceptionDump, padding) == RPCS3_DUMP_PADDING);
static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == RPCS3_DUMP_FPU);
static_assert(offsetof(ThreadExceptionDump, pstate) == RPCS3_DUMP_PSTATE);
static_assert(offsetof(ThreadExceptionDump, far) == RPCS3_DUMP_FAR);
static_assert(offsetof(ThreadExceptionDump, esr) == RPCS3_DUMP_PSTATE + 12);
static_assert(sizeof(ThreadExceptionDump) <= RPCS3_SLOT_FRAME);
static_assert(RPCS3_SLOT_INDEX + sizeof(u64) <= RPCS3_EXCEPTION_SLOT_SIZE);

static_assert(offsetof(ThreadExceptionFrameA64, lr) == RPCS3_FRAME_LR);
static_assert(offsetof(ThreadExceptionFrameA64, sp) == RPCS3_FRAME_SP);
static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == RPCS3_FRAME_PC);
static_assert(offsetof(ThreadExceptionFrameA64, pstate) == RPCS3_FRAME_PSTATE);
static_assert(offsetof(ThreadExceptionFrameA64, esr) == RPCS3_FRAME_PSTATE + 12);
static_assert(offsetof(ThreadExceptionFrameA64, far) == RPCS3_FRAME_FAR);

namespace
{
	constexpr u32 esr_exception_class_data_abort_lower = 0x24;
	constexpr u32 esr_exception_class_data_abort_same = 0x25;

	std::atomic<rpcs3::switch_runtime::exception_handler> s_handler{nullptr};
	std::atomic<void*> s_handler_user{nullptr};
	std::atomic<u32> s_active_dispatches{0};
	std::atomic_flag s_registration_lock = ATOMIC_FLAG_INIT;
	static_assert(decltype(s_handler)::is_always_lock_free);
	static_assert(decltype(s_handler_user)::is_always_lock_free);
	static_assert(decltype(s_active_dispatches)::is_always_lock_free);

	void lock_registration() noexcept
	{
		while (s_registration_lock.test_and_set(std::memory_order_acquire))
		{
			svcSleepThread(YieldType_WithoutCoreMigration);
		}
	}

	void unlock_registration() noexcept
	{
		s_registration_lock.clear(std::memory_order_release);
	}
} // namespace

namespace rpcs3::switch_runtime
{
	bool install_exception_handler(exception_handler handler, void* user) noexcept
	{
		if (!handler)
		{
			return false;
		}

		lock_registration();
		if (s_handler.load(std::memory_order_relaxed))
		{
			unlock_registration();
			return false;
		}

		s_handler_user.store(user, std::memory_order_relaxed);
		s_handler.store(handler, std::memory_order_release);
		unlock_registration();
		return true;
	}

	void uninstall_exception_handler() noexcept
	{
		lock_registration();
		s_handler.store(nullptr, std::memory_order_release);
		while (s_active_dispatches.load(std::memory_order_acquire))
		{
			svcSleepThread(YieldType_WithoutCoreMigration);
		}
		s_handler_user.store(nullptr, std::memory_order_relaxed);
		unlock_registration();
	}

	bool exception_handler_installed() noexcept
	{
		return s_handler.load(std::memory_order_acquire) != nullptr;
	}
} // namespace rpcs3::switch_runtime

extern "C" bool HorizonExceptionDispatch(ThreadExceptionDump* context)
{
	if (!context || !threadExceptionIsAArch64(context))
	{
		return false;
	}

	const u32 exception_class = context->esr >> 26;
	if (exception_class != esr_exception_class_data_abort_lower &&
		exception_class != esr_exception_class_data_abort_same)
	{
		return false;
	}

	const auto handler = s_handler.load(std::memory_order_acquire);
	if (!handler)
	{
		return false;
	}

	s_active_dispatches.fetch_add(1, std::memory_order_acquire);
	if (handler != s_handler.load(std::memory_order_acquire))
	{
		s_active_dispatches.fetch_sub(1, std::memory_order_release);
		return false;
	}

	const bool handled = handler(*context, s_handler_user.load(std::memory_order_relaxed));
	s_active_dispatches.fetch_sub(1, std::memory_order_release);
	return handled;
}

extern "C" [[noreturn]] void __libnx_exception_handler(ThreadExceptionDump* context)
{
	std::fprintf(stderr, "Unhandled Horizon exception: pc=%016lx far=%016lx esr=%08x\n",
		context ? context->pc.x : 0, context ? context->far.x : 0, context ? context->esr : 0);
	svcBreak(BreakReason_Panic, reinterpret_cast<uintptr_t>(context), context ? sizeof(*context) : 0);
	svcExitProcess();
}
