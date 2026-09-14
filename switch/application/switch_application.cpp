#include "switch_application.h"

#include "build_info.h"
#include "Loader/ELF.h"
#include "switch/runtime/exception_handler.h"
#include "util/atomic.hpp"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace rpcs3::switch_app
{
	namespace
	{
		constexpr const char* data_directory = "sdmc:/switch/rpcs3";
		constexpr const char* config_directory = "sdmc:/switch/rpcs3/config";
		constexpr const char* cache_directory = "sdmc:/switch/rpcs3/cache";
		constexpr const char* log_path = "sdmc:/switch/rpcs3/rpcs3.log";
		constexpr const char* config_path = "sdmc:/switch/rpcs3/config/switch-shell.yml";
		constexpr const char* boot_path = "sdmc:/switch/rpcs3/boot.elf";

		bool ensure_directory(const char* path)
		{
			return mkdir(path, 0777) == 0 || errno == EEXIST;
		}

		const char* elf_error_name(elf_error error)
		{
			switch (error)
			{
			case elf_error::ok: return "OK";
			case elf_error::stream: return "file not found";
			case elf_error::stream_header: return "failed to read ELF header";
			case elf_error::stream_phdrs: return "failed to read program headers";
			case elf_error::stream_shdrs: return "failed to read section headers";
			case elf_error::stream_data: return "failed to read ELF data";
			case elf_error::header_magic: return "not an ELF";
			case elf_error::header_version: return "unsupported ELF format";
			case elf_error::header_class: return "invalid ELF class";
			case elf_error::header_machine: return "not a PowerPC ELF";
			case elf_error::header_endianness: return "invalid ELF byte order";
			case elf_error::header_type: return "not an executable ELF";
			case elf_error::header_os: return "invalid ELF OS ABI";
			}

			return "unknown ELF error";
		}
	} // namespace

	bool application::initialize_paths()
	{
		if (!ensure_directory("sdmc:/switch") || !ensure_directory(data_directory) ||
			!ensure_directory(config_directory) || !ensure_directory(cache_directory))
		{
			std::printf("Failed to create RPCS3 data directories: errno %d\n", errno);
			return false;
		}

		m_log_file = std::fopen(log_path, "a");
		if (!m_log_file)
		{
			std::printf("Failed to open %s: errno %d\n", log_path, errno);
			return false;
		}
		setvbuf(m_log_file, nullptr, _IOLBF, 0);

		if (access(config_path, F_OK) != 0)
		{
			if (FILE* config = std::fopen(config_path, "w"))
			{
				std::fputs("# Initial Switch shell settings; emulator config integration is pending.\n"
						   "boot_path: /switch/rpcs3/boot.elf\n"
						   "renderer: 'Null'\n"
						   "audio: 'Null'\n"
						   "ppu_decoder: 'Interpreter'\n"
						   "spu_decoder: 'Interpreter'\n"
						   "llvm: false\n",
					config);
				std::fflush(config);
				fsync(fileno(config));
				std::fclose(config);
			}
			else
			{
				log("Failed to create %s: errno %d (%s)\n", config_path, errno, std::strerror(errno));
			}
		}

		return true;
	}

	void application::log(const char* format, ...)
	{
		std::lock_guard lock(m_log_mutex);
		va_list arguments;
		va_start(arguments, format);
		va_list copy;
		va_copy(copy, arguments);
		std::vprintf(format, arguments);
		if (m_log_file)
		{
			std::vfprintf(m_log_file, format, copy);
		}
		va_end(copy);
		va_end(arguments);
	}

	void application::flush_log()
	{
		std::lock_guard lock(m_log_mutex);
		std::fflush(stdout);
		if (m_log_file)
		{
			std::fflush(m_log_file);
			fsync(fileno(m_log_file));
		}
	}

	void application::initialize_callback_probe()
	{
		std::thread worker([this]
			{
				m_callbacks.post([this]
					{
						u64 current_thread_id = 0;
						const Result rc = svcGetThreadId(&current_thread_id, CUR_THREAD_HANDLE);
						m_callback_probe_ran_on_main.store(R_SUCCEEDED(rc) && current_thread_id == m_main_thread_id,
							std::memory_order_relaxed);
					},
					&m_callback_probe_complete);
			});
		worker.join();
	}

	void application::initialize_runtime_probe()
	{
		constexpr u32 thread_count = 4;
		constexpr u64 wait_timeout = 2'000'000'000;
		atomic_t<u32> ready{0};
		atomic_t<u32> gate{0};
		atomic_t<u32> completed{0};
		std::array<std::thread, thread_count> workers;

		for (auto& worker : workers)
		{
			worker = std::thread([&]
				{
					ready.fetch_add(1);
					gate.wait(0, static_cast<atomic_wait_timeout>(wait_timeout));
					if (gate == 1)
					{
						completed.fetch_add(1);
					}
				});
		}

		while (ready != thread_count)
		{
			svcSleepThread(100'000);
		}
		svcSleepThread(10'000'000);

		gate.release(1);
		gate.notify_all();
		for (auto& worker : workers)
		{
			worker.join();
		}

		atomic_t<u32> list_ready{0};
		atomic_t<u32> first{0};
		atomic_t<u32> second{0};
		atomic_t<u32> list_completed{0};
		std::thread list_worker([&]
			{
				atomic_wait::list<2> list{};
				list.set<0>(first, 0);
				list.set<1>(second, 0);
				list_ready.release(1);
				list.wait(static_cast<atomic_wait_timeout>(wait_timeout));
				list_completed.release(second == 1);
			});
		while (list_ready == 0)
		{
			svcSleepThread(100'000);
		}
		svcSleepThread(10'000'000);
		second.release(1);
		second.notify_one();
		list_worker.join();

		atomic_t<u32> timeout_gate{0};
		const u64 before = armGetSystemTick();
		timeout_gate.wait(0, static_cast<atomic_wait_timeout>(5'000'000));
		const u64 elapsed = armGetSystemTick() - before;
		m_runtime_timeout_us = elapsed * 1'000'000 / armGetSystemTickFreq();
		m_runtime_probe_passed = completed == thread_count && list_completed == 1 && timeout_gate == 0 &&
		                         m_runtime_timeout_us >= 4'000 && m_runtime_timeout_us < 250'000;

		log("Atomic wait runtime: %s (%u/%u notified, list %s, timeout %lu us)\n",
			m_runtime_probe_passed ? "PASS" : "FAIL", static_cast<u32>(completed), thread_count,
			list_completed == 1 ? "notified" : "FAIL", m_runtime_timeout_us);
	}

	void application::initialize_jit_probe()
	{
		constexpr std::size_t arena_size = static_cast<std::size_t>(RPCS3_SWITCH_JIT_SIZE_MB) * 1024 * 1024;
		m_jit_result = m_jit_memory.initialize(arena_size);
		if (R_FAILED(m_jit_result))
		{
			log("JIT arena: FAIL (result 0x%08x)\n", m_jit_result);
			return;
		}

		const auto code = m_jit_memory.allocate(2 * sizeof(u32), 16);
		if (!code)
		{
			log("JIT arena: FAIL (allocation)\n");
			return;
		}

		constexpr u32 ret = 0xd65f03c0;
		auto write_return_value = [&](u16 value)
		{
			const u32 move = 0x52800000 | (static_cast<u32>(value) << 5);
			std::memcpy(code.rw, &move, sizeof(move));
			std::memcpy(code.rw + sizeof(move), &ret, sizeof(ret));
			m_jit_memory.publish(code);
		};

		using probe_function = u32 (*)();
		const auto execute = reinterpret_cast<probe_function>(code.rx);
		write_return_value(0x1234);
		const u32 initial = execute();

		atomic_t<u32> gate{0};
		atomic_t<u32> worker_result{0};
		std::thread worker([&]
			{
				gate.wait(0, static_cast<atomic_wait_timeout>(2'000'000'000));
				worker_result.release(execute());
			});
		write_return_value(0x4321);
		gate.release(1);
		gate.notify_one();
		worker.join();

		const bool released = m_jit_memory.release(code);
		m_jit_probe_passed = initial == 0x1234 && worker_result == 0x4321 && released && m_jit_memory.used() == 0;
		log("JIT arena: %s (%u MiB, RW/RX aliases, initial 0x%x, patched 0x%x)\n",
			m_jit_probe_passed ? "PASS" : "FAIL", RPCS3_SWITCH_JIT_SIZE_MB, initial,
			static_cast<u32>(worker_result));
	}

	void application::initialize_guest_memory_probe()
	{
		constexpr std::size_t page_size = 0x1000;
		constexpr std::size_t backing_size = 16 * page_size;
		constexpr std::size_t address_space_size = 8ull * 1024 * 1024 * 1024;
		m_guest_memory_result = m_guest_memory.initialize(backing_size, address_space_size);
		if (R_FAILED(m_guest_memory_result))
		{
			log("Guest memory: FAIL (initialize result 0x%08x)\n", m_guest_memory_result);
			return;
		}

		auto finish = [&](Result rc, const char* operation)
		{
			m_guest_memory_result = rc;
			if (R_FAILED(rc))
			{
				log("Guest memory: FAIL (%s result 0x%08x)\n", operation, rc);
			}
		};

		finish(m_guest_memory.map(0, 0, page_size), "map first alias");
		if (R_FAILED(m_guest_memory_result))
			return;
		finish(m_guest_memory.map(0, 2 * page_size, page_size), "map second alias");
		if (R_FAILED(m_guest_memory_result))
			return;

		constexpr u32 first_pattern = 0x12345678;
		constexpr u32 second_pattern = 0x89abcdef;
		auto* const first = reinterpret_cast<volatile u32*>(m_guest_memory.address_space());
		auto* const second = reinterpret_cast<volatile u32*>(m_guest_memory.address_space() + 2 * page_size);
		*first = first_pattern;
		const bool alias_coherent = *second == first_pattern &&
		                            *reinterpret_cast<volatile u32*>(m_guest_memory.backing()) == first_pattern;

		MemoryInfo hole_info{};
		u32 hole_page_info = 0;
		const Result hole_result = svcQueryMemory(&hole_info, &hole_page_info,
			reinterpret_cast<u64>(m_guest_memory.address_space() + page_size));
		const bool sparse_hole = R_SUCCEEDED(hole_result) && hole_info.type == MemType_Unmapped;
		m_expected_fault_address = m_guest_memory.address_space() + page_size;
		const bool handler_installed = switch_runtime::install_exception_handler(guest_memory_fault_handler, this);
		u64 recovered_value = 0;
		if (handler_installed)
		{
			register u64 fault_address asm("x0") = reinterpret_cast<u64>(m_expected_fault_address);
			asm volatile("ldr w0, [x0]\nstr x0, %0" : "=m"(recovered_value), "+r"(fault_address) : : "memory");
			switch_runtime::uninstall_exception_handler();
		}
		m_expected_fault_address = nullptr;
		const bool fault_recovered = handler_installed && recovered_value == 0xfeedc0de && m_guest_fault_count == 1;

		finish(m_guest_memory.unmap(0, page_size), "unmap first alias");
		if (R_FAILED(m_guest_memory_result))
			return;
		*second = second_pattern;
		const bool independent_unmap = *reinterpret_cast<volatile u32*>(m_guest_memory.backing()) == second_pattern;

		finish(m_guest_memory.unmap(2 * page_size, page_size), "unmap second alias");
		if (R_FAILED(m_guest_memory_result))
			return;
		finish(m_guest_memory.map(page_size, 2 * page_size, page_size), "remap different offset");
		if (R_FAILED(m_guest_memory_result))
			return;
		constexpr u32 remap_pattern = 0x0badf00d;
		*reinterpret_cast<volatile u32*>(m_guest_memory.backing() + page_size) = remap_pattern;
		const bool remap_coherent = *second == remap_pattern;
		finish(m_guest_memory.unmap(2 * page_size, page_size), "unmap remapped alias");
		if (R_FAILED(m_guest_memory_result))
			return;

		const bool mappings_released = m_guest_memory.mapping_count() == 0;
		const Result finalize_result = m_guest_memory.finalize();
		m_guest_memory_result = finalize_result;
		m_guest_memory_probe_passed = alias_coherent && sparse_hole && fault_recovered && independent_unmap && remap_coherent &&
		                              mappings_released && R_SUCCEEDED(finalize_result);
		log("Guest memory: %s (8 GiB sparse window, aliases %s, hole %s, fault %s, unmap %s, remap %s, cleanup %s)\n",
			m_guest_memory_probe_passed ? "PASS" : "FAIL", alias_coherent ? "coherent" : "FAIL",
			sparse_hole ? "unmapped" : "FAIL", fault_recovered ? "recovered" : "FAIL",
			independent_unmap ? "independent" : "FAIL",
			remap_coherent ? "coherent" : "FAIL",
			mappings_released && R_SUCCEEDED(finalize_result) ? "complete" : "FAIL");
	}

	bool application::guest_memory_fault_handler(ThreadExceptionDump& context, void* user) noexcept
	{
		auto& app = *static_cast<application*>(user);
		if (context.far.x != reinterpret_cast<u64>(app.m_expected_fault_address))
		{
			return false;
		}

		context.cpu_gprs[0].x = 0xfeedc0de;
		context.pc.x += sizeof(u32);
		app.m_guest_fault_count++;
		return true;
	}

	void application::on_applet_hook(AppletHookType hook)
	{
		switch (hook)
		{
		case AppletHookType_OnFocusState:
			log("Lifecycle: focus state %d\n", static_cast<int>(appletGetFocusState()));
			break;
		case AppletHookType_OnOperationMode:
			log("Lifecycle: operation mode %d\n", static_cast<int>(appletGetOperationMode()));
			break;
		case AppletHookType_OnPerformanceMode:
			log("Lifecycle: performance mode %d\n", static_cast<int>(appletGetPerformanceMode()));
			break;
		case AppletHookType_OnExitRequest:
			log("Lifecycle: exit requested\n");
			flush_log();
			break;
		case AppletHookType_OnResume:
			log("Lifecycle: resumed\n");
			break;
		default:
			break;
		}
	}

	void application::applet_hook(AppletHookType hook, void* parameter)
	{
		static_cast<application*>(parameter)->on_applet_hook(hook);
	}

	int application::run()
	{
		consoleInit(nullptr);
		if (!initialize_paths())
		{
			consoleUpdate(nullptr);
			svcSleepThread(2'000'000'000);
			consoleExit(nullptr);
			return 1;
		}

		Result rc = svcGetThreadId(&m_main_thread_id, CUR_THREAD_HANDLE);
		log("\n=== RPCS3 Switch shell %s ===\n", RPCS3_SWITCH_SHELL_VERSION);
		log("RPCS3 revision: %s\n", RPCS3_SWITCH_REVISION);
		log("Platform: NintendoSwitch\n");
		log("Compiler: %s\n", __VERSION__);
		log("Heap: %u MiB\n", RPCS3_SWITCH_HEAP_SIZE_MB);
		log("Main thread: 0x%lx (result 0x%08x)\n", m_main_thread_id, rc);
		log("Data: %s\nConfig: %s\nCache: %s\n", data_directory, config_directory, cache_directory);

		struct stat boot_stat{};
		const bool boot_exists = stat(boot_path, &boot_stat) == 0 && S_ISREG(boot_stat.st_mode);
		log("Fixed launch path: %s [%s]\n", boot_path, boot_exists ? "ready" : "missing");

		elf_error boot_error = elf_error::stream;
		u64 boot_entry = 0;
		u32 boot_segments = 0;
		if (boot_exists)
		{
			const fs::file boot_file(boot_path);
			const ppu_exec_object boot_elf(boot_file, 0, +elf_opt::no_data);
			boot_error = boot_elf.get_error();
			if (boot_error == elf_error::ok)
			{
				boot_entry = boot_elf.header.e_entry;
				boot_segments = static_cast<u32>(boot_elf.progs.size());
			}
		}
		log("RPCS3 ELF loader: %s", elf_error_name(boot_error));
		if (boot_error == elf_error::ok)
		{
			log(" (entry 0x%lx, %u segments)", boot_entry, boot_segments);
		}
		log("\n");

		appletHook(&m_hook_cookie, applet_hook, this);
		padConfigureInput(1, HidNpadStyleSet_NpadStandard);
		PadState pad{};
		padInitializeDefault(&pad);
		initialize_callback_probe();
		initialize_runtime_probe();
		initialize_jit_probe();
		initialize_guest_memory_probe();

		bool reported_callback_probe = false;
		while (appletMainLoop())
		{
			m_callbacks.drain();
			if (!reported_callback_probe && m_callback_probe_complete.load(std::memory_order_acquire))
			{
				reported_callback_probe = true;
				log("Main thread callback queue: %s\n",
					m_callback_probe_ran_on_main.load(std::memory_order_relaxed) ? "PASS" : "FAIL");
				flush_log();
			}

			padUpdate(&pad);
			if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			{
				break;
			}

			std::printf("\x1b[2JRPCS3 Switch shell %s\n\n", RPCS3_SWITCH_SHELL_VERSION);
			std::printf("Boot ELF: %s\n", boot_exists ? elf_error_name(boot_error) : "missing (/switch/rpcs3/boot.elf)");
			std::printf("Callback queue: %s\n", reported_callback_probe ?
													(m_callback_probe_ran_on_main.load() ? "pass" : "FAIL") :
													"waiting");
			std::printf("Atomic wait runtime: %s (%lu us timeout)\n",
				m_runtime_probe_passed ? "pass" : "FAIL", m_runtime_timeout_us);
			std::printf("JIT arena: %s (%u MiB)\n", m_jit_probe_passed ? "pass" : "FAIL", RPCS3_SWITCH_JIT_SIZE_MB);
			std::printf("Guest memory: %s (8 GiB sparse window)\n", m_guest_memory_probe_passed ? "pass" : "FAIL");
			std::printf("\nPress + to exit.\n");
			consoleUpdate(nullptr);
			svcSleepThread(16'000'000);
		}

		m_callbacks.drain();
		appletUnhook(&m_hook_cookie);
		log("Clean exit\n");
		flush_log();
		std::fclose(m_log_file);
		m_log_file = nullptr;
		consoleExit(nullptr);
		return 0;
	}
} // namespace rpcs3::switch_app
