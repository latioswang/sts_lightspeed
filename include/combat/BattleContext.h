//
// Created by gamerpuppy on 7/4/2021.
//

#ifndef STS_LIGHTSPEED_BATTLECONTEXT_H
#define STS_LIGHTSPEED_BATTLECONTEXT_H

#include <vector>
#include <array>

#include "sts_common.h"

#include "data_structure/fixed_list.h"
#include "constants/Potions.h"
#include "constants/MonsterEncounters.h"
#include "combat/InputState.h"
#include "combat/Player.h"
#include "combat/Monster.h"
#include "combat/MonsterGroup.h"
#include "combat/ActionQueue.h"
#include "combat/CardQueue.h"
#include "combat/Actions.h"
#include "combat/CardManager.h"
#include "combat/CardSelectInfo.h"


namespace sts {



    enum class Outcome {
        UNDECIDED=0,
        PLAYER_VICTORY,
        PLAYER_LOSS,
    };

    static constexpr const char * battleOutcomeStrings[] {
            "UNDECIDED",
            "PLAYER_VICTORY",
            "PLAYER_LOSS",
    };

    class GameContext;

    extern thread_local BattleContext *g_debug_bc;

    struct BattleContext {

        // begin for debugging purposes
        inline static int sum = 0; // for preventing optimization in benchmarks
        bool haveUsedDiscoveryAction = false; // for tracking undefined behavior resulting from using the action
        bool undefinedBehaviorEvoked = false; // some cards cause inconsistent outcomes in games
        std::uint64_t seed = 0;
        int floorNum = 0;
        MonsterEncounter encounter = MonsterEncounter::INVALID;
        int loopCount = 0;
        int energyWasted = 0;
        int cardsDrawn = 0;
        // end for debugging purposes

        Random aiRng;
        Random cardRandomRng;
        Random miscRng;
        Random monsterHpRng;
        Random potionRng;
        Random shuffleRng;
        Random cardRng; // only Foreign Influence rolls it in a fight, through CardGroup.getRandomCard

        int ascension = 0;
        Outcome outcome = Outcome::UNDECIDED;
        InputState inputState = InputState::EXECUTING_ACTIONS;
        CardSelectInfo cardSelectInfo;

        int monsterTurnIdx = 6;

        bool isBattleOver = false;
        bool endTurnQueued = false;
        bool turnHasEnded = false;
        bool skipMonsterTurn = false;
        int monsterTurnsSkipped = 0; // by Vault. A monster's own counters don't move on a turn it doesn't take

        ActionQueue<50> actionQueue;
        CardQueue cardQueue;

        int potionCount = 0;
        int potionCapacity = 3;
        std::array<Potion, 5> potions;

        int turn = 0;
        Player player;
        MonsterGroup monsters;
        CardManager cards;

        CardQueueItem curCardQueueItem;

        std::bitset<32> miscBits; // 0 stolen gold check, 1 escaped, 2 Stone Calendar gained before Nilry's Codex

        // Lesson Learned upgrades a random card in the master deck. These are filled in only if the deck holds one.
        static constexpr std::int16_t SEARING_BLOW_DECK_IDX = 0x4000; // a flag on an entry below: it stays after an upgrade
        std::vector<std::int16_t> upgradableDeckIdxs;
        std::vector<std::int16_t> lessonLearnedUpgrades;

        BattleContext() = default;
        BattleContext(const BattleContext &rhs) = default;

// ****************************************

        void init(const GameContext &gc);
        void init(const GameContext &gc, MonsterEncounter encounterToInit);

        void initRelics(const GameContext &gc);

        void exitBattle(GameContext &g) const;
        void updateRelicsOnExit(GameContext &g) const;
        void updateCardsOnExit(Deck &d) const; // for cards like ritual dagger, and eventually lesson learned results

// ****************************************

        void setRequiresStolenGoldCheck(bool value);
        [[nodiscard]] bool requiresStolenGoldCheck() const;
        [[nodiscard]] int getMonsterTurnNumber() const;  // returns 1 for first turn, 2 for second, etc.

// ****************************************

        void executeActions();

        void playCardQueueItem(CardQueueItem);
        void useCard();
        void useNoTriggerCard();
        void noteCardTypePlayed(CardType type);

        void useAttackCard();
        void useSkillCard();
        void usePowerCard();

        void onUseAttackCard();
        void onUseSkillCard();
        void onUsePowerCard();

        void onUseStatusOrCurseCard();
        void onAfterUseCard();

        void setState(InputState state);
        void addToTop(const Action &a);
        void addToBot(const Action &a);
        void addToTop(Action &&a);  // PERF: temporaries are moved into the queue instead of copied (same behaviour)
        void addToBot(Action &&a);

        void addToTopCard(CardQueueItem item);
        void addToBotCard(CardQueueItem item);

        void checkCombat();
        void clearPostCombatActions();
        void cleanCardQueue();

        [[nodiscard]] bool isCardPlayAllowed() const;

        // **********************

        void endTurn(); // called when player clicks the end turn button
        void callEndOfTurnActions(); // GameActionManager.callEndTurnActions(), called when the end turn cardQueue item is reached.
        void onTurnEnding(); // AbstractRoom endTurn(), called when the turn actually ends.

        void callEndTurnEarlySequence(bool queueStillRunning=false); // time eater, and PressEndTurnButtonAction

        void applyEndOfRoundPowers(); // game : MonsterGroup applyEndOfTurnPowers
        void afterMonsterTurns();

        void obtainPotion(Potion p);
        void discardPotion(int idx);
        void drinkPotion(int idx, int target=0);

        void drawCards(int count);
        void discardAtEndOfTurn();
        void discardAtEndOfTurnHelper();

        void playTopCardInDrawPile(int monsterTargetIdx, bool exhausts);

        void applyMakeCopy(CardInstance &c) const;
        void moveToHandHelper(CardInstance c);
        void upgradeIfMasterReality(CardInstance &c) const; // what MakeTempCard and the card effects do with Master Reality
        void changeStance(Stance newStance); // ChangeStanceAction
        void applyMantra(int amount);
        void queueMantra(int amount);
        void scry(int amount); // ScryAction
        void finishScry();
        template <typename F> void forEachInBattleInstance(std::int16_t uniqueId, F f); // GetAllInBattleInstances
        void exhaustSpecificCardInHand(int idx, std::int16_t uniqueId);
        void restoreRetainedCards(int count);
        void exhaustTopCardInHand();

        // this is
        void triggerOnEndOfTurnForPlayingCards();
        void triggerOnOtherCardPlayed(const CardInstance &usedCard);

        template <MonsterStatus s>
        void debuffEnemy(int idx, int amount, bool isSourceMonster=true);

        [[nodiscard]] int calculateCardDamage(const CardInstance &card, int targetIdx, int baseDamage) const;
        [[nodiscard]] int calculateCardBlock(int baseBlock) const;

        void queuePurgeCard(const CardInstance &c, int target);
        void addPurgeCardToCardQueue(const CardQueueItem &item); // not really the front but hey
        void noOpRollMove(); // called by monsters to manipulate the aiRng counter when their rollMove function doesn't do anything

        void onManualDiscard(const CardInstance &c);
        // CardGroup.moveToDiscardPile, triggerOnManualDiscard and GameActionManager.incrementDiscard. Cards in the discard pile from
        // flyingFrom on, by default just this one, left with this action and land after it: they miss Eviscerate's discount.
        void discardFromHand(int handIdx, int flyingFrom=-1, bool relicsFirst=false); // relicsFirst: GameActionManager.incrementDiscard runs before the card's own trigger
        [[nodiscard]] DamageMatrix calculateMultiDamage(const CardInstance &card, int baseDamage) const; // AbstractCard.multiDamage
        void onShuffle();
        void triggerAndMoveToExhaustPile(CardInstance c);
        void mummifiedHandOnUsePower();

        // card select screens
        void openDiscoveryScreen(std::array<CardId, 3> discoveryCards, int copyCount);
        void openSimpleCardSelectScreen(CardSelectTask task, int count);

        // single card select helpers
        void chooseArmamentsCard(int handIdx);
        void chooseDiscardCard(int handIdx);
        void chooseNightmareCard(int handIdx);
        void chooseRetainCard(int handIdx);
        void chooseSetupCard(int handIdx);
        void chooseScryCard(int offsetFromTop);
        void chooseMeditateCard(int discardIdx);
        void chooseOmniscienceCard(int drawIdx);
        void chooseForeignInfluenceCard(int choice);
        void chooseWishOption(int choice);
        void chooseStanceOption(int choice);
        void chooseCodexCard(CardId id);
        void chooseDiscardToHandCard(int discardIdx, bool forZeroCost);
        void chooseDiscoveryCard(CardId id);
        void chooseDualWieldCard(int handIdx);
        void chooseExhaustOneCard(int handIdx);
        void chooseExhumeCard(int exhaustIdx);
        void chooseForethoughtCard(int handIdx);
        void chooseHeadbuttCard(int discardIdx);
        void chooseRecycleCard(int handIdx);
        void chooseWarcryCard(int handIdx);

        // multi card helpers
        void chooseDrawToHandCards(const int *idxs, int cardCount);
        void chooseExhaustCards(const fixed_list<int,10> &idxs);
        void chooseGambleCards(const fixed_list<int,10> &idxs);

    };

    std::ostream& operator<<(std::ostream &os, const BattleContext &bc);

    template <PlayerStatus s>
    Action Actions::BuffPlayer(int amount) {
        return {[=] (BattleContext &bc) {
            if (s == PlayerStatus::CORRUPTION && !bc.player.hasStatus<PS::CORRUPTION>()) {
                bc.cards.onBuffCorruption();
            }
            bc.player.buff<s>(amount);
        }};
    }

    template <PlayerStatus s>
    Action Actions::DebuffPlayer(int amount, bool isSourceMonster) {
        return {[=] (BattleContext &bc) {
            bc.player.debuff<s>(amount, isSourceMonster);
        }};
    }

    template<PlayerStatus s>
    Action Actions::DecrementStatus(int amount) {
        return {[=] (BattleContext &bc) {
            bc.player.decrementStatus<s>(amount);
        }};
    }

    template <PlayerStatus s>
    Action Actions::RemoveStatus() {
        return {[=] (BattleContext &bc) {
            bc.player.setHasStatus<s>(false);
        }};
    }

    // The game's limbo holds the copies waiting in the card queue.
    template <typename F>
    void BattleContext::forEachInBattleInstance(std::int16_t uniqueId, F f) {
        if (curCardQueueItem.card.getUniqueId() == uniqueId) {
            f(curCardQueueItem.card);
        }
        for (auto &c : cards.drawPile) {
            if (c.getUniqueId() == uniqueId) {
                f(c);
            }
        }
        for (auto &c : cards.discardPile) {
            if (c.getUniqueId() == uniqueId) {
                f(c);
            }
        }
        for (auto &c : cards.exhaustPile) {
            if (c.getUniqueId() == uniqueId) {
                f(c);
            }
        }
        for (int i = 0; i < cardQueue.size; ++i) {
            auto &c = cardQueue.arr[(cardQueue.frontIdx + i) % CardQueue::capacity].card;
            if (c.getUniqueId() == uniqueId) {
                f(c);
            }
        }
        for (int i = 0; i < cards.cardsInHand; ++i) {
            if (cards.hand[i].getUniqueId() == uniqueId) {
                f(cards.hand[i]);
            }
        }
    }

    template<MonsterStatus s>
    Action Actions::BuffEnemy(int idx, int amount) {
        return {[=] (BattleContext &bc) {
            // ApplyPowerAction does nothing to a dead, half-dead or escaped target.
            if (!bc.monsters.arr[idx].isDeadOrEscaped()) {
                bc.monsters.arr[idx].buff<s>(amount);
            }
        }};
    }

    template <MonsterStatus s>
    Action Actions::DebuffEnemy(int idx, int amount, bool isSourceMonster) {
        return {[=] (BattleContext &bc) {
            bc.debuffEnemy<s>(idx, amount, isSourceMonster);
        }};
    }

    template <MonsterStatus s>
    Action Actions::DebuffAllEnemy(int amount, bool isSourceMonster) {
        // todo this should just add all to bot immediately, not be called first
        // ^^ never mind i think adding to top is a workaround here

        return {[=] (BattleContext &bc) {
            for (int i = bc.monsters.monsterCount-1; i >= 0; --i) {
                if (bc.monsters.arr[i].isTargetable()) {
                    bc.addToTop(Actions::DebuffEnemy<s>(i, amount, isSourceMonster));
                }
            }
        }};
    }

    template <MonsterStatus s>
    void BattleContext::debuffEnemy(int idx, int amount, bool isSourceMonster) {
        Monster &e = monsters.arr[idx];

        // ApplyPowerAction does nothing to a dead, half-dead or escaped target.
        if (e.isDeadOrEscaped()) {
            return;
        }

        if (e.hasStatus<MS::ARTIFACT>()) {
            e.decrementStatus<MS::ARTIFACT>();
            return;
        }

        // ApplyPowerAction's constructor adds Snake Skull's point to whatever the player applies.
        if (s == MS::POISON && player.hasRelic<RelicId::SNECKO_SKULL>()) {
            ++amount;
        }

        e.addDebuff<s>(amount, isSourceMonster);
        if (player.hasStatus<PS::SADISTIC>()) {
            addToBot( Actions::DamageEnemy(idx, player.getStatus<PS::SADISTIC>()) );
        }

        if (s == MS::VULNERABLE && player.hasRelic<RelicId::CHAMPION_BELT>()) {
            addToBot(Actions::DebuffEnemy<MS::WEAK>(idx, 1, false) );
        }
    }


}

#endif //STS_LIGHTSPEED_BATTLECONTEXT_H
