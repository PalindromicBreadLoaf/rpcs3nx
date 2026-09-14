#pragma once

#include "callback_queue.h"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace rpcs3::switch_app
{
	class application
	{
	public:
		int run();

	private:
		bool initialize_paths();
		void initialize_callback_probe();
		void initialize_runtime_probe();
		void log(const char* format, ...);
		void flush_log();
		void on_applet_hook(AppletHookType hook);
		static void applet_hook(AppletHookType hook, void* parameter);

		callback_queue m_callbacks;
		AppletHookCookie m_hook_cookie{};
		std::atomic_bool m_callback_probe_complete{false};
		std::atomic_bool m_callback_probe_ran_on_main{false};
		bool m_runtime_probe_passed = false;
		u64 m_runtime_timeout_us = 0;
		std::mutex m_log_mutex;
		FILE* m_log_file = nullptr;
		u64 m_main_thread_id = 0;
	};
} // namespace rpcs3::switch_app
