#include "benchmark.h"

#include "lock_fifo.h"
#include "relaxed_fifo.h"
#include "concurrent_fifo.h"

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

	for (auto i : std::views::iota(0, test_iterations)) {
		std::cout << "Test run " << (i + 1) << " of " << test_iterations << std::endl;
		for (auto prefill : prefill_amounts) {
			std::cout << "Prefilling with " << prefill << std::endl;
			std::ofstream file{ std::format(format, test_name, prefill, std::chrono::round<std::chrono::seconds>(std::chrono::file_clock::now())) };
			for (const auto& imp : instances) {
				std::cout << "Testing " << imp->get_name() << std::endl;
				for (auto i : processor_counts) {
					std::cout << "With " << i << " processors" << std::endl;
					file << imp->get_name() << "," << i << ',';
					imp->test(i, test_time_seconds, prefill).output(file);
					file << '\n';
				}
			}
		}
	}
}

template <typename BENCHMARK>
void add_all_parameter_tuning(std::vector<std::unique_ptr<benchmark_provider<BENCHMARK>>>& instances) {
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 7>>("1,7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 15>>("1,15"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 31>>("1,31"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 63>>("1,63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 127>>("1,127"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 7>>("2,7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 15>>("2,15"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 31>>("2,31"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 63>>("2,63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 127>>("2,127"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 7>>("4,7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 15>>("4,15"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 31>>("4,31"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 63>>("4,63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 127>>("4,127"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 7>>("8,7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 15>>("8,15"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 31>>("8,31"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 63>>("8,63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 127>>("8,127"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 16, 7>>("16,7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 16, 15>>("16,15"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 16, 31>>("16,31"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 16, 63>>("16,63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 16, 127>>("16,127"));
}

template <typename BENCHMARK>
void add_all_benchmarking(std::vector<std::unique_ptr<benchmark_provider<BENCHMARK>>>& instances) {
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 1, 7>>("bbq-1-7"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 2, 63>>("bbq-2-63"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 4, 127>>("bbq-4-127"));
	instances.push_back(std::make_unique<benchmark_provider_relaxed<BENCHMARK, 8, 127>>("bbq-8-127"));
	instances.push_back(std::make_unique<benchmark_provider_generic<lock_fifo<uint64_t>, BENCHMARK>>("lock"));
	instances.push_back(std::make_unique<benchmark_provider_generic<concurrent_fifo<uint64_t>, BENCHMARK>>("concurrent"));
}

int main() {
#ifndef NDEBUG
	std::cout << "Running in debug mode!" << std::endl;
#endif // NDEBUG

	//test_consistency<8, 16>(20000, 200000, 0);

	std::vector<size_t> processor_counts;
	for (size_t i = 1; i <= std::thread::hardware_concurrency(); i *= 2) {
		processor_counts.emplace_back(i);
	}

	constexpr int TEST_ITERATIONS = 5;
	constexpr int TEST_TIME_SECONDS = 5;

	int input;
	std::cout << "Which experiment to run?\n"
		"[1] FIFO Comparison\n"
		"[2] Parameter Tuning\n"
		"[3] Quality\n"
		"[4] Fill\n"
		"[5] Empty\n"
		"[6] Strong Scaling\n"
		"[7] Bitset Size Comparison\n"
		"[8] Starvation Comparison\n"
		"Input: ";
	std::cin >> input;

	switch (input) {
	case 1: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		add_all_benchmarking(instances);
		run_benchmark("comp", instances, {0.5}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
		} break;
	case 2: {
		std::cout << "Benchmarking performance" << std::endl;
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		add_all_parameter_tuning(instances);
		run_benchmark("pt-block", instances, {0.5}, { processor_counts.back() }, TEST_ITERATIONS, TEST_TIME_SECONDS);

		std::cout << "Benchmarking quality" << std::endl;
		std::vector<std::unique_ptr<benchmark_provider<benchmark_quality>>> instances_q;
		add_all_parameter_tuning(instances_q);
		run_benchmark("pt-quality", instances_q, {0.5}, { processor_counts.back() }, TEST_ITERATIONS, 0);
		} break;
	case 3: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_quality>>> instances;
		add_all_benchmarking(instances);
		run_benchmark("quality", instances, {0.5}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
		} break;
	case 4: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_fill>>> instances;
		add_all_benchmarking(instances);
		run_benchmark("fill", instances, {0}, processor_counts, TEST_ITERATIONS, 0);
		} break;
	case 5: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_empty>>> instances;
		add_all_benchmarking(instances);
		run_benchmark("empty", instances, {1}, processor_counts, TEST_ITERATIONS, 0);
		} break;
	case 6: {
		static constexpr size_t THREADS = 128;

		std::cout << "Benchmarking performance" << std::endl;
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, THREADS, 7>, benchmark_default>>("bbq-1-7"));
		instances.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 2 * THREADS, 63>, benchmark_default>>("bbq-2-63"));
		instances.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 4 * THREADS, 127>, benchmark_default>>("bbq-4-127"));
		instances.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 8 * THREADS, 127>, benchmark_default>>("bbq-8-127"));
		run_benchmark("ss-performance", instances, {0.5}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);

		std::cout << "Benchmarking quality" << std::endl;
		std::vector<std::unique_ptr<benchmark_provider<benchmark_quality>>> instances_q;
		instances_q.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, THREADS, 7>, benchmark_quality>>("bbq-1-7"));
		instances_q.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 2 * THREADS, 63>, benchmark_quality>>("bbq-2-63"));
		instances_q.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 4 * THREADS, 127>, benchmark_quality>>("bbq-4-127"));
		instances_q.push_back(std::make_unique<benchmark_provider_generic<relaxed_fifo<uint64_t, 8 * THREADS, 127>, benchmark_quality>>("bbq-8-127"));
		run_benchmark("ss-quality", instances_q, {0.5}, processor_counts, TEST_ITERATIONS, 0);
		} break;
	case 7: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 1, 7, uint8_t>>("8-bit-bbq-1-7"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 2, 63, uint8_t>>("8-bit-bbq-2-63"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 4, 127, uint8_t>>("8-bit-bbq-4-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 8, 127, uint8_t>>("8-bit-bbq-8-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 1, 7, uint16_t>>("16-bit-bbq-1-7"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 2, 63, uint16_t>>("16-bit-bbq-2-63"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 4, 127, uint16_t>>("16-bit-bbq-4-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 8, 127, uint16_t>>("16-bit-bbq-8-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 1, 7, uint32_t>>("32-bit-bbq-1-7"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 2, 63, uint32_t>>("32-bit-bbq-2-63"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 4, 127, uint32_t>>("32-bit-bbq-4-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 8, 127, uint32_t>>("32-bit-bbq-8-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 1, 7, uint64_t>>("64-bit-bbq-1-7"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 2, 63, uint64_t>>("64-bit-bbq-2-63"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 4, 127, uint64_t>>("64-bit-bbq-4-127"));
		instances.push_back(std::make_unique<benchmark_provider_relaxed<benchmark_default, 8, 127, uint64_t>>("64-bit-bbq-8-127"));
		run_benchmark("bitset-sizes", instances, {0.5}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
		} break;
	case 8: {
		std::vector<std::unique_ptr<benchmark_provider<benchmark_default>>> instances;
		add_all_benchmarking(instances);
		run_benchmark("comp", instances, {0}, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
		} break;
	}

	return 0;
}
