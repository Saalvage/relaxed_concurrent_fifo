#ifndef BENCHMARK_H_INCLUDED
#define BENCHMARK_H_INCLUDED

#include "fifo.h"

#include <cmath>
#include <vector>
#include <barrier>
#include <thread>
#include <atomic>
#include <chrono>
#include <numeric>
#include <ranges>
#include <future>
#include <iostream>

class benchmark_base {
public:
	virtual ~benchmark_base() = default;
	virtual std::vector<size_t> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const = 0;
	virtual const std::string& get_name() const = 0;
};

template <template <typename> typename T> requires fifo<T>
class benchmark : public benchmark_base {
public:
	benchmark(std::string name) : name(std::move(name)) { }

	const std::string& get_name() const override {
		return name;
	}

	std::vector<size_t> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
		std::vector<size_t> results(num_its);
		for (auto i : std::views::iota((size_t)0, num_its)) {
			results[i] = test_single(num_threads, test_time_seconds, prefill_amount);
		}
		return results;
	}

private:
	std::string name;

	static size_t test_single(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
		T<size_t> fifo{1024};
		for (size_t i = 0; i < prefill_amount * 1024; i++) {
			fifo.push(i);
		}
		std::barrier a{ (ptrdiff_t)(num_threads + 1) };
		std::atomic_bool over = false;
		std::vector<std::jthread> threads(num_threads);
		std::vector<size_t> results(num_threads);
		for (size_t i = 0; i < num_threads; i++) {
			threads[i] = std::jthread([&, i]() {
				size_t its = 0;
				a.arrive_and_wait();
				while (!over) {
					fifo.push(5);
					fifo.pop();
					its++;
				}
				results[i] = its;
			});
		}
		auto start = std::chrono::system_clock::now();
		a.arrive_and_wait();
		std::this_thread::sleep_until(start + std::chrono::seconds(test_time_seconds));
		over = true;
		auto joined = std::async([&]() {
			for (auto& thread : threads) {
				thread.join();
			}
		});

		if (joined.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
			std::cout << "Threads did not complete within timeout, assuming deadlock!" << std::endl;
			std::exit(1);
		}

		return std::reduce(results.begin(), results.end());
	}
};

#endif // BENCHMARK_H_INCLUDED
