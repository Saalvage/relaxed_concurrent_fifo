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
#include <execution>

#ifdef _POSIX_VERSION
#include <pthread.h>
#endif // _POSIX_VERSION

#include "relaxed_fifo.h"

struct benchmark_base {
	static constexpr bool use_timing = true;

	// Make sure we have enough space for at least 4 (not 3 so it's PO2) windows where each window supports HW threads with HW blocks each with HW cells each.
	static inline size_t size = 4 * 64 * 64 * 64;
	// TODO: Temporarily downsized!
};

struct benchmark_default : benchmark_base {
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

struct benchmark_quality : benchmark_base {
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

	template <typename T>
	void output(T& stream) {
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
			uint64_t i = &pair - pushed_to_popped.data();
			auto& [pushed, popped] = pair;
			auto [popped_min, popped_max] = std::equal_range(popped_vec.begin(), popped_vec.end(), popped);
			uint64_t popped_index_max = popped_max - popped_vec.begin();
			uint64_t max_new = popped_index_max > i ? popped_index_max - i : i - popped_index_max;
			uint64_t max_curr = max;
			while (max_curr < max_new && !max.compare_exchange_strong(max_curr, max_new)) { }
			uint64_t popped_index_doubled = 2 * (popped_min - popped_vec.begin()) + (popped_max - popped_min);
			return popped_index_doubled > 2 * i ? popped_index_doubled - 2 * i : 2 * i - popped_index_doubled;
		});
		stream << total_diff_doubled / 2.0 / pushed_to_popped.size() << ',' << max << ',' << pushed_to_popped.size();
	}
};

struct benchmark_fill {
	static constexpr bool use_timing = false;
	static constexpr size_t size = 1 << 28;

	std::vector<uint64_t> results;
	uint64_t time_nanos;

	benchmark_fill(size_t num_threads) : results(num_threads) { }

	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, std::atomic_bool&) {
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		while (handle.push(thread_index + 1)) {
			results[thread_index]++;
		}
		std::cout << "DONE" << std::endl;
	}

	template <typename T>
	void output(T& stream) {
		stream << (double)std::reduce(results.begin(), results.end()) / size << ',' << time_nanos;
	}
};

struct benchmark_empty : benchmark_fill {
	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, std::atomic_bool&) {
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		while (handle.pop().has_value()) {
			results[thread_index]++;
		}
	}
};

template <typename BENCHMARK>
class benchmark_provider {
public:
	virtual ~benchmark_provider() = default;
	virtual std::vector<BENCHMARK> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const = 0;
	virtual const std::string& get_name() const = 0;

protected:
	template <typename FIFO>
	static BENCHMARK test_single(size_t num_threads, size_t test_time_seconds, double prefill_amount) {
		FIFO fifo{BENCHMARK::size};
		auto handle = fifo.get_handle();
		for (size_t i = 0; i < prefill_amount * BENCHMARK::size; i++) {
			handle.push(i + 1);
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
		auto start = std::chrono::steady_clock::now();
		a.arrive_and_wait();
		if constexpr (BENCHMARK::use_timing) {
			std::this_thread::sleep_until(start + std::chrono::seconds(test_time_seconds));
			over = true;
		}
		auto joined = std::async([&]() {
			for (auto& thread : threads) {
				thread.join();
			}
		});

		if constexpr (BENCHMARK::use_timing) {
			if (joined.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
				std::cout << "Threads did not complete within timeout, assuming deadlock!" << std::endl;
				std::exit(1);
			}
		} else {
			joined.wait();
			b.time_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		}

		return b;
	}
};

template <typename FIFO, typename BENCHMARK>
class benchmark_provider_generic : public benchmark_provider<BENCHMARK> {
public:
	benchmark_provider_generic(std::string name) : name(std::move(name)) { }

	const std::string& get_name() const override {
		return name;
	}

	std::vector<BENCHMARK> test(size_t num_threads, size_t num_its, size_t test_time_seconds, double prefill_amount) const override {
		std::vector<BENCHMARK> results(num_its, BENCHMARK(num_threads));
		for (auto i : std::views::iota((size_t)0, num_its)) {
			results[i] = benchmark_provider<BENCHMARK>::template test_single<FIFO>(num_threads, test_time_seconds, prefill_amount);
		}
		return results;
	}

private:
	std::string name;
};

template <size_t BLOCK_MULTIPLIER, typename BENCHMARK>
class benchmark_provider_relaxed : public benchmark_provider<BENCHMARK> {
	public:
		benchmark_provider_relaxed(std::string name) : name(std::move(name)) { }

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
			case 1: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 2: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 2 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 4: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 4 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 8: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 8 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 16: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 16 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 32: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 32 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 64: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 64 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 128: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 128 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 256: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 256 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 512: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 512 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			case 1024: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1024 * BLOCK_MULTIPLIER>>(num_threads, test_time_seconds, prefill_amount);
			default: throw std::runtime_error("Unsupported thread count!");
			}
		}
};

#endif // BENCHMARK_H_INCLUDED
