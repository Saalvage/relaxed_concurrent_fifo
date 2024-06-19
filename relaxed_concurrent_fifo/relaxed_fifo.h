#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <memory>
#include <atomic>
#include <unordered_map>
#include <random>

template <typename T, size_t SIZE>
class relaxed_fifo {
private:
	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct header {
		std::atomic<uint16_t> epoch;
		std::atomic<uint16_t> filled_cells;
		std::atomic<uint16_t> tail_count;
		std::atomic<uint16_t> head_count;
	};
	static_assert(sizeof(header) == 8);

	std::unique_ptr<std::atomic<T>[]> buffer = std::make_unique<std::atomic<T>[]>(SIZE);

	static constexpr size_t cells_per_block() {
		// TODO: Heuristic goes here.
		return 8;
	}
	static_assert(SIZE % cells_per_block() == 0);

	static constexpr size_t block_count() {
		return SIZE / cells_per_block();
	}

	struct thread_locals {
		size_t head_block;
		size_t tail_block;
	};

	std::unordered_map<std::thread::id, thread_locals> map;

	static inline thread_local std::uniform_int_distribution<std::mt19937::result_type> within_block_dist{ 0,
		static_cast<std::mt19937::result_type>(SIZE / block_count() - 2) };
	static inline thread_local std::random_device device;
	static inline thread_local std::mt19937 generator{ device() };

	header* get_header(size_t index) {
		return reinterpret_cast<header*>(buffer.get() + index * cells_per_block());
	}

	header* next_header(header* header) {
		auto next = header + cells_per_block();
		if (next >= buffer.get() + SIZE) {
			return buffer.get();
		}
	}

public:
	relaxed_fifo() {

	}

	bool push(T t) {
		auto& locals = map[std::this_thread::get_id()];
		
		// Get random cell within block and try to insert into it.
		auto* header = get_header(locals.head_block);
		auto offset = within_block_dist(generator);
		(void)offset;
		std::atomic<T>* res = (std::atomic<T>*)(header + within_block_dist(generator));
		T val = *res;
		if (val == 0 && std::atomic_compare_exchange_strong(res, &val, t)) {
			// Inserted on first try.
			return true;
		}

		// No luck, move onto next block? Heuristic time!
		if (header->elements > cells_per_block() / 2) {

		}

		return false;
	}

	std::optional<T> pop() {
		auto& locals = map[std::this_thread::get_id()];
	}
};

#endif // RELAXED_FIFO_H_INCLUDED
