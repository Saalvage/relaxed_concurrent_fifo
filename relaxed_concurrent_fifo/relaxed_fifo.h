#ifndef RELAXED_FIFO_H_INCLUDED
#define RELAXED_FIFO_H_INCLUDED

#include <array>
#include <memory>
#include <atomic>
#include <random>
#include <new>

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
	static constexpr size_t make_po2(size_t size) {
		size_t ret = 1;
		while (size > ret) {
			ret *= 2;
		}
		return ret;
	}

	// PO2 for modulo performance and at least as big as the bitset type.
	static constexpr size_t BLOCKS_PER_WINDOW = std::max(sizeof(BITSET_TYPE) * 8, make_po2(BLOCKS_PER_WINDOW_RAW));

	const size_t window_count;
	const size_t window_count_mod_mask;

	size_t size() const {
		return window_count * BLOCKS_PER_WINDOW * CELLS_PER_BLOCK;
	}

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct alignas(8) header_t {
		// 16 bits epoch, 16 bits read begin index, 16 bits read end index, 16 bits write index
		std::atomic_uint64_t epoch_and_indices;
	};
	static_assert(sizeof(header_t) == 8);

	struct block_t {
		header_t header;
		std::array<std::atomic<T>, CELLS_PER_BLOCK> cells;
	};
	static_assert(sizeof(block_t) == CELLS_PER_BLOCK * sizeof(T) + sizeof(header_t));

	struct window_t {
		alignas(128) atomic_bitset<BLOCKS_PER_WINDOW, BITSET_TYPE> filled_set;
		alignas(128) block_t blocks[BLOCKS_PER_WINDOW];
	};

	std::unique_ptr<window_t[]> buffer;

	window_t& get_window(size_t index) const {
		return buffer[index & window_count_mod_mask];
	}

	alignas(128) std::atomic_uint64_t read_window;
	alignas(128) std::atomic_uint64_t write_window;

public:
	relaxed_fifo(size_t size) : window_count(std::max<size_t>(4, make_po2(size / BLOCKS_PER_WINDOW / CELLS_PER_BLOCK))), window_count_mod_mask(window_count - 1) {
		buffer = std::make_unique<window_t[]>(window_count);
		read_window = window_count;
		write_window = window_count + 1;
		for (size_t i = 1; i < window_count; i++) {
			window_t& window = buffer[i];
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				header_t& header = window.blocks[j].header;
				header.epoch_and_indices = (window_count + i) << 48;
			}
		}
		window_t& window = buffer[0];
		for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
			header_t& header = window.blocks[j].header;
			header.epoch_and_indices = (window_count * 2) << 48;
		}
	}

	void debug_print() {
		std::cout << "Printing relaxed_fifo:\n"
			<< "Read: " << read_window << "; Write: " << write_window << '\n';
		for (size_t i = 0; i < window_count; i++) {
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				uint64_t val = buffer[i].blocks[j].header.epoch_and_indices;
				std::cout << (val >> 48) << " " << ((val >> 32) & 0xffff) << " " << ((val >> 16) & 0xffff) << " " << (val & 0xffff) << " | ";
			}
			std::cout << "\n======================\n";
		}
	}

	class handle {
	private:
		relaxed_fifo& fifo;

		// Doing it like this allows the push code to grab a new block instead of requiring special cases for first-time initialization.
		// An already active block will always trigger a check.
		static inline thread_local block_t dummy_block{header_t{0xffffull << 48}, {}};
	
		block_t* read_block = &dummy_block;
		block_t* write_block = &dummy_block;

		uint64_t write_window = 0;
		uint64_t read_window = 0;

		handle(relaxed_fifo& fifo) : fifo(fifo) { }

		friend relaxed_fifo;

		bool claim_new_block_write() {
			size_t free_bit;
			uint64_t window_index;
			window_t* window;
			do {
				window_index = fifo.write_window.load(std::memory_order_relaxed);
				window = &fifo.get_window(window_index);
				free_bit = window->filled_set.template claim_bit<false, true>(std::memory_order_relaxed);
				if (free_bit == std::numeric_limits<size_t>::max()) {
					// No more free bits, we move.
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
			write_block = &window->blocks[free_bit];
			return true;
		}

		bool claim_new_block_read() {
			size_t free_bit;
			uint64_t window_index;
			window_t* window;
			do {
				window_index = fifo.read_window.load(std::memory_order_relaxed);
				window = &fifo.get_window(window_index);
				free_bit = window->filled_set.template claim_bit<true, false>(std::memory_order_relaxed);
				if (free_bit == std::numeric_limits<size_t>::max()) {
					uint64_t write_window = fifo.write_window.load(std::memory_order_relaxed);
					if (write_window == window_index + 1) {
						if (!fifo.get_window(write_window).filled_set.any(std::memory_order_relaxed)) {
							return false;
						}
						// Before we force-move the write window, there might be unclaimed blocks in the current one.
						// We need to make sure we clean those up BEFORE we move the write window in order to prevent
						// the read window from being moved before all blocks have either been claimed or invalidated.
						window_t& new_window = fifo.get_window(write_window);
						uint64_t next_epoch = (write_window + fifo.window_count) << 48;
						for (size_t i = 0; i < BLOCKS_PER_WINDOW; i++) {
							// We can't rely on the bitset here because it might be experiencing a spurious claim.

							uint64_t ei = write_window << 48; // All empty with current epoch.
							new_window.blocks[i].header.epoch_and_indices.compare_exchange_strong(ei, next_epoch, std::memory_order_relaxed);
						}
						fifo.write_window.compare_exchange_strong(write_window, write_window + 1, std::memory_order_relaxed);
#if LOG_WINDOW_MOVE
						std::cout << "Write force move " << (write_window + 1) << std::endl;
#endif // LOG_WINDOW_MOVE
					}

					fifo.read_window.compare_exchange_strong(window_index, window_index + 1, std::memory_order_relaxed);
#if LOG_WINDOW_MOVE
					std::cout << "Read move " << (window_index + 1) << std::endl;
#endif // LOG_WINDOW_MOVE
				} else {
					break;
				}
			} while (true);

			read_window = window_index;
			read_block = &window->blocks[free_bit];
			return true;
		}

	public:
		bool push(T t) {
			assert(t != 0);

			header_t* header = &write_block->header;
			uint64_t ei = header->epoch_and_indices.load(std::memory_order_relaxed);
			uint16_t index;
			bool claimed = false;
			while (ei >> 48 != (write_window & 0xffff) || (index = (ei & 0xffff)) >= CELLS_PER_BLOCK || !header->epoch_and_indices.compare_exchange_weak(ei, ei + 1, std::memory_order_relaxed)) {
				// We need this in case of a spurious claim where we claim a bit, but can't place an element inside,
				// because the write window was already forced-moved.
				if (claimed && (index = (ei & 0xffff)) == 0) {
					// We're abandoning an empty block!
					window_t& window = fifo.get_window(write_window);
					auto diff = write_block - window.blocks;
					window.filled_set.reset(diff, std::memory_order_relaxed);
				}
				if (!claim_new_block_write()) {
					return false;
				}
				claimed = true;
				header = &write_block->header;
				ei = header->epoch_and_indices.load(std::memory_order_relaxed);
			}

			write_block->cells[index].store(std::move(t), std::memory_order_relaxed);

			return true;
		}

		std::optional<T> pop() {
			header_t* header = &read_block->header;
			uint64_t ei = header->epoch_and_indices.load(std::memory_order_relaxed);
			uint16_t index;

			while (ei >> 48 != (read_window & 0xffff) || (index = ((ei >> 32) & 0xffff)) >= (ei & 0xffff)
				|| !header->epoch_and_indices.compare_exchange_weak(ei, ei + (1ull << 32), std::memory_order_relaxed)) {
				if (!claim_new_block_read()) {
					return std::nullopt;
				}
				header = &read_block->header;
				ei = header->epoch_and_indices.load(std::memory_order_relaxed);
			}

			T ret;
			while ((ret = std::move(read_block->cells[index].load(std::memory_order_relaxed))) == 0) { }
			read_block->cells[index].store(0, std::memory_order_relaxed);

			uint16_t finished_index = static_cast<uint16_t>(header->epoch_and_indices.fetch_add(1 << 16, std::memory_order_relaxed) >> 16) + 1;
			if (finished_index >= (ei & 0xffff)) {
				// Apply local read index update.
				ei = (ei & (0xffffull << 48)) | (static_cast<uint64_t>(finished_index) << 32) | (static_cast<uint64_t>(finished_index) << 16) | finished_index;
				// Before we mark this block as empty, we make it unavailable for other readers and writers of this epoch.
				if (header->epoch_and_indices.compare_exchange_strong(ei, (read_window + fifo.window_count) << 48, std::memory_order_relaxed)) {
					window_t& window = fifo.get_window(read_window);
					auto diff = read_block - window.blocks;
					window.filled_set.reset(diff, std::memory_order_relaxed);

					// We don't need to invalidate the read window because it has been changed already.	
				} else {
					assert(finished_index < CELLS_PER_BLOCK);
				}
			}

			return ret;
		}
	};

	handle get_handle() { return handle(*this); }
};
static_assert(fifo<relaxed_fifo<uint64_t>, uint64_t>);

#endif // RELAXED_FIFO_H_INCLUDED
