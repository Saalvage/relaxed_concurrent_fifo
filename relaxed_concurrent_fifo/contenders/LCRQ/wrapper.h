#ifndef LCRQ_WRAPPER_H_INCLUDED
#define LCRQ_WRAPPER_H_INCLUDED

template <typename T, template <typename> typename FIFO>
struct adapter {
private:
	FIFO<void> queue;

	std::atomic_int curr_thread_id = 0;

public:
	adapter(int thread_count, size_t) : queue{thread_count} { }

	struct handle {
	private:
		FIFO<void>* queue;
		int thread_id;

	public:
		handle(FIFO<void>* queue, int thread_id) : queue(queue), thread_id(thread_id) { }

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
