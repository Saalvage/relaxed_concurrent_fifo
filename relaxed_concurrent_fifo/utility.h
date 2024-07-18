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

template <template <typename> typename Fifo, typename Elem>
class wrapper_handle {
	friend Fifo<Elem>;

private:
	Fifo<Elem>* fifo;

	wrapper_handle(Fifo<Elem>* fifo) : fifo(fifo) { }

public:
	bool push(Elem t) { return fifo->push(std::move(t)); }
	std::optional<Elem> pop() { return fifo->pop(); }
};

#endif // UTILITY_H_INCLUDED
