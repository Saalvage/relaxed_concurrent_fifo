#ifndef THREAD_POOL_H_INCLUDED
#define THREAD_POOL_H_INCLUDED

#include <vector>
#include <thread>
#include <semaphore>
#include <barrier>
#include <functional>

class thread_pool {
private:
	// Argh.
	struct sem {
		std::binary_semaphore inner{ 0 };
		std::binary_semaphore* operator->() { return &inner; }
	};

	std::vector<std::jthread> threads;
	std::vector<sem> sems;
	std::barrier<> barrier;
	std::stop_source ss;

	std::function<void(int, std::barrier<>&)> per_thread;

public:
	explicit thread_pool();
	~thread_pool();

	void do_work(std::function<void(int, std::barrier<>&)> func, int thread_count, bool do_signaling = false);
	void signal_and_wait();

	int max_threads() const;
};

#endif // THREAD_POOL_H_INCLUDED
