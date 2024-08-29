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
#include <map>
#include <set>
#include <execution>

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

template <size_t THREAD_COUNT, size_t BLOCK_MULTIPLIER, size_t FIFO_SIZE, uint32_t PER_THREAD_ELEMENTS>
void test_consistency(double prefill) {
	relaxed_fifo<uint64_t, THREAD_COUNT * BLOCK_MULTIPLIER> fifo{FIFO_SIZE};
	auto handle = fifo.get_handle();

	size_t pre_push = static_cast<size_t>(FIFO_SIZE * prefill);
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

template <typename BENCHMARK>
void run_benchmark(const std::vector<std::unique_ptr<benchmark_base<BENCHMARK>>>& instances, const std::vector<double>& prefill_amounts,
	const std::vector<size_t>& processor_counts, int test_iterations, int test_time_seconds) {
	constexpr const char* format = "fifo-data-{}-{:%FT%H-%M-%S}.csv";

	std::cout << "Expected running time: " << prefill_amounts.size() * test_iterations * test_time_seconds * processor_counts.size() * instances.size() << " seconds" << std::endl;
	for (auto prefill : prefill_amounts) {
		std::cout << "Prefilling with " << prefill << std::endl;
		std::ofstream file{std::format(format, prefill, std::chrono::round<std::chrono::seconds>(std::chrono::file_clock::now()))};
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

	//test_consistency<8, 512, 1 << 17, 1 << 20>(0);

	namespace fs = std::filesystem;

	std::vector<size_t> processor_counts;
	for (size_t i = 1; i <= std::thread::hardware_concurrency(); i *= 2) {
		processor_counts.emplace_back(i);
	}

	constexpr int OVERRIDE_CHOICE = 0;
	constexpr int TEST_ITERATIONS = 1;
	constexpr int TEST_TIME_SECONDS = 5;

	int input;
	if (OVERRIDE_CHOICE == 0) {
		std::cout << "Which experiment to run?\n[1] FIFO Comparison\n[2] Block per Window Multipliers\n[3] Quality\nInput: ";
		std::cin >> input;
	}

	if (OVERRIDE_CHOICE == 1 || (OVERRIDE_CHOICE == 0 && input == 1)) {
		std::vector<std::unique_ptr<benchmark_base<benchmark_default>>> instances;
		instances.push_back(std::make_unique<benchmark_relaxed<4, benchmark_default>>("relaxed"));
		instances.push_back(std::make_unique<benchmark<lock_fifo<uint64_t>, benchmark_default>>("lock"));
		instances.push_back(std::make_unique<benchmark<concurrent_fifo<uint64_t>, benchmark_default>>("concurrent"));
		run_benchmark(instances, { 0.5 }, processor_counts, TEST_ITERATIONS, TEST_TIME_SECONDS);
	} else if (OVERRIDE_CHOICE == 2 || (OVERRIDE_CHOICE == 0 && input == 2)) {
		std::vector<std::unique_ptr<benchmark_base<benchmark_default>>> instances;
		instances.push_back(std::make_unique<benchmark_relaxed<1, benchmark_default>>("1"));
		instances.push_back(std::make_unique<benchmark_relaxed<2, benchmark_default>>("2"));
		instances.push_back(std::make_unique<benchmark_relaxed<4, benchmark_default>>("4"));
		instances.push_back(std::make_unique<benchmark_relaxed<8, benchmark_default>>("8"));
		instances.push_back(std::make_unique<benchmark_relaxed<16, benchmark_default>>("16"));
		run_benchmark(instances, { 0.5 }, { processor_counts.back() }, TEST_ITERATIONS, TEST_TIME_SECONDS);
	} else if (OVERRIDE_CHOICE == 3 || (OVERRIDE_CHOICE == 0 && input == 3)) {
		auto results = benchmark_relaxed<4, benchmark_quality>("quality").test(4, 1, 5, 0.5)[0].results;
		auto start_time = std::chrono::steady_clock::now();
		auto total_count = std::accumulate(results.begin(), results.end(), (size_t)0, [](size_t size, const auto& v) { return size + v.size(); });
		std::vector<std::pair<uint64_t, uint64_t>> pushed_to_popped;
		pushed_to_popped.reserve(total_count);
		std::vector<uint64_t> popped_vec;
		popped_vec.reserve(total_count);
		for (const auto& thread_result : results) {
			for (const auto& [pushed, popped] : thread_result) {
				pushed_to_popped.emplace_back(pushed, popped);
				popped_vec.emplace_back(popped);
			}
		}
		std::sort(std::execution::par_unseq, pushed_to_popped.begin(), pushed_to_popped.end());
		std::sort(std::execution::par_unseq, popped_vec.begin(), popped_vec.end());
		std::atomic_uint64_t max = 0;
		// We're using the doubled diff because when there are multiple of the same pop timing
		// we're adding the average index which would require using floating point values.
		uint64_t total_diff_doubled = std::transform_reduce(std::execution::par_unseq, pushed_to_popped.begin(), pushed_to_popped.end(), 0ull, std::plus<uint64_t>(), [&](const auto& pair) {
			uint64_t i = &pair - &pushed_to_popped[0];
			auto& [pushed, popped] = pair;
			auto [popped_min, popped_max] = std::equal_range(popped_vec.begin(), popped_vec.end(), popped);
			uint64_t popped_index_max = popped_max - popped_vec.begin();
			uint64_t max_new = popped_index_max > i ? popped_index_max - i : i - popped_index_max;
			uint64_t max_curr = max;
			while (max_curr < max_new && !max.compare_exchange_strong(max_curr, max_new)) { }
			uint64_t popped_index_doubled = 2 * (popped_min - popped_vec.begin()) + (popped_max - popped_min);
			return popped_index_doubled > 2 * i ? popped_index_doubled - 2 * i : 2 * i - popped_index_doubled;
		});
		std::cout << "Avg pop error: " << total_diff_doubled / 2.0 / pushed_to_popped.size() << " with " << pushed_to_popped.size() << " elements and a max of " << max << std::endl;
		std::cout << "Took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() << "ms";
	}

	return 0;
}
