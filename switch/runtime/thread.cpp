#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"
#include "util/tsc.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <pthread.h>

namespace stx
{
	atomic_t<u32> g_launch_retainer{0};
}

thread_local thread_base* thread_ctrl::g_tls_this_thread = nullptr;
thread_local void (*thread_ctrl::g_tls_error_callback)() = nullptr;
atomic_t<native_core_arrangement> thread_ctrl::g_native_core_layout{native_core_arrangement::undefined};

void thread_base::start()
{
	m_sync.atomic_op([](u32& value)
		{
			value &= ~static_cast<u32>(thread_state::mask);
			value |= static_cast<u32>(thread_state::created);
		});

	pthread_t thread{};
	if (pthread_create(&thread, nullptr, entry_point, this) != 0)
	{
		thread_ctrl::emergency_exit("Failed to create a Horizon thread");
	}

	u64 value = 0;
	static_assert(sizeof(thread) <= sizeof(value));
	std::memcpy(&value, &thread, sizeof(thread));
	if (!m_thread && !m_thread.compare_and_swap_test(0, value) && m_thread != value)
	{
		thread_ctrl::emergency_exit("Horizon thread handle mismatch");
	}
}

void thread_base::initialize(void (*error_cb)())
{
	pthread_t thread = pthread_self();
	u64 value = 0;
	std::memcpy(&value, &thread, sizeof(thread));
	if (!m_thread && !m_thread.compare_and_swap_test(0, value) && m_thread != value)
	{
		thread_ctrl::emergency_exit("Horizon thread initialization mismatch");
	}

	m_handle = threadGetCurHandle();
	thread_ctrl::g_tls_this_thread = this;
	thread_ctrl::g_tls_error_callback = error_cb;
	set_name(*m_tname.load());
}

void thread_base::set_name(std::string name)
{
	static_cast<void>(name);
}

u64 thread_base::finalize(thread_state result_state) noexcept
{
	const u64 self = m_thread;
	m_sync.fetch_op([&](u32& value)
		{
			value &= ~static_cast<u32>(thread_state::mask);
			value |= static_cast<u32>(result_state);
		});
	m_sync.notify_all();
	return self;
}

thread_base::native_entry thread_base::finalize(u64 self) noexcept
{
	thread_ctrl::g_tls_this_thread = nullptr;
	thread_ctrl::g_tls_error_callback = nullptr;
	if (self == umax)
	{
		return nullptr;
	}
	pthread_exit(nullptr);
}

thread_state thread_ctrl::state()
{
	auto* const self = g_tls_this_thread;
	if (!self)
	{
		return thread_state::finished;
	}

	static thread_local bool executing = false;
	if (!executing)
	{
		executing = true;
		self->exec();
		executing = false;
	}
	return static_cast<thread_state>(self->m_sync & static_cast<u32>(thread_state::mask));
}

void thread_ctrl::wait_for(u64 usec, bool alert)
{
	if (!usec)
	{
		return;
	}

	auto* const self = g_tls_this_thread;
	if (!self)
	{
		if (usec == umax)
		{
			svcSleepThread(INT64_MAX);
			return;
		}
		svcSleepThread(static_cast<s64>(std::min<u64>(usec, static_cast<u64>(INT64_MAX) / 1000) * 1000));
		return;
	}

	if (!alert && usec > 50'000)
	{
		usec = 50'000;
	}
	if (alert && (self->m_sync.bit_test_reset(2) || self->m_taskq))
	{
		return;
	}

	atomic_wait::list<2> list{};
	if (alert)
	{
		list.set<0>(self->m_sync, 0);
		list.set<1>(self->m_taskq);
	}
	else
	{
		list.set<0>(self->m_dummy, 0);
	}
	list.wait(atomic_wait_timeout{usec <= std::numeric_limits<u64>::max() / 1000 ? usec * 1000 : std::numeric_limits<u64>::max()});
}

void thread_ctrl::wait_until(u64* wait_time, u64 add_time, u64 min_wait, bool update_to_current_time)
{
	auto add_saturate = [](u64 left, u64 right)
	{
		return left <= std::numeric_limits<u64>::max() - right ? left + right : std::numeric_limits<u64>::max();
	};
	*wait_time = add_saturate(*wait_time, add_time);
	const u64 current_time = utils::get_tsc() * 1'000'000 / armGetSystemTickFreq();
	if (current_time > *wait_time)
	{
		if (update_to_current_time && add_time)
		{
			*wait_time = current_time + (add_time - (current_time - *wait_time) % add_time);
		}
		else if (!min_wait)
		{
			return;
		}
	}
	if (min_wait)
	{
		*wait_time = std::max(*wait_time, add_saturate(current_time, min_wait));
	}
	wait_for(*wait_time - current_time);
}

void thread_ctrl::wait_for_accurate(u64 usec)
{
	if (!usec)
	{
		return;
	}
	if (usec > 50'000)
	{
		emergency_exit("thread_ctrl::wait_for_accurate: unsupported amount");
	}

	const u64 frequency = armGetSystemTickFreq();
	const u64 deadline = utils::get_tsc() + (usec * frequency + 999'999) / 1'000'000;
	for (;;)
	{
		const u64 now = utils::get_tsc();
		if (now >= deadline)
		{
			break;
		}
		const u64 remaining = (deadline - now) * 1'000'000 / frequency;
		if (remaining >= 500)
		{
			wait_for(remaining - remaining % 500, false);
		}
		else if (remaining >= 250)
		{
			svcSleepThread(YieldType_WithoutCoreMigration);
		}
		else
		{
			__asm__ volatile("yield");
		}
	}
}

std::string thread_ctrl::get_name_cached()
{
	auto* const self = g_tls_this_thread;
	return self ? *self->m_tname.load() : std::string{};
}

thread_base::thread_base(native_entry entry, std::string name) noexcept
	: entry_point(entry), m_tname(make_single_value(std::move(name)))
{
}

thread_base::~thread_base() noexcept
{
	exec();
	pthread_t thread{};
	const u64 value = m_thread;
	std::memcpy(&thread, &value, sizeof(thread));
	if (thread)
	{
		pthread_join(thread, nullptr);
	}
}

bool thread_base::join(bool) const
{
	while ((m_sync & static_cast<u32>(thread_state::mask)) <= static_cast<u32>(thread_state::aborting))
	{
		m_sync.wait(m_sync & ~2u);
	}
	return (m_sync & static_cast<u32>(thread_state::mask)) == static_cast<u32>(thread_state::finished);
}

void thread_base::notify()
{
	m_sync |= 4;
	m_sync.notify_all();
}

u64 thread_base::get_native_id() const
{
	u64 id = 0;
	const Handle handle = m_handle;
	return handle && R_SUCCEEDED(svcGetThreadId(&id, handle)) ? id : 0;
}

u64 thread_base::get_cycles()
{
	u64 ticks = 0;
	const Handle handle = m_handle;
	if (!handle || R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)))
	{
		return 0;
	}
	if (const u64 previous = m_cycles.exchange(ticks))
	{
		return ticks - previous;
	}
	return 0;
}

void thread_base::push(shared_ptr<thread_future> task)
{
	const auto next = &task->next;
	m_taskq.push_head(*next, std::move(task));
	m_taskq.notify_one();
}

void thread_base::exec()
{
	while (shared_ptr<thread_future> head = m_taskq.exchange(null_ptr))
	{
		thread_future* tail = head.get();
		for (thread_future* previous = nullptr;;)
		{
			if (auto* const next = tail->next.get())
			{
				previous = std::exchange(tail, next);
				tail->prev = previous;
			}
			else
			{
				break;
			}
		}
		for (auto* task = tail; task; task = task->prev)
		{
			if (auto callback = task->exec.load())
			{
				callback((m_sync & static_cast<u32>(thread_state::mask)) == 0 ? this : nullptr, task);
				task->done.release(1);
				task->done.notify_all();
			}
			if (task->next)
			{
				task->next.reset();
			}
		}
	}
}

void thread_ctrl::set_name(std::string name)
{
	if (!g_tls_this_thread)
	{
		emergency_exit("Cannot name an unmanaged thread");
	}
	g_tls_this_thread->m_tname.store(make_single<std::string>(name));
	g_tls_this_thread->set_name(std::move(name));
}

[[noreturn]] void thread_ctrl::emergency_exit(std::string_view reason)
{
	std::fprintf(stderr, "RPCS3 fatal error: %.*s\n", static_cast<int>(reason.size()), reason.data());
	if (g_tls_this_thread)
	{
		if (g_tls_error_callback)
		{
			g_tls_error_callback();
		}
		g_tls_this_thread->finalize(thread_state::errored);
		thread_base::finalize(umax);
		pthread_exit(nullptr);
	}
	std::abort();
}

[[noreturn]] void thread_ctrl::silent_exit() noexcept
{
	if (g_tls_this_thread)
	{
		if (g_tls_error_callback)
		{
			g_tls_error_callback();
		}
		g_tls_this_thread->finalize(thread_state::errored);
		thread_base::finalize(umax);
	}
	pthread_exit(nullptr);
}

void thread_ctrl::detect_cpu_layout()
{
	g_native_core_layout.compare_and_swap_test(native_core_arrangement::undefined, native_core_arrangement::generic);
}

u64 thread_ctrl::get_affinity_mask(thread_class)
{
	return process_affinity_mask;
}

void thread_ctrl::set_native_priority(int priority)
{
	static thread_local s32 base_priority = []
	{
		s32 value = 0x2c;
		svcGetThreadPriority(&value, CUR_THREAD_HANDLE);
		return value;
	}();
	const s32 target = std::clamp<s32>(base_priority - priority, 0, 0x3f);
	svcSetThreadPriority(CUR_THREAD_HANDLE, target);
}

u64 thread_ctrl::get_process_affinity_mask()
{
	u64 mask = 0;
	return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? mask : 0xf;
}

const u64 thread_ctrl::process_affinity_mask = thread_ctrl::get_process_affinity_mask();

void thread_ctrl::set_thread_affinity_mask(u64 mask)
{
	const u32 target = static_cast<u32>(mask ? mask : process_affinity_mask);
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, target);
}

u64 thread_ctrl::get_thread_affinity_mask()
{
	s32 preferred_core = -1;
	u64 mask = 0;
	return R_SUCCEEDED(svcGetThreadCoreMask(&preferred_core, &mask, CUR_THREAD_HANDLE)) ? mask : 0;
}

std::pair<void*, usz> thread_ctrl::get_thread_stack()
{
	if (Thread* const thread = threadGetSelf())
	{
		return {thread->stack_mem, thread->stack_sz};
	}
	return {};
}

u64 thread_ctrl::get_tid()
{
	static thread_local u64 id = []
	{
		u64 value = 0;
		svcGetThreadId(&value, CUR_THREAD_HANDLE);
		return value;
	}();
	return id;
}

bool thread_ctrl::is_main()
{
	return get_tid() == utils::main_tid;
}

usz map_workload(std::string_view thread_name, usz thread_count, usz count, std::function<void(usz)>&& function)
{
	if (!function || !count)
	{
		return 0;
	}
	thread_count = std::max<usz>(1, std::min(thread_count, count));
	atomic_t<usz> index{0};
	named_thread_group workers(thread_name, static_cast<u32>(thread_count), [&]
		{
			for (;;)
			{
				const usz current = index.fetch_add(1);
				if (current >= count)
				{
					break;
				}
				function(current);
			}
		});
	workers.join();
	return thread_count;
}

usz map_workload(std::string_view thread_name, usz thread_count, std::function<void()>&& function)
{
	return map_workload(thread_name, thread_count, thread_count, [&](usz)
		{
			function();
		});
}
