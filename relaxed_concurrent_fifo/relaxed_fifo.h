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

template <typename T, size_t BLOCKS_PER_WINDOW_RAW = 8, size_t CELLS_PER_BLOCK = CACHE_SIZE / sizeof(T) - 1>
class relaxed_fifo {
private:
	// TODO: Do properly, we need multiples of 8 for the bitset to work correctly.
	static constexpr size_t BLOCKS_PER_WINDOW = std::max<size_t>(8, BLOCKS_PER_WINDOW_RAW);

	// TODO: Optimize modulo.
	const size_t window_count;

	size_t size() const {
		return window_count * BLOCKS_PER_WINDOW * CELLS_PER_BLOCK;
	}

	using handle_id = uint8_t;

	static_assert(sizeof(T) == 8);
	static_assert(sizeof(std::atomic<T>) == 8);

	struct alignas(8) header_t {
		// 16 bits epoch, 16 bits read begin index, 16 bits read end index, 16 bits write index
		std::atomic_uint64_t epoch_and_indices;
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

	static constexpr size_t make_po2(size_t size) {
		size_t ret = 1;
		while (size > ret) {
			ret *= 2;
		}
		return ret;
	}

public:
	relaxed_fifo(size_t size) : window_count(make_po2(size / BLOCKS_PER_WINDOW / CELLS_PER_BLOCK)) {
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
				header_t& header = buffer[i].blocks[j].header;
				std::cout << header.epoch_and_indices << " | ";
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
						? set ? bits.set(idx, std::memory_order_relaxed) : bits.reset(idx, std::memory_order_relaxed)
						: set ? !bits.test(idx, std::memory_order_relaxed) : bits.test(idx, std::memory_order_relaxed)) {
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
				window_index = fifo.write_window.load(std::memory_order_relaxed);
				window = &fifo.buffer[window_index % fifo.window_count];
				free_bit = window->filled_set.template claim_bit<false, true>(std::memory_order_relaxed);
				if (free_bit == std::numeric_limits<size_t>::max()) {
					// No more free bits, we move.
					if (window_index + 1 - fifo.read_window.load(std::memory_order_acquire) == fifo.window_count) {
						return false;
					}
					fifo.write_window.compare_exchange_strong(window_index, window_index + 1, std::memory_order_release, std::memory_order_relaxed);
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

		// TODO: Relax!
		bool claim_new_block_read() {
			size_t free_bit;
			uint64_t window_index;
			window_t* window;
			do {
				window_index = fifo.read_window.load(std::memory_order_acquire);
				window = &fifo.buffer[window_index % fifo.window_count];
				free_bit = window->filled_set.template claim_bit<true, false>(std::memory_order_relaxed);
				if (free_bit == std::numeric_limits<size_t>::max()) {
					uint64_t write_window = fifo.write_window.load();
					if (write_window == window_index + 1) {
						if (!fifo.buffer[write_window % fifo.window_count].filled_set.any()) {
							return false;
						}
						if (fifo.write_window.compare_exchange_strong(write_window, write_window + 1)) {
							// We force-moved the write window, there might be unclaimed blocks in the old one.
							// Before we move the read window, let's invalidate all unclaimed write epochs in the blocks.
							// TODO: We should be able to do this AFTER the moving the read window, saving one other thread from potentially
							// having to move it in the meantime.
							window_t& new_window = fifo.buffer[write_window % fifo.window_count];
							for (size_t i = 0; i < BLOCKS_PER_WINDOW; i++) {
								if (new_window.filled_set.test(i)) {
									continue;
								}

								block_t& block = new_window.blocks[i];
								uint64_t ei = block.header.epoch_and_indices; // TODO: We know what we want here, save the load!
								if ((ei >> 48) == (write_window & 0xffff) && (ei & 0xffff) == 0 && block.header.epoch_and_indices.compare_exchange_strong(ei, (write_window + fifo.window_count) << 48)) {
									// If any of these checks fails, the block was claimed and written to in the meantime, so we have it be read normally as well.
								}
							}
						}
#if LOG_WINDOW_MOVE
						std::cout << "Write force move " << (write_window + 1) << std::endl;
#endif // LOG_WINDOW_MOVE
					}

					fifo.read_window.compare_exchange_strong(window_index, window_index + 1);
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
			uint16_t index = 0; // TODO: Don't initialize.
			bool claimed = false;
			do {
				if (ei >> 48 != (write_window & 0xffff)
						|| (index = (ei & 0xffff)) >= CELLS_PER_BLOCK) {
					// TODO: We need this for the case where we claim a bit, but can't place an element inside,
					// because the write window was already forced-moved.
					// Maybe we could instead detect this elsewhere? (E.g. when encountering the same situation on claiming a new read block)
					// TODO: This also still fails very sporadically.
					if (claimed && index == 0) {
						// We're abandoning an empty block!
						window_t& window = fifo.buffer[write_window % fifo.window_count];
						auto diff = write_block - window.blocks;
						window.filled_set.reset(diff, std::memory_order_relaxed);
					}
					if (!claim_new_block_write()) {
						return false;
					}
					claimed = true;
					header = &write_block->header;
					// Keep wie to iterate once more.
					index = 0; // TODO: This is unnecessary.
					ei = ~ei; // TODO: Better way to invalidate?
				}
			// TODO: We can assume a failure here means we need a new block. Use for optimization?
			} while (!header->epoch_and_indices.compare_exchange_weak(ei, ei + 1, std::memory_order_relaxed, std::memory_order_acquire));

			write_block->cells[index].store(std::move(t), std::memory_order_relaxed);

			return true;
		}

		// TODO: Relax again.
		std::optional<T> pop() {
			header_t* header = &read_block->header;
			uint64_t ei = header->epoch_and_indices.load(std::memory_order_acquire);
			uint16_t index;

			do {
				if (ei >> 48 != (read_window & 0xffff)
						|| (index = ((ei >> 32) & 0xffff)) >= (ei & 0xffff)) {
					if (!claim_new_block_read()) {
						return std::nullopt;
					}
					header = &read_block->header;
					index = 0; // TODO: This is unnecessary.
					ei = ~ei; // TODO: Better way to invalidate?
				}
			} while (!header->epoch_and_indices.compare_exchange_weak(ei, ei + (1ull << 32), std::memory_order_seq_cst));

			T ret;
			while ((ret = std::move(read_block->cells[index].load(std::memory_order_relaxed))) == 0) { }
			read_block->cells[index].store(0, std::memory_order_relaxed);

			uint16_t finished_index = static_cast<uint16_t>(header->epoch_and_indices.fetch_add(1 << 16, std::memory_order_relaxed) >> 16) + 1;
			if (finished_index >= (ei & 0xffff)) {
				// Apply local read index update.
				ei = (ei & (0xffffull << 48)) | (static_cast<uint64_t>(finished_index) << 32) | (static_cast<uint64_t>(finished_index) << 16) | finished_index;
				// Before we mark this block as empty, we make it unavailable for other readers and writers of this epoch.
				if (header->epoch_and_indices.compare_exchange_strong(ei, (read_window + fifo.window_count) << 48, std::memory_order_seq_cst)) {
					window_t& window = fifo.buffer[read_window % fifo.window_count];
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
