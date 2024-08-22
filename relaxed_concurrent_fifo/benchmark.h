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

struct benchmark_default {
	std::vector<size_t> results;

	benchmark_default(size_t num_threads) : results(num_threads) { }

	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, std::atomic_bool& over) {
		size_t its = 0;
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		while (!over) {
			handle.push(5);
			handle.pop();
			its++;
		}
		results[thread_index] = its;
	}

	template <typename T>
	void output(T& stream) {
		stream << std::reduce(results.begin(), results.end());
	}
};

struct benchmark_quality {
	std::vector<std::vector<std::tuple<uint64_t, uint64_t>>> results;

	benchmark_quality(size_t num_threads) : results(num_threads) { }

	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, std::atomic_bool& over) {
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		while (!over) {
			handle.push(std::chrono::steady_clock::now().time_since_epoch().count());
			results[thread_index].push_back({handle.pop().value(), std::chrono::steady_clock::now().time_since_epoch().count()});
		}
	}
};

template <typename BENCHMARK>
class benchmark_base {
public:
	virtual ~benchmark_base() = default;
	virtual std::vector<BENCHMARK> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const = 0;
	virtual const std::string& get_name() const = 0;

protected:
	template <typename FIFO>
	static BENCHMARK test_single(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
		constexpr size_t SIZE = 2 * 65536;
		FIFO fifo{SIZE};
		auto handle = fifo.get_handle();
		for (size_t i = 0; i < prefill_amount * SIZE; i++) {
			handle.push(0);
		}
		std::barrier a{ (ptrdiff_t)(num_threads + 1) };
		std::atomic_bool over = false;
		std::vector<std::jthread> threads(num_threads);
		BENCHMARK b{num_threads};
		for (size_t i = 0; i < num_threads; i++) {
			threads[i] = std::jthread([&, i]() {
				b.template per_thread<FIFO>(i, fifo, a, over);
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

		return b;
	}
};

template <typename FIFO, typename BENCHMARK>
class benchmark : public benchmark_base<BENCHMARK> {
public:
	benchmark(std::string name) : name(std::move(name)) { }

	const std::string& get_name() const override {
		return name;
	}

	std::vector<BENCHMARK> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
		std::vector<BENCHMARK> results(num_its, BENCHMARK(num_threads));
		for (auto i : std::views::iota((size_t)0, num_its)) {
			results[i] = benchmark_base<BENCHMARK>::template test_single<FIFO>(num_threads, test_time_seconds, prefill_amount);
		}
		return results;
	}

private:
	std::string name;
};

template <size_t BLOCK_MULTIPLIER, typename BENCHMARK>
class benchmark_relaxed : public benchmark_base<BENCHMARK> {
	public:
		benchmark_relaxed(std::string name) : name(std::move(name)) { }

		const std::string& get_name() const override {
			return name;
		}

		std::vector<BENCHMARK> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
			std::vector<BENCHMARK> results(num_its, BENCHMARK(num_threads));
			for (auto i : std::views::iota((size_t)0, num_its)) {
				results[i] = test_single_helper(num_threads, test_time_seconds, prefill_amount);
			}
			return results;
		}

	private:
		std::string name;

		static BENCHMARK test_single_helper(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
			switch (num_threads) {
			case 1: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 2: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 2 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 4: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 4 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 8: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 8 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 16: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 16 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 32: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 32 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 64: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 64 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 128: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 128 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 256: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 256 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 512: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 512 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 1024: return benchmark_base<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1024 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			default: throw std::runtime_error("Unsupported thread count!");
			}
		}
};

#endif // BENCHMARK_H_INCLUDED
