#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"

#include <switch.h>

namespace utils
{
	u64 _get_main_tid()
	{
		u64 thread_id = 0;
		svcGetThreadId(&thread_id, CUR_THREAD_HANDLE);
		return thread_id;
	}
} // namespace utils
