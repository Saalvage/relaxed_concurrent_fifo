#ifndef LCRQ_WRAPPER_H_INCLUDED
#define LCRQ_WRAPPER_H_INCLUDED

#include "LCRQueue.hpp"

template <typename T>
struct lcrq {
private:
	LCRQueue<void> queue{};

	int curr_thread_id = 0;

public:
	lcrq(size_t) {
		
	}

	struct handle {
	private:
		LCRQueue<void>* queue;
		int thread_id;

	public:
		handle(LCRQueue<void>* queue, int thread_id) : queue(queue), thread_id(thread_id) { }

		bool push(const T& t) {
			queue->enqueue(reinterpret_cast<void*>(t), thread_id);
			return true;
		}

		std::optional<T> pop() {
			auto ret = queue->dequeue(thread_id);
			return ret == nullptr ? std::nullopt : std::optional<T>(reinterpret_cast<T>(ret));
		}
	};

	handle get_handle() {
		return handle{&queue, curr_thread_id++};
	}
};

#endif // LCRQ_WRAPPER_H_INCLUDED
