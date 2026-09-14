#include "util/sysinfo.hpp"

#include <switch.h>

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace
{
	u64 get_process_info(InfoType type)
	{
		u64 value = 0;
		return R_SUCCEEDED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)) ? value : 0;
	}

	std::string get_horizon_version(bool simple)
	{
		const u32 version = hosversionGet();
		char buffer[96]{};
		std::snprintf(buffer, sizeof(buffer), simple ? "Horizon %u.%u.%u%s" : "Operating system: Horizon, Version: %u.%u.%u%s",
			HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
			hosversionIsAtmosphere() ? " Atmosphere" : "");
		return buffer;
	}
} // namespace

namespace utils
{
	u64 s_tsc_freq = armGetSystemTickFreq();

	bool has_ssse3()
	{
		return false;
	}
	bool has_sse41()
	{
		return false;
	}
	bool has_avx()
	{
		return false;
	}
	bool has_avx2()
	{
		return false;
	}
	bool has_rtm()
	{
		return false;
	}
	bool has_tsx_force_abort()
	{
		return false;
	}
	bool has_rtm_always_abort()
	{
		return false;
	}
	bool has_mpx()
	{
		return false;
	}
	bool has_avx512()
	{
		return false;
	}
	bool has_avx512_icl()
	{
		return false;
	}
	bool has_avx512_vnni()
	{
		return false;
	}
	bool has_avx10()
	{
		return false;
	}
	u32 avx10_isa_version()
	{
		return 0;
	}
	bool has_xop()
	{
		return false;
	}
	bool has_clwb()
	{
		return false;
	}
	bool has_invariant_tsc()
	{
		return true;
	}
	bool has_fma3()
	{
		return false;
	}
	bool has_fma4()
	{
		return false;
	}
	bool has_fast_vperm2b()
	{
		return false;
	}
	bool has_erms()
	{
		return false;
	}
	bool has_fsrm()
	{
		return false;
	}
	bool has_waitx()
	{
		return false;
	}
	bool has_waitpkg()
	{
		return false;
	}
	bool has_appropriate_um_wait()
	{
		return false;
	}
	bool has_um_wait()
	{
		return false;
	}
	u32 get_rep_movsb_threshold()
	{
		return umax;
	}

	bool has_neon()
	{
		return true;
	}
	bool has_sha3()
	{
		return false;
	}
	bool has_dotprod()
	{
		return false;
	}
	bool has_i8mm()
	{
		return false;
	}
	bool has_sve()
	{
		return false;
	}
	bool has_sve2()
	{
		return false;
	}
	int sve_length()
	{
		return 0;
	}

	std::string get_cpu_brand()
	{
		return "ARM Cortex A57";
	}

	std::string_view get_architecture()
	{
		return "arm64";
	}

	std::string get_system_info()
	{
		char buffer[160]{};
		std::snprintf(buffer, sizeof(buffer), "%s | %u Threads | %.2f GiB RAM | TSC: %.03fGHz | Neon",
			get_cpu_brand().c_str(), get_thread_count(), get_total_memory() / (1024.0 * 1024 * 1024),
			get_tsc_freq() / 1'000'000'000.0);
		return buffer;
	}

	std::pair<u64, u64> get_memory_usage()
	{
		return {get_process_info(InfoType_TotalMemorySize), get_process_info(InfoType_UsedMemorySize)};
	}

	OS_version get_OS_version()
	{
		const u32 version = hosversionGet();
		return {
			.type = "horizon",
			.arch = "arm64",
			.version_major = static_cast<int>(HOSVER_MAJOR(version)),
			.version_minor = static_cast<int>(HOSVER_MINOR(version)),
			.version_patch = static_cast<int>(HOSVER_MICRO(version)),
		};
	}

	std::string get_OS_version_string(bool simple)
	{
		return get_horizon_version(simple);
	}

	int get_maxfiles()
	{
		return FOPEN_MAX;
	}

	bool get_low_power_mode()
	{
		return false;
	}

	u64 get_total_memory()
	{
		return get_process_info(InfoType_TotalMemorySize);
	}

	u32 get_thread_count()
	{
		const u32 count = std::popcount(get_process_info(InfoType_CoreMask));
		return count ? count : 1;
	}

	u32 get_cpu_family()
	{
		return 0;
	}
	u32 get_cpu_model()
	{
		return 0;
	}

	std::pair<bool, usz> string_to_number(std::string_view str)
	{
		char* end = nullptr;
		const usz number = std::strtoul(str.data(), &end, 10);
		return end == str.data() + str.size() ? std::pair<bool, usz>{true, number} : std::pair<bool, usz>{false, 0};
	}
} // namespace utils
