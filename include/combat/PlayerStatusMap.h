#ifndef STS_LIGHTSPEED_PLAYERSTATUSMAP_H
#define STS_LIGHTSPEED_PLAYERSTATUSMAP_H

#include <array>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "constants/PlayerStatusEffects.h"

namespace sts {

    // What std::map<PlayerStatus, std::int16_t> did for Player::statusMap, in a flat array: one slot per status and a
    // bit for each key the map would hold. A key, once written, stays until clear(), as it did in the map (nothing
    // erased from it), and iteration goes in key order, as the map's did. at() throws for a missing key, like the map.
    // It is trivially copyable, so copying a Player copies it with a memcpy instead of a tree.
    class PlayerStatusMap {
    public:
        static constexpr int kKeys = 128;  // PlayerStatus is a uint8_t below 128 (statusBits0 and statusBits1)

        using key_type = PlayerStatus;
        using mapped_type = std::int16_t;
        using value_type = std::pair<PlayerStatus, std::int16_t>;
        using size_type = std::size_t;

        class const_iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = PlayerStatusMap::value_type;
            using difference_type = std::ptrdiff_t;
            using pointer = void;
            using reference = value_type;

            struct arrow {
                value_type pair;
                const value_type *operator->() const { return &pair; }
            };

            const_iterator() = default;
            const_iterator(const PlayerStatusMap *m, int idx) : m(m), idx(idx) {}

            value_type operator*() const { return {static_cast<PlayerStatus>(idx), m->values[idx]}; }
            arrow operator->() const { return {**this}; }
            const_iterator &operator++() { idx = m->nextKey(idx + 1); return *this; }
            const_iterator operator++(int) { auto ret = *this; ++*this; return ret; }
            bool operator==(const const_iterator &rhs) const { return idx == rhs.idx; }
            bool operator!=(const const_iterator &rhs) const { return idx != rhs.idx; }

        private:
            const PlayerStatusMap *m = nullptr;
            int idx = kKeys;
        };
        using iterator = const_iterator;

        [[nodiscard]] bool contains(PlayerStatus s) const {
            const int i = static_cast<int>(s);
            return (keyBits[i >> 6] >> (i & 63)) & 1ULL;
        }
        [[nodiscard]] size_type count(PlayerStatus s) const { return contains(s) ? 1 : 0; }

        std::int16_t &at(PlayerStatus s) {
            if (!contains(s)) throw std::out_of_range("map::at:  key not found");
            return values[static_cast<int>(s)];
        }
        [[nodiscard]] const std::int16_t &at(PlayerStatus s) const {
            if (!contains(s)) throw std::out_of_range("map::at:  key not found");
            return values[static_cast<int>(s)];
        }

        // Adds the key with 0 if it isn't there, like std::map::operator[].
        std::int16_t &operator[](PlayerStatus s) {
            const int i = static_cast<int>(s);
            keyBits[i >> 6] |= 1ULL << (i & 63);
            return values[i];
        }

        [[nodiscard]] const_iterator find(PlayerStatus s) const {
            return contains(s) ? const_iterator(this, static_cast<int>(s)) : end();
        }
        [[nodiscard]] const_iterator begin() const { return {this, nextKey(0)}; }
        [[nodiscard]] const_iterator end() const { return {this, kKeys}; }

        [[nodiscard]] size_type size() const {
            return static_cast<size_type>(__builtin_popcountll(keyBits[0]) + __builtin_popcountll(keyBits[1]));
        }
        [[nodiscard]] bool empty() const { return (keyBits[0] | keyBits[1]) == 0; }
        void clear() { *this = PlayerStatusMap(); }

    private:
        // The first key at or after idx, or kKeys.
        [[nodiscard]] int nextKey(int idx) const {
            while (idx < kKeys) {
                const std::uint64_t word = keyBits[idx >> 6] >> (idx & 63);
                if (word) return idx + __builtin_ctzll(word);
                idx = (idx | 63) + 1;
            }
            return kKeys;
        }

        std::array<std::uint64_t, 2> keyBits{};
        std::array<std::int16_t, kKeys> values{};  // 0 where there's no key
    };
    static_assert(static_cast<int>(PlayerStatus::STUDY) < PlayerStatusMap::kKeys, "a PlayerStatus past the map's keys");

}

#endif //STS_LIGHTSPEED_PLAYERSTATUSMAP_H
