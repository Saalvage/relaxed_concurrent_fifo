#ifndef ATOMIC_BITSET_H_INCLUDED
#define ATOMIC_BITSET_H_INCLUDED

#include <cstdint>
#include <type_traits>
#include <array>
#include <atomic>
#include <cassert>

template <bool SET, typename T>
constexpr bool set_bit_atomic(std::atomic<T>& data, size_t index) {
    T old_val;
    T new_val;
    do {
        old_val = data;
        if constexpr (SET) {
            new_val = old_val | (1ull << index);
        } else {
            new_val = old_val & ~(1ull << index);
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
    std::array<std::atomic<uint64_t>, N / 64 + (N % 64 ? 1 : 0)> data;

    static inline thread_local std::random_device dev;
    static inline thread_local std::minstd_rand rng{ dev() };
    static inline thread_local std::uniform_int_distribution dist{ 0, static_cast<int>(N - 1) };

    template <bool IS_SET, bool WRITE>
    static constexpr size_t claim_bit_singular(std::atomic<uint64_t>& data) {
        auto initial_rot = dist(rng);
        auto rotated = std::rotr<uint64_t>(data, initial_rot);
        int counted;
        for (int i = 0; i < 64; i += counted) {
            counted = IS_SET ? std::countr_zero(rotated) : std::countr_one(rotated);
            auto original_index = (initial_rot + i + counted) % 64;
            if constexpr (WRITE) {
                if (set_bit_atomic<!IS_SET>(data, original_index)) {
                    return original_index;
                }
            } else {
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
    constexpr bool set(size_t index) {
        assert(index < size());
        return set_bit_atomic<true>(data[index / 64], index % 64);
    }

    /// <summary>
    /// Resets a specified bit in the bitset to 0.
    /// </summary>
    /// <param name="index">The index of the bit to set.</param>
    /// <returns>Whether the bit has been newly set. false means the bit had already been set.</returns>
    constexpr bool reset(size_t index) {
        assert(index < size());
        return set_bit_atomic<false>(data[index / 64], index % 64);
    }

    constexpr bool test(size_t index, std::memory_order order = std::memory_order_seq_cst) const {
        assert(index < size());
        return data[index / 64].load(order) & (1ull << (index % 64));
    }

    constexpr bool operator[](size_t index) const {
        return test(index);
    }

    constexpr bool any() const {
        for (auto& elem : data) {
            if (elem) {
                return true;
            }
        }
        return false;
    }

    template <bool IS_SET, bool WRITE>
    constexpr size_t claim_bit() {
        for (size_t i = 0; i < data.size(); i++) {
            if (auto ret = claim_bit_singular<IS_SET, WRITE>(data[i]); ret != std::numeric_limits<size_t>::max()) {
                return ret + i * 64;
            }
        }
        return std::numeric_limits<size_t>::max();
    }
};

#endif // ATOMIC_BITSET_H_INCLUDED
