#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
#include <random>

template <typename T, size_t SIZE>
class relaxed_fifo {
private:
	using handle_id = uint8_t;

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct header {
		std::atomic_bool operation_active;
		std::atomic<handle_id> active_handle_id;
		std::atomic_uint16_t curr_index;
		uint32_t padding;
	};
	static_assert(sizeof(header) == 8);

	static constexpr size_t cells_per_block() {
		return std::hardware_constructive_interference_size / sizeof(T) - 1;
	}

	struct block {
		header header;
		std::array<std::atomic<T>, cells_per_block()> cells;
	};
	static_assert(sizeof(block) == cells_per_block() * sizeof(T) + sizeof(header));
	static_assert(sizeof(block) >= std::hardware_destructive_interference_size);
	static_assert(sizeof(block) <= std::hardware_constructive_interference_size);

	static constexpr size_t blocks_per_window() {
		return 8;
	}

	struct window {
		block blocks[blocks_per_window()];
	};

	static constexpr size_t window_count() {
		return SIZE / blocks_per_window() / cells_per_block();
	}

	std::unique_ptr<window[]> buffer = std::make_unique<window[]>(window_count());
	
	std::atomic_uint64_t read_window = 1;
	std::atomic_uint64_t write_window = 2;

	std::atomic_bool read_wants_move;
	std::atomic_bool write_wants_move;

	std::atomic_uint8_t read_occupied_set;
	std::atomic_uint8_t write_occupied_set;

	std::atomic_uint8_t read_currently_claiming;
	std::atomic_uint8_t write_currently_claiming;

	// 0 is a reserved id!
	std::atomic<handle_id> latest_handle_id = 1;
	handle_id get_handle_id() {
		return latest_handle_id++;
	}

public:
	class handle {
	private:
		relaxed_fifo& fifo;

		handle_id id;
	
		block* read_block;
		block* write_block;

		uint64_t current_window = 0;

		handle(relaxed_fifo& fifo) : fifo(fifo), id(fifo.get_handle_id()) { }

		friend relaxed_fifo;

		static constexpr uint8_t first_free_bit(uint8_t bit) {
			for (uint8_t i = 0; i < 8; i++) {
				if (((bit >> i) & 1) == 0) {
					return i;
				}
			}
			return std::numeric_limits<uint8_t>::max();
		}

		void claim_new_block(header* header) {
			while (true) {
				fifo.write_currently_claiming++;
				if (fifo.write_wants_move) {
					fifo.write_currently_claiming--;
				} else {
					break;
				}
				// Wait until moving finished.
				while (fifo.write_wants_move) { }
			}

			if (header) {
				// TODO: Is this ok? Could this initiate a window move WITHOUT checking for write_wants_move?
				header->operation_active = false;
			}

			uint8_t free_bit;
			uint8_t occupied_set = fifo.write_occupied_set;
			do {
				free_bit = first_free_bit(occupied_set);
				if (free_bit == std::numeric_limits<uint8_t>::max()) {
					bool expected = false;
					if (fifo.write_wants_move.compare_exchange_strong(expected, true)) {
						// We're now moving the window!
						// Wait until no more new blocks are being claimed.
						while (fifo.write_currently_claiming > 1) { }
						if (header) {
							header->operation_active = false;
						}
						for (block& block : fifo.buffer[fifo.write_window % window_count()].blocks) {
							// Busy wait until operation concludes.
							while (block.header.operation_active) { }
							// Clean up header.
							block.header.active_handle_id = 0;
						}
						// Now no block can be active.
						fifo.write_occupied_set = 0;
						auto new_window = fifo.write_window + 1;
						if ((new_window - fifo.read_window) % window_count() != 0) {
							fifo.write_window = new_window;
						} else {
							// TODO: We can't move!
							throw 2;
						}
						// Reset values for loop.
						header = nullptr; // We don't have a header now because the window just moved and we need a new block from the new window!
						occupied_set = fifo.write_occupied_set;
						free_bit = first_free_bit(occupied_set);
						fifo.write_wants_move = false;
					} else {
						// Someone else is already moving the window, we give up the block and wait until they're done.
						if (header) {
							header->operation_active = false;
						}
						fifo.write_currently_claiming--;
						claim_new_block(header);
						return;
					}
				}
			} while (!fifo.write_occupied_set.compare_exchange_strong(occupied_set, occupied_set | (1 << free_bit)));
			current_window = fifo.write_window;
			write_block = &fifo.buffer[current_window % window_count()].blocks[free_bit];
			// TODO: Consider what could happen between claiming the block via the occupied set and setting the id here (if stealing was possible).
			header = &write_block->header;
			// The order here is important, we first set the state to active, then decrease the claim counter, otherwise  
			header->operation_active = true;
			header->active_handle_id = id; // TODO: Maybe CAS to detect stealing or sth? TODO Writers don't ever use this!
			fifo.write_currently_claiming--;
		}

	public:
		bool push(T t) {
push_start:
			// if (write_block->header.active_handle_id != id) { /* ... */ }
			// Push blocks can currently not be stolen.

			if (current_window != fifo.write_window) {
				// The window moved!
				claim_new_block(nullptr);
				goto ready_to_write;
			}

			if (fifo.write_wants_move) {
				while (fifo.write_wants_move) {
					// TODO: Maybe something better than busy waiting?
				}
				claim_new_block(nullptr);
				goto ready_to_write;
			}

			header* header = &write_block->header;
			// TODO: Can we even do this? Between the check and the write the window could've moved and the read window moved onto this one.
			// Possible solution: operation_active gets an epoch counter with which we can CAS.
			header->operation_active = true;

			// It changed between the last check and now, we need to let the window move progress because it might've missed we're busy!
			// TODO: What could be a possibility here is the window moving, then the read window moving onto this one and the block being claimed?
			// (All between the set and this check)
			if (fifo.write_wants_move) {
				header->operation_active = false;
				return push(std::move(t));
			}

			if (current_window != fifo.write_window) {
				goto push_start;
			}

			if (header->curr_index >= cells_per_block()) {
				claim_new_block(header);
				header = &write_block->header;
			}

ready_to_write:
			write_block->cells[header->curr_index++] = std::move(t);
			
			return true;
		}

		std::optional<T> pop() {
			return std::nullopt;
		}
	};

	handle get_handle() { return handle(*this); }
};
//static_assert(fifo<relaxed_fifo>);

#endif // RELAXED_FIFO_H_INCLUDED
