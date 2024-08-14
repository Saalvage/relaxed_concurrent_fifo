#ifndef FIFO_H_INCLUDED
#define FIFO_H_INCLUDED

#include <concepts>
#include <optional>
#include <cstdint>

template <typename T, typename U>
concept fifo = std::constructible_from<size_t> && requires(T fifo, typename T::handle handle, U u) {
	{ fifo.get_handle() } -> std::same_as<typename T::handle>;
	{ handle.push(u) } -> std::same_as<bool>;
	{ handle.pop() } -> std::same_as<std::optional<U>>;
};

#endif // FIFO_H_INCLUDED
