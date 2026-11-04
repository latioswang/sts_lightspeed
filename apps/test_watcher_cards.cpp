// Plays each Watcher card in a set-up fight and checks the result against what the game's bytecode does, then plays random
// fights with every purple card, and runs the search on them. The checks encode a reading of the bytecode, so this catches
// regressions and crashes and doesn't show the simulator matches the game. Mirror mode does that.
// Build target: test_watcher_cards.

#include "combat/BattleContext.h"
#include "game/GameContext.h"
#include "game/Game.h"
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <vector>

using namespace sts;
using SAction = search::Action;
using search::ActionType;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::cerr << __func__ << ":" << __LINE__ << " failed: " #cond "\n"; } } while (0)
#define CHECK_EQ(a, b) do { ++checks; const auto va = (a); const auto vb = (b); if (!(va == vb)) { ++failures; std::cerr << __func__ << ":" << __LINE__ << " " #a " is " << va << ", expected " << vb << "\n"; } } while (0)

struct Fight {
    GameContext gc;
    BattleContext bc;

    explicit Fight(MonsterEncounter encounter = MonsterEncounter::CULTIST) : gc(CharacterClass::IRONCLAD, 77, 0) {
        gc.curRoom = Room::MONSTER;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, encounter);
        while (bc.cards.cardsInHand > 0) {
            bc.cards.removeFromHandAtIdx(0);
        }
        bc.cards.drawPile.clear();
        bc.cards.discardPile.clear();
        bc.cards.exhaustPile.clear();
        bc.player.energy = 10;
        // With no card left anywhere a fight ends at once, unless something can still do damage. A Dark orb that's never evoked does nothing else.
        bc.player.orbSlots = 1;
        bc.player.orbs[0] = Orb::DARK;
        bc.player.darkOrbEvoke[0] = 6;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            auto &m = bc.monsters.arr[i];
            m.curHp = m.maxHp = 500;
        }
    }

    Monster &monster(int i = 0) { return bc.monsters.arr[i]; }

    CardInstance card(CardId id, bool up = false) {
        CardInstance c(id, up);
        c.setUniqueId(bc.cards.nextUniqueCardId++);
        return c;
    }
    int toHand(CardId id, bool up = false) {
        bc.cards.moveToHand(card(id, up));
        return bc.cards.cardsInHand - 1;
    }
    void toDraw(CardId id, bool up = false) { bc.cards.moveToDrawPileTop(card(id, up)); }
    void toDiscard(CardId id, bool up = false) { bc.cards.moveToDiscardPile(card(id, up)); }

    void playIdx(int idx, int target = 0) { SAction(ActionType::CARD, idx, target).execute(bc); }
    void play(CardId id, bool up = false, int target = 0) { playIdx(toHand(id, up), target); }
    void select(int idx) { SAction(ActionType::SINGLE_CARD_SELECT, idx).execute(bc); }
    void endTurn() { SAction(ActionType::END_TURN).execute(bc); }

    int damageTaken(int i = 0) { return monster(i).maxHp - monster(i).curHp; }
    bool selecting(CardSelectTask task) const { return bc.inputState == InputState::CARD_SELECT && bc.cardSelectInfo.cardSelectTask == task; }
    void calmPlayer() { bc.player.curHp = bc.player.maxHp = 1000; } // so the turns this plays through can't end the fight
};

// ******** Numbers ********

void attacks() {
    struct Row { CardId id; int base; int up; };
    for (const Row &r : std::initializer_list<Row>{
            {CardId::STRIKE_PURPLE, 6, 9}, {CardId::CONSECRATE, 5, 8}, {CardId::CRUSH_JOINTS, 8, 10}, {CardId::CUT_THROUGH_FATE, 7, 9},
            {CardId::EMPTY_FIST, 9, 14}, {CardId::ERUPTION, 9, 9}, {CardId::FEAR_NO_EVIL, 8, 11}, {CardId::FLURRY_OF_BLOWS, 4, 6},
            {CardId::FOLLOW_UP, 7, 11}, {CardId::JUST_LUCKY, 3, 4}, {CardId::LESSON_LEARNED, 10, 13}, {CardId::REACH_HEAVEN, 10, 15},
            {CardId::SANDS_OF_TIME, 20, 26}, {CardId::SASH_WHIP, 8, 10}, {CardId::SIGNATURE_MOVE, 30, 40}, {CardId::SMITE, 12, 16},
            {CardId::THROUGH_VIOLENCE, 20, 30}, {CardId::WALLOP, 9, 12}, {CardId::WEAVE, 4, 6}, {CardId::WHEEL_KICK, 15, 20},
            {CardId::WINDMILL_STRIKE, 7, 10}, {CardId::TALK_TO_THE_HAND, 5, 7}, {CardId::CARVE_REALITY, 6, 10},
            {CardId::BOWLING_BASH, 7, 10}, {CardId::BRILLIANCE, 12, 16}, {CardId::CONCLUDE, 12, 16}}) {
        for (const bool up : {false, true}) {
            Fight f;
            f.calmPlayer();
            f.toDraw(CardId::WOUND);
            f.toDraw(CardId::WOUND);
            f.toDraw(CardId::WOUND);
            f.play(r.id, up);
            while (f.selecting(CardSelectTask::SCRY)) {
                f.select(CardSelectInfo::SCRY_DONE_IDX);
            }
            CHECK_EQ(f.damageTaken(), up ? r.up : r.base);
        }
    }

    for (const bool up : {false, true}) {
        Fight sleeves;
        sleeves.play(CardId::FLYING_SLEEVES, up);
        CHECK_EQ(sleeves.damageTaken(), 2 * (up ? 6 : 4));
        Fight tantrum;
        tantrum.toDraw(CardId::WOUND);
        tantrum.play(CardId::TANTRUM, up);
        CHECK_EQ(tantrum.damageTaken(), 3 * (up ? 4 : 3));
        CHECK(tantrum.bc.player.stance == Stance::WRATH);
        CHECK_EQ(tantrum.bc.cards.discardPile.size(), 0u);  // it goes back into the draw pile
        CHECK_EQ(tantrum.bc.cards.drawPile.size(), 2u);
        Fight ragnarok;
        ragnarok.play(CardId::RAGNAROK, up);
        CHECK_EQ(ragnarok.damageTaken(), up ? 36 : 25);
    }

    Fight bash(MonsterEncounter::THREE_LOUSE);  // one hit on the target for each monster alive
    const int alive = bash.bc.monsters.monstersAlive;
    CHECK_EQ(alive, 3);
    bash.play(CardId::BOWLING_BASH, false, 0);
    CHECK_EQ(bash.damageTaken(0), 3 * 7);
    CHECK_EQ(bash.damageTaken(1), 0);

    Fight all(MonsterEncounter::THREE_LOUSE);
    all.play(CardId::CONSECRATE, false);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(all.damageTaken(i), 5);
    }
}

void block() {
    struct Row { CardId id; int base; int up; };
    for (const Row &r : std::initializer_list<Row>{
            {CardId::DEFEND_PURPLE, 5, 8}, {CardId::VIGILANCE, 8, 12}, {CardId::PROTECT, 12, 16}, {CardId::SAFETY, 12, 16},
            {CardId::EMPTY_BODY, 7, 10}, {CardId::EVALUATE, 6, 10}, {CardId::PERSEVERANCE, 5, 7}, {CardId::SWIVEL, 8, 11},
            {CardId::DECEIVE_REALITY, 4, 7}, {CardId::THIRD_EYE, 7, 9}, {CardId::SANCTITY, 6, 9}, {CardId::PROSTRATE, 4, 4}}) {
        for (const bool up : {false, true}) {
            Fight f;
            f.toDraw(CardId::WOUND);
            f.play(r.id, up);
            while (f.selecting(CardSelectTask::SCRY)) {
                f.select(CardSelectInfo::SCRY_DONE_IDX);
            }
            CHECK_EQ(f.bc.player.block, up ? r.up : r.base);
        }
    }
}

void costsAndFlags() {
    struct Row { CardId id; int base; int up; };
    for (const Row &r : std::initializer_list<Row>{
            {CardId::ERUPTION, 2, 1}, {CardId::CRESCENDO, 1, 0}, {CardId::TRANQUILITY, 1, 0}, {CardId::VAULT, 3, 2}, {CardId::OMNISCIENCE, 4, 3},
            {CardId::STUDY, 2, 1}, {CardId::BETA, 2, 1}, {CardId::MASTER_REALITY, 1, 0}, {CardId::RUSHDOWN, 1, 0}, {CardId::SCRAWL, 1, 0},
            {CardId::CONJURE_BLADE, -1, -1}, {CardId::COLLECT, -1, -1}, {CardId::DEUS_EX_MACHINA, -2, -2}, {CardId::SANDS_OF_TIME, 4, 4},
            {CardId::WISH, 3, 3}, {CardId::OMEGA, 3, 3}, {CardId::DEVA_FORM, 3, 3}, {CardId::RAGNAROK, 3, 3}, {CardId::WEAVE, 0, 0},
            {CardId::HALT, 0, 0}, {CardId::THROUGH_VIOLENCE, 0, 0}, {CardId::SIGNATURE_MOVE, 2, 2}}) {
        CHECK_EQ(getEnergyCost(r.id, false), r.base);
        CHECK_EQ(getEnergyCost(r.id, true), r.up);
    }
    CHECK(isCardEthereal(CardId::DEVA_FORM, false) && !isCardEthereal(CardId::DEVA_FORM, true));
    CHECK(doesCardSelfRetain(CardId::WORSHIP, true) && !doesCardSelfRetain(CardId::WORSHIP, false));
    CHECK(doesCardSelfRetain(CardId::BLASPHEMY, true) && !doesCardSelfRetain(CardId::BLASPHEMY, false));
    CHECK(isCardInnate(CardId::ALPHA, true) && isCardInnate(CardId::BATTLE_HYMN, true) && isCardInnate(CardId::ESTABLISHMENT, true));
}

// ******** Stances ********

void stanceDamage() {
    Fight wrath;
    wrath.bc.player.stance = Stance::WRATH;
    wrath.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(wrath.damageTaken(), 12);

    Fight divinity;
    divinity.bc.player.stance = Stance::DIVINITY;
    divinity.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(divinity.damageTaken(), 18);

    Fight calm;
    calm.bc.player.stance = Stance::CALM;
    calm.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(calm.damageTaken(), 6);

    // Wrath is worked out when the attack is played: Eruption's own hit isn't doubled.
    Fight eruption;
    eruption.play(CardId::ERUPTION);
    CHECK_EQ(eruption.damageTaken(), 9);
    CHECK(eruption.bc.player.stance == Stance::WRATH);

    // Pressure Points is HP loss, so no stance doubles it and block doesn't stop it.
    Fight points;
    points.bc.player.stance = Stance::WRATH;
    points.monster().block = 50;
    points.play(CardId::PRESSURE_POINTS);
    CHECK_EQ(points.damageTaken(), 8);
    CHECK_EQ(points.monster().block, 50);
}

void stanceEnergy() {
    Fight fromCalm;
    fromCalm.bc.player.stance = Stance::CALM;
    const int energy = fromCalm.bc.player.energy;
    fromCalm.play(CardId::CRESCENDO);
    CHECK(fromCalm.bc.player.stance == Stance::WRATH);
    CHECK_EQ(fromCalm.bc.player.energy, energy - 1 + 2);

    Fight sameStance;  // no change at all: no energy
    sameStance.bc.player.stance = Stance::CALM;
    sameStance.play(CardId::TRANQUILITY);
    CHECK_EQ(sameStance.bc.player.energy, energy - 1);

    Fight toDivinity;
    toDivinity.play(CardId::BLASPHEMY);
    CHECK(toDivinity.bc.player.stance == Stance::DIVINITY);
    CHECK_EQ(toDivinity.bc.player.energy, energy - 1 + 3);
    CHECK(toDivinity.bc.player.hasStatus<PS::BLASPHEMER>());

    Fight neutral;
    neutral.bc.player.stance = Stance::WRATH;
    neutral.play(CardId::EMPTY_BODY);
    CHECK(neutral.bc.player.stance == Stance::NEUTRAL);
}

void divinityLeavesNextTurn() {
    Fight f;
    f.calmPlayer();
    f.bc.player.stance = Stance::DIVINITY;
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK(f.bc.player.stance == Stance::NEUTRAL);
}

void blasphemyKills() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::BLASPHEMY);
    f.endTurn();
    CHECK(f.bc.outcome == Outcome::PLAYER_LOSS);

    Fight intangible;  // HP loss: Intangible holds it to 1
    intangible.calmPlayer();
    intangible.play(CardId::BLASPHEMY);
    intangible.bc.player.buff<PS::INTANGIBLE>(5);
    const int hp = intangible.bc.player.curHp;
    intangible.endTurn();
    CHECK(intangible.bc.outcome == Outcome::UNDECIDED);
    CHECK(intangible.bc.player.curHp < hp);
    CHECK(!intangible.bc.player.hasStatus<PS::BLASPHEMER>());
}

void wrathDoublesIncoming() {
    Fight f(MonsterEncounter::JAW_WORM);
    f.calmPlayer();
    const int hp = f.bc.player.curHp;
    f.endTurn();
    const int neutralLoss = hp - f.bc.player.curHp;

    Fight w(MonsterEncounter::JAW_WORM);
    w.calmPlayer();
    w.bc.player.stance = Stance::WRATH;
    const int hp2 = w.bc.player.curHp;
    w.endTurn();
    CHECK_EQ(hp2 - w.bc.player.curHp, neutralLoss * 2);
}

void stancePowers() {
    Fight fortress;
    fortress.play(CardId::MENTAL_FORTRESS);
    fortress.play(CardId::CRESCENDO);
    CHECK_EQ(fortress.bc.player.block, 4);
    fortress.play(CardId::CRESCENDO);  // already in Wrath, so nothing
    CHECK_EQ(fortress.bc.player.block, 4);

    Fight rushdown;
    rushdown.toDraw(CardId::WOUND);
    rushdown.toDraw(CardId::WOUND);
    rushdown.play(CardId::RUSHDOWN);
    rushdown.play(CardId::TRANQUILITY);
    CHECK_EQ(rushdown.bc.cards.cardsInHand, 0);  // only entering Wrath draws
    rushdown.play(CardId::CRESCENDO);
    CHECK_EQ(rushdown.bc.cards.cardsInHand, 2);

    Fight water;
    water.calmPlayer();
    water.bc.player.stance = Stance::CALM;
    water.play(CardId::LIKE_WATER);
    water.toHand(CardId::WOUND);
    water.bc.player.block = 0;
    SAction(ActionType::END_TURN).execute(water.bc);
    CHECK(water.bc.player.hasStatus<PS::LIKE_WATER>());
}

void flurryComesBack() {
    Fight f;
    f.toDiscard(CardId::FLURRY_OF_BLOWS);
    f.toDiscard(CardId::WOUND);
    f.play(CardId::CRESCENDO);
    CHECK_EQ(f.bc.cards.cardsInHand, 1);
    CHECK(f.bc.cards.hand[0].getId() == CardId::FLURRY_OF_BLOWS);

    Fight noChange;
    noChange.bc.player.stance = Stance::WRATH;
    noChange.toDiscard(CardId::FLURRY_OF_BLOWS);
    noChange.play(CardId::CRESCENDO);
    CHECK_EQ(noChange.bc.cards.cardsInHand, 0);
}

// ******** Mantra ********

void mantra() {
    Fight f;
    f.play(CardId::WORSHIP);
    CHECK_EQ(f.bc.player.getStatus<PS::MANTRA>(), 5);
    f.play(CardId::WORSHIP);
    CHECK(f.bc.player.stance == Stance::DIVINITY);
    CHECK(!f.bc.player.hasStatus<PS::MANTRA>());  // exactly 10 removes the power

    Fight left;
    left.play(CardId::WORSHIP);
    left.play(CardId::PRAY, true);
    CHECK_EQ(left.bc.player.getStatus<PS::MANTRA>(), 9);
    left.play(CardId::PROSTRATE);
    CHECK(left.bc.player.stance == Stance::DIVINITY);
    CHECK_EQ(left.bc.player.getStatus<PS::MANTRA>(), 1);

    Fight brilliance;  // counts all Mantra gained this fight, spent or not
    brilliance.play(CardId::WORSHIP);
    brilliance.play(CardId::WORSHIP);
    brilliance.bc.player.stance = Stance::NEUTRAL;
    brilliance.play(CardId::BRILLIANCE);
    CHECK_EQ(brilliance.damageTaken(), 12 + 10);

    Fight first;  // a first application of 10 or more only goes through once more is stacked
    first.bc.player.buff<PS::MANTRA>(12);
    CHECK(first.bc.player.stance == Stance::NEUTRAL);
    first.play(CardId::PROSTRATE);
    CHECK(first.bc.player.stance == Stance::DIVINITY);
    CHECK_EQ(first.bc.player.getStatus<PS::MANTRA>(), 4);

    Fight devotion;
    devotion.calmPlayer();
    devotion.toDraw(CardId::WOUND);
    devotion.play(CardId::DEVOTION);
    devotion.endTurn();
    CHECK_EQ(devotion.bc.player.getStatus<PS::MANTRA>(), 2);
    CHECK_EQ(static_cast<int>(devotion.bc.player.mantraGained), 2);

    Fight bigDevotion;  // with no Mantra and 10 or more to give, straight to Divinity and no Mantra counted
    bigDevotion.calmPlayer();
    bigDevotion.toDraw(CardId::WOUND);
    bigDevotion.bc.player.buff<PS::DEVOTION>(10);
    bigDevotion.endTurn();
    CHECK(bigDevotion.bc.player.stance == Stance::DIVINITY);
    CHECK(!bigDevotion.bc.player.hasStatus<PS::MANTRA>());
    CHECK_EQ(static_cast<int>(bigDevotion.bc.player.mantraGained), 0);
}

// ******** Scry ********

void scry() {
    Fight f;
    f.toDraw(CardId::STRIKE_PURPLE);  // bottom
    f.toDraw(CardId::DEFEND_PURPLE);
    f.toDraw(CardId::ERUPTION);
    f.toDraw(CardId::VIGILANCE);  // top
    f.play(CardId::THIRD_EYE);
    CHECK(f.selecting(CardSelectTask::SCRY));
    CHECK_EQ(f.bc.cardSelectInfo.pickCount, 3);
    f.select(0);  // Vigilance
    CHECK(f.selecting(CardSelectTask::SCRY));
    CHECK_EQ(f.bc.cardSelectInfo.pickCount, 2);
    f.select(1);  // Defend: Eruption is passed over at offset 0
    CHECK(f.bc.inputState != InputState::CARD_SELECT);
    CHECK_EQ(f.bc.cards.drawPile.size(), 2u);
    CHECK(f.bc.cards.drawPile.back().getId() == CardId::ERUPTION);
    CHECK_EQ(f.bc.cards.discardPile.size(), 3u);  // the two picks and Third Eye
    CHECK(f.bc.cards.discardPile[0].getId() == CardId::VIGILANCE);
    CHECK(f.bc.cards.discardPile[1].getId() == CardId::DEFEND_PURPLE);

    Fight keep;
    keep.toDraw(CardId::WOUND);
    keep.play(CardId::CUT_THROUGH_FATE);
    keep.select(CardSelectInfo::SCRY_DONE_IDX);
    CHECK_EQ(keep.bc.cards.cardsInHand, 1);  // and then its draw

    Fight nirvana;  // Nirvana goes off even with nothing to look at, and no screen opens
    nirvana.play(CardId::NIRVANA);
    nirvana.play(CardId::JUST_LUCKY);
    CHECK(!nirvana.selecting(CardSelectTask::SCRY));
    CHECK_EQ(nirvana.bc.player.block, 3 + 2);

    Fight weave;
    weave.toDiscard(CardId::WEAVE);
    weave.toDraw(CardId::WOUND);
    weave.play(CardId::JUST_LUCKY);
    weave.select(CardSelectInfo::SCRY_DONE_IDX);
    CHECK_EQ(weave.bc.cards.cardsInHand, 1);
    CHECK(weave.bc.cards.hand[0].getId() == CardId::WEAVE);

    Fight weavePicked;  // scried into the discard pile, it comes straight back
    weavePicked.toDraw(CardId::WEAVE);
    weavePicked.play(CardId::JUST_LUCKY);
    weavePicked.select(0);
    CHECK_EQ(weavePicked.bc.cards.cardsInHand, 1);

    Fight noDraw;  // no screen, so Weave stays
    noDraw.toDiscard(CardId::WEAVE);
    noDraw.play(CardId::JUST_LUCKY);
    CHECK_EQ(noDraw.bc.cards.cardsInHand, 0);

    Fight foresight;  // before the turn's draw, shuffling first if the pile is empty
    foresight.calmPlayer();
    foresight.toDiscard(CardId::WOUND);
    foresight.toDiscard(CardId::WOUND);
    foresight.play(CardId::FORESIGHT);
    foresight.endTurn();
    CHECK(foresight.selecting(CardSelectTask::SCRY));
    foresight.select(CardSelectInfo::SCRY_DONE_IDX);
    CHECK(foresight.bc.cards.cardsInHand > 0);
}

// ******** Retain ********

void retained() {
    Fight f;
    f.calmPlayer();
    f.toHand(CardId::WINDMILL_STRIKE);
    f.toHand(CardId::PERSEVERANCE);
    f.toHand(CardId::SANDS_OF_TIME);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    int windmill = -1, perseverance = -1, sands = -1;
    for (int i = 0; i < f.bc.cards.cardsInHand; ++i) {
        const auto id = f.bc.cards.hand[i].getId();
        if (id == CardId::WINDMILL_STRIKE) windmill = i;
        if (id == CardId::PERSEVERANCE) perseverance = i;
        if (id == CardId::SANDS_OF_TIME) sands = i;
    }
    CHECK(windmill != -1 && perseverance != -1 && sands != -1);
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[windmill].specialData), 4);
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[perseverance].specialData), 2);
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[sands].cost), 3);
    const int block = f.bc.player.block;
    const int hp = f.damageTaken();
    f.playIdx(perseverance);
    CHECK_EQ(f.bc.player.block - block, 7);
    for (int i = 0; i < f.bc.cards.cardsInHand; ++i) {
        if (f.bc.cards.hand[i].getId() == CardId::WINDMILL_STRIKE) {
            f.playIdx(i);
            break;
        }
    }
    CHECK_EQ(f.damageTaken() - hp, 11);

    Fight est;
    est.calmPlayer();
    est.toHand(CardId::PROTECT);
    est.toDraw(CardId::WOUND);
    est.play(CardId::ESTABLISHMENT);
    est.endTurn();
    bool found = false;
    for (int i = 0; i < est.bc.cards.cardsInHand; ++i) {
        if (est.bc.cards.hand[i].getId() == CardId::PROTECT) {
            CHECK_EQ(static_cast<int>(est.bc.cards.hand[i].cost), 1);
            found = true;
        }
    }
    CHECK(found);
}

// ******** Conditions on the last card ********

void previousCard() {
    Fight follow;
    const int energy = follow.bc.player.energy;
    follow.play(CardId::STRIKE_PURPLE);
    follow.play(CardId::FOLLOW_UP);
    CHECK_EQ(follow.bc.player.energy, energy - 1 - 1 + 1);

    Fight noFollow;
    noFollow.play(CardId::DEFEND_PURPLE);
    noFollow.play(CardId::FOLLOW_UP);
    CHECK_EQ(noFollow.bc.player.energy, energy - 2);

    Fight firstCard;  // the card itself is the last one, and there's nothing before it
    firstCard.play(CardId::FOLLOW_UP);
    CHECK_EQ(firstCard.bc.player.energy, energy - 1);

    Fight crush;
    crush.play(CardId::DEFEND_PURPLE);
    crush.play(CardId::CRUSH_JOINTS);
    CHECK_EQ(crush.monster().getStatus<MS::VULNERABLE>(), 1);

    Fight sash;
    sash.play(CardId::STRIKE_PURPLE);
    sash.play(CardId::SASH_WHIP, true);
    CHECK_EQ(sash.monster().getStatus<MS::WEAK>(), 2);

    Fight sanctity;
    sanctity.toDraw(CardId::WOUND);
    sanctity.toDraw(CardId::WOUND);
    sanctity.play(CardId::DEFEND_PURPLE);
    sanctity.play(CardId::SANCTITY);
    CHECK_EQ(sanctity.bc.cards.cardsInHand, 2);

    Fight acrossTurns;  // cardsPlayedThisCombat carries on from the last turn
    acrossTurns.calmPlayer();
    acrossTurns.toDraw(CardId::WOUND);
    acrossTurns.play(CardId::STRIKE_PURPLE);
    acrossTurns.endTurn();
    const int turnEnergy = acrossTurns.bc.player.energy;
    acrossTurns.play(CardId::FOLLOW_UP);
    CHECK_EQ(acrossTurns.bc.player.energy, turnEnergy - 1 + 1);

    Fight signature;
    signature.toHand(CardId::STRIKE_PURPLE);
    const int idx = signature.toHand(CardId::SIGNATURE_MOVE);
    CHECK(!signature.bc.cards.hand[idx].canUse(signature.bc, 0, false));
    Fight signatureAlone;
    signatureAlone.toHand(CardId::DEFEND_PURPLE);
    const int alone = signatureAlone.toHand(CardId::SIGNATURE_MOVE);
    CHECK(signatureAlone.bc.cards.hand[alone].canUse(signatureAlone.bc, 0, false));
}

// ******** Others, one by one ********

void haltAndStanceCards() {
    Fight halt;
    halt.play(CardId::HALT);
    CHECK_EQ(halt.bc.player.block, 3);
    Fight haltWrath;
    haltWrath.bc.player.stance = Stance::WRATH;
    haltWrath.play(CardId::HALT);
    CHECK_EQ(haltWrath.bc.player.block, 3 + 9);
    Fight haltUp;
    haltUp.bc.player.stance = Stance::WRATH;
    haltUp.bc.player.buff<PS::DEXTERITY>(2);
    haltUp.play(CardId::HALT, true);
    CHECK_EQ(haltUp.bc.player.block, (4 + 2) + (14 + 2));  // Dexterity counts on both

    Fight peace;
    peace.toDraw(CardId::WOUND);
    peace.toDraw(CardId::WOUND);
    peace.toDraw(CardId::WOUND);
    peace.play(CardId::INNER_PEACE);
    CHECK(peace.bc.player.stance == Stance::CALM);
    CHECK_EQ(peace.bc.cards.cardsInHand, 0);
    peace.play(CardId::INNER_PEACE);
    CHECK_EQ(peace.bc.cards.cardsInHand, 3);

    Fight indignation;
    indignation.play(CardId::INDIGNATION);
    CHECK(indignation.bc.player.stance == Stance::WRATH);
    indignation.play(CardId::INDIGNATION, true);
    CHECK_EQ(indignation.monster().getStatus<MS::VULNERABLE>(), 5);

    Fight fear(MonsterEncounter::JAW_WORM);  // its first move is an attack
    CHECK(fear.monster().isAttacking());
    fear.play(CardId::FEAR_NO_EVIL);
    CHECK(fear.bc.player.stance == Stance::CALM);
    CHECK_EQ(fear.damageTaken(), 8);
}

void judgementAndWallop() {
    Fight judgement;
    judgement.monster().curHp = 30;
    judgement.play(CardId::JUDGMENT);
    CHECK(!judgement.monster().isAlive());

    Fight notYet;
    notYet.monster().curHp = 31;
    notYet.monster().block = 10;
    notYet.play(CardId::JUDGMENT);
    CHECK(notYet.monster().isAlive());
    CHECK_EQ(notYet.monster().curHp, 31);

    Fight wallop;
    wallop.play(CardId::WALLOP);
    CHECK_EQ(wallop.bc.player.block, 9);

    Fight blocked;
    blocked.monster().block = 5;
    blocked.play(CardId::WALLOP);
    CHECK_EQ(blocked.bc.player.block, 4);  // only what got past block

    Fight overkill;
    overkill.monster().curHp = 3;
    overkill.play(CardId::WALLOP);
    CHECK_EQ(overkill.bc.player.block, 3);  // stops at the HP it had
}

void marksAndTalk() {
    Fight marks(MonsterEncounter::THREE_LOUSE);
    marks.play(CardId::PRESSURE_POINTS, false, 0);
    CHECK_EQ(marks.damageTaken(0), 8);
    marks.play(CardId::PRESSURE_POINTS, true, 1);
    CHECK_EQ(marks.damageTaken(0), 16);  // every marked monster loses its Mark each time
    CHECK_EQ(marks.damageTaken(1), 11);

    Fight talk;
    talk.play(CardId::TALK_TO_THE_HAND);
    CHECK_EQ(talk.bc.player.block, 0);  // its own hit lands before the power
    talk.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(talk.bc.player.block, 2);
    talk.monster().block = 100;
    talk.play(CardId::STRIKE_PURPLE);  // a blocked hit still counts
    CHECK_EQ(talk.bc.player.block, 4);
    talk.play(CardId::PRESSURE_POINTS);  // HP loss doesn't
    CHECK_EQ(talk.bc.player.block, 4);

    Fight wave(MonsterEncounter::THREE_LOUSE);
    wave.play(CardId::WAVE_OF_THE_HAND, true);
    wave.play(CardId::DEFEND_PURPLE);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(wave.monster(i).getStatus<MS::WEAK>(), 2);
    }
    wave.calmPlayer();
    wave.toDraw(CardId::WOUND);
    wave.endTurn();
    CHECK(!wave.bc.player.hasStatus<PS::WAVE_OF_THE_HAND>());  // the whole power goes at the end of the round

    Fight swivel;
    swivel.play(CardId::SWIVEL);
    const int energy = swivel.bc.player.energy;
    swivel.play(CardId::WALLOP);
    CHECK_EQ(swivel.bc.player.energy, energy);
    CHECK(!swivel.bc.player.hasStatus<PS::FREE_ATTACK_POWER>());
    swivel.play(CardId::WALLOP);
    CHECK_EQ(swivel.bc.player.energy, energy - 2);
}

void endingTheTurn() {
    Fight conclude;
    conclude.calmPlayer();
    conclude.toDraw(CardId::WOUND);
    const int turn = conclude.bc.turn;
    conclude.play(CardId::CONCLUDE);
    CHECK_EQ(conclude.bc.turn, turn + 1);
    CHECK_EQ(conclude.damageTaken(), 12);

    Fight vault(MonsterEncounter::JAW_WORM);
    vault.calmPlayer();
    vault.bc.player.buff<PS::WEAK>(2);
    const int hp = vault.bc.player.curHp;
    vault.toDraw(CardId::WOUND);
    vault.play(CardId::VAULT);
    CHECK_EQ(vault.bc.player.curHp, hp);  // no monster acted
    CHECK_EQ(vault.bc.turn, 1);
    CHECK_EQ(vault.bc.player.getStatus<PS::WEAK>(), 2);  // and the round didn't end, so nothing ticked

    Fight meditate;
    meditate.calmPlayer();
    meditate.toDraw(CardId::WOUND);
    meditate.toDiscard(CardId::STRIKE_PURPLE);
    meditate.toDiscard(CardId::DEFEND_PURPLE);
    meditate.play(CardId::MEDITATE);
    CHECK(meditate.selecting(CardSelectTask::MEDITATE));
    meditate.select(0);
    CHECK(meditate.bc.player.stance == Stance::CALM);
    bool kept = false;
    for (int i = 0; i < meditate.bc.cards.cardsInHand; ++i) {
        kept = kept || meditate.bc.cards.hand[i].getId() == CardId::STRIKE_PURPLE;
    }
    CHECK(kept);  // retained through the end of the turn it ended

    Fight meditateAll;  // no more cards than picks: no screen
    meditateAll.calmPlayer();
    meditateAll.toDraw(CardId::WOUND);
    meditateAll.toDiscard(CardId::STRIKE_PURPLE);
    meditateAll.play(CardId::MEDITATE);
    CHECK(!meditateAll.selecting(CardSelectTask::MEDITATE));
}

void madeCards() {
    Fight alpha;
    alpha.play(CardId::ALPHA);
    CHECK_EQ(alpha.bc.cards.drawPile.size(), 1u);
    CHECK(alpha.bc.cards.drawPile[0].getId() == CardId::BETA);
    Fight beta;
    beta.play(CardId::BETA);
    CHECK(beta.bc.cards.drawPile[0].getId() == CardId::OMEGA);
    Fight omega;
    omega.calmPlayer();
    omega.toDraw(CardId::WOUND);
    omega.play(CardId::OMEGA);
    omega.endTurn();
    CHECK_EQ(omega.damageTaken(), 50);

    Fight heaven;
    heaven.play(CardId::REACH_HEAVEN);
    CHECK(heaven.bc.cards.drawPile[0].getId() == CardId::THROUGH_VIOLENCE);
    Fight pray;
    pray.play(CardId::PRAY);
    CHECK(pray.bc.cards.drawPile[0].getId() == CardId::INSIGHT);
    Fight eval;
    eval.play(CardId::EVALUATE);
    CHECK(eval.bc.cards.drawPile[0].getId() == CardId::INSIGHT);
    Fight carve;
    carve.play(CardId::CARVE_REALITY);
    CHECK(carve.bc.cards.hand[0].getId() == CardId::SMITE);
    Fight deceive;
    deceive.play(CardId::DECEIVE_REALITY);
    CHECK(deceive.bc.cards.hand[0].getId() == CardId::SAFETY);

    Fight study;
    study.calmPlayer();
    study.toDraw(CardId::WOUND);
    study.play(CardId::STUDY);
    study.endTurn();
    int insights = 0;
    for (const auto &c : study.bc.cards.drawPile) insights += c.getId() == CardId::INSIGHT;
    for (int i = 0; i < study.bc.cards.cardsInHand; ++i) insights += study.bc.cards.hand[i].getId() == CardId::INSIGHT;
    CHECK_EQ(insights, 1);

    Fight hymn;
    hymn.calmPlayer();
    hymn.toDraw(CardId::WOUND);
    hymn.play(CardId::BATTLE_HYMN);
    hymn.endTurn();
    bool smite = false;
    for (int i = 0; i < hymn.bc.cards.cardsInHand; ++i) smite = smite || hymn.bc.cards.hand[i].getId() == CardId::SMITE;
    CHECK(smite);

    Fight reality;
    reality.play(CardId::MASTER_REALITY);
    reality.play(CardId::CARVE_REALITY);
    CHECK(reality.bc.cards.hand[0].isUpgraded());
    reality.play(CardId::ALPHA);
    CHECK(reality.bc.cards.drawPile[0].isUpgraded());
    Fight wound;  // not statuses
    wound.play(CardId::MASTER_REALITY);
    wound.bc.addToBot( Actions::MakeTempCardInDiscard(CardInstance(CardId::WOUND)) );
    wound.bc.inputState = InputState::EXECUTING_ACTIONS;
    wound.bc.executeActions();
    CHECK(!wound.bc.cards.discardPile.back().isUpgraded());

    Fight insight;
    insight.toDraw(CardId::WOUND);
    insight.toDraw(CardId::WOUND);
    insight.toDraw(CardId::WOUND);
    insight.play(CardId::INSIGHT, true);
    CHECK_EQ(insight.bc.cards.cardsInHand, 3);
    Fight miracle;
    const int energy = miracle.bc.player.energy;
    miracle.play(CardId::MIRACLE, true);
    CHECK_EQ(miracle.bc.player.energy, energy + 2);

    Fight deus;
    deus.toDraw(CardId::DEUS_EX_MACHINA);
    deus.bc.addToBot( Actions::DrawCards(1) );
    deus.bc.inputState = InputState::EXECUTING_ACTIONS;
    deus.bc.executeActions();
    CHECK_EQ(deus.bc.cards.cardsInHand, 2);
    CHECK(deus.bc.cards.hand[0].getId() == CardId::MIRACLE);
    CHECK_EQ(deus.bc.cards.exhaustPile.size(), 1u);
}

void xCosts() {
    Fight conjure;
    conjure.bc.player.energy = 3;
    conjure.play(CardId::CONJURE_BLADE);
    CHECK_EQ(conjure.bc.player.energy, 0);
    CHECK_EQ(conjure.bc.cards.drawPile.size(), 1u);
    CHECK(conjure.bc.cards.drawPile[0].getId() == CardId::EXPUNGER);
    CHECK_EQ(static_cast<int>(conjure.bc.cards.drawPile[0].specialData), 3);

    Fight conjureUp;
    conjureUp.bc.player.energy = 2;
    conjureUp.play(CardId::CONJURE_BLADE, true);
    CHECK_EQ(static_cast<int>(conjureUp.bc.cards.drawPile[0].specialData), 3);

    Fight empty;  // no check for 0
    empty.bc.player.energy = 0;
    empty.play(CardId::CONJURE_BLADE);
    CHECK_EQ(static_cast<int>(empty.bc.cards.drawPile[0].specialData), 0);

    Fight expunger;
    CardInstance c = expunger.card(CardId::EXPUNGER);
    c.specialData = 3;
    expunger.bc.cards.moveToHand(c);
    expunger.playIdx(0);
    CHECK_EQ(expunger.damageTaken(), 27);

    Fight collect;
    collect.calmPlayer();
    collect.bc.player.energy = 2;
    collect.toDraw(CardId::WOUND);
    collect.play(CardId::COLLECT, true);
    CHECK_EQ(collect.bc.player.energy, 0);
    CHECK_EQ(collect.bc.player.getStatus<PS::COLLECT>(), 3);
    collect.endTurn();
    CHECK_EQ(collect.bc.player.getStatus<PS::COLLECT>(), 2);
    bool miracle = false;
    for (int i = 0; i < collect.bc.cards.cardsInHand; ++i) {
        const auto &h = collect.bc.cards.hand[i];
        miracle = miracle || (h.getId() == CardId::MIRACLE && h.isUpgraded());
    }
    CHECK(miracle);
}

void powersOverTurns() {
    Fight deva;
    deva.calmPlayer();
    deva.toDraw(CardId::WOUND);
    deva.play(CardId::DEVA_FORM);
    deva.endTurn();
    CHECK_EQ(deva.bc.player.energy, deva.bc.player.energyPerTurn + 1);
    deva.endTurn();
    CHECK_EQ(deva.bc.player.energy, deva.bc.player.energyPerTurn + 2);

    Fight twoDevas;
    twoDevas.calmPlayer();
    twoDevas.toDraw(CardId::WOUND);
    twoDevas.play(CardId::DEVA_FORM);
    twoDevas.play(CardId::DEVA_FORM);
    twoDevas.endTurn();
    CHECK_EQ(twoDevas.bc.player.energy, twoDevas.bc.player.energyPerTurn + 2);
    twoDevas.endTurn();
    CHECK_EQ(twoDevas.bc.player.energy, twoDevas.bc.player.energyPerTurn + 4);

    Fight fasting;
    fasting.calmPlayer();
    fasting.toDraw(CardId::WOUND);
    fasting.play(CardId::FASTING);
    CHECK_EQ(fasting.bc.player.getStatus<PS::STRENGTH>(), 3);
    CHECK_EQ(fasting.bc.player.getStatus<PS::DEXTERITY>(), 3);
    fasting.endTurn();
    CHECK_EQ(fasting.bc.player.energy, fasting.bc.player.energyPerTurn - 1);

    Fight artifact;  // Artifact stops the debuff and not the buffs
    artifact.bc.player.buff<PS::ARTIFACT>(1);
    artifact.play(CardId::FASTING);
    CHECK(!artifact.bc.player.hasStatus<PS::FASTING>());
    CHECK_EQ(artifact.bc.player.getStatus<PS::STRENGTH>(), 3);

    Fight fury;
    fury.calmPlayer();
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.toDraw(CardId::WOUND);
    fury.play(CardId::SIMMERING_FURY);
    fury.endTurn();
    CHECK(fury.bc.player.stance == Stance::WRATH);
    CHECK_EQ(fury.bc.cards.cardsInHand, 7);

    Fight wreath;
    wreath.play(CardId::WREATH_OF_FLAME);
    wreath.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(wreath.damageTaken(), 11);
    wreath.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(wreath.damageTaken(), 17);

    Fight ragnarokVigor;  // these hits work their damage out with Vigor still there
    ragnarokVigor.bc.player.buff<PS::VIGOR>(1);
    ragnarokVigor.play(CardId::RAGNAROK);
    CHECK_EQ(ragnarokVigor.damageTaken(), 5 * 6);
}

void screens() {
    Fight shield;
    shield.toHand(CardId::WOUND);
    shield.toHand(CardId::WOUND);
    shield.play(CardId::SPIRIT_SHIELD);
    CHECK_EQ(shield.bc.player.block, 2 * 3);

    Fight scrawl;
    for (int i = 0; i < 12; ++i) scrawl.toDraw(CardId::WOUND);
    scrawl.toHand(CardId::WOUND);
    scrawl.play(CardId::SCRAWL);
    CHECK_EQ(scrawl.bc.cards.cardsInHand, 10);

    Fight foreign;
    const auto cardRngBefore = foreign.bc.cardRng.counter;
    foreign.play(CardId::FOREIGN_INFLUENCE, true);
    CHECK(foreign.selecting(CardSelectTask::FOREIGN_INFLUENCE));
    CHECK(foreign.bc.cardRng.counter >= cardRngBefore + 3);  // its pick rolls the run's card stream
    const auto first = foreign.bc.cardSelectInfo.cards[0];
    for (const auto id : foreign.bc.cardSelectInfo.cards) {
        CHECK(getCardType(id) == CardType::ATTACK);
    }
    CHECK(foreign.bc.cardSelectInfo.cards[0] != foreign.bc.cardSelectInfo.cards[1]);
    foreign.select(0);
    CHECK_EQ(foreign.bc.cards.cardsInHand, 1);
    CHECK(foreign.bc.cards.hand[0].getId() == first);
    CHECK_EQ(static_cast<int>(foreign.bc.cards.hand[0].costForTurn), 0);  // upgraded: free this turn

    Fight wish;
    wish.play(CardId::WISH);
    CHECK(wish.selecting(CardSelectTask::WISH));
    wish.select(0);
    CHECK_EQ(wish.bc.player.getStatus<PS::STRENGTH>(), 3);
    Fight wishGold;
    const int gold = wishGold.bc.player.gold;
    wishGold.play(CardId::WISH, true);
    wishGold.select(1);
    CHECK_EQ(wishGold.bc.player.gold, gold + 30);
    Fight wishArmor;
    wishArmor.play(CardId::WISH);
    wishArmor.select(2);
    CHECK_EQ(wishArmor.bc.player.getStatus<PS::PLATED_ARMOR>(), 6);

    Fight omni;
    omni.toDraw(CardId::STRIKE_PURPLE);
    omni.toDraw(CardId::WOUND);
    omni.play(CardId::OMNISCIENCE);
    CHECK(omni.selecting(CardSelectTask::OMNISCIENCE));
    omni.select(0);  // the Strike, played twice and exhausted
    CHECK_EQ(omni.damageTaken(), 12);
    CHECK_EQ(omni.bc.cards.drawPile.size(), 1u);
    CHECK_EQ(omni.bc.cards.exhaustPile.size(), 2u);  // Omniscience and the Strike
    CHECK_EQ(omni.bc.cards.discardPile.size(), 0u);
}

void lessonLearned() {
    GameContext gc(CharacterClass::IRONCLAD, 77, 0);
    gc.curRoom = Room::MONSTER;
    gc.deck = Deck{};
    gc.deck.obtainRaw(Card(CardId::LESSON_LEARNED));
    gc.deck.obtainRaw(Card(CardId::STRIKE_RED));
    gc.deck.obtainRaw(Card(CardId::DEFEND_RED, 1));  // can't be upgraded again
    BattleContext bc;
    bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
    bc.init(gc, MonsterEncounter::CULTIST);
    bc.executeActions();
    CHECK_EQ(bc.upgradableDeckIdxs.size(), 2u);
    const auto miscBefore = bc.miscRng.counter;
    while (bc.cards.cardsInHand > 0) bc.cards.removeFromHandAtIdx(0);
    CardInstance c(CardId::LESSON_LEARNED);
    c.setUniqueId(0);
    bc.cards.moveToHand(c);
    bc.player.energy = 5;
    bc.monsters.arr[0].curHp = 5;
    SAction(ActionType::CARD, 0, 0).execute(bc);
    CHECK(bc.miscRng.counter == miscBefore + 1);
    CHECK_EQ(bc.lessonLearnedUpgrades.size(), 1u);
    CHECK_EQ(bc.upgradableDeckIdxs.size(), 1u);
}

// ******** Every purple card, played at random ********

const std::vector<CardId> &purpleCards() {
    static const std::vector<CardId> cards = [] {
        std::vector<CardId> out;
        const int count = static_cast<int>(sizeof(cardStringIds) / sizeof(cardStringIds[0]));
        for (int i = 1; i < count; ++i) {
            const auto id = static_cast<CardId>(i);
            if (getCardColor(id) == CardColor::PURPLE) {
                out.push_back(id);
            }
        }
        for (const auto id : {CardId::MIRACLE, CardId::INSIGHT, CardId::SMITE, CardId::SAFETY, CardId::THROUGH_VIOLENCE,
                              CardId::BETA, CardId::OMEGA, CardId::EXPUNGER}) {
            out.push_back(id);
        }
        return out;
    }();
    return cards;
}

std::vector<SAction> legalActions(const BattleContext &bc) {
    if (bc.inputState == InputState::CARD_SELECT) {
        return SAction::enumerateCardSelectActions(bc);
    }
    std::vector<SAction> actions;
    if (bc.isCardPlayAllowed()) {
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            const auto &c = bc.cards.hand[i];
            if (c.requiresTarget()) {
                for (int t = 0; t < bc.monsters.monsterCount; ++t) {
                    if (bc.monsters.arr[t].isTargetable() && c.canUse(bc, t, false)) {
                        actions.emplace_back(ActionType::CARD, i, t);
                    }
                }
            } else if (c.canUse(bc, 0, false)) {
                actions.emplace_back(ActionType::CARD, i, 0);
            }
        }
    }
    actions.emplace_back(ActionType::END_TURN);
    return actions;
}

// Random fights with purple-heavy decks. Each card comes up in the deck in turn, so none is left out.
void randomFights(int fights, std::map<CardId, int> &played) {
    static const std::vector<MonsterEncounter> encounters {
        MonsterEncounter::CULTIST, MonsterEncounter::JAW_WORM, MonsterEncounter::TWO_LOUSE, MonsterEncounter::SMALL_SLIMES,
        MonsterEncounter::GREMLIN_GANG, MonsterEncounter::GREMLIN_NOB, MonsterEncounter::LAGAVULIN, MonsterEncounter::THREE_SENTRIES,
        MonsterEncounter::SLIME_BOSS, MonsterEncounter::THE_GUARDIAN, MonsterEncounter::HEXAGHOST, MonsterEncounter::CHOSEN,
        MonsterEncounter::BOOK_OF_STABBING, MonsterEncounter::GREMLIN_LEADER, MonsterEncounter::CHAMP, MonsterEncounter::AUTOMATON,
        MonsterEncounter::COLLECTOR, MonsterEncounter::THREE_BYRDS, MonsterEncounter::REPTOMANCER, MonsterEncounter::TRANSIENT,
        MonsterEncounter::THREE_DARKLINGS, MonsterEncounter::AWAKENED_ONE, MonsterEncounter::TIME_EATER, MonsterEncounter::DONU_AND_DECA,
        MonsterEncounter::WRITHING_MASS, MonsterEncounter::MAW, MonsterEncounter::SHIELD_AND_SPEAR, MonsterEncounter::THE_HEART,
    };

    const auto &purple = purpleCards();
    for (int fight = 0; fight < fights; ++fight) {
        std::mt19937 rng(static_cast<unsigned>(fight) * 2654435761u + 11);
        GameContext gc(CharacterClass::IRONCLAD, static_cast<std::uint64_t>(fight) + 2000, fight % 2 == 0 ? 0 : 20);
        gc.curRoom = Room::MONSTER;
        gc.curHp = gc.maxHp = 300;
        gc.deck = Deck{};
        // A few runs get the relics these cards interact with.
        if (fight % 7 == 0) gc.relics.add(RelicInstance{RelicId::CHEMICAL_X, 0});
        if (fight % 11 == 0) gc.relics.add(RelicInstance{RelicId::RUNIC_PYRAMID, 0});
        if (fight % 13 == 0) gc.relics.add(RelicInstance{RelicId::GOLDEN_EYE, 0});
        if (fight % 17 == 0) gc.relics.add(RelicInstance{RelicId::SNECKO_EYE, 0});
        if (fight % 19 == 0) gc.relics.add(RelicInstance{RelicId::UNCEASING_TOP, 0});
        if (fight % 23 == 0) gc.relics.add(RelicInstance{RelicId::VIOLET_LOTUS, 0});
        if (fight % 29 == 0) gc.relics.add(RelicInstance{RelicId::MELANGE, 0});

        for (int i = 0; i < 18; ++i) {
            const CardId id = i == 0 ? purple[fight % purple.size()] : purple[rng() % purple.size()];
            gc.deck.obtainRaw(Card(id, static_cast<int>(rng() % 2)));
        }
        gc.deck.obtainRaw(Card(CardId::DOUBT));
        gc.deck.obtainRaw(Card(CardId::PAIN));

        BattleContext bc;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, encounters[fight % encounters.size()]);
        bc.executeActions();

        int steps = 0;
        while (bc.outcome == Outcome::UNDECIDED && ++steps < 2500) {
            if (bc.inputState != InputState::CARD_SELECT && bc.inputState != InputState::PLAYER_NORMAL) {
                break;
            }
            auto actions = legalActions(bc);
            if (actions.empty()) {
                std::cerr << "no action in fight " << fight << " task " << static_cast<int>(bc.cardSelectInfo.cardSelectTask) << "\n";
                ++failures;
                break;
            }
            const SAction a = actions[rng() % actions.size()];
            if (a.getActionType() == ActionType::CARD) {
                played[bc.cards.hand[a.getSourceIdx()].getId()] += 1;
            }
            CHECK(a.isValidAction(bc));
            a.execute(bc);

            for (int i = 0; i < bc.cards.cardsInHand; ++i) {
                CHECK(bc.cards.hand[i].getId() != CardId::INVALID);
            }
            CHECK(bc.cards.cardsInHand >= 0 && bc.cards.cardsInHand <= 10);
        }
    }
}

// The agent's search walks every select screen too; it crashed on the ones it couldn't list.
void searchRuns(int fights) {
    const auto &purple = purpleCards();
    for (int fight = 0; fight < fights; ++fight) {
        GameContext gc(CharacterClass::IRONCLAD, static_cast<std::uint64_t>(fight) + 6000, 0);
        gc.curRoom = Room::MONSTER;
        gc.deck = Deck{};
        for (int i = 0; i < 12; ++i) {
            gc.deck.obtainRaw(Card(purple[(fight * 12 + i) % purple.size()], i % 2));
        }
        BattleContext bc;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, MonsterEncounter::JAW_WORM);
        bc.executeActions();
        search::BattleScumSearcher2 searcher(bc);
        searcher.search(200);
        CHECK(searcher.root.simulationCount > 0);
    }
}

}  // namespace

int main(int argc, char **argv) {
    const int randomFightCount = argc > 1 ? std::atoi(argv[1]) : 3000;

    attacks();
    block();
    costsAndFlags();
    stanceDamage();
    stanceEnergy();
    divinityLeavesNextTurn();
    blasphemyKills();
    wrathDoublesIncoming();
    stancePowers();
    flurryComesBack();
    mantra();
    scry();
    retained();
    previousCard();
    haltAndStanceCards();
    judgementAndWallop();
    marksAndTalk();
    endingTheTurn();
    madeCards();
    xCosts();
    powersOverTurns();
    screens();
    lessonLearned();

    std::map<CardId, int> played;
    randomFights(randomFightCount, played);
    int missing = 0;
    for (const CardId id : purpleCards()) {
        if (played[id] == 0 && id != CardId::DEUS_EX_MACHINA) {
            std::cerr << "never played at random: " << cardEnumStrings[static_cast<int>(id)] << "\n";
            ++missing;
        }
    }
    CHECK_EQ(missing, 0);
    searchRuns(25);

    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
