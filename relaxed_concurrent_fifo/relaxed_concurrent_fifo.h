#ifndef RELAXED_CONCURRENT_FIFO_H_INCLUDED
#define RELAXED_CONCURRENT_FIFO_H_INCLUDED

#include <utility>
#include <optional>

// TODO: How should a full queue be handled?
// Currently popping from a full queue is not possible.
template <typename T, size_t SIZE>
class circular_buffer {
private:
	T buffer[SIZE];

	size_t head = 0;
	size_t tail = 0;

	inline size_t next(size_t curr) {
		return (curr + 1) % SIZE;
	}

public:
	void push(T t) {
		head = next(head);
		buffer[head] = std::move(t);
	}

	std::optional<T> pop() {
		if (head == tail) {
			return std::nullopt;
		}

		tail = next(tail);
		return std::move(buffer[tail]);
	}
};

#endif // RELAXED_CONCURRENT_FIFO_H_INCLUDED
