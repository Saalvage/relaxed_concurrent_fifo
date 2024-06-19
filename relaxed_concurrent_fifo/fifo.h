#ifndef FIFO_H_INCLUDED
#define FIFO_H_INCLUDED

#include <concepts>
#include <optional>

template <template <typename, size_t> typename T>
concept fifo = std::constructible_from<size_t> && requires(T<int, 32> t, int u) {
	{ t.push(u) } -> std::same_as<bool>;
	{ t.pop() } -> std::same_as<std::optional<int>>;
};

#endif // FIFO_H_INCLUDEDs
