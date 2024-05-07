#include "relaxed_concurrent_fifo.h"

#include <ranges>
#include <cassert>

static constexpr int COUNT = 512;

template <template <typename, size_t> typename T>
void test_full_capacity() {
	T<int, COUNT + 1> buf;
	for (int i : std::views::iota(0, COUNT)) {
		buf.push(i);
	}
	for (int i : std::views::iota(0, COUNT)) {
		assert(buf.pop() == i);
	}
}

template <template <typename, size_t> typename T>
void test_single_element() {
	T<int, COUNT + 1> buf;
	for (int i : std::views::iota(0, COUNT * 10)) {
		buf.push(i);
		assert(buf.pop() == i);
	}
}

template <template <typename, size_t> typename T>
void test_empty_pop() {
	T<int, COUNT + 1> buf;
	assert(!buf.pop().has_value());
	buf.push(1);
	buf.pop();
	assert(!buf.pop().has_value());
	for (int i : std::views::iota(0, COUNT * 10)) {
		buf.push(i);
		buf.pop();
	}
	assert(!buf.pop().has_value());
}

template <template <typename, size_t> typename T>
void test_all() {
	test_full_capacity<T>();
	test_single_element<T>();
	test_empty_pop<T>();
}

int main() {
	test_all<circular_buffer>();
	return 0;
}
