#ifndef ATOMIC_BITSET_H_INCLUDED
#define ATOMIC_BITSET_H_INCLUDED

#include <cstdint>
#include <type_traits>
#include <array>
#include <atomic>
#include <cassert>

template <size_t BIT_COUNT>
struct min_fit_int {
    using type = std::conditional_t<BIT_COUNT <= 8, uint8_t,
                     std::conditional_t<BIT_COUNT <= 16, uint16_t,
                         std::conditional_t<BIT_COUNT <= 32, uint32_t,
                             std::conditional_t<BIT_COUNT <= 64, uint64_t,
                                 void>>>>;
};

template <bool SET, typename T>
constexpr bool set_bit_atomic(std::atomic<T>& data, size_t index) {
    uint8_t old_val;
    uint8_t new_val;
    do {
        old_val = data;
        if constexpr (SET) {
            new_val = old_val | (1 << index);
        } else {
            new_val = old_val & ~(1 << index);
        }
        if (old_val == new_val) {
            return false;
        }
    } while (!data.compare_exchange_weak(old_val, new_val));
    return true;
}

template <size_t N>
class atomic_bitset {
private:
    using internal_type = typename min_fit_int<N>::type;

    std::conditional_t<N <= 64, std::atomic<internal_type>, std::array<std::atomic<uint8_t>, N / 8 + (N % 8 ? 1 : 0)>> data;

    static constexpr bool uses_array = N > 64;

public:
    static constexpr size_t size() { return N; }

    /// <summary>
    /// Sets a specified bit in the bitset to 1.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been set.</returns>
    constexpr bool set(size_t index) {
        assert(index < size());
        if constexpr (uses_array) {
            return set_bit_atomic<true>(data[index / 8], index % 8);
        } else if constexpr (!uses_array) {
            return set_bit_atomic<true>(data, index);
        }
    }

    /// <summary>
    /// Resets a specified bit in the bitset to 0.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been set.</returns>
    constexpr bool reset(size_t index) {
        assert(index < size());
        if constexpr (uses_array) {
            return set_bit_atomic<false>(data[index / 8], index % 8);
        } else if constexpr (!uses_array) {
            return set_bit_atomic<false>(data, index);
        }
    }

    constexpr bool test(size_t index, std::memory_order order = std::memory_order_seq_cst) const {
        assert(index < size());
        if constexpr (uses_array) {
            return data[index / 8].load(order) & (1 << (index % 8));
        }
        else if constexpr (!uses_array) {
            return data.load(order) & (1 << index);
        }
    }

    constexpr bool operator[](size_t index) const {
        return test(index);
    }
};

#endif // ATOMIC_BITSET_H_INCLUDED
