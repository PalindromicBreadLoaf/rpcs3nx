#include "callback_queue.h"

#include <utility>

namespace rpcs3::switch_app
{
	void callback_queue::post(callback function, std::atomic_bool* completed)
	{
		std::lock_guard lock(m_mutex);
		m_callbacks.push({std::move(function), completed});
	}

	std::size_t callback_queue::drain()
	{
		std::queue<entry> pending;
		{
			std::lock_guard lock(m_mutex);
			pending.swap(m_callbacks);
		}

		const std::size_t count = pending.size();
		while (!pending.empty())
		{
			entry current = std::move(pending.front());
			pending.pop();
			current.function();
			if (current.completed)
			{
				current.completed->store(true, std::memory_order_release);
				current.completed->notify_all();
			}
		}
		return count;
	}
} // namespace rpcs3::switch_app
