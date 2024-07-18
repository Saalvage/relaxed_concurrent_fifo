#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
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

	static constexpr size_t cells_per_block() {
		// TODO: Heuristic goes here.
		return 8;
	}
	static_assert(SIZE% cells_per_block() == 0);

	struct block {
		header header;
		std::array<std::atomic<T>, cells_per_block() - 1> cells;
	};
	static_assert(sizeof(block) == cells_per_block() * sizeof(T));

	std::unique_ptr<block[]> buffer = std::make_unique<block[]>(SIZE);

	static constexpr size_t block_count() {
		return SIZE / cells_per_block();
	}

	static inline thread_local std::uniform_int_distribution<std::mt19937::result_type> within_block_dist{ 0,
		static_cast<std::mt19937::result_type>(SIZE / block_count() - 2) };
	static inline thread_local std::mt19937 generator{ std::random_device()() };

public:
	class handle {
	private:
		relaxed_fifo& fifo;
	
		size_t head_block;
		size_t tail_block;

		handle(relaxed_fifo& fifo) : fifo(fifo) { }

		friend relaxed_fifo;

	public:
		bool push(T t) {
			// Get random cell within block and try to insert into it.
			auto& block = fifo.buffer[head_block];
			auto res = &block.cells[within_block_dist(generator)];
			T val = *res;
			if (val == 0 && std::atomic_compare_exchange_strong(res, &val, t)) {
				// Inserted on first try.
				return true;
			}

			// No luck, move onto next block? Use some kind of heuristic.
			if (block.header.filled_cells > cells_per_block() / 2) {

			}

			return false;
		}

		std::optional<T> pop() {

		}
	};

	handle get_handle() { return handle(*this); }
};
//static_assert(fifo<relaxed_fifo>);

#endif // RELAXED_FIFO_H_INCLUDED
