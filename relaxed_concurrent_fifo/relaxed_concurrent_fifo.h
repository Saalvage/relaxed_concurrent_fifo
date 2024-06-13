#ifndef RELAXED_CONCURRENT_FIFO_H_INCLUDED
#define RELAXED_CONCURRENT_FIFO_H_INCLUDED

#include <utility>
#include <optional>
#include <mutex>

template <typename T, size_t SIZE>
class circular_buffer {
private:
	static consteval size_t next_po2(size_t size) {
		size_t it = 1;
		while (it < size) {
			it *= 2;
		}
		return it;
	}

	static constexpr size_t ACTUAL_SIZE = next_po2(SIZE);

	// TODO: Always put data on the heap?
	T buffer[ACTUAL_SIZE];

	size_t head = 0;
	size_t tail = 0;

	std::mutex mut;

public:
	bool push(T t) {
		std::scoped_lock lock(mut);

		if (head - tail == ACTUAL_SIZE) {
			return false;
		}

		head++;
		buffer[head % ACTUAL_SIZE] = std::move(t);
		return true;
	}

	std::optional<T> pop() {
		std::scoped_lock lock(mut);

		if (head == tail) {
			return std::nullopt;
		}

		tail++;
		return std::move(buffer[tail % ACTUAL_SIZE]);
	}
};

#endif // RELAXED_CONCURRENT_FIFO_H_INCLUDED
