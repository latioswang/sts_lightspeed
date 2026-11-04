//
// Created by gamerpuppy on 8/24/2021.
//

#ifndef STS_LIGHTSPEED_CARDSELECTINFO_H
#define STS_LIGHTSPEED_CARDSELECTINFO_H

#include <array>

#include "constants/Cards.h"

namespace sts {

    enum class CardSelectTask {
        INVALID=0,
        ARMAMENTS,
        CODEX,
        DISCOVERY,
        DUAL_WIELD,
        EXHAUST_ONE,
        EXHAUST_MANY,
        EXHUME,
        FORETHOUGHT,
        GAMBLE,
        HEADBUTT,
        HOLOGRAM,
        LIQUID_MEMORIES_POTION,
        MEDITATE,
        NIGHTMARE,
        RECYCLE,
        SECRET_TECHNIQUE,
        SECRET_WEAPON,
        SEEK,
        SETUP,
        WARCRY,
        // Added for the Silent's cards. They go last so the ids above keep their values.
        DISCARD,
        RETAIN,
    };

    static constexpr const char* cardSelectTaskStrings[] = {
            "INVALID",
            "ARMAMENTS",
            "CODEX",
            "DISCOVERY",
            "DUAL_WIELD",
            "EXHAUST_ONE",
            "EXHAUST_MANY",
            "EXHUME",
            "FORETHOUGHT",
            "GAMBLE",
            "HEADBUTT",
            "HOLOGRAM",
            "LIQUID_MEMORIES_POTION",
            "MEDITATE",
            "NIGHTMARE",
            "RECYCLE",
            "SECRET_TECHNIQUE",
            "SECRET_WEAPON",
            "SEEK",
            "SETUP",
            "WARCRY",
            "DISCARD",
            "RETAIN",
    };

    struct CardSelectInfo {
        std::array<CardId, 3> cards;
        bool canPickZero = false;
        bool canPickAnyNumber = false;
        int pickCount = 0;
        int data0 = 0;
        bool discoveryIsFree = true; // Toolbox shares the screen, but its card keeps its cost

        CardSelectTask cardSelectTask;

        std::array<CardId, 3>& discovery_Cards() { return cards; }
        int& discovery_CopyCount() { return data0; }
        int& dualWield_CopyCount() { return data0; }
        std::array<CardId, 3>& codexCards() { return cards; }
        int& retain_SelectedCount() { return data0; } // the cards already picked sit at the end of the hand
        int& discard_FlyingFrom() { return data0; } // see BattleContext::discardFromHand

        // The RETAIN action that picks nothing more and closes the screen: no hand index reaches it.
        static constexpr int RETAIN_DONE_IDX = 10;
    };

}

#endif //STS_LIGHTSPEED_CARDSELECTINFO_H
