﻿#include "benchmark.h"

#include "lock_fifo.h"
#include "relaxed_fifo.h"
#include "concurrent_fifo.h"

#include <ranges>
#include <cassert>
#include <filesystem>
#include <fstream>
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

int main() {
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

	std::cout << "Expected running time: " << sizeof(PREFILL_AMOUNTS) / sizeof(*PREFILL_AMOUNTS) * TEST_ITERATIONS * TEST_TIME_SECONDS * processor_counts.size() * sizeof(instances) / sizeof(*instances) << " seconds" << std::endl;
	for (auto prefill : PREFILL_AMOUNTS) {
		std::cout << "Prefilling with " << prefill << std::endl;
		std::ofstream file{std::format(format, prefill, std::chrono::round<std::chrono::seconds>(std::chrono::file_clock::now()))};
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
