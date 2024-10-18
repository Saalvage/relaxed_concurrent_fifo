#include "thread_pool.h"

#include <cassert>
#include <iostream>
#include <ostream>

#ifdef _POSIX_VERSION
#include <pthread.h>
#endif // _POSIX_VERSION

thread_pool::thread_pool(bool do_signaling) : do_signaling(do_signaling), sems(std::thread::hardware_concurrency()), barrier(std::thread::hardware_concurrency() + (do_signaling ? 1 : 2)) {
	auto thread_count = std::thread::hardware_concurrency();
	threads.reserve(thread_count);
	auto lambda = [this](std::stop_token token, size_t i) {
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
	for (size_t i = 0; i < thread_count; i++) {
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
void thread_pool::do_work(std::function<void(size_t, std::barrier<>&)> func, size_t thread_count) {
	assert(thread_count <= threads.size());

	per_thread = std::move(func);
	for (size_t i = 0; i < thread_count; i++) {
		sems[i]->release();
	}
	if (thread_count < threads.size()) {
		std::ignore = barrier.arrive(threads.size() - thread_count);
	}

	barrier.arrive_and_wait();

	// Wait until all threads have concluded.
	if (thread_count < threads.size()) {
		std::ignore = barrier.arrive(threads.size() - thread_count);
	}
	if (!do_signaling) {
		std::ignore = barrier.arrive();
	}
	barrier.arrive_and_wait();
}

void thread_pool::signal_and_wait() {
	assert(!do_signaling);
	barrier.arrive_and_wait();
}

