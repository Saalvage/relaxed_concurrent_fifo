#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
#include <random>
#include <new>
#include <bitset>

#include "atomic_bitset.h"

template <typename T, size_t SIZE, size_t CELLS_PER_BLOCK = std::hardware_constructive_interference_size / sizeof(T) - 1,
	size_t BLOCKS_PER_WINDOW = 8>
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

	struct block {
		header header;
		std::array<std::atomic<T>, CELLS_PER_BLOCK> cells;
	};
	static_assert(sizeof(block) == CELLS_PER_BLOCK * sizeof(T) + sizeof(header));
	static_assert(sizeof(block) >= std::hardware_destructive_interference_size);
	static_assert(sizeof(block) <= std::hardware_constructive_interference_size);

	struct window {
		atomic_bitset<BLOCKS_PER_WINDOW> occupied_set;
		block blocks[BLOCKS_PER_WINDOW];
	};

	static constexpr size_t window_count() {
		return SIZE / BLOCKS_PER_WINDOW / CELLS_PER_BLOCK;
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
			read_window = fifo.read_window;
			write_occ = make_state<true>(write_window);
			read_occ = make_state<false>(read_window);
		}

		friend relaxed_fifo;

		std::random_device dev;
		std::mt19937 rng{dev()};
		// TODO: Check template parameter here.
		std::uniform_int_distribution<size_t> dist{0, BLOCKS_PER_WINDOW - 1};

		template <bool is_write>
		size_t claim_free_bit(atomic_bitset<BLOCKS_PER_WINDOW>& bits) {
			auto off = dist(rng);
			for (size_t i = 0; i < bits.size(); i++) {
				auto idx = (i + off) % bits.size();
				if (is_write ? bits.set(idx) : bits.reset(idx)) {
					return idx;
				}
			}
			return std::numeric_limits<size_t>::max();
		}

		template <std::atomic_uint8_t relaxed_fifo::* currently_claiming, std::atomic_bool relaxed_fifo::* wants_move>
		void claim_currently_claiming() {
			while (true) {
				(fifo.*currently_claiming)++;
				if (fifo.*wants_move) {
					(fifo.*currently_claiming)--;
				} else {
					break;
				}
				// Wait until moving finished.
				while (fifo.*wants_move) {
					// TODO: Maybe something better than busy waiting?
				}
			}
		}

		enum class move_window_result {
			moved_by_me, other_is_moving, failure
		};

		// Precondition: write_currently_claiming claimed.
		// Postcondition: write_wants_move (unless other_is_moving), write_currently_claiming still claimed.
		template <bool is_write, std::atomic_uint8_t relaxed_fifo::* currently_claiming, std::atomic_bool relaxed_fifo::* wants_move,
			std::atomic_uint64_t relaxed_fifo::* fifo_window, std::atomic_uint64_t relaxed_fifo::* other_window, state handle::* occupation,
			window& (relaxed_fifo::* get_window)()>
		move_window_result move_window_int() {
			bool expected = false;
			if ((fifo.*wants_move).compare_exchange_strong(expected, true)) {
				// We're now moving the window!
				// Wait until no more new blocks are being claimed.
				while (fifo.*currently_claiming > 1) {}
				auto expected = this->*occupation;
				auto write = is_write ? make_read(expected) : make_write(expected);
				for (auto& block : (fifo.*get_window)().blocks) {
					// Busy wait until operation concludes.
					while (!block.header.state.compare_exchange_weak(expected, write)) {
						// If something besides activity is incorrect here we're in trouble!
						assert(expected == make_active(this->*occupation));
						expected = this->*occupation;
					}
					// Clean up header.
					block.header.active_handle_id = 0;
				}
				auto new_window = fifo.*fifo_window + 1;
				if ((new_window - fifo.*other_window) % window_count() != 0) {
					fifo.*fifo_window = new_window;
				} else {
					if constexpr (is_write) {
						// Whelp, we're out of luck here! Can't force forward the read window.
						return move_window_result::failure;
					} else {
						// Force forward the write window.
						claim_currently_claiming<&relaxed_fifo::write_currently_claiming, &relaxed_fifo::write_wants_move>();
						switch (move_window<is_write>()) {
						case move_window_result::failure:
							assert(false); // Both windows would be right in front of one another.
						case move_window_result::other_is_moving:
							fifo.write_currently_claiming--;
							while (fifo.write_wants_move) { }
							fifo.*fifo_window = new_window;
							break;
						case move_window_result::moved_by_me:
							fifo.write_currently_claiming--;
							fifo.*fifo_window = new_window;
							break;
						}
						
					}
				}
				return move_window_result::moved_by_me;
			} else {
				return move_window_result::other_is_moving;
			}
		}

		template <bool is_write>
		move_window_result move_window();

		template <>
		move_window_result move_window<true>() {
			return move_window_int<true, &relaxed_fifo::write_currently_claiming, &relaxed_fifo::write_wants_move,
				&relaxed_fifo::write_window, &relaxed_fifo::read_window, &handle::write_occ, &relaxed_fifo::get_write_window>();
		}

		template <>
		move_window_result move_window<false>() {
			return move_window_int<false, &relaxed_fifo::read_currently_claiming, &relaxed_fifo::read_wants_move,
				&relaxed_fifo::read_window, &relaxed_fifo::write_window, &handle::read_occ, &relaxed_fifo::get_read_window>();
		}

		// Postcondition (on true return): A valid block in the active window is claimed and correctly marked as occupied.
		template <bool is_write, std::atomic_uint8_t relaxed_fifo::*currently_claiming, std::atomic_bool relaxed_fifo::*wants_move,
			uint64_t handle::*handle_window, std::atomic_uint64_t relaxed_fifo::*fifo_window, state handle::*occupation, block* handle::*block,
			window& (relaxed_fifo::*get_window)()>
		bool claim_new_block_int() {
			claim_currently_claiming<currently_claiming, wants_move>();

			size_t free_bit = claim_free_bit<is_write>((fifo.*get_window)().occupied_set);
			// This can be a if instead of a while because we are guaranteed to claim a free bit in a new window (at least when writing).
			if (free_bit == std::numeric_limits<size_t>::max()) {
				switch (move_window<is_write>()) {
				case move_window_result::other_is_moving:
					// Someone else is already moving the window, we give up the block and wait until they're done.
					(fifo.*currently_claiming)--;
					// TODO: Get rid of recursion.
					return claim_new_block<is_write>();
				case move_window_result::failure:
					// TODO: Deal better with an almost full queue? Each push will try to move the window unsuccessfully.
					fifo.*wants_move = false;
					(fifo.*currently_claiming)--;
					return false;
				case move_window_result::moved_by_me:
					// Claim a new block, we're the only one able to at this point so we can be sure that we get one.
					free_bit = claim_free_bit<is_write>((fifo.*get_window)().occupied_set);
					fifo.*wants_move = false;
					if constexpr (is_write) {
						assert(free_bit != std::numeric_limits<size_t>::max());
					} else {
						// No writes happened in windows.
						// This theoretically shouldn't happen but handling this case shouldn't hurt.
						if (free_bit == std::numeric_limits<size_t>::max()) {
							(fifo.*currently_claiming)--;
							return false;
						}
					}
					break;
				}
			}
			this->*handle_window = fifo.*fifo_window;
			this->*occupation = make_state<is_write>(this->*handle_window);
			this->*block = &(fifo.*get_window)().blocks[free_bit];
			// TODO: Consider what could happen between claiming the block via the occupied set and setting the id here (if stealing was possible).
			auto& header = (this->*block)->header;
			// The order here is important, we first set the state to active, then decrease the claim counter, otherwise a new window switch might occur inbetween.
			header.state = make_active(this->*occupation);
			header.active_handle_id = id; // TODO: Maybe CAS to detect stealing or sth? TODO Writers don't ever use this!
			(fifo.*currently_claiming)--;
			return true;
		}

		template <bool is_write>
		bool claim_new_block() {
			return claim_new_block_int<is_write, is_write ? &relaxed_fifo::write_currently_claiming : &relaxed_fifo::read_currently_claiming,
				is_write ? &relaxed_fifo::write_wants_move : &relaxed_fifo::read_wants_move,
				is_write ? &handle::write_window : &handle::read_window,
				is_write ? &relaxed_fifo::write_window : &relaxed_fifo::read_window,
				is_write ? &handle::write_occ : &handle::read_occ,
				is_write ? &handle::write_block : &handle::read_block,
				is_write ? &relaxed_fifo::get_write_window : &relaxed_fifo::get_read_window>();
		}

	public:
		bool push(T t) {
			header* header = &write_block->header;
			auto expected = write_occ;
			if (!header->state.compare_exchange_strong(expected, make_active(expected))
				|| fifo.write_wants_move
				|| header->curr_index >= CELLS_PER_BLOCK) {
				// Something happened, someone wants to move the window or our block is full, we get a new block!
				if (expected == write_occ) {
					// We only reset the header state if the CAS succeeded (we managed to claim the block).
					// TODO: Preferable to split the condition again?
					header->state = write_occ;
				}
				if (!claim_new_block<true>()) {
					return false;
				}
				header = &write_block->header;
			}

			write_block->cells[header->curr_index++] = std::move(t);
			
			header->state = write_occ;
			return true;
		}

		std::optional<T> pop() {
			header* header = &read_block->header;
			auto expected = read_occ;
			if (!header->state.compare_exchange_strong(expected, make_active(expected))
				|| fifo.read_wants_move
				|| header->curr_index == 0) {
				// Something happened, someone wants to move the window or our block is full, we get a new block!
				if (expected == read_occ) {
					// We only reset the header state if the CAS succeeded (we managed to claim the block).
					// TODO: Preferable to split the method again?
					header->state = read_occ;
				}
				if (!claim_new_block<false>()) {
					return std::nullopt;
				}
				header = &read_block->header;
			}

			auto&& ret = std::move(read_block->cells[--header->curr_index]);
			
			header->state = read_occ;
			return ret;
		}
	};

	handle get_handle() { return handle(*this); }
};
//static_assert(fifo<relaxed_fifo>);

#endif // RELAXED_FIFO_H_INCLUDED
