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

#ifdef _POSIX_VERSION
#include <pthread.h>
#endif // _POSIX_VERSION

#include "relaxed_fifo.h"

class benchmark_base {
public:
	virtual ~benchmark_base() = default;
	virtual std::vector<size_t> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const = 0;
	virtual const std::string& get_name() const = 0;

protected:
	template <typename T>
	static size_t test_single(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
		T fifo{65536};
		auto handle = fifo.get_handle();
		for (size_t i = 0; i < prefill_amount * 65536; i++) {
			handle.push(i);
		}
		std::barrier a{ (ptrdiff_t)(num_threads + 1) };
		std::atomic_bool over = false;
		std::vector<std::jthread> threads(num_threads);
		std::vector<size_t> results(num_threads);
		for (size_t i = 0; i < num_threads; i++) {
			threads[i] = std::jthread([&, i]() {
				size_t its = 0;
				auto handle = fifo.get_handle();
				a.arrive_and_wait();
				while (!over) {
					handle.push(5);
					handle.pop();
					its++;
				}
				results[i] = its;
			});
#ifdef _POSIX_VERSION
			cpu_set_t cpu_set;
			CPU_ZERO(&cpu_set);
			CPU_SET(i, &cpu_set);
			if (pthread_setaffinity_np(threads[i].native_handle(), sizeof(cpu_set_t), &cpu_set)) {
				throw std::runtime_error("Failed to set thread affinity!");
			}
#endif // _POSIX_VERSION
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

template <typename T>// requires fifo<T>
class benchmark : public benchmark_base {
public:
	benchmark(std::string name) : name(std::move(name)) { }

	const std::string& get_name() const override {
		return name;
	}

	std::vector<size_t> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
		std::vector<size_t> results(num_its);
		for (auto i : std::views::iota((size_t)0, num_its)) {
			results[i] = test_single<T>(num_threads, test_time_seconds, prefill_amount);
		}
		return results;
	}

private:
	std::string name;
};

class benchmark_relaxed : public benchmark_base {
	public:
		benchmark_relaxed(std::string name) : name(std::move(name)) { }

		const std::string& get_name() const override {
			return name;
		}

		std::vector<size_t> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
			std::vector<size_t> results(num_its);
			for (auto i : std::views::iota((size_t)0, num_its)) {
				results[i] = test_single_helper(num_threads, test_time_seconds, prefill_amount);
			}
			return results;
		}

	private:
		std::string name;

		static size_t test_single_helper(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
			switch (num_threads) {
			case 1: return test_single<relaxed_fifo<size_t, 1>>(num_threads, test_time_seconds, prefill_amount);
			case 2: return test_single<relaxed_fifo<size_t, 2>>(num_threads, test_time_seconds, prefill_amount);
			case 4: return test_single<relaxed_fifo<size_t, 4>>(num_threads, test_time_seconds, prefill_amount);
			case 8: return test_single<relaxed_fifo<size_t, 8>>(num_threads, test_time_seconds, prefill_amount);
			case 16: return test_single<relaxed_fifo<size_t, 16>>(num_threads, test_time_seconds, prefill_amount);
			case 32: return test_single<relaxed_fifo<size_t, 32>>(num_threads, test_time_seconds, prefill_amount);
			case 64: return test_single<relaxed_fifo<size_t, 64>>(num_threads, test_time_seconds, prefill_amount);
			case 128: return test_single<relaxed_fifo<size_t, 128>>(num_threads, test_time_seconds, prefill_amount);
			case 256: return test_single<relaxed_fifo<size_t, 256>>(num_threads, test_time_seconds, prefill_amount);
			case 512: return test_single<relaxed_fifo<size_t, 512>>(num_threads, test_time_seconds, prefill_amount);
			case 1024: return test_single<relaxed_fifo<size_t, 1024>>(num_threads, test_time_seconds, prefill_amount);
			default: throw std::runtime_error("Unsupported thread count!");
			}
		}
};

#endif // BENCHMARK_H_INCLUDED
