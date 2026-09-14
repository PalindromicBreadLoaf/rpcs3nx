#include "guest_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace rpcs3::switch_runtime
{
	namespace
	{
		constexpr std::size_t page_size = 0x1000;
		constexpr std::size_t initial_mapping_capacity = 16'384;

		constexpr Result bad_input()
		{
			return MAKERESULT(Module_Libnx, LibnxError_BadInput);
		}

		constexpr Result not_initialized()
		{
			return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
		}

		bool is_page_aligned(std::size_t value)
		{
			return (value & (page_size - 1)) == 0;
		}

		bool range_fits(std::size_t offset, std::size_t size, std::size_t capacity)
		{
			return size && is_page_aligned(offset) && is_page_aligned(size) && offset <= capacity &&
			       size <= capacity - offset;
		}

		VirtmemReservation* reserve_address_space(void*& address, std::size_t size, bool code)
		{
			virtmemLock();
			address = code ? virtmemFindCodeMemory(size, page_size) : virtmemFindAslr(size, 0);
			VirtmemReservation* reservation = address ? virtmemAddReservation(address, size) : nullptr;
			virtmemUnlock();
			return reservation;
		}

		void release_address_space(VirtmemReservation*& reservation)
		{
			if (!reservation)
			{
				return;
			}

			virtmemLock();
			virtmemRemoveReservation(reservation);
			virtmemUnlock();
			reservation = nullptr;
		}
	} // namespace

	guest_memory::~guest_memory()
	{
		finalize();
	}

	Result guest_memory::initialize(std::size_t backing_size, std::size_t address_space_size)
	{
		std::lock_guard lock(m_mutex);
		if (m_backing)
		{
			return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
		}
		if (!is_page_aligned(backing_size) || !is_page_aligned(address_space_size) ||
			!backing_size || !address_space_size)
		{
			return bad_input();
		}
		if (envGetOwnProcessHandle() == INVALID_HANDLE)
		{
			return MAKERESULT(Module_Libnx, LibnxError_JitUnavailable);
		}

		constexpr unsigned required_syscalls[] = {0x73, 0x74, 0x75, 0x77, 0x78};
		if (std::any_of(std::begin(required_syscalls), std::end(required_syscalls),
				[](unsigned syscall)
				{
					return !envIsSyscallHinted(syscall);
				}))
		{
			return MAKERESULT(Module_Libnx, LibnxError_JitUnavailable);
		}

		m_allocation = std::aligned_alloc(page_size, backing_size);
		if (!m_allocation)
		{
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}

		void* canonical = nullptr;
		m_backing_reservation = reserve_address_space(canonical, backing_size, true);
		if (!m_backing_reservation)
		{
			std::free(m_allocation);
			m_allocation = nullptr;
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}

		const Handle self = envGetOwnProcessHandle();
		Result rc = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
			reinterpret_cast<u64>(m_allocation), backing_size);
		if (R_FAILED(rc))
		{
			release_address_space(m_backing_reservation);
			std::free(m_allocation);
			m_allocation = nullptr;
			return rc;
		}

		rc = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(canonical), backing_size, Perm_Rw);
		if (R_FAILED(rc))
		{
			svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
				reinterpret_cast<u64>(m_allocation), backing_size);
			release_address_space(m_backing_reservation);
			std::free(m_allocation);
			m_allocation = nullptr;
			return rc;
		}

		void* address_space = nullptr;
		m_address_space_reservation = reserve_address_space(address_space, address_space_size, false);
		if (!m_address_space_reservation)
		{
			svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
				reinterpret_cast<u64>(m_allocation), backing_size);
			release_address_space(m_backing_reservation);
			std::free(m_allocation);
			m_allocation = nullptr;
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}

		try
		{
			m_mappings.reserve(initial_mapping_capacity);
		}
		catch (const std::bad_alloc&)
		{
			release_address_space(m_address_space_reservation);
			svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
				reinterpret_cast<u64>(m_allocation), backing_size);
			release_address_space(m_backing_reservation);
			std::free(m_allocation);
			m_allocation = nullptr;
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}

		m_backing = static_cast<u8*>(canonical);
		m_address_space = static_cast<u8*>(address_space);
		m_backing_size = backing_size;
		m_address_space_size = address_space_size;
		std::memset(m_backing, 0, m_backing_size);
		return 0;
	}

	Result guest_memory::finalize()
	{
		std::lock_guard lock(m_mutex);
		if (!m_backing)
		{
			return 0;
		}

		const Handle self = envGetOwnProcessHandle();
		while (!m_mappings.empty())
		{
			const mapping& current = m_mappings.back();
			const Result rc = svcUnmapProcessMemory(current.destination, self,
				reinterpret_cast<u64>(current.source), current.size);
			if (R_FAILED(rc))
			{
				return rc;
			}
			m_mappings.pop_back();
		}
		release_address_space(m_address_space_reservation);

		const Result rc = svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(m_backing),
			reinterpret_cast<u64>(m_allocation), m_backing_size);
		if (R_FAILED(rc))
		{
			return rc;
		}

		release_address_space(m_backing_reservation);
		std::free(m_allocation);
		m_allocation = nullptr;
		m_backing = nullptr;
		m_address_space = nullptr;
		m_backing_size = 0;
		m_address_space_size = 0;
		return 0;
	}

	Result guest_memory::map(std::size_t backing_offset, std::size_t address_space_offset, std::size_t size)
	{
		std::lock_guard lock(m_mutex);
		if (!m_backing)
		{
			return not_initialized();
		}
		if (!range_fits(backing_offset, size, m_backing_size) ||
			!range_fits(address_space_offset, size, m_address_space_size))
		{
			return bad_input();
		}

		u8* const destination = m_address_space + address_space_offset;
		u8* const source = m_backing + backing_offset;
		try
		{
			m_mappings.push_back({destination, source, size});
		}
		catch (const std::bad_alloc&)
		{
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}

		const Result rc = svcMapProcessMemory(destination, envGetOwnProcessHandle(),
			reinterpret_cast<u64>(source), size);
		if (R_FAILED(rc))
		{
			m_mappings.pop_back();
			return rc;
		}
		return 0;
	}

	Result guest_memory::unmap(std::size_t address_space_offset, std::size_t size)
	{
		std::lock_guard lock(m_mutex);
		if (!m_backing)
		{
			return not_initialized();
		}
		if (!range_fits(address_space_offset, size, m_address_space_size))
		{
			return bad_input();
		}

		u8* const destination = m_address_space + address_space_offset;
		const auto found = std::find_if(m_mappings.begin(), m_mappings.end(),
			[destination, size](const mapping& candidate)
			{
				return candidate.destination == destination && candidate.size == size;
			});
		if (found == m_mappings.end())
		{
			return MAKERESULT(Module_Libnx, LibnxError_NotFound);
		}

		const Result rc = svcUnmapProcessMemory(found->destination, envGetOwnProcessHandle(),
			reinterpret_cast<u64>(found->source), found->size);
		if (R_SUCCEEDED(rc))
		{
			m_mappings.erase(found);
		}
		return rc;
	}

	u8* guest_memory::backing() const
	{
		std::lock_guard lock(m_mutex);
		return m_backing;
	}

	u8* guest_memory::address_space() const
	{
		std::lock_guard lock(m_mutex);
		return m_address_space;
	}

	std::size_t guest_memory::backing_size() const
	{
		std::lock_guard lock(m_mutex);
		return m_backing_size;
	}

	std::size_t guest_memory::address_space_size() const
	{
		std::lock_guard lock(m_mutex);
		return m_address_space_size;
	}

	std::size_t guest_memory::mapping_count() const
	{
		std::lock_guard lock(m_mutex);
		return m_mappings.size();
	}
} // namespace rpcs3::switch_runtime
