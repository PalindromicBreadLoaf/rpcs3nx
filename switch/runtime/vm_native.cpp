#include "util/vm.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace utils
{
	namespace
	{
		constexpr usz page_size = 0x1000;
		constexpr usz allocation_alignment = 0x10000;

		bool is_aligned(const void* pointer, usz size)
		{
			return !(reinterpret_cast<uptr>(pointer) & (page_size - 1)) && !(size & (page_size - 1));
		}

		bool is_unmapped(void* address, usz size)
		{
			u64 current = reinterpret_cast<u64>(address);
			const u64 end = current + size;
			if (end < current)
			{
				return false;
			}

			while (current < end)
			{
				MemoryInfo info{};
				u32 page_info = 0;
				if (R_FAILED(svcQueryMemory(&info, &page_info, current)) ||
					(info.type & MemState_Type) != MemType_Unmapped || info.addr > current ||
					info.size <= current - info.addr)
				{
					return false;
				}
				current = std::min(end, info.addr + info.size);
			}
			return true;
		}

		VirtmemReservation* reserve(void*& address, usz size)
		{
			virtmemLock();
			if (!address)
			{
				address = virtmemFindAslr(size, 0);
			}
			VirtmemReservation* const result = address ? virtmemAddReservation(address, size) : nullptr;
			virtmemUnlock();
			return result;
		}

		void release(VirtmemReservation* reservation)
		{
			if (!reservation)
			{
				return;
			}

			virtmemLock();
			virtmemRemoveReservation(reservation);
			virtmemUnlock();
		}

		struct backing
		{
			void* allocation{};
			u8* canonical{};
			VirtmemReservation* reservation{};
			usz size{};

			~backing()
			{
				if (!canonical)
				{
					return;
				}

				const Handle self = envGetOwnProcessHandle();
				if (R_SUCCEEDED(svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
						reinterpret_cast<u64>(allocation), size)))
				{
					release(reservation);
					std::free(allocation);
				}
			}
		};

		std::shared_ptr<backing> make_backing(usz size)
		{
			if (!size || size % page_size || envGetOwnProcessHandle() == INVALID_HANDLE)
			{
				return {};
			}

			auto result = std::make_shared<backing>();
			result->allocation = std::aligned_alloc(page_size, size);
			if (!result->allocation)
			{
				return {};
			}

			virtmemLock();
			result->canonical = static_cast<u8*>(virtmemFindCodeMemory(size, page_size));
			result->reservation = result->canonical ? virtmemAddReservation(result->canonical, size) : nullptr;
			virtmemUnlock();
			if (!result->reservation)
			{
				std::free(result->allocation);
				result->allocation = nullptr;
				return {};
			}

			const Handle self = envGetOwnProcessHandle();
			Result rc = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(result->canonical),
				reinterpret_cast<u64>(result->allocation), size);
			if (R_FAILED(rc))
			{
				release(result->reservation);
				std::free(result->allocation);
				result->allocation = nullptr;
				result->canonical = nullptr;
				result->reservation = nullptr;
				return {};
			}

			rc = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(result->canonical), size, Perm_Rw);
			if (R_FAILED(rc))
			{
				if (R_SUCCEEDED(svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(result->canonical),
						reinterpret_cast<u64>(result->allocation), size)))
				{
					release(result->reservation);
					std::free(result->allocation);
				}
				result->allocation = nullptr;
				result->canonical = nullptr;
				result->reservation = nullptr;
				return {};
			}

			result->size = size;
			std::memset(result->canonical, 0, size);
			return result;
		}

		struct mapping
		{
			u8* destination{};
			std::shared_ptr<backing> memory;
			usz source_offset{};
			usz size{};
			protection prot{protection::no};
			VirtmemReservation* reservation{};
		};

		bool map_alias(mapping& value)
		{
			if (value.prot != protection::rw)
			{
				return true;
			}

			return R_SUCCEEDED(svcMapProcessMemory(value.destination, envGetOwnProcessHandle(),
				reinterpret_cast<u64>(value.memory->canonical + value.source_offset), value.size));
		}

		bool unmap_alias(const mapping& value)
		{
			if (value.prot != protection::rw)
			{
				return true;
			}

			return R_SUCCEEDED(svcUnmapProcessMemory(value.destination, envGetOwnProcessHandle(),
				reinterpret_cast<u64>(value.memory->canonical + value.source_offset), value.size));
		}

		struct reservation
		{
			u8* address{};
			usz size{};
			VirtmemReservation* handle{};
			std::vector<mapping> mappings;
		};

		std::mutex s_reservation_mutex;
		std::vector<reservation> s_reservations;

		auto find_reservation(void* pointer, usz size)
		{
			const uptr begin = reinterpret_cast<uptr>(pointer);
			return std::find_if(s_reservations.begin(), s_reservations.end(), [begin, size](const reservation& value)
				{
					const uptr reserved = reinterpret_cast<uptr>(value.address);
					return begin >= reserved && begin - reserved <= value.size && size <= value.size - (begin - reserved);
				});
		}

		bool remap_protection(reservation& area, u8* begin, usz size, protection prot)
		{
			area.mappings.reserve(area.mappings.size() + 2);
			const u8* const end = begin + size;
			for (usz index = 0; index < area.mappings.size();)
			{
				mapping current = area.mappings[index];
				u8* const current_end = current.destination + current.size;
				if (current_end <= begin || current.destination >= end)
				{
					index++;
					continue;
				}

				if (!unmap_alias(current))
				{
					return false;
				}

				area.mappings.erase(area.mappings.begin() + index);
				const usz prefix = begin > current.destination ? begin - current.destination : 0;
				const usz suffix = current_end > end ? current_end - end : 0;
				std::vector<mapping> replacements;
				replacements.reserve(3);
				if (prefix)
				{
					replacements.push_back({current.destination, current.memory, current.source_offset, prefix, current.prot});
				}

				const usz changed_offset = std::max(begin, current.destination) - current.destination;
				const usz changed_size = current.size - prefix - suffix;
				replacements.push_back({current.destination + changed_offset, current.memory,
					current.source_offset + changed_offset, changed_size, prot});
				if (suffix)
				{
					replacements.push_back({current_end - suffix, current.memory,
						current.source_offset + current.size - suffix, suffix, current.prot});
				}

				for (mapping& replacement : replacements)
				{
					if (!map_alias(replacement))
					{
						return false;
					}
					area.mappings.insert(area.mappings.begin() + index++, std::move(replacement));
				}
			}
			return true;
		}
	} // namespace

	struct shm::horizon_state
	{
		std::mutex mutex;
		std::shared_ptr<backing> memory;
		std::vector<mapping> mappings;
	};

	long get_page_size()
	{
		return page_size;
	}

	void* memory_reserve(usz size, void* use_addr, bool, bool)
	{
		if (!size || size % page_size || (use_addr && !is_aligned(use_addr, size)))
		{
			return nullptr;
		}

		std::lock_guard lock(s_reservation_mutex);
		if (use_addr && !is_unmapped(use_addr, size))
		{
			return nullptr;
		}
		void* address = use_addr;
		VirtmemReservation* const handle = reserve(address, size);
		if (!handle)
		{
			return nullptr;
		}

		try
		{
			s_reservations.push_back({static_cast<u8*>(address), size, handle, {}});
		}
		catch (const std::bad_alloc&)
		{
			release(handle);
			return nullptr;
		}
		return address;
	}

	void memory_commit(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}
		ensure(is_aligned(pointer, size));
		ensure(prot == protection::rw);

		std::lock_guard lock(s_reservation_mutex);
		auto area = find_reservation(pointer, size);
		ensure(area != s_reservations.end());
		ensure(std::none_of(area->mappings.begin(), area->mappings.end(), [pointer, size](const mapping& value)
			{
				return value.destination < static_cast<u8*>(pointer) + size &&
			           value.destination + value.size > static_cast<u8*>(pointer);
			}));
		area->mappings.reserve(area->mappings.size() + 1);
		auto memory = make_backing(size);
		ensure(memory);
		mapping value{static_cast<u8*>(pointer), std::move(memory), 0, size, protection::rw};
		ensure(map_alias(value));
		area->mappings.push_back(std::move(value));
	}

	void memory_decommit(void* pointer, usz size, bool)
	{
		if (!size)
		{
			return;
		}
		ensure(is_aligned(pointer, size));
		std::lock_guard lock(s_reservation_mutex);
		auto area = find_reservation(pointer, size);
		ensure(area != s_reservations.end());
		ensure(remap_protection(*area, static_cast<u8*>(pointer), size, protection::no));
		const u8* const begin = static_cast<u8*>(pointer);
		std::erase_if(area->mappings, [begin, size](const mapping& value)
			{
				return value.prot == protection::no && value.destination >= begin &&
			           value.destination + value.size <= begin + size;
			});
	}

	void memory_reset(void* pointer, usz size, protection prot, bool can_be_jit)
	{
		memory_decommit(pointer, size, can_be_jit);
		memory_commit(pointer, size, prot);
	}

	void memory_release(void* pointer, usz size)
	{
		if (!size)
		{
			return;
		}
		std::lock_guard lock(s_reservation_mutex);
		auto area = std::find_if(s_reservations.begin(), s_reservations.end(), [pointer, size](const reservation& value)
			{
				return value.address == pointer && value.size == size;
			});
		ensure(area != s_reservations.end());
		for (const mapping& value : area->mappings)
		{
			ensure(unmap_alias(value));
		}
		release(area->handle);
		s_reservations.erase(area);
	}

	void memory_protect(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}
		ensure(is_aligned(pointer, size));
		ensure(prot == protection::rw || prot == protection::ro || prot == protection::no);
		std::lock_guard lock(s_reservation_mutex);
		auto area = find_reservation(pointer, size);
		ensure(area != s_reservations.end());
		if (std::none_of(area->mappings.begin(), area->mappings.end(), [pointer, size](const mapping& value)
				{
					return value.destination < static_cast<u8*>(pointer) + size &&
			               value.destination + value.size > static_cast<u8*>(pointer);
				}))
		{
			if (prot == protection::rw)
			{
				area->mappings.reserve(area->mappings.size() + 1);
				auto memory = make_backing(size);
				ensure(memory);
				mapping value{static_cast<u8*>(pointer), std::move(memory), 0, size, prot};
				ensure(map_alias(value));
				area->mappings.push_back(std::move(value));
			}
			return;
		}
		ensure(remap_protection(*area, static_cast<u8*>(pointer), size, prot));
	}

	bool memory_lock(void*, usz)
	{
		return true;
	}

	void* memory_map_fd(native_handle, usz, protection)
	{
		return nullptr;
	}

	shm::shm(u64 size, u32 flags)
		: m_horizon(new horizon_state), m_flags(flags), m_size((size + allocation_alignment - 1) & -allocation_alignment)
	{
		m_horizon->memory = ensure(make_backing(m_size));
	}

	shm::shm(u64 size, const std::string&)
		: shm(size, 0)
	{
	}

	shm::~shm()
	{
		unmap_self();
		{
			std::lock_guard lock(m_horizon->mutex);
			for (const mapping& value : m_horizon->mappings)
			{
				if (unmap_alias(value))
				{
					release(value.reservation);
				}
			}
		}
		delete m_horizon;
	}

	u8* shm::map(void* ptr, protection prot, bool cow) const
	{
		if (cow || prot == protection::wx || prot == protection::rx)
		{
			return nullptr;
		}
		std::lock_guard lock(m_horizon->mutex);
		m_horizon->mappings.reserve(m_horizon->mappings.size() + 1);
		mapping value{static_cast<u8*>(ptr), m_horizon->memory, 0, m_size, prot};
		if (!value.destination)
		{
			virtmemLock();
			value.destination = static_cast<u8*>(virtmemFindAslr(m_size, 0));
			value.reservation = value.destination ? virtmemAddReservation(value.destination, m_size) : nullptr;
			virtmemUnlock();
			if (!value.reservation)
			{
				return nullptr;
			}
		}
		if (!map_alias(value))
		{
			release(value.reservation);
			return nullptr;
		}
		m_horizon->mappings.push_back(value);
		return value.destination;
	}

	u8* shm::try_map(void* ptr, protection prot, bool cow) const
	{
		return ptr ? map(ptr, prot, cow) : nullptr;
	}

	std::pair<u8*, std::string> shm::map_critical(void* ptr, protection prot, bool cow)
	{
		u8* const result = map(ptr, prot, cow);
		return result ? std::pair<u8*, std::string>{result, {}} :
		                std::pair<u8*, std::string>{nullptr, "Horizon alias mapping failed or requested unsupported protection"};
	}

	u8* shm::map_self(protection prot)
	{
		if (prot != protection::rw)
		{
			return nullptr;
		}
		void* expected = nullptr;
		m_ptr.compare_exchange(expected, m_horizon->memory->canonical);
		return static_cast<u8*>(+m_ptr);
	}

	void shm::unmap(void* ptr) const
	{
		std::lock_guard lock(m_horizon->mutex);
		auto found = std::find_if(m_horizon->mappings.begin(), m_horizon->mappings.end(), [ptr](const mapping& value)
			{
				return value.destination == ptr;
			});
		if (found != m_horizon->mappings.end() && unmap_alias(*found))
		{
			release(found->reservation);
			m_horizon->mappings.erase(found);
		}
	}

	void shm::unmap_critical(void* ptr)
	{
		unmap(ptr);
	}

	void shm::unmap_self()
	{
		m_ptr.exchange(nullptr);
	}
} // namespace utils
