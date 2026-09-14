#include "jit_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>

namespace rpcs3::switch_runtime
{
	namespace
	{
		constexpr std::size_t page_size = 0x1000;
		constexpr std::size_t max_block_count = 16'384;

		bool align_up(std::size_t value, std::size_t alignment, std::size_t& result)
		{
			if (!alignment || (alignment & (alignment - 1)))
			{
				return false;
			}

			const std::size_t mask = alignment - 1;
			if (value > std::numeric_limits<std::size_t>::max() - mask)
			{
				return false;
			}

			result = (value + mask) & ~mask;
			return true;
		}
	} // namespace

	jit_memory::~jit_memory()
	{
		finalize();
	}

	Result jit_memory::initialize(std::size_t size)
	{
		std::lock_guard lock(m_mutex);
		if (m_rx)
		{
			return 0;
		}

		std::size_t aligned_size = 0;
		if (!size || !align_up(size, page_size, aligned_size))
		{
			return MAKERESULT(Module_Libnx, LibnxError_BadInput);
		}

		Result rc = jitCreate(&m_jit, aligned_size);
		if (R_FAILED(rc))
		{
			return rc;
		}

		m_rw = static_cast<u8*>(jitGetRwAddr(&m_jit));
		m_rx = static_cast<u8*>(jitGetRxAddr(&m_jit));
		if (!m_rw || !m_rx || m_rw == m_rx || m_jit.type != JitType_CodeMemory)
		{
			jitClose(&m_jit);
			m_jit = {};
			m_rw = nullptr;
			m_rx = nullptr;
			return MAKERESULT(Module_Libnx, LibnxError_JitUnavailable);
		}

		rc = jitTransitionToExecutable(&m_jit);
		if (R_FAILED(rc))
		{
			jitClose(&m_jit);
			m_jit = {};
			m_rw = nullptr;
			m_rx = nullptr;
			return rc;
		}

		m_size = aligned_size;
		try
		{
			m_blocks.reserve(max_block_count);
			m_blocks.push_back({0, aligned_size, true});
		}
		catch (const std::bad_alloc&)
		{
			jitClose(&m_jit);
			m_jit = {};
			m_rw = nullptr;
			m_rx = nullptr;
			m_size = 0;
			return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		}
		return 0;
	}

	void jit_memory::finalize()
	{
		std::lock_guard lock(m_mutex);
		if (!m_rx)
		{
			return;
		}

		jitClose(&m_jit);
		m_jit = {};
		m_rx = nullptr;
		m_rw = nullptr;
		m_size = 0;
		m_blocks.clear();
	}

	jit_memory::allocation jit_memory::allocate(std::size_t size, std::size_t alignment)
	{
		std::lock_guard lock(m_mutex);
		if (!m_rx || !size || !alignment || (alignment & (alignment - 1)))
		{
			return {};
		}

		for (std::size_t index = 0; index < m_blocks.size(); index++)
		{
			const block current = m_blocks[index];
			if (!current.free)
			{
				continue;
			}

			std::size_t offset = 0;
			if (!align_up(current.offset, alignment, offset) || offset < current.offset ||
				size > current.size - (offset - current.offset))
			{
				continue;
			}

			const std::size_t prefix = offset - current.offset;
			const std::size_t suffix = current.size - prefix - size;
			std::array<block, 3> replacement{};
			std::size_t replacement_count = 0;
			if (prefix)
			{
				replacement[replacement_count++] = {current.offset, prefix, true};
			}
			replacement[replacement_count++] = {offset, size, false};
			if (suffix)
			{
				replacement[replacement_count++] = {offset + size, suffix, true};
			}
			if (m_blocks.size() - 1 + replacement_count > m_blocks.capacity())
			{
				return {};
			}

			m_blocks.erase(m_blocks.begin() + index);
			m_blocks.insert(m_blocks.begin() + index, replacement.begin(), replacement.begin() + replacement_count);
			return {m_rx + offset, m_rw + offset, size};
		}

		return {};
	}

	bool jit_memory::release(const allocation& allocation)
	{
		std::lock_guard lock(m_mutex);
		const std::uintptr_t rx = reinterpret_cast<std::uintptr_t>(allocation.rx);
		const std::uintptr_t rw = reinterpret_cast<std::uintptr_t>(allocation.rw);
		const std::uintptr_t rx_base = reinterpret_cast<std::uintptr_t>(m_rx);
		const std::uintptr_t rw_base = reinterpret_cast<std::uintptr_t>(m_rw);
		if (!m_rx || !allocation.rx || !allocation.rw || rx < rx_base || rx >= rx_base + m_size ||
			rw < rw_base || rw >= rw_base + m_size || rw - rw_base != rx - rx_base)
		{
			return false;
		}

		const std::size_t offset = rx - rx_base;
		auto found = std::find_if(m_blocks.begin(), m_blocks.end(), [offset, &allocation](const block& candidate)
			{
				return !candidate.free && candidate.offset == offset && candidate.size == allocation.size;
			});
		if (found == m_blocks.end())
		{
			return false;
		}

		found->free = true;
		if (found != m_blocks.begin() && (found - 1)->free)
		{
			(found - 1)->size += found->size;
			found = m_blocks.erase(found);
			found--;
		}
		if (found + 1 != m_blocks.end() && (found + 1)->free)
		{
			found->size += (found + 1)->size;
			m_blocks.erase(found + 1);
		}
		return true;
	}

	void jit_memory::publish(const allocation& allocation, std::size_t offset, std::size_t size)
	{
		if (!allocation || offset > allocation.size)
		{
			return;
		}

		if (!size)
		{
			size = allocation.size - offset;
		}
		if (size > allocation.size - offset)
		{
			return;
		}

		armDCacheFlush(allocation.rw + offset, size);
		armICacheInvalidate(allocation.rx + offset, size);
	}

	bool jit_memory::available() const
	{
		std::lock_guard lock(m_mutex);
		return m_rx != nullptr;
	}

	std::size_t jit_memory::capacity() const
	{
		std::lock_guard lock(m_mutex);
		return m_size;
	}

	std::size_t jit_memory::used() const
	{
		std::lock_guard lock(m_mutex);
		std::size_t result = 0;
		for (const block& current : m_blocks)
		{
			if (!current.free)
			{
				result += current.size;
			}
		}
		return result;
	}
} // namespace rpcs3::switch_runtime
