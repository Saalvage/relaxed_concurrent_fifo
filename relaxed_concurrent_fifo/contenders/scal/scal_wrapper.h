#ifndef BSKFIFO_WRAPPER_H_INCLUDED
#define BSKFIFO_WRAPPER_H_INCLUDED

#include "boundedsize_kfifo.h"
#include "rts_queue.h"
#include "segment_queue.h"

template <typename T, template <typename> typename FIFO>
struct scal_wrapper_base {
private:
	std::atomic_int curr_thread_id = 0;

protected:
	FIFO<T> queue;

	template <typename... Args>
	scal_wrapper_base(Args&&... args) : queue{std::forward<Args>(args)...} { }

public:
	struct handle {
	private:
		FIFO<T>* queue;
		int thread_id;

	public:
		handle(FIFO<T>* queue, int thread_id) : queue(queue), thread_id(thread_id) { }

		bool push(T t) {
			return queue->enqueue(std::move(t));
		}

		std::optional<T> pop() {
			T t;
			return queue->dequeue(&t) ? std::optional<T>(t) : std::nullopt;
		}
	};

	handle get_handle() {
		return handle{ &queue, curr_thread_id++ };
	}
};

template <typename T, size_t K>
struct bs_k_fifo : scal_wrapper_base<T, scal::BoundedSizeKFifo> {
public:
	// TODO: We allow the queue here the same courtesy of at least 4 segments, not sure if that's correct or they should have more.
	bs_k_fifo(size_t thread_count, size_t size) : scal_wrapper_base<T, scal::BoundedSizeKFifo>{thread_count * K, std::max<size_t>(4, size / K / thread_count)} { }
};

template <typename T>
struct rts_queue : scal_wrapper_base<T, RTSQueue> {
public:
	// TODO: We allow the queue here the same courtesy of at least 4 segments, not sure if that's correct or they should have more.
	rts_queue(size_t thread_count, [[maybe_unused]] size_t size) {
		scal_wrapper_base<T, RTSQueue>::queue.initialize(thread_count);
	}
};

#endif // BSKFIFO_WRAPPER_H_INCLUDED
