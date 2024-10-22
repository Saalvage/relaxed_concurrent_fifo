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

template <typename T>
struct ws_k_fifo : scal_wrapper_base<T, scal::BoundedSizeKFifo> {
public:
	ws_k_fifo(size_t thread_count, size_t size, size_t k) : scal_wrapper_base<T, scal::BoundedSizeKFifo>{thread_count * k, std::max<size_t>(4, size / k / thread_count)} { }
};

template <typename T>
struct ss_k_fifo : scal_wrapper_base<T, scal::BoundedSizeKFifo> {
public:
	ss_k_fifo([[maybe_unused]] size_t thread_count, size_t size, size_t k) : scal_wrapper_base<T, scal::BoundedSizeKFifo>{ k, std::max<size_t>(4, size / k) } {}
};

template <typename T>
struct rts_queue : scal_wrapper_base<T, RTSQueue> {
public:
	rts_queue(size_t thread_count, [[maybe_unused]] size_t size) {
		scal_wrapper_base<T, RTSQueue>::queue.initialize(thread_count);
	}
};

template <typename T>
struct ws_segment_queue : scal_wrapper_base<T, scal::SegmentQueue> {
	ws_segment_queue(size_t thread_count, [[maybe_unused]] size_t size, size_t s) : scal_wrapper_base<T, scal::SegmentQueue>{thread_count * s} { }
};

template <typename T>
struct ss_segment_queue : scal_wrapper_base<T, scal::SegmentQueue> {
	ss_segment_queue([[maybe_unused]] size_t thread_count, [[maybe_unused]] size_t size, size_t s) : scal_wrapper_base<T, scal::SegmentQueue>{ s } {}
};

#endif // BSKFIFO_WRAPPER_H_INCLUDED
