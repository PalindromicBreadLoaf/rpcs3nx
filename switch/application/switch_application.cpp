#include "switch_application.h"

#include "build_info.h"
#include "Loader/ELF.h"
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
