#ifndef THREAD_POOL_H_INCLUDED
#define THREAD_POOL_H_INCLUDED

#include <vector>
#include <thread>
#include <semaphore>
#include <barrier>
#include <functional>

class thread_pool {
private:
	bool do_signaling;

	// Argh.
	struct sem {
		std::binary_semaphore inner{ 0 };
		std::binary_semaphore* operator->() { return &inner; }
	};

	std::vector<std::jthread> threads;
	std::vector<sem> sems;
	std::barrier<> barrier;
	std::stop_source ss;

	std::function<void(size_t, std::barrier<>&)> per_thread;

public:
	explicit thread_pool(bool do_signaling);
	~thread_pool();

	void do_work(std::function<void(size_t, std::barrier<>&)> func, size_t thread_count);
	void signal_and_wait();
};

#endif // THREAD_POOL_H_INCLUDED
