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
#include <future>
#include <iostream>
#include <execution>

#include "replay_tree.hpp"

#include "thread_pool.h"
#include "relaxed_fifo.h"

// There are two components to a benchmark:
// The benchmark itself which dictates what each thread does and what exactly is being measured;
// and the benchmark provider, which effectively provides a concrete queue implementation to the benchmark.

struct benchmark_info {
	size_t num_threads;
	size_t test_time_seconds;
};

template <bool HAS_TIMEOUT_T = true, bool RECORD_TIME_T = false, bool PREFILL_IN_ORDER_T = false, size_t SIZE_T = 0>
struct benchmark_base {
	static constexpr bool HAS_TIMEOUT = HAS_TIMEOUT_T;
	static constexpr bool RECORD_TIME = RECORD_TIME_T;
	static constexpr bool PREFILL_IN_ORDER = PREFILL_IN_ORDER_T;

	// Make sure we have enough space for at least 4 (not 3 so it's PO2) windows where each window supports HW threads with HW blocks each with HW cells each.
	static const inline size_t SIZE = SIZE_T != 0 ? SIZE_T : 4 * std::thread::hardware_concurrency() * std::thread::hardware_concurrency() * std::thread::hardware_concurrency();
};

template <bool PREFILL_IN_ORDER = false, size_t SIZE = 0>
struct benchmark_timed : benchmark_base<false, true, PREFILL_IN_ORDER, SIZE> {
	uint64_t time_nanos;
};

struct benchmark_default : benchmark_base<> {
	std::vector<size_t> results;
	size_t test_time_seconds;

	benchmark_default(const benchmark_info& info) : results(info.num_threads), test_time_seconds(info.test_time_seconds) { }

	template <typename T>
	void per_thread(size_t thread_index, typename T::handle& handle, std::barrier<>& a, std::atomic_bool& over) {
		size_t its = 0;
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

struct benchmark_quality : benchmark_base<false, false, true> {
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
	static constexpr size_t CHUNK_SIZE  = 5'000;
	static constexpr size_t CHUNK_COUNT = 1'000;

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
	void per_thread(size_t thread_index, typename T::handle& handle, std::barrier<>& a) {
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
		auto total_count = std::accumulate(results.begin(), results.end(), static_cast<size_t>(0), [](size_t size, const auto& v) { return size + v.size(); });
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

struct benchmark_fill : benchmark_timed<false, 1 << 28> {
	std::vector<uint64_t> results;

	benchmark_fill(const benchmark_info& info) : results(info.num_threads) { }

	template <typename T>
	void per_thread(size_t thread_index, typename T::handle& handle, std::barrier<>& a) {
		a.arrive_and_wait();
		while (handle.push(thread_index + 1)) {
			results[thread_index]++;
		}
	}

	template <typename T>
	void output(T& stream) {
		stream << static_cast<double>(std::reduce(results.begin(), results.end())) / SIZE << ',' << time_nanos;
	}
};

struct benchmark_empty : benchmark_fill {
	template <typename T>
	void per_thread(size_t thread_index, typename T::handle& handle, std::barrier<>& a) {
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
	virtual BENCHMARK test(thread_pool& pool, size_t num_threads, size_t test_time_seconds, double prefill_amount) const = 0;
	virtual const std::string& get_name() const = 0;

protected:
	template <fifo FIFO>
	static BENCHMARK test_single(thread_pool& pool, size_t num_threads, size_t test_time_seconds, double prefill_amount) {
		FIFO fifo{num_threads, BENCHMARK::SIZE};

		std::vector<typename FIFO::handle> handles;
		handles.reserve(num_threads);
		std::atomic_size_t s = 0;
		std::condition_variable cv;
		std::mutex m;
		// We prefill with all threads since this may improve performance for certain implementations.
		pool.do_work([&](size_t idx, std::barrier<>& a) {
			// We want to execute these in order so that the indices in the array are correct
			// while still having been initialized by the correct thread.
			// We cannot rely on the default constructability of handles.
			std::unique_lock lock{m};
			cv.wait(lock, [&] { return s == idx; });
			handles.push_back(fifo.get_handle());
			++s;
			lock.unlock();
			cv.notify_all();
			a.arrive_and_wait();
			// If PREFILL_IN_ORDER is set we sequentially fill the queue from a single thread.
			if (BENCHMARK::PREFILL_IN_ORDER && idx != 0) {
				return;
			}
			for (size_t i = 0; i < prefill_amount * BENCHMARK::SIZE / (BENCHMARK::PREFILL_IN_ORDER ? 1 : num_threads); i++) {
				handles[idx].push(i + 1);
			}
		}, num_threads, true);

		std::atomic_bool over = false;
		BENCHMARK b{benchmark_info{num_threads, test_time_seconds}};

		auto joined = std::async(&thread_pool::do_work, &pool, [&](size_t i, std::barrier<>& a) {
			// Make sure handle is on the stack.
			typename FIFO::handle handle = std::move(handles[i]);
			if constexpr (BENCHMARK::HAS_TIMEOUT) {
				b.template per_thread<FIFO>(i, handle, a, over);
			} else {
				b.template per_thread<FIFO>(i, handle, a);
			}
		}, num_threads, false);
		// We signal, then start taking the time because some threads might not have arrived at the signal.
		pool.signal_and_wait();
		auto start = std::chrono::steady_clock::now();
		if constexpr (BENCHMARK::HAS_TIMEOUT) {
			std::this_thread::sleep_until(start + std::chrono::seconds(test_time_seconds));
			over = true;

			if (joined.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
				std::cout << "Threads did not complete within timeout, assuming deadlock!" << std::endl;
				std::exit(1);
			}
		} else {
			joined.wait();
			if constexpr (BENCHMARK::RECORD_TIME) {
				b.time_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
			}
		}

		return b;
	}
};

template <fifo FIFO, typename BENCHMARK>
class benchmark_provider_generic : public benchmark_provider<BENCHMARK> {
public:
	benchmark_provider_generic(std::string name) : name(std::move(name)) { }

	const std::string& get_name() const override {
		return name;
	}

	BENCHMARK test(thread_pool& pool, size_t num_threads, size_t test_time_seconds, double prefill_amount) const override {
		return benchmark_provider<BENCHMARK>::template test_single<FIFO>(pool, num_threads, test_time_seconds, prefill_amount);
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

		BENCHMARK test(thread_pool& pool, size_t num_threads, size_t test_time_seconds, double prefill_amount) const override {
			switch (num_threads) {
			case 1: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 1 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 2: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 2 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 4: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 4 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 8: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 8 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 16: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 16 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 32: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 32 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 64: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 64 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 128: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 128 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			case 256: return benchmark_provider<BENCHMARK>::template test_single<relaxed_fifo<size_t, 256 * BLOCK_MULTIPLIER, CELLS_PER_BLOCK, BITSET_TYPE>>(pool, num_threads, test_time_seconds, prefill_amount);
			default: throw std::runtime_error("Unsupported thread count!");
			}
		}

	private:
		std::string name;
};

#endif // BENCHMARK_H_INCLUDED
