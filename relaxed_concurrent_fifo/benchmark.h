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

#include "replay_tree.hpp"

#include "relaxed_fifo.h"

struct benchmark_info {
	size_t num_threads;
	size_t test_time_seconds;
};

struct benchmark_base {
	static constexpr bool use_timing = true;

	// Make sure we have enough space for at least 4 (not 3 so it's PO2) windows where each window supports HW threads with HW blocks each with HW cells each.
	static inline size_t size = 4 * std::thread::hardware_concurrency() * std::thread::hardware_concurrency() * std::thread::hardware_concurrency();
};

struct benchmark_default : benchmark_base {
	std::vector<size_t> results;
	size_t test_time_seconds;

	benchmark_default(const benchmark_info& info) : results(info.num_threads), test_time_seconds(info.test_time_seconds) { }

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
		stream << std::reduce(results.begin(), results.end()) / test_time_seconds;
	}
};

struct benchmark_quality : benchmark_base {
private:
	std::atomic_uint64_t chunks_done = 0;

	std::vector<std::vector<std::tuple<uint64_t, uint64_t>>> results;

	struct pop_op {
		std::uint64_t pop_time;
		std::uint64_t push_time;
	};

	struct data_point {
		double avg;
		double std;
		std::uint64_t max;
	};

	static data_point analyze(const std::vector<std::uint64_t>& data) {
		double avg = std::reduce(std::execution::par_unseq, data.begin(), data.end()) / static_cast<double>(data.size());
		std::uint64_t max = 0;
		double std = std::accumulate(data.begin(), data.end(), 0., [&max, avg](double std_it, std::uint64_t new_val) {
			max = std::max(max, new_val);
			double diff = new_val - avg;
			return std_it + diff * diff;
			});
		std /= data.size();
		std = std::sqrt(std);
		return { avg, std, max };
	}

public:
	static constexpr bool use_timing = false;

	static constexpr size_t CHUNK_SIZE  = 5'000;
	static constexpr size_t CHUNK_COUNT = 1'000;

	uint64_t time_nanos; // Ignored.

	benchmark_quality(const benchmark_info& info) : results(info.num_threads) {
		// Double the amount of "expected" load for this thread.
		size_t size_per_thread = CHUNK_SIZE * CHUNK_COUNT / info.num_threads * 2;
		for (auto& vec : results) {
			vec.reserve(size_per_thread);
		}
	}

	benchmark_quality(const benchmark_quality& other) : results(other.results) { }
	benchmark_quality& operator=(const benchmark_quality& other) {
		results = other.results;
		return *this;
	}

	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, [[maybe_unused]] std::atomic_bool& over) {
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		do {
			for (size_t i = 0; i < CHUNK_SIZE; i++) {
				handle.push(std::chrono::steady_clock::now().time_since_epoch().count());
				auto popped = handle.pop();
				assert(popped.has_value());
				results[thread_index].push_back({ popped.value(), std::chrono::steady_clock::now().time_since_epoch().count() });
			}
		} while (chunks_done.fetch_add(1) < CHUNK_COUNT);
	}

	template <typename T>
	void output(T& stream) {
		auto total_count = std::accumulate(results.begin(), results.end(), (size_t)0, [](size_t size, const auto& v) { return size + v.size(); });
		std::vector<pop_op> pops;
		pops.reserve(total_count);
		std::vector<uint64_t> pushes;
		pushes.reserve(total_count);
		for (const auto& thread_result : results) {
			for (const auto& [pushed, popped] : thread_result) {
				pops.emplace_back(popped, pushed);
				pushes.emplace_back(pushed);
			}
		}
		std::sort(std::execution::par_unseq, pops.begin(), pops.end(), [](const auto& a, const auto& b) { return a.pop_time < b.pop_time; });
		std::sort(std::execution::par_unseq, pushes.begin(), pushes.end());

		std::vector<std::uint64_t> rank_errors;
		std::vector<std::uint64_t> delays;
		rank_errors.reserve(pushes.size());
		delays.reserve(pushes.size());
		struct id {
			static std::uint64_t const& get(std::uint64_t const& value) { return value; }
		};
		ReplayTree<std::uint64_t, std::uint64_t, id> replay_tree{};
		auto push_it = pushes.begin();
		for (auto const& pop : pops) {
			while (push_it != pushes.end() && *push_it <= pop.pop_time) {
				replay_tree.insert(*push_it);
				++push_it;
			}
			assert(
				!replay_tree.empty());  // Assume push times are always smaller than
			// pop times, not guaranteed if timestamps
			// are taken in the wrong order
			auto [success, rank_error, delay] = replay_tree.erase_val(
				pop.push_time);   // Points to first element at the
			// same timestamp as the current pop
			assert(success);  // The element to pop has to be in the set
			rank_errors.emplace_back(rank_error);
			delays.emplace_back(delay);
		}

		auto [r_avg, r_std, r_max] = analyze(rank_errors);
		stream << r_avg << ',' << r_std << ',' << r_max << ',';
		auto [d_avg, d_std, d_max] = analyze(delays);
		stream << d_avg << ',' << d_std << ',' << d_max;
	}
};

struct benchmark_fill {
	static constexpr bool use_timing = false;

	static constexpr size_t size = 1 << 28;

	std::vector<uint64_t> results;
	uint64_t time_nanos;

	benchmark_fill(const benchmark_info& info) : results(info.num_threads) { }

	template <typename T>
	void per_thread(size_t thread_index, T& fifo, std::barrier<>& a, std::atomic_bool&) {
		auto handle = fifo.get_handle();
		a.arrive_and_wait();
		while (handle.push(thread_index + 1)) {
			results[thread_index]++;
		}
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
	virtual BENCHMARK test(size_t num_threads, size_t test_time_seconds, double prefill_amount) const = 0;
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
		BENCHMARK b{benchmark_info{num_threads, test_time_seconds}};
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

	BENCHMARK test(size_t num_threads, size_t test_time_seconds, double prefill_amount) const override {
		return benchmark_provider<BENCHMARK>::template test_single<FIFO>(num_threads, test_time_seconds, prefill_amount);
	}

private:
	std::string name;
};

template <typename BENCHMARK, size_t BLOCK_MULTIPLIER, size_t CELLS_PER_BLOCK, typename BITSET_TYPE = uint8_t>
class benchmark_provider_relaxed : public benchmark_provider<BENCHMARK> {
	public:
		benchmark_provider_relaxed(std::string name) : name(std::move(name)) { }

		const std::string& get_name() const override {
			return name;
		}

		BENCHMARK test(size_t num_threads, size_t test_time_seconds, double prefill_amount) const override {
			switch (num_threads) {
			case 1: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 2: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 2 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 4: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 4 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 8: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 8 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 16: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 16 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 32: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 32 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 64: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 64 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 128: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 128 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			case 256: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 256 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(num_threads, test_time_seconds, prefill_amount);
			default: throw std::runtime_error("Unsupported thread count!");
			}
		}

	private:
		std::string name;
};

#endif // BENCHMARK_H_INCLUDED
