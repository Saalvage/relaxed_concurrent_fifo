#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
#include <random>
#include <new>
#include <bitset>

#include "atomic_bitset.h"

template <typename T, size_t SIZE>
class relaxed_fifo {
private:
	using handle_id = uint8_t;

	// TODO: We likely want the state to include the claiming thread as well.
	// 14 bits epoch, 1 bit write/read, 1 bit inactive/active
	using state = uint16_t;

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	static constexpr state make_active(state marker) { return marker | 1; }
	static constexpr state make_inactive(state marker) { return marker ^ 1; }

	static constexpr state make_write(state marker) { return marker ^ 0b10; }
	static constexpr state make_read(state marker) { return marker | 0b10; }

	struct alignas(8) header {
		std::atomic<handle_id> active_handle_id;
		std::atomic<state> state;
		std::atomic_uint16_t curr_index;
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
		atomic_bitset<blocks_per_window()> occupied_set;
		block blocks[blocks_per_window()];
	};

	static constexpr size_t window_count() {
		return SIZE / blocks_per_window() / cells_per_block();
	}

	// TODO: Stupid positioning, needs access to window_count().
	template <bool is_write>
	static constexpr state make_state(size_t window) {
		return static_cast<state>((window / window_count()) << 2) | (is_write << 1);
	}

	std::unique_ptr<window[]> buffer = std::make_unique<window[]>(window_count());
	
	std::atomic_uint64_t read_window = 0;
	std::atomic_uint64_t write_window = 1;

	window& get_read_window() { return buffer[read_window % window_count()]; }
	window& get_write_window() { return buffer[write_window % window_count()]; }

	std::atomic_bool read_wants_move;
	std::atomic_bool write_wants_move;

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

		// Doing it like this allows the push code to grab a new block instead of requiring special cases for first-time initialization.
		// An already active block will always trigger a check.
		static inline block dummy_block{header{0, make_active(0)}};
	
		block* read_block = &dummy_block;
		block* write_block = &dummy_block;

		uint64_t write_window;
		uint64_t read_window;

		// These represent an inactive occupation of the current epoch.
		state write_occ;
		state read_occ;

		handle(relaxed_fifo& fifo) : fifo(fifo), id(fifo.get_handle_id()) {
			write_window = fifo.write_window;
			write_occ = make_state<true>(write_window);
			read_occ = make_state<false>(write_window);
		}

		friend relaxed_fifo;

		std::random_device dev;
		std::mt19937 rng{dev()};
		// TODO: Check template parameter here.
		std::uniform_int_distribution<size_t> dist{0, blocks_per_window() - 1};

		// TODO: Simply continuously set bits from a given starting point?
		size_t claim_free_bit(atomic_bitset<blocks_per_window()>& bits) {
			auto off = dist(rng);
			for (size_t i = 0; i < bits.size(); i++) {
				auto idx = (i + off) % bits.size();
				if (bits.set(idx)) {
					return idx;
				}
			}
			return std::numeric_limits<size_t>::max();
		}

		enum class move_window_result {
			moved_by_me, other_is_moving, failure
		};

		// Precondition: write_currently_claiming claimed.
		// Postcondition: write_wants_move (unless other_is_moving), write_currently_claiming still claimed.
		move_window_result move_write_window() {
			bool expected = false;
			if (fifo.write_wants_move.compare_exchange_strong(expected, true)) {
				// We're now moving the window!
				// Wait until no more new blocks are being claimed.
				while (fifo.write_currently_claiming > 1) {}
				auto expected = write_occ;
				auto write = make_read(expected);
				for (block& block : fifo.get_write_window().blocks) {
					// Busy wait until operation concludes.
					while (!block.header.state.compare_exchange_weak(expected, write)) {
						// If something besides activity is incorrect here we're in trouble!
						assert(expected == make_active(write_occ));
						expected = write_occ;
					}
					// Clean up header.
					block.header.active_handle_id = 0;
				}
				auto new_window = fifo.write_window + 1;
				if ((new_window - fifo.read_window) % window_count() != 0) {
					fifo.write_window = new_window;
				} else {
					return move_window_result::failure;
				}
				return move_window_result::moved_by_me;
			} else {
				return move_window_result::other_is_moving;
			}
		}

		// Postcondition (on true return): A valid block in the active window is claimed and correctly marked as occupied.
		bool claim_new_block() {
			while (true) {
				fifo.write_currently_claiming++;
				if (fifo.write_wants_move) {
					fifo.write_currently_claiming--;
				} else {
					break;
				}
				// Wait until moving finished.
				while (fifo.write_wants_move) {
					// TODO: Maybe something better than busy waiting?
				}
			}

			size_t free_bit = claim_free_bit(fifo.get_write_window().occupied_set);
			// This can be a if instead of a while because we are guaranteed to claim a free bit in a new window (at least when writing).
			if (free_bit == std::numeric_limits<size_t>::max()) {
				switch (move_write_window()) {
				case move_window_result::other_is_moving:
					// Someone else is already moving the window, we give up the block and wait until they're done.
					fifo.write_currently_claiming--;
					// TODO: Get rid of recursion.
					return claim_new_block();
				case move_window_result::failure:
					// TODO: Deal better with an almost full queue? Each push will try to move the window unsuccessfully.
					fifo.write_wants_move = false;
					fifo.write_currently_claiming--;
					return false;
				case move_window_result::moved_by_me:
					// Claim a new block, we're the only one able to at this point so we can be sure that we get one.
					free_bit = claim_free_bit(fifo.get_write_window().occupied_set);
					fifo.write_wants_move = false;
					break;
				}
			}
			write_window = fifo.write_window;
			write_occ = make_state<true>(write_window);
			write_block = &fifo.get_write_window().blocks[free_bit];
			// TODO: Consider what could happen between claiming the block via the occupied set and setting the id here (if stealing was possible).
			auto& header = write_block->header;
			// The order here is important, we first set the state to active, then decrease the claim counter, otherwise a new window switch might occur inbetween.
			header.state = make_active(write_occ);
			header.active_handle_id = id; // TODO: Maybe CAS to detect stealing or sth? TODO Writers don't ever use this!
			fifo.write_currently_claiming--;
			return true;
		}

	public:
		bool push(T t) {
			header* header = &write_block->header;
			auto expected = write_occ;
			if (!header->state.compare_exchange_strong(expected, make_active(expected))
				|| fifo.write_wants_move
				|| header->curr_index >= cells_per_block()) {
				// Something happened, someone wants to move the window or our block is full, we get a new block!
				header->state = write_occ;
				if (!claim_new_block()) {
					return false;
				}
				header = &write_block->header;
			}

			write_block->cells[header->curr_index++] = std::move(t);
			
			header->state = write_occ;
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
