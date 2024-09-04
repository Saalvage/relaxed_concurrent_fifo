#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
#include <random>
#include <new>
#include <bitset>

#include "atomic_bitset.h"

#include <iostream>

constexpr size_t CACHE_SIZE =
#if __cpp_lib_hardware_interference_size >= 201603
	std::hardware_constructive_interference_size;
#else
	64;
#endif // __cpp_lib_hardware_interference_size

template <typename T, size_t BLOCKS_PER_WINDOW = 8, size_t CELLS_PER_BLOCK = CACHE_SIZE / sizeof(T) - 1>
class relaxed_fifo {
private:
	// TODO: Optimize modulo.
	const size_t window_count;

	size_t size() const {
		return window_count * BLOCKS_PER_WINDOW * CELLS_PER_BLOCK;
	}

	using handle_id = uint8_t;

	// 8 bits handle, 6 bits epoch, 1 bit write/read, 1 bit inactive/active
	using state_t = uint16_t;

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	static constexpr state_t make_active(state_t marker) { return marker | 1; }
	static constexpr state_t make_inactive(state_t marker) { return marker & ~1; }
	static constexpr bool is_active(state_t marker) { return marker & 1; }

	static constexpr state_t make_write(state_t marker) { return marker & ~0b10; }
	static constexpr state_t make_read(state_t marker) { return marker | 0b10; }
	static constexpr bool is_read(state_t marker) { return marker & 0b10; }
	static constexpr bool is_write(state_t marker) { return !is_read(marker); }

	static constexpr state_t set_id(state_t marker, handle_id id) { return (marker ^ 0xff00) | (id << 8); }

	static constexpr size_t EPOCH_MASK = 0b111111;

	static constexpr size_t get_epoch(state_t marker) { return (marker >> 2) & EPOCH_MASK; }
	size_t window_to_epoch(size_t window) const { return window / window_count; }

	template <bool is_write>
	state_t make_state(handle_id id, size_t window) const {
		return (static_cast<state_t>(id) << 8) | ((window_to_epoch(window) & EPOCH_MASK) << 2) | (!is_write << 1);
	}

	struct alignas(8) header_t {
		std::atomic<state_t> state;
		// read_index < write_index XOR read_index == write_index == 0
		std::atomic_uint16_t write_index;
		std::atomic_uint16_t read_index;
	};
	static_assert(sizeof(header_t) == 8);

	struct block_t {
		header_t header;
		std::array<std::atomic<T>, CELLS_PER_BLOCK> cells;
	};
	static_assert(sizeof(block_t) == CELLS_PER_BLOCK * sizeof(T) + sizeof(header_t));

	struct window_t {
		// While writing these two sets are consistent.
		// While reading a block can be occupied but it can still be filled. (Filled is a superset of occupied.)
		atomic_bitset<BLOCKS_PER_WINDOW> occupied_set;
		atomic_bitset<BLOCKS_PER_WINDOW> filled_set;
		block_t blocks[BLOCKS_PER_WINDOW];
	};

	std::unique_ptr<window_t[]> buffer;
	
	std::atomic_uint64_t read_window = 0;
	std::atomic_uint64_t write_window = 1;

	window_t& get_read_window() { return buffer[read_window % window_count]; }
	window_t& get_write_window() { return buffer[write_window % window_count]; }

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
	relaxed_fifo(size_t size) : window_count(size / BLOCKS_PER_WINDOW / CELLS_PER_BLOCK) {
		buffer = std::make_unique<window_t[]>(window_count);
		if (window_count <= 2) {
			throw std::runtime_error("FIFO parameters would result in less than 3 windows!");
		}
	}

	void debug_print() {
		std::cout << "Printing relaxed_fifo:\n"
			<< "Read: " << read_window << "; Write: " << write_window << '\n';
		for (size_t i = 0; i < window_count; i++) {
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				header_t& header = buffer[i].blocks[j].header;
				std::cout << std::bitset<16>(header.state) << " " << header.read_index << " " << header.write_index << " | ";
			}
			std::cout << "\n======================\n";
		}
	}

	class handle {
	private:
		relaxed_fifo& fifo;

		handle_id id;

		// Doing it like this allows the push code to grab a new block instead of requiring special cases for first-time initialization.
		// An already active block will always trigger a check.
		static inline block_t dummy_block{header_t{0, make_active(0), 0}, {}};
	
		block_t* read_block = &dummy_block;
		block_t* write_block = &dummy_block;

		uint64_t write_window;
		uint64_t read_window;

		// These represent an inactive occupation of the current epoch.
		state_t write_occ;
		state_t read_occ;

		handle(relaxed_fifo& fifo) : fifo(fifo), id(fifo.get_handle_id()) {
			write_window = fifo.write_window;
			read_window = fifo.read_window;
			write_occ = fifo.make_state<true>(id, write_window);
			read_occ = fifo.make_state<false>(id, read_window);
		}

		friend relaxed_fifo;

		std::random_device dev;
		std::mt19937 rng{dev()};
		// TODO: Check template parameter here.
		std::uniform_int_distribution<size_t> dist{0, BLOCKS_PER_WINDOW - 1};

		// set: true if we want bits that are 0
		// instantly_write: true if we want to instantly set the given bit
		template <bool set, bool instantly_write = true>
		size_t claim_free_bit(atomic_bitset<BLOCKS_PER_WINDOW>& bits) {
			auto off = dist(rng);
			for (size_t i = 0; i < bits.size(); i++) {
				auto idx = (i + off) % bits.size();
				if (instantly_write
						? set ? bits.set(idx) : bits.reset(idx)
						: set ? !bits[idx] : bits[idx]) {
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
		move_window_result move_window_write() {
			bool expected = false;
			if (fifo.write_wants_move.compare_exchange_strong(expected, true)) {
				// We're now moving the window!
				// Wait until no more new blocks are being claimed.
				while (fifo.write_currently_claiming > 1) {}
				// The new state has the new read/write state and epoch.
				auto write = fifo.make_state<false>(0, fifo.write_window);
				for (auto& block : fifo.get_write_window().blocks) {
					auto expected = make_inactive(block.header.state);
					//assert(is_write(expected)); // TODO: Commented out?
					// Busy wait until operation concludes.
					while (!block.header.state.compare_exchange_weak(expected, write)) {
						// If something besides activity or id is incorrect here we're in trouble!
						//assert(is_write(expected));
						expected = make_inactive(expected);
					}
				}
				auto new_window = fifo.write_window + 1;
				if (new_window - fifo.read_window != fifo.window_count) { // TOOD: Make sure this is actually equivalent to the variant with modulo.
					fifo.write_window = new_window;
				} else {
					// Whelp, we're out of luck here! Can't force forward the read window.
					return move_window_result::failure;
				}
				return move_window_result::moved_by_me;
			} else {
				return move_window_result::other_is_moving;
			}
		}

		// Precondition: read_currently_claiming claimed.
		// Postcondition: read_wants_move (unless other_is_moving), read_currently_claiming still claimed.
		move_window_result move_window_read() {
			bool expected = false;
			if (fifo.read_wants_move.compare_exchange_strong(expected, true)) {
				// We're now moving the window!
				// Wait until no more new blocks are being claimed.
				while (fifo.read_currently_claiming > 1) {}
				// The new state has the new read/write state and epoch.
				auto write = fifo.make_state<true>(0, fifo.read_window + fifo.window_count); // Advance by one epoch.
				for (auto& block : fifo.get_read_window().blocks) {
					auto expected = make_inactive(block.header.state);
					// Busy wait until operation concludes.
					while (!block.header.state.compare_exchange_weak(expected, write)) {
						expected = make_inactive(expected);
					}
				}
				auto new_window = fifo.read_window + 1;
				if ((fifo.write_window - new_window) % fifo.window_count != 0) {
					fifo.read_window = new_window;
				} else {
					// TODO: Have this check happen earlier?
					// Are there any pushes to the current write window?
					if (!fifo.get_write_window().occupied_set.any()) {
						return move_window_result::failure;
					}

					// Force forward the write window.
					claim_currently_claiming<&relaxed_fifo::write_currently_claiming, &relaxed_fifo::write_wants_move>();
					switch (move_window_write()) {
					case move_window_result::failure:
						// This should basically never happen as both windows should be right in front of one another, if it does we just try again for now.
						// TODO: This branch might only be triggered by a bug?
						fifo.write_wants_move = false;
						fifo.write_currently_claiming--;
						return move_window_read();
					case move_window_result::other_is_moving:
						fifo.write_currently_claiming--;
						while (fifo.write_wants_move) { }
						fifo.read_window = new_window;
						break;
					case move_window_result::moved_by_me:
						fifo.read_window = new_window;
						fifo.write_wants_move = false;
						fifo.write_currently_claiming--;
						break;
					}
				}
				return move_window_result::moved_by_me;
			} else {
				return move_window_result::other_is_moving;
			}
		}

		// Postcondition (on true return): A valid block in the active window is claimed and correctly marked as occupied.
		bool claim_new_block_write() {
			claim_currently_claiming<&relaxed_fifo::write_currently_claiming, &relaxed_fifo::write_wants_move>();

			size_t free_bit = claim_free_bit<true>(fifo.get_write_window().occupied_set);
			// This can be a if instead of a while because we are guaranteed to claim a free bit in a new window (at least when writing).
			if (free_bit == std::numeric_limits<size_t>::max()) {
				switch (move_window_write()) {
				case move_window_result::other_is_moving:
					// Someone else is already moving the window, we give up the block and wait until they're done.
					fifo.write_currently_claiming--;
					// TODO: Get rid of recursion.
					return claim_new_block_write();
				case move_window_result::failure:
					// TODO: Deal better with an almost full queue? Each push will try to move the window unsuccessfully.
					fifo.write_wants_move = false;
					fifo.write_currently_claiming--;
					return false;
				case move_window_result::moved_by_me:
					// Claim a new block, we're the only one able to at this point so we can be sure that we get one.
					free_bit = claim_free_bit<true>(fifo.get_write_window().occupied_set);
					fifo.write_wants_move = false;
					assert(free_bit != std::numeric_limits<size_t>::max());
					break;
				}
			}
			// Each newly claimed block will have at least one element.
			fifo.get_write_window().filled_set.set(free_bit);
			write_window = fifo.write_window;
			write_occ = fifo.make_state<true>(id, write_window);
			write_block = &fifo.get_write_window().blocks[free_bit];
			// TODO: Consider what could happen between claiming the block via the occupied set and setting the id here (if stealing was possible).
			auto& header = write_block->header;
			// The order here is important, we first set the state to active, then decrease the claim counter, otherwise a new window switch might occur inbetween.
			header.state = fifo.make_active(write_occ);
			fifo.write_currently_claiming--;
			return true;
		}

		// TODO: This method needs to be cleaned up!
		// Postcondition (on true return): A valid block in the active window is claimed and correctly marked as occupied.
		bool claim_new_block_read() {
			claim_currently_claiming<&relaxed_fifo::read_currently_claiming, &relaxed_fifo::read_wants_move>();

			size_t free_bit;
			do {
				free_bit = claim_free_bit<false>(fifo.get_read_window().occupied_set);
				if (free_bit == std::numeric_limits<size_t>::max()) {
					break;
				}
				auto& my_header = fifo.get_read_window().blocks[free_bit].header;
				state_t state = my_header.state;
				if (!is_active(state) && my_header.state.compare_exchange_strong(state, fifo.make_active(fifo.make_state<false>(id, fifo.read_window)))) {
					if (my_header.read_index != my_header.write_index) {
						break;
					} else {
						// It was emptied in the meantime!
						my_header.state = fifo.make_inactive(my_header.state); // TODO: Is this correct??
					}
				}
			} while (true);

			// This can be a if instead of a while because we are guaranteed to claim a free bit in a new window (at least when writing).
			if (free_bit == std::numeric_limits<size_t>::max()) {
				// We need to check if there are any blocks with elements still present!
				auto& bits = fifo.get_read_window().filled_set;
				auto off = dist(rng);
				bool any_left;
				do {
					any_left = false;
					for (size_t i = 0; i < bits.size(); i++) {
						auto idx = (i + off) % bits.size();
						if (bits[idx]) {
							header_t& header = fifo.get_read_window().blocks[idx].header;
							state_t state = header.state;
							if (!is_active(state) && header.state.compare_exchange_strong(state, fifo.make_active(fifo.make_state<false>(id, fifo.read_window)))) {
								if (header.write_index != 0) {
									// We managed to steal a block!
									any_left = false;
									free_bit = idx;
									break;
								} else {
									// It was emptied in the meantime!
									header.state = fifo.make_inactive(header.state); // TODO: Is this correct??
								} 
							} else {
								// The block is active or got stolen by someone else, we'll look for another one and eventually return if we fail!
								any_left = true;
							}
						}
					}
				} while (any_left);

				// No more blocks left, we can finally move!
				if (free_bit == std::numeric_limits<size_t>::max()) {
					switch (move_window_read()) {
					case move_window_result::other_is_moving:
						// Someone else is already moving the window, we give up the block and wait until they're done.
						fifo.read_currently_claiming--;
						// TODO: Get rid of recursion.
						return claim_new_block_read();
					case move_window_result::failure:
						// TODO: Deal better with an almost full queue? Each push will try to move the window unsuccessfully.
						fifo.read_wants_move = false;
						fifo.read_currently_claiming--;
						return false;
					case move_window_result::moved_by_me:
						// Claim a new block, we're the only one able to at this point so we can be sure that we get one.
						free_bit = claim_free_bit<false>(fifo.get_read_window().occupied_set);
						auto& header = fifo.get_read_window().blocks[free_bit].header; // We need to claim immediately.
						header.state = fifo.make_active(fifo.make_state<false>(id, fifo.read_window));
						fifo.read_wants_move = false;
						// No write happened in this window.
						// This theoretically shouldn't happen but handling this case shouldn't hurt.
						if (free_bit == std::numeric_limits<size_t>::max()) {
							fifo.read_currently_claiming--;
							return false;
						}
						break;
					}
				}
			}
			read_window = fifo.read_window;
			read_occ = fifo.make_state<false>(id, read_window);
			read_block = &fifo.get_read_window().blocks[free_bit];
			// TODO: Consider what could happen between claiming the block via the occupied set and setting the id here (if stealing was possible).
			auto& header = read_block->header;
			assert(header.write_index != 0);
			// The order here is important, we first set the state to active, then decrease the claim counter, otherwise a new window switch might occur inbetween.
			header.state = fifo.make_active(read_occ);
			fifo.read_currently_claiming--;
			return true;
		}

		bool claim_new_block_write_new() {
			// We will be trying to get a block from this window.
			uint64_t window_index = fifo.write_window;
			window_t& window = fifo.buffer[window_index % fifo.window_count];

			auto free_bit = claim_free_bit<true>(window.occupied_set);
			if (free_bit == std::numeric_limits<size_t>::max()) {
				if (window_index + 1 - fifo.read_window == fifo.window_count) {
					// TODO: Maybe consider if the write window already moved?
					return false;
				}
				fifo.write_window.compare_exchange_strong(window_index, window_index + 1);
				return claim_new_block_write_new();
			}
			// Possibilities:
			// 1. We're in the current write block or in front of the current read block, all good.
			// 2. We're in the current read block, since the occupied bit was unset this means:
			// (a) This block is currently being worked on (filled bit still set)
			// (b) This block was not filled in the previous write block (filled bit unset)
			// (c) This block has already been emptied fully (filled bit unset as well)
			// 3. We're behind the read window.
			// To catch all of these cases we're checking that we're still in the "good" area:
			if (fifo.read_window >= window_index) {
				// Pretend like we weren't here.
				// If we're in an active write window where we don't belong this might result in this block being ignored, but that is ok.
				// If we're in the active read window... they'll just have to deal with it (problem for future me).
				window.occupied_set.reset(free_bit);
				return claim_new_block_write_new();
			}

			// We have marked the block as having been occupied by a writer.
			// The occupied bit is set, while the filled bit is not, this implies a new write block claim in progress.
			write_block = &window.blocks[free_bit];
			auto& header = write_block->header;
			state_t state = header.state;
			(void)state;
			assert(!is_active(state));
			//assert(is_write(state)); // TODO ???
			write_occ = fifo.make_state<true>(id, window_index);
			write_window = window_index;
			header.state = make_active(write_occ);
			header.state = write_occ = make_active(fifo.make_state<true>(id, window_index));
			window.filled_set.set(free_bit);
			return true;
		}

	public:
		bool push(T t) {
			header_t* header = &write_block->header;
			auto expected = write_occ;
			if (!header->state.compare_exchange_strong(expected, make_active(expected))
				|| fifo.write_wants_move
				|| header->write_index >= CELLS_PER_BLOCK) {
				// Something happened, someone wants to move the window or our block is full, we get a new block!
				if (expected == write_occ) {
					// We only reset the header state if the CAS succeeded (we managed to claim the block).
					// TODO: Preferable to split the condition again?
					header->state = write_occ;
				}
				if (!claim_new_block_write_new()) {
					return false;
				}
				header = &write_block->header;
			}

			write_block->cells[header->write_index++] = std::move(t);
			
			header->state = write_occ;
			return true;
		}

		std::optional<T> pop() {
			header_t* header = &read_block->header;
			auto expected = read_occ;
			if (!header->state.compare_exchange_strong(expected, make_active(expected))
				|| fifo.read_wants_move) {
				// Something happened, someone wants to move the window or our block is full, we get a new block!
				if (expected == read_occ) { // TODO: I don't think this is allowed!!!
					// We only reset the header state if the CAS succeeded (we managed to claim the block).
					// TODO: Preferable to split the method again?
					header->state = read_occ;
				}
				if (!claim_new_block_read()) {
					return std::nullopt;
				}
				header = &read_block->header;
			}

			T ret = std::move(read_block->cells[header->read_index++].load());

			// We're resetting the filled bit asap so we can avoid a deeper-going check for this block while claiming.
			if (header->read_index == header->write_index) {
				window_t& window = fifo.buffer[read_window % fifo.window_count];
				auto diff = read_block - window.blocks;
				window.filled_set.reset(diff);
				read_block = &dummy_block;
				// Reset block indices.
				header->read_index = header->write_index = 0;
			}
			
			header->state = read_occ;
			return ret;
		}
	};

	handle get_handle() { return handle(*this); }
};
static_assert(fifo<relaxed_fifo<uint64_t>, uint64_t>);

#endif // RELAXED_FIFO_H_INCLUDED
