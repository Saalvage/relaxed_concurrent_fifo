#ifndef ATOMIC_BITSET_H_INCLUDED
#define ATOMIC_BITSET_H_INCLUDED

#include <cstdint>
#include <type_traits>
#include <array>
#include <atomic>
#include <cassert>

template <bool SET, typename T>
constexpr bool set_bit_atomic(std::atomic<T>& data, size_t index, std::memory_order order = std::memory_order_seq_cst) {
    T old_val;
    T new_val;
    do {
        old_val = data.load(order);
        if constexpr (SET) {
            new_val = old_val | (1ull << index);
        } else {
            new_val = old_val & ~(1ull << index);
        }
        if (old_val == new_val) {
            return false;
        }
    } while (!data.compare_exchange_weak(old_val, new_val, order));
    return true;
}

template <size_t N, typename ARR_TYPE = uint8_t>
class atomic_bitset {
private:
    static_assert(N % (sizeof(ARR_TYPE) * 8) == 0, "Bit count must be dividable by size of array type!");

    static constexpr size_t bit_count = sizeof(ARR_TYPE) * 8;
    static constexpr size_t array_members = N / bit_count + (N % bit_count ? 1 : 0);
    std::array<std::atomic<ARR_TYPE>, array_members> data;

    template <size_t BIT_COUNT>
    struct min_fit_int {
        using type = std::conditional_t<BIT_COUNT <= 8, uint8_t,
            std::conditional_t<BIT_COUNT <= 16, uint16_t,
            std::conditional_t<BIT_COUNT <= 32, uint32_t,
            uint64_t>>>;
    };

    template <bool IS_SET, bool SET>
    static constexpr size_t claim_bit_singular(std::atomic<ARR_TYPE>& data, int initial_rot, std::memory_order order) {
        constexpr size_t actual_size = sizeof(ARR_TYPE) * 8;

        auto rotated = std::rotr(data.load(order), initial_rot);
        int counted;
        for (size_t i = 0; i < actual_size; i += counted) {
            counted = IS_SET ? std::countr_zero(rotated) : std::countr_one(rotated);
            if (counted == actual_size) {
                return std::numeric_limits<size_t>::max();
            }
            size_t original_index = (initial_rot + i + counted) % actual_size;
            if (!SET || set_bit_atomic<!IS_SET>(data, original_index, order)) {
                return original_index;
            }
            rotated >>= ++counted;
        }

        return std::numeric_limits<size_t>::max();
    }

public:
    static constexpr size_t size() { return N; }

    /// <summary>
    /// Sets a specified bit in the bitset to 1.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been set.</returns>
    constexpr bool set(size_t index, std::memory_order order = std::memory_order_seq_cst) {
        assert(index < size());
        return set_bit_atomic<true>(data[index / bit_count], index % bit_count, order);
    }

    /// <summary>
    /// Resets a specified bit in the bitset to 0.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been set.</returns>
    constexpr bool reset(size_t index, std::memory_order order = std::memory_order_seq_cst) {
        assert(index < size());
        return set_bit_atomic<false>(data[index / bit_count], index % bit_count, order);
    }

    constexpr bool test(size_t index, std::memory_order order = std::memory_order_seq_cst) const {
        assert(index < size());
        return data[index / bit_count].load(order) & (1ull << (index % bit_count));
    }

    constexpr bool operator[](size_t index) const {
        return test(index);
    }

    constexpr bool any(std::memory_order order = std::memory_order_seq_cst) const {
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

    // Simple, alternative implementation for claim_bit.
    template <bool IS_SET, bool SET>
    size_t claim_bit_simple(std::memory_order order = std::memory_order_seq_cst) {
        static thread_local std::random_device dev;
        static thread_local std::minstd_rand rng{ dev() };
        static thread_local std::uniform_int_distribution dist_inner{ 0, static_cast<int>(N - 1) };
        static thread_local std::uniform_int_distribution dist_outer{ 0, static_cast<int>(array_members - 1) };

        auto offset_outer = dist_outer(rng);
        auto offset_inner = dist_inner(rng);

        constexpr size_t BITS_IN_VAL = 8 * sizeof(ARR_TYPE);

        for (size_t i = 0; i < array_members; i++) {
            auto index_outer = (i + offset_outer) % array_members;
            auto val = data[index_outer].load(order);
            for (size_t j = 0; j < BITS_IN_VAL; j++) {
                auto index_inner = (j + offset_inner) % BITS_IN_VAL;
                if (static_cast<bool>(val & (1 << index_inner)) == IS_SET && (!SET || set_bit_atomic<!IS_SET>(data[index_outer], index_inner, order))) {
                    return (index_outer * BITS_IN_VAL + index_inner) % N;
                }
            }
        }

        return std::numeric_limits<size_t>::max();
    }
};

#endif // ATOMIC_BITSET_H_INCLUDED
