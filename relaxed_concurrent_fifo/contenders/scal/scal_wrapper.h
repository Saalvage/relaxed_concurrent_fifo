#ifndef BSKFIFO_WRAPPER_H_INCLUDED
#define BSKFIFO_WRAPPER_H_INCLUDED

#include "boundedsize_kfifo.h"

template <typename T>
struct scal_wrapper {
private:
	scal::BoundedSizeKFifo<T> queue{};

	std::atomic_int curr_thread_id = 0;

public:
	// TODO: We want the k to be equivalent to our implementation.
	scal_wrapper(size_t thread_count, size_t size) : queue{thread_count * 7 * 4, size / thread_count / 7 / 4 } { }

	struct handle {
	private:
		scal::BoundedSizeKFifo<T>* queue;
		int thread_id;

	public:
		handle(scal::BoundedSizeKFifo<T>* queue, int thread_id) : queue(queue), thread_id(thread_id) { }

		bool push(T t) {
			return queue->enqueue(std::move(t));
		}

		std::optional<T> pop() {
			T t;
			return queue->dequeue(&t) ? std::optional<T>(t) : std::nullopt;
		}
	};

	handle get_handle() {
		return handle{&queue, curr_thread_id++};
	}
};

#endif // BSKFIFO_WRAPPER_H_INCLUDED
