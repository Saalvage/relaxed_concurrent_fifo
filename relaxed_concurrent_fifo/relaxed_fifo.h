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

#define LOG_WINDOW_MOVE 0

constexpr size_t CACHE_SIZE =
#if __cpp_lib_hardware_interference_size >= 201603
	std::hardware_constructive_interference_size;
#else
	64;
#endif // __cpp_lib_hardware_interference_size

template <typename T, size_t BLOCKS_PER_WINDOW_RAW = 8, size_t CELLS_PER_BLOCK = CACHE_SIZE / sizeof(T) - 1, typename BITSET_TYPE = uint8_t>
class relaxed_fifo {
private:
	static constexpr size_t make_multiple(size_t val, size_t multiple_of) {
		size_t ret = multiple_of;
		while (ret < val) {
			ret += multiple_of;
		}
		return ret;
	}

	static constexpr size_t BLOCKS_PER_WINDOW = make_multiple(BLOCKS_PER_WINDOW_RAW, sizeof(BITSET_TYPE) * 8);

	const size_t window_count;
	const size_t window_count_mod_mask;

	size_t size() const {
		return window_count * BLOCKS_PER_WINDOW * CELLS_PER_BLOCK;
	}

	using handle_id = uint8_t;

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct alignas(8) header_t {
		// 15 bits epoch, 1 bit filled, 16 bits read begin index, 16 bits read end index, 16 bits write index
		std::atomic_uint64_t state;
	};
	static_assert(sizeof(header_t) == 8);

	static constexpr uint64_t STATE_FILLED_MASK = 1ull << 48;

	struct block_t {
		header_t header;
		std::array<std::atomic<T>, CELLS_PER_BLOCK> cells;
	};
	static_assert(sizeof(block_t) == CELLS_PER_BLOCK * sizeof(T) + sizeof(header_t));

	struct window_t {
		block_t blocks[BLOCKS_PER_WINDOW];

		static inline thread_local std::random_device dev;
		static inline thread_local std::minstd_rand rng{ dev() };
		static inline thread_local std::uniform_int_distribution<size_t> dist{ 0, BLOCKS_PER_WINDOW - 1 };

		template <bool FILLED>
		block_t* find_block(uint16_t epoch) {
			size_t offset = dist(rng);
			for (size_t i = 0; i < BLOCKS_PER_WINDOW; i++) {
				block_t& block = blocks[(i + offset) % BLOCKS_PER_WINDOW];
				header_t& header = block.header;
				uint64_t state = header.state.load(std::memory_order_relaxed);
				// All cases in which the CAS could fail involve a claim by another handle.
				if (static_cast<bool>(state & STATE_FILLED_MASK) == FILLED && (state >> 49) == epoch && (FILLED || header.state.compare_exchange_strong(state, state | STATE_FILLED_MASK | 1, std::memory_order_relaxed))) {
					assert(!FILLED ^ ((state & 0xffff) != 0));
					return &block;
				}
			}
			return nullptr;
		}
	};

	std::unique_ptr<window_t[]> buffer;

	window_t& get_window(size_t index) const {
		return buffer[index & window_count_mod_mask];
	}

	std::atomic_uint64_t read_window;
	std::atomic_uint64_t write_window;

	std::atomic<handle_id> latest_handle_id = 0;
	handle_id get_handle_id() {
		return latest_handle_id++;
	}

	static constexpr size_t make_po2(size_t size) {
		size_t ret = 1;
		while (size > ret) {
			ret *= 2;
		}
		return ret;
	}

public:
	relaxed_fifo(size_t size) : window_count(make_po2(size / BLOCKS_PER_WINDOW / CELLS_PER_BLOCK)), window_count_mod_mask(window_count - 1) {
		buffer = std::make_unique<window_t[]>(window_count);
		if (window_count <= 2) {
			throw std::runtime_error("FIFO parameters would result in less than 3 windows!");
		}
		read_window = window_count;
		write_window = window_count + 1;
		for (size_t i = 1; i < window_count; i++) {
			window_t& window = buffer[i];
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				header_t& header = window.blocks[j].header;
				header.state = (window_count + i) << 49;
			}
		}
		window_t& window = buffer[0];
		for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
			header_t& header = window.blocks[j].header;
			header.state = (window_count * 2) << 49;
		}
	}

	void debug_print() {
		std::cout << "Printing relaxed_fifo:\n"
			<< "Read: " << read_window << "; Write: " << write_window << '\n';
		for (size_t i = 0; i < window_count; i++) {
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				uint64_t val = buffer[i].blocks[j].header.state;
				std::cout << (val >> 49) << " " << ((val >> 48) & 1) << " " << ((val >> 32) & 0xffff) << " " << ((val >> 16) & 0xffff) << " " << (val & 0xffff) << " | ";
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
		static inline block_t dummy_block{header_t{0xffffull << 48}, {}};
	
		block_t* read_block = &dummy_block;
		block_t* write_block = &dummy_block;

		uint64_t write_window = 0;
		uint64_t read_window = 0;

		handle(relaxed_fifo& fifo) : fifo(fifo), id(fifo.get_handle_id()) { }

		friend relaxed_fifo;

		bool claim_new_block_write() {
			block_t* found_block;
			uint64_t window_index;
			do {
				window_index = fifo.write_window.load(std::memory_order_relaxed);
				found_block = fifo.get_window(window_index).template find_block<false>(window_index & 0x7fff);
				if (found_block == nullptr) {
					// No more free blocks, we move.
					if (window_index + 1 - fifo.read_window.load(std::memory_order_relaxed) == fifo.window_count) {
						return false;
					}
					fifo.write_window.compare_exchange_strong(window_index, window_index + 1, std::memory_order_relaxed);
#if LOG_WINDOW_MOVE
					std::cout << "Write move " << (window_index + 1) << std::endl;
#endif // LOG_WINDOW_MOVE
				} else {
					break;
				}
			} while (true);

			write_window = window_index;
			write_block = found_block;
			return true;
		}

		bool claim_new_block_read() {
			block_t* found_block;
			uint64_t window_index;
			do {
				window_index = fifo.read_window.load(std::memory_order_relaxed);
				found_block = fifo.get_window(window_index).template find_block<true>(window_index & 0x7fff);
				if (found_block == nullptr) {
					bool found_any = true;
					uint64_t write_window = fifo.write_window.load(std::memory_order_relaxed);
					if (write_window == window_index + 1) {
						// We move the window, if it's empty or not.
						// This should improve the expected case where there are elements here at the cost of one extra CAS in the case that there aren't.
						// TODO: Is that true?
						if (fifo.write_window.compare_exchange_strong(write_window, write_window + 1)) {
							// We force-moved the write window, there might be unclaimed blocks in the old one.
							// Before we move the read window, let's invalidate all unclaimed write epochs in the blocks.
							// TODO: We should be able to do this AFTER the moving the read window, saving one other thread from potentially
							// having to move it in the meantime.
							// Edit: I don't think we can.
							window_t& new_window = fifo.get_window(write_window);
							found_any = false;
							for (size_t i = 0; i < BLOCKS_PER_WINDOW; i++) {
								block_t& block = new_window.blocks[i];

								if (block.header.state & STATE_FILLED_MASK) {
									found_any = true;
									continue;
								}

								uint64_t ei = write_window << 49; // All empty with current epoch.
								// If this fails it was written to, it'll just be read normally in that case.
								if (!block.header.state.compare_exchange_strong(ei, (write_window + fifo.window_count) << 49, std::memory_order_relaxed)) {
									found_any = true;
								}
							}
						}
#if LOG_WINDOW_MOVE
						std::cout << "Write force move " << (write_window + 1) << std::endl;
#endif // LOG_WINDOW_MOVE
					}

					fifo.read_window.compare_exchange_strong(window_index, window_index + 1, std::memory_order_relaxed);
#if LOG_WINDOW_MOVE
					std::cout << "Read move " << (window_index + 1) << std::endl;
#endif // LOG_WINDOW_MOVE

					if (!found_any) {
						return false;
					}
				} else {
					break;
				}
			} while (true);

			read_window = window_index;
			read_block = found_block;
			return true;
		}

	public:
		bool push(T t) {
			assert(t != 0);

			header_t* header = &write_block->header;
			uint64_t ei = header->state.load(std::memory_order_relaxed);
			uint16_t index;
			if (ei >> 49 != (write_window & 0x7fff) || (index = (ei & 0xffff)) >= CELLS_PER_BLOCK || !header->state.compare_exchange_weak(ei, ei + 1, std::memory_order_relaxed)) {
				if (!claim_new_block_write()) {
					return false;
				}
				index = 0;
			}

			write_block->cells[index].store(std::move(t), std::memory_order_relaxed);

			return true;
		}

		std::optional<T> pop() {
			header_t* header = &read_block->header;
			uint64_t ei = header->state.load(std::memory_order_relaxed);
			uint16_t index;

			while (ei >> 49 != (read_window & 0x7fff) || (index = ((ei >> 32) & 0xffff)) >= (ei & 0xffff)
				|| !header->state.compare_exchange_weak(ei, ei + (1ull << 32), std::memory_order_relaxed)) {
				if (!claim_new_block_read()) {
					return std::nullopt;
				}
				header = &read_block->header;
				ei = header->state.load(std::memory_order_relaxed);
			}

			T ret;
			while ((ret = std::move(read_block->cells[index].load(std::memory_order_relaxed))) == 0) { }
			read_block->cells[index].store(0, std::memory_order_relaxed);

			uint16_t finished_index = static_cast<uint16_t>(header->state.fetch_add(1 << 16, std::memory_order_relaxed) >> 16) + 1;
			if (finished_index >= (ei & 0xffff)) {
				// Apply local read index update.
				ei = (ei & (0xffffull << 48)) | (static_cast<uint64_t>(finished_index) << 32) | (static_cast<uint64_t>(finished_index) << 16) | finished_index;
				// Mark block as empty.
				header->state.compare_exchange_strong(ei, (read_window + fifo.window_count) << 49, std::memory_order_relaxed);
				// TODO: Can this fail??
			}

			return ret;
		}
	};

	handle get_handle() { return handle(*this); }
};
static_assert(fifo<relaxed_fifo<uint64_t>, uint64_t>);

#endif // RELAXED_FIFO_H_INCLUDED
