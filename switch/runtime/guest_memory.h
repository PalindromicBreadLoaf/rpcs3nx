#pragma once

#include <switch.h>

#include <cstddef>
#include <mutex>
#include <vector>

struct VirtmemReservation;

namespace rpcs3::switch_runtime
{
	class guest_memory
	{
	public:
		guest_memory() = default;
		guest_memory(const guest_memory&) = delete;
		guest_memory& operator=(const guest_memory&) = delete;
		~guest_memory();

		Result initialize(std::size_t backing_size, std::size_t address_space_size);
		Result finalize();
		Result map(std::size_t backing_offset, std::size_t address_space_offset, std::size_t size);
		Result unmap(std::size_t address_space_offset, std::size_t size);

		u8* backing() const;
		u8* address_space() const;
		std::size_t backing_size() const;
		std::size_t address_space_size() const;
		std::size_t mapping_count() const;

	private:
		struct mapping
		{
			u8* destination;
			u8* source;
			std::size_t size;
		};

		mutable std::mutex m_mutex;
		void* m_allocation = nullptr;
		u8* m_backing = nullptr;
		u8* m_address_space = nullptr;
		VirtmemReservation* m_backing_reservation = nullptr;
		VirtmemReservation* m_address_space_reservation = nullptr;
		std::size_t m_backing_size = 0;
		std::size_t m_address_space_size = 0;
		std::vector<mapping> m_mappings;
	};
} // namespace rpcs3::switch_runtime
