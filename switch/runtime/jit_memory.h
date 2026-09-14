#pragma once

#include <switch.h>

#include <cstddef>
#include <mutex>
#include <vector>

namespace rpcs3::switch_runtime
{
	class jit_memory
	{
	public:
		struct allocation
		{
			u8* rx = nullptr;
			u8* rw = nullptr;
			std::size_t size = 0;

			explicit operator bool() const
			{
				return rx && rw;
			}
		};

		jit_memory() = default;
		jit_memory(const jit_memory&) = delete;
		jit_memory& operator=(const jit_memory&) = delete;
		~jit_memory();

		Result initialize(std::size_t size);
		void finalize();
		allocation allocate(std::size_t size, std::size_t alignment = 16);
		bool release(const allocation& allocation);
		void publish(const allocation& allocation, std::size_t offset = 0, std::size_t size = 0);

		bool available() const;
		std::size_t capacity() const;
		std::size_t used() const;

	private:
		struct block
		{
			std::size_t offset;
			std::size_t size;
			bool free;
		};

		mutable std::mutex m_mutex;
		Jit m_jit{};
		u8* m_rx = nullptr;
		u8* m_rw = nullptr;
		std::size_t m_size = 0;
		std::vector<block> m_blocks;
	};
} // namespace rpcs3::switch_runtime
