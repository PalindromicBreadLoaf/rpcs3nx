#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>

namespace rpcs3::switch_app
{
	class callback_queue
	{
	public:
		using callback = std::function<void()>;

		void post(callback function, std::atomic_bool* completed = nullptr);
		std::size_t drain();

	private:
		struct entry
		{
			callback function;
			std::atomic_bool* completed;
		};

		std::mutex m_mutex;
		std::queue<entry> m_callbacks;
	};
} // namespace rpcs3::switch_app
