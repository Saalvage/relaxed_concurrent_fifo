/**
******************************************************************************
* @file:   multififo.hpp
*
* @author: Marvin Williams
* @date:   2021/03/29 17:19
* @brief:
*******************************************************************************
**/
#pragma once

#include "handle.hpp"
#include "queue_guard.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <memory>

namespace multififo {

template <typename T, typename Allocator = std::allocator<T>>
class MultiFifo {
   public:
    using value_type = T;
    using reference = value_type &;
    using const_reference = value_type const &;
    using size_type = std::size_t;
    using allocator_type = Allocator;

   private:
    struct Element {
        std::uint64_t tick;
        value_type value;
    };
    using queue_type = RingBuffer<Element>;
    using guard_type = QueueGuard<queue_type>;
    using internal_allocator_type = typename std::allocator_traits<allocator_type>::template rebind_alloc<guard_type>;

    class Context {
        friend MultiFifo;

       public:
        using value_type = MultiFifo::value_type;
        using guard_type = MultiFifo::guard_type;
        using clock_type = std::chrono::steady_clock;

       private:
        int num_queues_{};
        guard_type *queue_guards_{nullptr};
        std::atomic_int id_count{0};
        int stickiness_{16};
        int seed_{1};
        [[no_unique_address]] internal_allocator_type alloc_;

        static constexpr size_t make_po2(size_t size) {
            size_t ret = 1;
            while (size > ret) {
                ret *= 2;
            }
            return ret;
        }

        explicit Context(int queue_count, size_t size, int stickiness, int seed,
                         allocator_type const &alloc)
            : num_queues_{queue_count},
              queue_guards_{std::allocator_traits<internal_allocator_type>::allocate(alloc_, num_queues_)},
              stickiness_{stickiness},
              seed_{seed},
              alloc_{alloc} {
            assert(num_queues_ > 0);

            auto cap_per_queue = make_po2(size / num_queues_);
            assert((cap_per_queue & (cap_per_queue - 1)) == 0);

            for (auto *it = queue_guards_; it != queue_guards_ + num_queues_; ++it) {
                std::allocator_traits<internal_allocator_type>::construct(alloc_, it, queue_type{cap_per_queue});
            }
        }

        ~Context() noexcept {
            for (auto *it = queue_guards_; it != queue_guards_ + num_queues_; ++it) {
                std::allocator_traits<internal_allocator_type>::destroy(alloc_, it);
            }
            std::allocator_traits<internal_allocator_type>::deallocate(alloc_, queue_guards_, num_queues_);
        }

       public:
        Context(const Context &) = delete;
        Context(Context &&) = delete;
        Context &operator=(const Context &) = delete;
        Context &operator=(Context &&) = delete;

        [[nodiscard]] constexpr size_type num_queues() const noexcept {
            return num_queues_;
        }

        [[nodiscard]] guard_type *queue_guards() const noexcept {
            return queue_guards_;
        }

        [[nodiscard]] int stickiness() const noexcept {
            return stickiness_;
        }

        [[nodiscard]] int seed() const noexcept {
            return seed_;
        }

        [[nodiscard]] int new_id() noexcept {
            return id_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    Context context_;

   public:
    using handle = Handle<Context>;

    explicit MultiFifo(int num_threads, size_t size, int thread_multiplier, int stickiness = 16,
                       int seed = 1, allocator_type const &alloc = {})
        : context_{num_threads * thread_multiplier, size, stickiness, seed,
                   internal_allocator_type(alloc)} {
    }

    handle get_handle() noexcept {
        return handle(context_);
    }

    [[nodiscard]] constexpr size_type num_queues() const noexcept {
        return context_.num_queues_;
    }

    [[nodiscard]] allocator_type get_allocator() const {
        return allocator_type_(context_.alloc_);
    }
};
}  // namespace multififo
