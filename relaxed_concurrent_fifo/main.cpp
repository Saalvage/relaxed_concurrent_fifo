#include "benchmark.h"

#include "lock_fifo.h"
#include "relaxed_fifo.h"
#include "concurrent_fifo.h"

#include <ranges>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <iostream>

static constexpr int COUNT = 512;

template <template <typename, size_t> typename T>
void test_full_capacity() {
	T<int, COUNT> buf;
	for (int i : std::views::iota(0, COUNT)) {
		assert(buf.push(i));
	}
	for (int i : std::views::iota(0, COUNT)) {
		assert(buf.pop() == i);
	}
}

template <template <typename, size_t> typename T>
void test_single_element() {
	T<int, COUNT> buf;
	for (int i : std::views::iota(0, COUNT * 10)) {
		assert(buf.push(i));
		assert(buf.pop() == i);
	}
}

template <template <typename, size_t> typename T>
void test_empty_pop() {
	T<int, COUNT> buf;
	assert(!buf.pop().has_value());
	assert(buf.push(1));
	buf.pop();
	assert(!buf.pop().has_value());
	for (int i : std::views::iota(0, COUNT * 10)) {
		buf.push(i);
		buf.pop();
	}
	assert(!buf.pop().has_value());
}

template <template <typename, size_t> typename T>
void test_full_push() {
	T<int, 1> buf;
	buf.push(1);
	assert(!buf.push(1));
}

template <template <typename, size_t> typename T>
void test_all() {
	test_full_capacity<T>();
	test_single_element<T>();
	test_empty_pop<T>();
	test_full_push<T>();
}

template <size_t THREAD_COUNT, size_t FIFO_SIZE, uint32_t PER_THREAD_ELEMENTS>
void test_consistency() {
	relaxed_fifo<uint64_t, THREAD_COUNT> fifo{FIFO_SIZE};
	auto handle = fifo.get_handle();

	constexpr int pre_push = FIFO_SIZE / 2;
	std::unordered_multiset<uint64_t> test_ints;
	for (size_t index = 0; index < pre_push; index++) {
		auto i = index | (1ull << 63);
		handle.push(i);
		test_ints.emplace(i);
	}

	std::barrier a{ (ptrdiff_t)(THREAD_COUNT + 1) };
	std::vector<std::jthread> threads(THREAD_COUNT);
	std::vector<std::vector<uint64_t>> test(THREAD_COUNT);
	std::vector<std::vector<uint64_t>> popped(THREAD_COUNT);
	for (size_t i = 0; i < THREAD_COUNT; i++) {
		threads[i] = std::jthread([&, i]() {
			size_t its = 0;
			auto handle = fifo.get_handle();
			a.arrive_and_wait();
			for (uint64_t j = 0; j < PER_THREAD_ELEMENTS; j++) {
				auto val = (i << 32) | j;
				test[i].push_back(val);
				while (!handle.push(val)) {}
				std::optional<uint64_t> pop;
				do {
					pop = handle.pop();
				} while (!pop.has_value());
				popped[i].push_back(pop.value());
			}
		});
	}
	a.arrive_and_wait();
	for (auto& thread : threads) {
		thread.join();
	}

	std::unordered_multiset<uint64_t> popped_ints;
	for (size_t index = 0; index < pre_push; index++) {
		popped_ints.emplace(handle.pop().value());
	}

	for (size_t i = 0; i < THREAD_COUNT; i++) {
		for (auto i : popped[i]) {
			popped_ints.emplace(i);
		}
		for (auto i : test[i]) {
			test_ints.emplace(i);
		}
	}

	if (handle.pop().has_value()) {
		throw std::runtime_error("Invalid element left!");
	}

	if (popped_ints != test_ints) {
		throw std::runtime_error("Sets did not match!");
	}
}

int main() {
	test_consistency<8, 1 << 12, 1 << 20>();

	namespace fs = std::filesystem;

	constexpr const char* format = "fifo-data-{}-{:%FT%H-%M-%S}.csv";

	static constexpr double PREFILL_AMOUNTS[] = {0.5};
	static constexpr int TEST_TIME_SECONDS = 10;
	static constexpr int TEST_ITERATIONS = 5;
	std::vector<size_t> processor_counts;
	for (size_t i = 1; i <= std::thread::hardware_concurrency(); i *= 2) {
		processor_counts.emplace_back(i);
	}
	std::unique_ptr<benchmark_base> instances[] = {std::make_unique<benchmark_relaxed>("relaxed"), std::make_unique<benchmark<lock_fifo<uint64_t>>>("lock"), std::make_unique<benchmark<concurrent_fifo<uint64_t>>>("concurrent")};

#ifndef NDEBUG
	std::cout << "Running in debug mode!" << std::endl;
#endif // NDEBUG

	std::cout << "Expected running time: " << sizeof(PREFILL_AMOUNTS) / sizeof(*PREFILL_AMOUNTS) * TEST_ITERATIONS * TEST_TIME_SECONDS * processor_counts.size() * sizeof(instances) / sizeof(*instances) << " seconds" << std::endl;
	for (auto prefill : PREFILL_AMOUNTS) {
		std::cout << "Prefilling with " << prefill << std::endl;
		std::ofstream file{"fifo-data-latest.csv"};
		for (const auto& imp : instances) {
			std::cout << "Testing " << imp->get_name() << std::endl;
			//test_all<lock_fifo>();
			for (auto i : processor_counts) {
				std::cout << "With " << i << " processors" << std::endl;
				for (auto res : imp->test(i, TEST_ITERATIONS, TEST_TIME_SECONDS, prefill)) {
					file << imp->get_name() << "," << i << ',' << res << '\n';
				}
			}
		}
	}

	return 0;
}
