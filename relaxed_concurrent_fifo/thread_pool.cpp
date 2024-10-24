#include "thread_pool.h"

#include <cassert>
#include <iostream>
#include <ostream>

#ifdef _POSIX_VERSION
#include <pthread.h>
#endif // _POSIX_VERSION

thread_pool::thread_pool() : sems(std::thread::hardware_concurrency()), barrier(std::thread::hardware_concurrency() + 2) {
	int thread_count = static_cast<int>(std::thread::hardware_concurrency());
	threads.reserve(thread_count);
	auto lambda = [this](std::stop_token token, int i) {
#ifdef _POSIX_VERSION
		cpu_set_t cpu_set;
		CPU_ZERO(&cpu_set);
		CPU_SET(i, &cpu_set);
		if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set)) {
			throw std::runtime_error("Failed to set thread affinity!");
		}
#endif // _POSIX_VERSION

		while (true) {
			sems[i]->acquire();
			if (token.stop_requested()) {
				return;
			}
			per_thread(i, barrier);
			barrier.arrive_and_wait();
		}
	};
	for (int i = 0; i < thread_count; i++) {
		threads.emplace_back(lambda, ss.get_token(), i);
	}
}

thread_pool::~thread_pool() {
	ss.request_stop();
	for (size_t i = 0; i < threads.size(); i++) {
		sems[i]->release();
		threads[i].join();
	}
}

/// <summary>
/// Executes <paramref name="func"/> on <paramref name="thread_count"/> threads.
/// </summary>
/// <param name="func">Must arrive at the barrier.</param>
/// <param name="thread_count"></param>
/// <param name="do_signaling">Whether the caller wants to manually signal the start of the execution.</param>
void thread_pool::do_work(std::function<void(int, std::barrier<>&)> func, int thread_count, bool do_signaling) {
	assert(thread_count <= threads.size());

	per_thread = std::move(func);
	for (int i = 0; i < thread_count; i++) {
		sems[i]->release();
	}
	if (thread_count < static_cast<int>(threads.size())) {
		std::ignore = barrier.arrive(threads.size() - thread_count);
	}

	if (do_signaling) {
		std::ignore = barrier.arrive();
	}
	barrier.arrive_and_wait();

	// Wait until all threads have concluded.
	if (thread_count < static_cast<int>(threads.size())) {
		std::ignore = barrier.arrive(threads.size() - thread_count);
	}
	std::ignore = barrier.arrive();
	barrier.arrive_and_wait();
}

void thread_pool::signal_and_wait() {
	barrier.arrive_and_wait();
}

