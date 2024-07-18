#ifndef UTILITY_H_INCLUDED
#define UTILITY_H_INCLUDED

static constexpr bool is_po2(size_t size) {
	size_t it = 1;
	while (it < size) {
		it *= 2;
	}
	return it == size;
}

static constexpr size_t modulo_po2(size_t dividend, size_t divisor) {
	return dividend & (divisor - 1);
}

#endif // UTILITY_H_INCLUDED
