#ifndef BSKFIFO_WRAPPER_H_INCLUDED
#define BSKFIFO_WRAPPER_H_INCLUDED

#include "boundedsize_kfifo.h"
#include "segment_queue.h"

template <typename T, size_t K>
struct scal_wrapper {
private:
	scal::BoundedSizeKFifo<T> queue;

	std::atomic_int curr_thread_id = 0;

public:
	scal_wrapper(size_t thread_count, size_t size) : queue{thread_count * K, size / K / thread_count} { }

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
