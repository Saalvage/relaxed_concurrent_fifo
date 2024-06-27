#ifndef FIFO_H_INCLUDED
#define FIFO_H_INCLUDED

#include <concepts>
#include <optional>

template <template <typename, size_t> typename T>
concept fifo = std::constructible_from<size_t> && requires(T<uint64_t, 32> fifo, T<uint64_t, 32>::handle handle, uint64_t u) {
	{ fifo.get_handle() } -> std::same_as<typename T<uint64_t, 32>::handle>;
	{ handle.push(u) } -> std::same_as<bool>;
	{ handle.pop() } -> std::same_as<std::optional<uint64_t>>;
};

#endif // FIFO_H_INCLUDEDs
