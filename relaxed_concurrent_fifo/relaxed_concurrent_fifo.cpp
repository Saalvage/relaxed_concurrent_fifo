#include "relaxed_concurrent_fifo.h"

#include "benchmark.h"

#include <ranges>
#include <cassert>
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
	//test_all<circular_buffer>();
	benchmark<circular_buffer> a;
	for (auto i : {1, 2, 4, 8, 16, 32, 64}) {
		auto [avg, std] = a.test(i, 5);
		std::cout << "T: " << i << '\n';
		std::cout << "AVG: " << avg << "; STD: " << std << std::endl;
	}
	return 0;
}
