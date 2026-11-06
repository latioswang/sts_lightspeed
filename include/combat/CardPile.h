#ifndef STS_LIGHTSPEED_CARDPILE_H
#define STS_LIGHTSPEED_CARDPILE_H

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "combat/CardInstance.h"

namespace sts {

    // A std::vector<CardInstance> for the draw, discard and exhaust piles that keeps up to kInline cards in place and
    // only goes to the heap past that. A pile has no limit in the game, and some rollouts grow one past any fixed size
    // (Dead Branch with Corruption), so this grows like a vector rather than dropping cards. Copying copies the cards
    // in use only. The interface is the part of std::vector the simulator and research code use, with the same
    // semantics: size() is a size_t, insert and erase keep the order, push_back of one of its own cards works.
    class CardPile {
    public:
        static constexpr std::size_t kInline = 48;

        using value_type = CardInstance;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = CardInstance &;
        using const_reference = const CardInstance &;
        using pointer = CardInstance *;
        using const_pointer = const CardInstance *;
        using iterator = CardInstance *;
        using const_iterator = const CardInstance *;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        CardPile() = default;
        CardPile(const CardPile &rhs) { assign(rhs.begin(), rhs.end()); }
        CardPile(CardPile &&rhs) noexcept { take(rhs); }
        CardPile(std::initializer_list<CardInstance> l) { assign(l.begin(), l.end()); }
        explicit CardPile(const std::vector<CardInstance> &v) { assign(v.data(), v.data() + v.size()); }
        ~CardPile() = default;

        CardPile &operator=(const CardPile &rhs) {
            if (this != &rhs) assign(rhs.begin(), rhs.end());
            return *this;
        }
        CardPile &operator=(CardPile &&rhs) noexcept {
            if (this != &rhs) { heap.reset(); cap = kInline; count = 0; take(rhs); }
            return *this;
        }
        CardPile &operator=(const std::vector<CardInstance> &v) {
            assign(v.data(), v.data() + v.size());
            return *this;
        }
        // For code written against the vector, like SelfTest's helpers taking a const std::vector<CardInstance> &.
        operator std::vector<CardInstance>() const { return {begin(), end()}; }  // NOLINT: implicit on purpose

        [[nodiscard]] size_type size() const { return count; }
        [[nodiscard]] bool empty() const { return count == 0; }
        [[nodiscard]] size_type capacity() const { return cap; }

        CardInstance *data() { return heap ? heap.get() : local.cards; }
        [[nodiscard]] const CardInstance *data() const { return heap ? heap.get() : local.cards; }

        iterator begin() { return data(); }
        iterator end() { return data() + count; }
        [[nodiscard]] const_iterator begin() const { return data(); }
        [[nodiscard]] const_iterator end() const { return data() + count; }
        [[nodiscard]] const_iterator cbegin() const { return begin(); }
        [[nodiscard]] const_iterator cend() const { return end(); }
        reverse_iterator rbegin() { return reverse_iterator(end()); }
        reverse_iterator rend() { return reverse_iterator(begin()); }
        [[nodiscard]] const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
        [[nodiscard]] const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }

        CardInstance &operator[](size_type i) { return data()[i]; }
        const CardInstance &operator[](size_type i) const { return data()[i]; }
        CardInstance &at(size_type i) {
            if (i >= count) throw std::out_of_range("vector");
            return data()[i];
        }
        [[nodiscard]] const CardInstance &at(size_type i) const {
            if (i >= count) throw std::out_of_range("vector");
            return data()[i];
        }
        CardInstance &front() { return data()[0]; }
        [[nodiscard]] const CardInstance &front() const { return data()[0]; }
        CardInstance &back() { return data()[count - 1]; }
        [[nodiscard]] const CardInstance &back() const { return data()[count - 1]; }

        void reserve(size_type n) {
            if (n > cap) regrow(n);
        }

        void push_back(const CardInstance &c) {
            const CardInstance copy = c;  // c may be one of this pile's cards
            if (count == cap) regrow(cap * 2);
            data()[count++] = copy;
        }

        template <typename... Args>
        CardInstance &emplace_back(Args &&...args) {
            const CardInstance made(std::forward<Args>(args)...);
            push_back(made);
            return back();
        }

        void pop_back() { --count; }

        iterator insert(const_iterator pos, const CardInstance &c) {
            const CardInstance copy = c;
            const auto at = static_cast<size_type>(pos - begin());
            if (count == cap) regrow(cap * 2);
            CardInstance *d = data();
            std::copy_backward(d + at, d + count, d + count + 1);
            d[at] = copy;
            ++count;
            return d + at;
        }

        iterator erase(const_iterator pos) {
            const auto at = static_cast<size_type>(pos - begin());
            CardInstance *d = data();
            std::copy(d + at + 1, d + count, d + at);
            --count;
            return d + at;
        }

        iterator erase(const_iterator first, const_iterator last) {
            const auto at = static_cast<size_type>(first - begin());
            const auto n = static_cast<size_type>(last - first);
            CardInstance *d = data();
            std::copy(d + at + n, d + count, d + at);
            count -= n;
            return d + at;
        }

        void clear() { count = 0; }

        void resize(size_type n) { resize(n, CardInstance()); }
        void resize(size_type n, const CardInstance &value) {
            const CardInstance copy = value;
            if (n > cap) regrow(std::max(n, cap * 2));
            CardInstance *d = data();
            for (size_type i = count; i < n; ++i) d[i] = copy;
            count = n;
        }

        template <typename It>
        void assign(It first, It last) {
            const auto n = static_cast<size_type>(std::distance(first, last));
            count = 0;
            if (n > cap) regrow(n);
            std::copy(first, last, data());
            count = n;
        }

    private:
        static_assert(std::is_trivially_copyable_v<CardInstance>);

        // Moves the cards into a buffer of n >= count.
        void regrow(size_type n) {
            std::unique_ptr<CardInstance[]> bigger(new CardInstance[n]);
            std::copy(begin(), end(), bigger.get());
            heap = std::move(bigger);
            cap = n;
        }

        void take(CardPile &rhs) {
            if (rhs.heap) {
                heap = std::move(rhs.heap);
                cap = rhs.cap;
                count = rhs.count;
            } else {
                std::copy(rhs.begin(), rhs.end(), local.cards);
                count = rhs.count;
            }
            rhs.heap.reset();
            rhs.cap = kInline;
            rhs.count = 0;
        }

        // The cards past count are never read, so the inline buffer isn't initialised.
        union Local {
            Local() {}  // NOLINT: leaves the cards uninitialised on purpose
            CardInstance cards[kInline];
        };

        size_type count = 0;
        size_type cap = kInline;
        std::unique_ptr<CardInstance[]> heap;
        Local local;
    };

}

#endif //STS_LIGHTSPEED_CARDPILE_H
