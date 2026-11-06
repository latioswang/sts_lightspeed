//
// Created by gamerpuppy on 7/4/2021.
//

#ifndef STS_LIGHTSPEED_ACTIONQUEUE_H
#define STS_LIGHTSPEED_ACTIONQUEUE_H

#include "sts_common.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <cassert>


namespace sts {

    class BattleContext;

    // What std::function<void (BattleContext &)> did for the action queue, without the heap and the type-erased copy:
    // a function pointer and the lambda's captures in a fixed buffer. Captures must be trivially copyable and fit in
    // kCaptureBytes; the static asserts below name any lambda that doesn't. Copying one is copying its bytes, so a
    // BattleContext (whose queue holds 50 of these, live or stale) copies its queue with a memcpy.
    class ActionFunction {
    public:
        static constexpr std::size_t kCaptureBytes = 32;
        static constexpr std::size_t kCaptureAlign = alignof(void *);

        ActionFunction() = default;

        template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, ActionFunction>>>
        ActionFunction(F f) : invoke(&invokeAs<F>) {  // NOLINT: implicit, like std::function
            static_assert(sizeof(F) <= kCaptureBytes, "action lambda captures too much for ActionFunction");
            static_assert(alignof(F) <= kCaptureAlign, "action lambda capture is over-aligned");
            static_assert(std::is_trivially_copyable_v<F>, "action lambda captures something not trivially copyable");
            static_assert(std::is_trivially_destructible_v<F>, "action lambda captures something with a destructor");
            ::new (static_cast<void *>(storage)) F(f);
        }

        void operator()(BattleContext &bc) const { invoke(storage, bc); }
        explicit operator bool() const { return invoke != nullptr; }

    private:
        template <typename F>
        static void invokeAs(const void *captures, BattleContext &bc) {
            (*std::launder(static_cast<const F *>(captures)))(bc);
        }

        void (*invoke)(const void *, BattleContext &) = nullptr;
        alignas(kCaptureAlign) unsigned char storage[kCaptureBytes];
    };
    static_assert(std::is_trivially_copyable_v<ActionFunction>);

    struct Action {
        ActionFunction actFunc;
        bool clearOnCombatVictory = true;

        Action() = default;
        Action(ActionFunction a) : actFunc(std::move(a)) {}
        Action(ActionFunction a, bool b) : actFunc(std::move(a)), clearOnCombatVictory(b) {}
    };


    // Simple deque
    template<int capacity>
    struct ActionQueue {
        friend BattleContext;
        int front = 0;
        int back = 0;
        int size = 0;
        std::bitset<capacity> bits; // for the shouldClear field

#ifdef sts_action_queue_use_raw_array
        ActionFunction arr[capacity];
        ActionQueue() = default;
        ActionQueue(const ActionQueue &rhs) : size(rhs.size), back(rhs.back), front(rhs.front), bits(rhs.bits) {
            int cur = rhs.front;
            for (int i = 0; i < rhs.size; ++i) {
                if (cur >= capacity) {
                    cur = 0;
                }
                arr[cur] = rhs.arr[cur];
            }
        }
#else
        std::array<ActionFunction,capacity> arr;
#endif

        void clear();
        void pushFront(Action a);
        void pushBack(Action a);
        bool isEmpty();
        ActionFunction popFront();
        [[nodiscard]] int getCapacity() const;
    };

    template<int capacity>
    void ActionQueue<capacity>::clear() {
        size = 0;
        back = 0;
        front = 0;
    }

    template <int capacity>
    void ActionQueue<capacity>::pushFront(Action a) {
        // Only a huge X cost fills the queue (Multi-Cast at 100 energy with Juggernaut). Dropping an action beats writing over one.
        if (size >= capacity) {
            return;
        }
        --front;
        ++size;
        if (front < 0) {
            front = capacity-1;
        }
        bits.set(front, a.clearOnCombatVictory);
        arr[front] = std::move(a.actFunc);
    }

    template<int capacity>
    void ActionQueue<capacity>::pushBack(Action a) {
        if (size >= capacity) {
            return;
        }
        if (back >= capacity) {
            back = 0;
        }
        bits.set(back, a.clearOnCombatVictory);
        arr[back] = std::move(a.actFunc);
        ++back;
        ++size;
    }

    template<int capacity>
    bool ActionQueue<capacity>::isEmpty() {
        return size == 0;
    }

    template<int capacity>
    ActionFunction ActionQueue<capacity>::popFront() {
#ifdef sts_asserts
        assert(size > 0 );
#endif
        ActionFunction a = arr[front];
        ++front;
        --size;
        if (front >= capacity) {
            front = 0;
        }
        return a;
    }

    template<int capacity>
    int ActionQueue<capacity>::getCapacity() const {
        return capacity;
    }

}


#endif //STS_LIGHTSPEED_ACTIONQUEUE_H
