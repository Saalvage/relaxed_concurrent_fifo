#include "benchmark.h"

#include "lock_fifo.h"
#include "relaxed_fifo.h"
#include "concurrent_fifo.h"

#include "contenders/LCRQ/wrapper.h"
#include "contenders/LCRQ/MichaelScottQueue.hpp"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "contenders/LCRQ/LCRQueue.hpp"

template <typename T>
using LCRQWrapped = LCRQueue<T>;

#include "contenders/scal/scal_wrapper.h"
#pragma GCC diagnostic pop
#endif // __GNUC__

#include <thread>
#include <functional>
#include <ranges>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <iostream>

/*static constexpr int COUNT = 512;

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
}*/

template <size_t THREAD_COUNT, size_t BLOCK_MULTIPLIER>
void test_consistency(size_t fifo_size, size_t elements_per_thread, double prefill) {
	relaxed_fifo<uint64_t, THREAD_COUNT * BLOCK_MULTIPLIER> fifo{ fifo_size };
	auto handle = fifo.get_handle();

	size_t pre_push = static_cast<size_t>(fifo_size * prefill);
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
			auto handle = fifo.get_handle();
			a.arrive_and_wait();
			for (uint64_t j = 0; j < elements_per_thread; j++) {
				auto val = (i << 32) | (j + 1);
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

template <size_t BITSET_SIZE = 128>
void test_continuous_bitset_claim() {
	auto gen = std::bind(std::uniform_int_distribution<>(0, 1), std::default_random_engine());
	while (true) {
		atomic_bitset<BITSET_SIZE> a;
		std::vector<bool> b(BITSET_SIZE);
		for (int i = 0; i < BITSET_SIZE; i++) {
			if (gen()) {
				a.set(i);
				b[i] = true;
			}
		}
		auto result = a.template claim_bit<true, true>();
		if (result != std::numeric_limits<size_t>::max() && (a[result] || !b[result])) {
			throw std::runtime_error("Incorrect!");
		}
	}
}

template <typename BENCHMARK>
void run_benchmark(const std::string& test_name, const std::vector<std::unique_ptr<benchmark_provider<BENCHMARK>>>& instances, const std::vector<double>& prefill_amounts,
	const std::vector<size_t>& processor_counts, int test_iterations, int test_time_seconds) {
	constexpr const char* format = "fifo-{}-{}-{:%FT%H-%M-%S}.csv";

	if (BENCHMARK::use_timing) {
		std::cout << "Expected running time: " << prefill_amounts.size() * test_iterations * test_time_seconds * processor_counts.size() * instances.size() << " seconds" << std::endl;
	}

	for (auto prefill : prefill_amounts) {
		std::cout << "Prefilling with " << prefill << std::endl;
		std::ofstream file{std::format(format, test_name, prefill, std::chrono::round<std::chrono::seconds>(std::chrono::file_clock::now()))};
		for (const auto& imp : instances) {
			std::cout << "Testing " << imp->get_name() << std::endl;
			for (auto i : processor_counts) {
				std::cout << "With " << i << " processors" << std::endl;
				for (auto res : imp->test(i, test_iterations, test_time_seconds, prefill)) {
					file << imp->get_name() << "," << i << ',';
					res.output(file);
					file << '\n';
				}
			}
		}
	}
}

int main() {
#ifndef NDEBUG
	std::cout << "Running in debug mode!" << std::endl;
#endif // NDEBUG

	//test_consistency<8, 16>(20000, 200000, 0);

#ifdef __GNUC__
	scal::ThreadLocalAllocator::Get().Init(1024 * 1024, true);
#endif // __GNUC__

	scal::SegmentQueue<uint64_t> a{ 1024 };
	a.enqueue(2);
	//uint64_t x;
	//a.dequeue(&x);

	std::vector<size_t> processor_counts;
	for (size_t i = 1; i <= std::thread::hardware_concurrency(); i *= 2) {
		processor_counts.emplace_back(i);
	}

	constexpr int OVERRIDE_CHOICE = 0;
	constexpr int TEST_ITERATIONS = 1;
	constexpr int TEST_TIME_SECONDS = 5;

	int input;
	if (OVERRIDE_CHOICE == 0) {
		std::cout << "Which experiment to run?\n[1] FIFO Comparison\n[2] Block per Window Multipliers\n[3] Quality\n[4] Fill\n[5] Empty\nInput: ";
		std::cin >> input;
	}

	if (OVERRIDE_CHOICE == 1 || (OVERRIDE_CHOICE == 0 && input == 1)) {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		//instances.push_back(std::make_unique<benchmark_provider_relaxed<4, benchmark_default>>("relaxed"));
		//instances.push_back(std::make_unique<benchmark_provider_generic<lock_fifo<uint64_t>, benchmark_default>>("lock"));
		//instances.push_back(std::make_unique<benchmark_provider_generic<concurrent_fifo<uint64_t>, benchmark_default>>("concurrent"));
#ifdef __GNUC__
		instances.push_back(std::make_unique<benchmark_provider_generic<scal_wrapper<uint64_t, 1024>, benchmark_default>>("bs-kfifo"));
		instances.push_back(std::make_unique<benchmark_provider_generic<adapter<uint64_t, LCRQWrapped>, benchmark_default>>("lcrq"));
#endif // __GNUC__
		instances.push_back(std::make_unique<benchmark_provider_generic<adapter<uint64_t, MichaelScottQueue>, benchmark_default>>("msq"));
		run_benchmark("comp", instances, { 0.5 }, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
	} else if (OVERRIDE_CHOICE == 2 || (OVERRIDE_CHOICE == 0 && input == 2)) {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_relaxed<1, benchmark_default>>("1"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<2, benchmark_default>>("2"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<4, benchmark_default>>("4"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<8, benchmark_default>>("8"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<16, benchmark_default>>("16"));
		run_benchmark("block", instances, {0.5}, { processor_counts.back() }, TEST_ITERATIONS, TEST_TIME_SECONDS);
	} else if (OVERRIDE_CHOICE == 3 || (OVERRIDE_CHOICE == 0 && input == 3)) {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_quality>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_relaxed<4, benchmark_quality>>("relaxed"));
		run_benchmark("quality", instances, {0.5}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
	} else if (OVERRIDE_CHOICE == 4 || (OVERRIDE_CHOICE == 0 && input == 4)) {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_fill>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_relaxed<4, benchmark_fill>>("relaxed"));
		run_benchmark("fill", instances, {0}, processor_counts, TEST_ITERATIONS, 0);
	} else if (OVERRIDE_CHOICE == 5 || (OVERRIDE_CHOICE == 0 && input == 5)) {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_empty>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_relaxed<4, benchmark_empty>>("relaxed"));
		run_benchmark("empty", instances, {1}, processor_counts, TEST_ITERATIONS, 0);
	}

	return 0;
}
