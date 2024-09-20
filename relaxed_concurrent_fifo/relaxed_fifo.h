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

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct alignas(8) header_t {
		// 16 bits epoch, 16 bits index
		std::atomic_uint32_t write_index_and_epoch;
		std::atomic_uint32_t read_started_index_and_epoch;
		std::atomic_uint16_t read_finished_index;
	};
	// TODO
	//static_assert(sizeof(header_t) == 8);

	struct block_t {
		header_t header;
		std::array<std::atomic<T>, CELLS_PER_BLOCK> cells;
	};
	static_assert(sizeof(block_t) == CELLS_PER_BLOCK * sizeof(T) + sizeof(header_t));

	struct window_t {
		// While writing these two sets are consistent.
		// While reading a block can be occupied but it can still be filled. (Filled is a superset of occupied.)
		atomic_bitset<BLOCKS_PER_WINDOW> filled_set;
		block_t blocks[BLOCKS_PER_WINDOW];
	};

	std::unique_ptr<window_t[]> buffer;
	
	std::atomic_uint64_t read_window;
	std::atomic_uint64_t write_window;

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
		read_window = window_count;
		write_window = window_count + 1;
		for (int i = 0; i < window_count; i++) {
			window_t& window = buffer[i];
			for (int j = 0; j < BLOCKS_PER_WINDOW; j++) {
				header_t& header = window.blocks[j].header;
				header.read_started_index_and_epoch = static_cast<uint32_t>(window_count + i) << 16;
				header.write_index_and_epoch = header.read_started_index_and_epoch + 1;
			}
		}
	}

	void debug_print() {
		std::cout << "Printing relaxed_fifo:\n"
			<< "Read: " << read_window << "; Write: " << write_window << '\n';
		for (size_t i = 0; i < window_count; i++) {
			for (size_t j = 0; j < BLOCKS_PER_WINDOW; j++) {
				header_t& header = buffer[i].blocks[j].header;
				std::cout << std::bitset<16>(header.write_index_and_epoch) << " | ";
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
		static inline block_t dummy_block{header_t{1 << 16, 0, 0}, {}};
	
		block_t* read_block = &dummy_block;
		block_t* write_block = &dummy_block;

		uint64_t write_window = 0;
		uint64_t read_window = 0;

		handle(relaxed_fifo& fifo) : fifo(fifo), id(fifo.get_handle_id()) { }

		friend relaxed_fifo;

		std::random_device dev;
		std::minstd_rand rng{dev()};
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

		bool claim_new_block_write() {
			size_t free_bit;
			uint64_t window_index;
			window_t* window;
			do {
				window_index = fifo.write_window;
				window = &fifo.buffer[window_index % fifo.window_count];
				free_bit = window->filled_set.template claim_bit<false, true>();
				std::cout << free_bit << std::endl;
				if (free_bit == std::numeric_limits<size_t>::max()) {
					// No more free bits, we move.
					if (fifo.read_window == window_index + 1) {
						return false;
					}
					fifo.write_window.compare_exchange_strong(window_index, window_index + 1);
					std::cout << "NEW WEINDOW" << std::endl;
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
				window_index = fifo.read_window;
				window = &fifo.buffer[window_index % fifo.window_count];
				free_bit = window->filled_set.template claim_bit<true, false>();
				if (free_bit == std::numeric_limits<size_t>::max()) {
					// TODO: I don't like this.
					// Before we move, let's invalidate all write epochs in the blocks.
					bool all_empty = true;
					for (int i = 0; i < BLOCKS_PER_WINDOW; i++) {
						block_t& block = window->blocks[i];
						uint32_t wie = block.header.write_index_and_epoch;
						do {
							// We keep the inner index of the block but poison the epoch.
							auto inner_index = wie & 0xffff; // Either 1 (unused, we check that later) or fully emptied.
							if (inner_index != 1 && inner_index != (block.header.read_finished_index & 0xffff)) {
								// It got pushed into in the meantime, so this means the block must be occupied and there's more to be popped.
								all_empty = false;
								break;
							}
						} while (!block.header.write_index_and_epoch.compare_exchange_weak(wie, static_cast<uint32_t>(window_index + fifo.window_count + 1)));
					}
					// Make sure that none of the potential blocks with index 1 were actually used.
					if (!all_empty || window->filled_set.any()) {
						continue;
					}
					// TODO Until here.
					uint64_t write_window = fifo.write_window;
					if (write_window == window_index + 1) {
						if (!fifo.buffer[write_window % fifo.window_count].filled_set.any()) {
							return false;
						}
						fifo.write_window.compare_exchange_strong(write_window, write_window + 1);
					}

					fifo.read_window.compare_exchange_strong(window_index, window_index + 1);
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
			header_t* header = &write_block->header;
			uint32_t wie = header->write_index_and_epoch;
			uint16_t index;
			if ((wie >> 16) != write_window || (index = (wie & 0xffff)) >= CELLS_PER_BLOCK || !header->write_index_and_epoch.compare_exchange_strong(wie, wie + 1)) {
				if (!claim_new_block_write()) {
					return false;
				}
				index = 0;
			}

			write_block->cells[index] = std::move(t);

			return true;
		}

		std::optional<T> pop() {
			header_t* header = &read_block->header;
			uint32_t rie = header->read_started_index_and_epoch;
			uint32_t index;
			do {
				if ((rie >> 16) != read_window || (index = (rie & 0xffff)) >= (header->write_index_and_epoch & 0xffff)) {
					if (!claim_new_block_read()) {
						return false;
					}
					header = &read_block->header;
					// Keep rie to iterate once more.
				}
			} while (!header->read_started_index_and_epoch.compare_exchange_strong(rie, rie + 1));

			T ret;
			while ((ret = std::move(read_block->cells[index].load())) == 0) { }
			read_block->cells[index] = 0;

			auto finished_index = header->read_finished_index.fetch_add(1) + 1;
			if (finished_index == (header->write_index_and_epoch & 0xffff)) {
				window_t& window = fifo.buffer[read_window % fifo.window_count];
				auto diff = read_block - window.blocks;
				window.filled_set.reset(diff);
				read_window = 0; // Invalidate read window.
				// Increase epoch.
				header->read_started_index_and_epoch = static_cast<uint32_t>(read_window + fifo.window_count) << 16;

				header->read_finished_index = 0;
			}

			return ret;
		}
	};

	handle get_handle() { return handle(*this); }
};
static_assert(fifo<relaxed_fifo<uint64_t>, uint64_t>);

#endif // RELAXED_FIFO_H_INCLUDED
