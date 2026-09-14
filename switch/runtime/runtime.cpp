#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>

namespace utils
{
	u64 _get_main_tid()
	{
		u64 thread_id = 0;
		svcGetThreadId(&thread_id, CUR_THREAD_HANDLE);
		return thread_id;
	}
} // namespace utils

[[noreturn]] void thread_ctrl::emergency_exit(std::string_view reason)
{
	std::fprintf(stderr, "RPCS3 fatal error: %.*s\n", static_cast<int>(reason.size()), reason.data());
	std::abort();
}
