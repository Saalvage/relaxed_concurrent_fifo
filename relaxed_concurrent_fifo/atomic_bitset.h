#ifndef ATOMIC_BITSET_H_INCLUDED
#define ATOMIC_BITSET_H_INCLUDED

#include <cstdint>
#include <array>
#include <atomic>
#include <cassert>
#include <limits>
#include <random>

template <bool SET, typename T>
constexpr bool set_bit_atomic(std::atomic<T>& data, size_t index, std::memory_order order = std::memory_order_seq_cst) {
    T mask = static_cast<T>(1) << index;
    if constexpr (SET) {
        return !(data.fetch_or(mask, order) & mask);
    } else {
        return data.fetch_and(~mask, order) & mask;
    }
}

template <size_t N, typename ARR_TYPE = uint8_t>
class atomic_bitset {
private:
    static constexpr size_t bit_count = sizeof(ARR_TYPE) * 8;
    static constexpr size_t array_members = N / bit_count;
    std::array<std::atomic<ARR_TYPE>, array_members> data;

    // This requirement could be lifted in exchange for a more complicated implementation of the claim bit function.
    static_assert(N % bit_count == 0, "Bit count must be dividable by size of array type!");

    template <bool IS_SET, bool SET>
    static constexpr std::size_t claim_bit_singular(std::atomic<ARR_TYPE>& data, int initial_rot, std::memory_order order) {
        auto raw = data.load(order);
        ARR_TYPE rotated;
        while (true) {
            rotated = std::rotr(raw, initial_rot);
            int counted = IS_SET ? std::countr_zero(rotated) : std::countr_one(rotated);
            if (counted == bit_count) {
                return std::numeric_limits<std::size_t>::max();
            }
            std::size_t original_index = (initial_rot + counted) % bit_count;
            if constexpr (SET) {
                ARR_TYPE test;
                while (true) {
                    if constexpr (IS_SET) {
                        test = raw & ~(1ull << original_index);
                    } else {
                        test = raw | (1ull << original_index);
                    }
                    if (test == raw) {
                        break;
                    }
                    if (data.compare_exchange_weak(raw, test, order)) {
                        return original_index;
                    }
                }
            } else {
                return original_index;
            }
        }
    }

public:
    [[nodiscard]] static constexpr size_t size() { return N; }

    /// <summary>
    /// Sets a specified bit in the bitset to 1.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been 1.</returns>
    constexpr bool set(size_t index, std::memory_order order = std::memory_order_seq_cst) {
        assert(index < size());
        return set_bit_atomic<true>(data[index / bit_count], index % bit_count, order);
    }

    /// <summary>
    /// Resets a specified bit in the bitset to 0.
    /// </summary>
    /// <param name="index">The index of the bit to reset.</param>
    /// <returns>Whether the bit has been newly reset. false means the bit had already been 0.</returns>
    constexpr bool reset(size_t index, std::memory_order order = std::memory_order_seq_cst) {
        assert(index < size());
        return set_bit_atomic<false>(data[index / bit_count], index % bit_count, order);
    }

    [[nodiscard]] constexpr bool test(size_t index, std::memory_order order = std::memory_order_seq_cst) const {
        assert(index < size());
        return data[index / bit_count].load(order) & (1ull << (index % bit_count));
    }

    [[nodiscard]] constexpr bool operator[](size_t index) const {
        return test(index);
    }

    [[nodiscard]] constexpr bool any(std::memory_order order = std::memory_order_seq_cst) const {
        for (auto& elem : data) {
            if (elem.load(order)) {
                return true;
            }
        }
        return false;
    }

    template <bool IS_SET, bool SET>
    size_t claim_bit(std::memory_order order = std::memory_order_seq_cst) {
        static thread_local std::random_device dev;
        static thread_local std::minstd_rand rng{ dev() };
        static thread_local std::uniform_int_distribution dist_inner{ 0, static_cast<int>(N - 1) };
        static thread_local std::uniform_int_distribution dist_outer{ 0, static_cast<int>(array_members - 1) };

        int off;
        if constexpr (array_members > 1) {
            off = dist_outer(rng);
        } else {
            off = 0;
        }
        auto initial_rot = dist_inner(rng);
        for (size_t i = 0; i < data.size(); i++) {
            auto index = (i + off) % data.size();
            if (auto ret = claim_bit_singular<IS_SET, SET>(data[index], initial_rot, order); ret != std::numeric_limits<size_t>::max()) {
                return ret + index * bit_count;
            }
        }
        return std::numeric_limits<size_t>::max();
    }
};

#endif // ATOMIC_BITSET_H_INCLUDED
