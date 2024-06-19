#include "benchmark.h"

#include "lock_fifo.h"
#include "relaxed_fifo.h"

#include <ranges>
#include <cassert>
#include <filesystem>
#include <fstream>

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

	constexpr const char* latest = "fifo-data-latest.csv";
	constexpr const char* format = "fifo-data-{:%FT%H-%M-%S}.csv";

	if (fs::exists(latest)) {
		auto write_time = fs::last_write_time(latest);
		fs::rename(latest, std::format(format, std::chrono::round<std::chrono::seconds>(write_time)));
	}

	std::ofstream file{latest};

	for (const auto& imp : {benchmark<lock_fifo>{"lock"}}) {
		//test_all<lock_fifo>();
		for (auto i : {1, 2, 4, 8, 16, 32, 64}) {
			for (auto res : imp.test(i, 1)) {
				file << imp.get_name() << "," << i << ',' << res << '\n';
			}
		}
	}

	return 0;
}
