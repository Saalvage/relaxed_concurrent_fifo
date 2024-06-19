#ifndef LOCK_FIFO_H_INCLUDED
#define LOCK_FIFO_H_INCLUDED

#include "fifo.h"

#include <utility>
#include <optional>
#include <mutex>
#include <memory>

template <typename T, size_t SIZE>
class lock_fifo {
private:
	static consteval bool is_po2(size_t size) {
		size_t it = 1;
		while (it < size) {
			it *= 2;
		}
		return it == size;
	}

	static_assert(is_po2(SIZE), "Please only use sizes that are a power of two as this allows for more efficient code generation");

	std::unique_ptr<T[]> buffer = std::make_unique<T[]>(SIZE);

	size_t head = 0;
	size_t tail = 0;

	std::mutex mut;

public:
	bool push(T t) {
		std::scoped_lock lock(mut);

		if (head - tail == SIZE) {
			return false;
		}

		head++;
		buffer[head % SIZE] = std::move(t);
		return true;
	}

	std::optional<T> pop() {
		std::scoped_lock lock(mut);

		if (head == tail) {
			return std::nullopt;
		}

		tail++;
		return std::move(buffer[tail % SIZE]);
	}
};
static_assert(fifo<lock_fifo>);

#endif // LOCK_FIFO_H_INCLUDED
