#ifndef BSKFIFO_WRAPPER_H_INCLUDED
#define BSKFIFO_WRAPPER_H_INCLUDED

#include "boundedsize_kfifo.h"
#include "rts_queue.h"
#include "segment_queue.h"
#include "random_dequeue_queue.h"
#include "lru_distributed_queue.h"

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
	ws_k_fifo(int thread_count, size_t size, size_t k) : scal_wrapper_base<T, scal::BoundedSizeKFifo>{thread_count * k, std::max<size_t>(4, size / k / thread_count)} { }
};

template <typename T>
struct ss_k_fifo : scal_wrapper_base<T, scal::BoundedSizeKFifo> {
public:
	ss_k_fifo([[maybe_unused]] int thread_count, size_t size, size_t k) : scal_wrapper_base<T, scal::BoundedSizeKFifo>{ k, std::max<size_t>(4, size / k) } {}
};

template <typename T>
struct rts_queue : scal_wrapper_base<T, RTSQueue> {
public:
	rts_queue(int thread_count, [[maybe_unused]] size_t size) {
		scal_wrapper_base<T, RTSQueue>::queue.initialize(thread_count);
	}
};

template <typename T>
struct ws_segment_queue : scal_wrapper_base<T, scal_sq::SegmentQueue> {
	ws_segment_queue(int thread_count, [[maybe_unused]] size_t size, size_t s) : scal_wrapper_base<T, scal_sq::SegmentQueue>{thread_count * s} { }
};

template <typename T>
struct ss_segment_queue : scal_wrapper_base<T, scal_sq::SegmentQueue> {
	ss_segment_queue([[maybe_unused]] int thread_count, [[maybe_unused]] size_t size, size_t s) : scal_wrapper_base<T, scal_sq::SegmentQueue>{ s } {}
};

template <typename T>
struct ws_random_dequeue_queue : scal_wrapper_base<T, scal::RandomDequeueQueue> {
	ws_random_dequeue_queue(int thread_count, [[maybe_unused]] size_t size, size_t quasi_factor, size_t max_retries) : scal_wrapper_base<T, scal::RandomDequeueQueue>{ thread_count * quasi_factor, max_retries } {}
};

template <typename T>
struct ss_random_dequeue_queue : scal_wrapper_base<T, scal::RandomDequeueQueue> {
	ss_random_dequeue_queue([[maybe_unused]] int thread_count, [[maybe_unused]] size_t size, size_t quasi_factor, size_t max_retries) : scal_wrapper_base<T, scal::RandomDequeueQueue>{ quasi_factor, max_retries } {}
};

template <typename T>
struct ws_lru_distributed_queue : scal_wrapper_base<T, scal::LRUDistributedQueue> {
	ws_lru_distributed_queue(int thread_count, [[maybe_unused]] size_t size, size_t quasi_factor) : scal_wrapper_base<T, scal::LRUDistributedQueue>{ thread_count * quasi_factor } {}
};

template <typename T>
struct ss_lru_distributed_queue : scal_wrapper_base<T, scal::LRUDistributedQueue> {
	ss_lru_distributed_queue([[maybe_unused]] int thread_count, [[maybe_unused]] size_t size, size_t quasi_factor) : scal_wrapper_base<T, scal::LRUDistributedQueue>{ quasi_factor } {}
};

#endif // BSKFIFO_WRAPPER_H_INCLUDED
