#pragma once

#include <switch.h>

namespace rpcs3::switch_runtime
{
	using exception_handler = bool (*)(ThreadExceptionDump& context, void* user) noexcept;

	bool install_exception_handler(exception_handler handler, void* user) noexcept;
	void uninstall_exception_handler() noexcept;
	bool exception_handler_installed() noexcept;
} // namespace rpcs3::switch_runtime
