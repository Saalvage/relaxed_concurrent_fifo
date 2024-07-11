#ifndef CONCURRENT_FIFO_H_INCLUDED
#define CONCURRENT_FIFO_H_INCLUDED

#include "fifo.h"

#include <utility>
#include <optional>
#include <mutex>
#include <memory>

template <typename T, size_t SIZE>
class concurrent_fifo {
private:
	static consteval bool is_po2(size_t size) {
		size_t it = 1;
		while (it < size) {
			it *= 2;
		}
		return it == size;
	}

	static_assert(is_po2(SIZE), "Please only use sizes that are a power of two as this allows for more efficient code generation");

	struct slot {
		std::atomic<T> value;
		std::atomic<uint64_t> epoch;
	};

	std::unique_ptr<slot[]> buffer = std::make_unique<slot[]>(SIZE);

	std::atomic<size_t> head = 0;
	std::atomic<size_t> tail = 0;


	static constexpr uint64_t slot_to_epoch(uint64_t index, bool written) {
		return ((index) & ~(1ull << 63)) | (static_cast<uint64_t>(written) << 63);
	}

public:
	concurrent_fifo() {
		for (size_t i = 0; i < SIZE; i++) {
			buffer[i].epoch = slot_to_epoch(i, false);
		}
	}

	bool push(T t) {
		size_t slot;

		slot = head.load();
		do {
			if (slot - tail >= SIZE) {
				return false;
			}
		} while (!head.compare_exchange_weak(slot, slot + 1));

		auto my_epoch = slot_to_epoch(slot, false);
		while (buffer[slot % SIZE].epoch != my_epoch) { }
		buffer[slot % SIZE].value.store(t);
		buffer[slot % SIZE].epoch.store(slot_to_epoch(slot, true));

		return true;
	}

	std::optional<T> pop() {
		size_t slot = tail.load();
		do {
			if (slot == head) {
				return std::nullopt;
			}
		} while (!tail.compare_exchange_weak(slot, slot + 1));

		auto my_epoch = slot_to_epoch(slot, true);
		while (buffer[slot % SIZE].epoch != my_epoch) { }
		auto ret = buffer[slot % SIZE].value.load();
		buffer[slot % SIZE].epoch.store(slot_to_epoch(slot + SIZE, false));

		return ret;
	}

	using handle = concurrent_fifo&;

	handle get_handle() { return *this; }
};
static_assert(fifo<concurrent_fifo>);

#endif // CONCURRENT_FIFO_H_INCLUDED
