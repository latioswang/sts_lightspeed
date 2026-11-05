// Plays each Defect card in a set-up fight and checks the result against what the game's bytecode does.
// It says nothing about the real game by itself: mirror mode does that. Build target: test_defect_cards.

#include "combat/BattleContext.h"
#include "game/GameContext.h"
#include "game/Game.h"
#include "sim/search/Action.h"

#include <initializer_list>
#include <iostream>
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

    explicit Fight(int slots = 1, MonsterEncounter encounter = MonsterEncounter::CULTIST) : gc(CharacterClass::IRONCLAD, 77, 0) {
        gc.curRoom = Room::MONSTER;
        gc.masterMaxOrbs = static_cast<std::int8_t>(slots);
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, encounter);
        while (bc.cards.cardsInHand > 0) {
            bc.cards.removeFromHandAtIdx(0);
        }
        bc.cards.drawPile.clear();
        bc.cards.discardPile.clear();
        bc.cards.exhaustPile.clear();
        bc.player.energy = 10;
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
    void channel(Orb orb) { bc.player.channelOrb(bc, orb); bc.executeActions(); }
};

// ******** Orb rules ********

void orbSlots() {
    Fight none(0);
    none.play(CardId::ZAP);
    CHECK_EQ(static_cast<int>(none.bc.player.orbSlots), 0);
    CHECK_EQ(none.bc.player.lightningChanneled, 0);  // channelOrb returns before counting
    CHECK_EQ(none.damageTaken(), 0);

    Fight f(9);
    f.play(CardId::CAPACITOR, true);  // 9 + 3 stops at 10: IncreaseMaxOrbAction adds one at a time
    CHECK_EQ(static_cast<int>(f.bc.player.orbSlots), 10);

    GameContext gc(CharacterClass::IRONCLAD, 77, 0);
    CHECK_EQ(static_cast<int>(gc.masterMaxOrbs), 0);
    gc.obtainRelic(RelicId::PRISMATIC_SHARD);
    CHECK_EQ(static_cast<int>(gc.masterMaxOrbs), 1);
}

void channelWhenFullEvokesFirst() {
    Fight f(1);
    f.play(CardId::ZAP);
    CHECK(f.bc.player.orbs[0] == Orb::LIGHTNING);
    CHECK_EQ(f.damageTaken(), 0);
    f.play(CardId::ZAP);
    CHECK(f.bc.player.orbs[0] == Orb::LIGHTNING);
    CHECK_EQ(f.damageTaken(), 8);
    CHECK_EQ(f.bc.player.lightningChanneled, 2);
}

void focusChangesOrbs() {
    Fight f(4);
    f.channel(Orb::LIGHTNING);
    f.channel(Orb::FROST);
    f.channel(Orb::DARK);
    f.channel(Orb::PLASMA);
    auto &p = f.bc.player;
    CHECK_EQ(p.orbPassiveAmount(0), 3); CHECK_EQ(p.orbEvokeAmount(0), 8);
    CHECK_EQ(p.orbPassiveAmount(1), 2); CHECK_EQ(p.orbEvokeAmount(1), 5);
    CHECK_EQ(p.orbPassiveAmount(2), 6); CHECK_EQ(p.orbEvokeAmount(2), 6);
    CHECK_EQ(p.orbPassiveAmount(3), 1); CHECK_EQ(p.orbEvokeAmount(3), 2);
    p.focus = 2;
    CHECK_EQ(p.orbPassiveAmount(0), 5); CHECK_EQ(p.orbEvokeAmount(0), 10);
    CHECK_EQ(p.orbPassiveAmount(2), 8); CHECK_EQ(p.orbEvokeAmount(2), 6);  // Dark's evoke doesn't follow Focus
    CHECK_EQ(p.orbPassiveAmount(3), 1); CHECK_EQ(p.orbEvokeAmount(3), 2);  // nor does Plasma's anything
    p.focus = -9;
    CHECK_EQ(p.orbPassiveAmount(0), 0); CHECK_EQ(p.orbEvokeAmount(0), 0);
    CHECK_EQ(p.orbPassiveAmount(1), 0); CHECK_EQ(p.orbEvokeAmount(1), 0);
    CHECK_EQ(p.orbPassiveAmount(2), 0); CHECK_EQ(p.orbEvokeAmount(2), 6);
}

void endOfTurnPassives() {
    Fight f(3);
    f.channel(Orb::LIGHTNING);
    f.channel(Orb::FROST);
    f.channel(Orb::DARK);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.damageTaken(), 3);
    CHECK_EQ(f.bc.player.darkOrbEvoke[2], 12);
    // The block came at the end of the player's turn and went at the start of the next.
    CHECK_EQ(f.bc.player.block, 0);
}

void plasmaAtStartOfTurn() {
    Fight f(1);
    f.channel(Orb::PLASMA);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.bc.player.energy, f.bc.player.energyPerTurn + 1);
}

void lockOnRoundsDown() {
    Fight f(2);
    f.channel(Orb::LIGHTNING);
    f.monster().addDebuff<MS::LOCK_ON>(2, false);
    f.play(CardId::DUALCAST);
    CHECK_EQ(f.damageTaken(), 24);
    f.monster().curHp = f.monster().maxHp;
    f.channel(Orb::LIGHTNING);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.damageTaken(), 4);  // (int)(3 * 1.5f)
    CHECK_EQ(f.monster().getStatus<MS::LOCK_ON>(), 1);
}

void darkHitsLowestHp() {
    Fight f(1, MonsterEncounter::THREE_CULTIST);
    f.monster(0).curHp = 100;
    f.monster(1).curHp = 40;
    f.monster(2).curHp = 40;
    f.channel(Orb::DARK);
    f.play(CardId::DUALCAST);
    CHECK_EQ(f.monster(1).curHp, 28);  // the first of the lowest, twice with the same amount
    CHECK_EQ(f.monster(2).curHp, 40);
    CHECK_EQ(f.monster(0).curHp, 100);
    CHECK(!f.bc.player.hasOrb());
}

void electrodynamicsHitsAll() {
    Fight f(1, MonsterEncounter::THREE_CULTIST);
    f.play(CardId::ELECTRODYNAMICS);  // channels 2 into 1 slot, evoking the first
    CHECK(f.bc.player.hasStatus<PS::ELECTRO>());
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(f.damageTaken(i), 8);
    }
}

// ******** Channels and evokes ********

void evokes() {
    Fight dual(1);
    dual.channel(Orb::LIGHTNING);
    dual.play(CardId::DUALCAST);
    CHECK_EQ(dual.damageTaken(), 16);
    CHECK(!dual.bc.player.hasOrb());

    Fight multi(1);
    multi.channel(Orb::LIGHTNING);
    multi.bc.player.energy = 3;
    multi.play(CardId::MULTI_CAST);
    CHECK_EQ(multi.damageTaken(), 24);
    CHECK_EQ(multi.bc.player.energy, 0);
    CHECK(!multi.bc.player.hasOrb());

    Fight multiUp(1);
    multiUp.channel(Orb::PLASMA);
    multiUp.bc.player.energy = 2;
    multiUp.play(CardId::MULTI_CAST, true);
    CHECK_EQ(multiUp.bc.player.energy, 6);  // spends 2, then 3 evokes of 2

    Fight empty(1);  // MulticastAction with no orb keeps the energy
    empty.bc.player.energy = 3;
    empty.play(CardId::MULTI_CAST);
    CHECK_EQ(empty.bc.player.energy, 3);

    Fight frost(1);
    frost.channel(Orb::FROST);
    frost.bc.player.focus = 1;
    frost.play(CardId::DUALCAST);
    CHECK_EQ(frost.bc.player.block, 12);
}

void tempest() {
    Fight f(4);
    f.bc.player.energy = 3;
    f.play(CardId::TEMPEST);
    CHECK_EQ(f.bc.player.energy, 0);
    CHECK_EQ(f.bc.player.filledOrbCount(), 3);
    CHECK_EQ(f.bc.cards.exhaustPile.size(), 1);

    Fight up(5);
    up.bc.player.energy = 3;
    up.play(CardId::TEMPEST, true);
    CHECK_EQ(up.bc.player.filledOrbCount(), 4);

    Fight zero(1);
    zero.bc.player.energy = 0;
    zero.play(CardId::TEMPEST);
    CHECK_EQ(zero.bc.player.filledOrbCount(), 0);
}

void recursionKeepsDarkCharge() {
    Fight f(1);
    f.channel(Orb::DARK);
    f.bc.player.darkOrbEvoke[0] = 20;
    f.play(CardId::RECURSION);
    CHECK_EQ(f.damageTaken(), 20);
    CHECK(f.bc.player.orbs[0] == Orb::DARK);
    CHECK_EQ(f.bc.player.darkOrbEvoke[0], 20);

    Fight none(1);
    none.play(CardId::RECURSION);
    CHECK(!none.bc.player.hasOrb());

    Fight count(1);
    count.channel(Orb::FROST);
    count.play(CardId::RECURSION);
    CHECK_EQ(count.bc.player.block, 5);
    CHECK_EQ(count.bc.player.frostChanneled, 2);  // channeling it again counts for Blizzard
}

void chaosRollsInOrbListOrder() {
    Fight f(2);
    Random rng = f.bc.cardRandomRng;
    const Orb order[] {Orb::DARK, Orb::FROST, Orb::LIGHTNING, Orb::PLASMA};
    const Orb first = order[rng.random(3)];
    const Orb second = order[rng.random(3)];
    f.play(CardId::CHAOS, true);
    CHECK(f.bc.player.orbs[0] == first);
    CHECK(f.bc.player.orbs[1] == second);
    CHECK_EQ(f.bc.cardRandomRng.counter, rng.counter);
}

void simpleChannels() {
    Fight rainbow(3);
    rainbow.play(CardId::RAINBOW);
    CHECK(rainbow.bc.player.orbs[0] == Orb::LIGHTNING);
    CHECK(rainbow.bc.player.orbs[1] == Orb::FROST);
    CHECK(rainbow.bc.player.orbs[2] == Orb::DARK);
    CHECK_EQ(rainbow.bc.cards.exhaustPile.size(), 1);

    Fight rainbowUp(3);
    rainbowUp.play(CardId::RAINBOW, true);
    CHECK_EQ(rainbowUp.bc.cards.exhaustPile.size(), 0);

    Fight snap(1);
    snap.play(CardId::COLD_SNAP, true);
    CHECK_EQ(snap.damageTaken(), 9);
    CHECK(snap.bc.player.orbs[0] == Orb::FROST);

    Fight ball(1);
    ball.play(CardId::BALL_LIGHTNING, true);
    CHECK_EQ(ball.damageTaken(), 10);
    CHECK(ball.bc.player.orbs[0] == Orb::LIGHTNING);

    Fight doom(1, MonsterEncounter::THREE_CULTIST);
    doom.play(CardId::DOOM_AND_GLOOM);
    CHECK_EQ(doom.damageTaken(2), 10);
    CHECK(doom.bc.player.orbs[0] == Orb::DARK);

    Fight meteor(3);
    meteor.play(CardId::METEOR_STRIKE);
    CHECK_EQ(meteor.damageTaken(), 24);
    CHECK_EQ(meteor.bc.player.filledOrbCount(), 3);
    CHECK(meteor.bc.player.orbs[2] == Orb::PLASMA);

    Fight glacier(2);
    glacier.play(CardId::GLACIER);
    CHECK_EQ(glacier.bc.player.block, 7);
    CHECK_EQ(glacier.bc.player.filledOrbCount(), 2);

    Fight cool(1);
    cool.toDraw(CardId::WOUND);
    cool.toDraw(CardId::WOUND);
    cool.play(CardId::COOLHEADED, true);
    CHECK(cool.bc.player.orbs[0] == Orb::FROST);
    CHECK_EQ(cool.bc.cards.cardsInHand, 2);

    Fight fusion(1);
    fusion.bc.player.energy = 2;
    fusion.play(CardId::FUSION);
    CHECK_EQ(fusion.bc.player.energy, 0);
    CHECK(fusion.bc.player.orbs[0] == Orb::PLASMA);

    Fight chill(3, MonsterEncounter::THREE_CULTIST);
    chill.monster(1).curHp = 0;
    chill.bc.monsters.monstersAlive = 2;
    chill.play(CardId::CHILL);
    CHECK_EQ(chill.bc.player.filledOrbCount(), 2);
    CHECK_EQ(chill.bc.cards.exhaustPile.size(), 1);

    Fight dark(1);
    dark.play(CardId::DARKNESS, true);
    CHECK_EQ(dark.bc.player.darkOrbEvoke[0], 12);  // DarkImpulseAction
}

void consumeAndCapacitor() {
    Fight f(1);
    f.channel(Orb::FROST);
    f.play(CardId::CONSUME);
    CHECK_EQ(f.bc.player.focus, 2);
    CHECK_EQ(static_cast<int>(f.bc.player.orbSlots), 0);
    CHECK(!f.bc.player.hasOrb());
    f.play(CardId::CONSUME, true);
    CHECK_EQ(f.bc.player.focus, 5);  // still gives Focus with no slot left

    Fight last(2);
    last.channel(Orb::LIGHTNING);
    last.channel(Orb::FROST);
    last.play(CardId::CONSUME);
    CHECK(last.bc.player.orbs[0] == Orb::LIGHTNING);  // the last slot goes
    CHECK(last.bc.player.orbs[1] == Orb::EMPTY);

    Fight cap(1);
    cap.play(CardId::CAPACITOR);
    CHECK_EQ(static_cast<int>(cap.bc.player.orbSlots), 3);
}

void fission() {
    Fight f(3);
    f.channel(Orb::LIGHTNING);
    f.channel(Orb::FROST);
    for (int i = 0; i < 3; ++i) {
        f.toDraw(CardId::WOUND);
    }
    f.bc.player.energy = 0;
    f.play(CardId::FISSION);
    CHECK_EQ(f.bc.player.filledOrbCount(), 0);
    CHECK_EQ(f.damageTaken(), 0);  // no evoke without the upgrade
    CHECK_EQ(f.bc.player.block, 0);
    CHECK_EQ(f.bc.player.energy, 2);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);
    CHECK_EQ(f.bc.cards.exhaustPile.size(), 1);

    Fight up(3);
    up.channel(Orb::LIGHTNING);
    up.channel(Orb::FROST);
    up.play(CardId::FISSION, true);
    CHECK_EQ(up.damageTaken(), 8);
    CHECK_EQ(up.bc.player.block, 5);
    CHECK_EQ(up.bc.player.filledOrbCount(), 0);
}

// ******** Attacks ********

void attacksThatCountOrbs() {
    Fight barrage(3);
    barrage.channel(Orb::FROST);
    barrage.channel(Orb::FROST);
    barrage.play(CardId::BARRAGE);
    CHECK_EQ(barrage.damageTaken(), 8);

    Fight none(3);
    none.play(CardId::BARRAGE, true);
    CHECK_EQ(none.damageTaken(), 0);

    Fight driver(4);
    driver.channel(Orb::FROST);
    driver.channel(Orb::FROST);
    driver.channel(Orb::DARK);
    for (int i = 0; i < 4; ++i) {
        driver.toDraw(CardId::WOUND);
    }
    driver.play(CardId::COMPILE_DRIVER);
    CHECK_EQ(driver.damageTaken(), 7);
    CHECK_EQ(driver.bc.cards.cardsInHand, 2);

    Fight blizzard(1, MonsterEncounter::THREE_CULTIST);
    blizzard.play(CardId::ZAP);
    blizzard.channel(Orb::FROST);  // evokes the Zap's Lightning at a random monster
    blizzard.channel(Orb::FROST);
    const int before = blizzard.damageTaken(0);
    blizzard.play(CardId::BLIZZARD);
    CHECK_EQ(blizzard.damageTaken(0) - before, 4);  // 2 Frost channeled, 2 each

    Fight strike(1);
    strike.channel(Orb::LIGHTNING);
    strike.channel(Orb::LIGHTNING);
    strike.play(CardId::THUNDER_STRIKE);
    CHECK_EQ(strike.damageTaken(), 8 + 14);  // the evoke, then two hits for two channeled

    Fight rip(1);
    rip.play(CardId::RIP_AND_TEAR, true);
    CHECK_EQ(rip.damageTaken(), 18);
}

void clawGrows() {
    Fight f(0);
    f.toDiscard(CardId::CLAW);
    f.toDraw(CardId::CLAW);
    const int inHand = f.toHand(CardId::CLAW);
    const int played = f.toHand(CardId::CLAW);
    (void)inHand;
    f.playIdx(played);
    CHECK_EQ(f.damageTaken(), 3);
    CHECK_EQ(f.bc.cards.hand[0].specialData, 2);
    CHECK_EQ(f.bc.cards.drawPile[0].specialData, 2);
    CHECK_EQ(f.bc.cards.discardPile[0].specialData, 2);
    CHECK_EQ(f.bc.cards.discardPile[1].specialData, 2);  // the card played lands behind the one there
    f.playIdx(0);
    CHECK_EQ(f.damageTaken(), 3 + 5);
    CHECK_EQ(f.bc.cards.drawPile[0].specialData, 4);

    // Double Tap's copy isn't in a pile when the first one's GashAction runs.
    Fight tap(0);
    tap.bc.player.buff<PS::DOUBLE_TAP>(1);
    tap.play(CardId::CLAW);
    CHECK_EQ(tap.damageTaken(), 6);
    CHECK_EQ(tap.bc.cards.discardPile[0].specialData, 4);
}

void streamlineGetsCheaper() {
    Fight f(0);
    f.bc.player.energy = 10;
    f.play(CardId::STREAMLINE);
    CHECK_EQ(f.damageTaken(), 15);
    CHECK_EQ(f.bc.player.energy, 8);
    CHECK_EQ(static_cast<int>(f.bc.cards.discardPile[0].cost), 1);
    CHECK_EQ(static_cast<int>(f.bc.cards.discardPile[0].costForTurn), 1);
}

void sunder() {
    Fight kill(0);
    kill.monster(0).curHp = 10;
    kill.toDraw(CardId::WOUND);
    Fight survive(0);
    survive.bc.player.energy = 3;
    survive.play(CardId::SUNDER);
    CHECK_EQ(survive.damageTaken(), 24);
    CHECK_EQ(survive.bc.player.energy, 0);

    Fight two(0, MonsterEncounter::THREE_CULTIST);
    two.monster(0).curHp = 10;
    two.bc.player.energy = 3;
    two.play(CardId::SUNDER);
    CHECK_EQ(two.bc.player.energy, 3);
}

void ftl() {
    Fight f(0);
    f.toDraw(CardId::WOUND);
    f.play(CardId::FTL);
    CHECK_EQ(f.damageTaken(), 5);
    CHECK_EQ(f.bc.cards.cardsInHand, 1);

    Fight late(0);
    late.toDraw(CardId::WOUND);
    late.bc.player.cardsPlayedThisTurn = 3;
    late.play(CardId::FTL);  // 4 played this turn, 3 before it: not less than 3
    CHECK_EQ(late.bc.cards.cardsInHand, 0);

    // The draw comes before the card lands in the discard pile, so a shuffle leaves it out.
    Fight shuffle(0);
    shuffle.toDiscard(CardId::WOUND);
    shuffle.play(CardId::FTL);
    CHECK_EQ(shuffle.bc.cards.cardsInHand, 1);
    CHECK_EQ(shuffle.bc.cards.discardPile.size(), 1);
    CHECK(shuffle.bc.cards.discardPile[0].getId() == CardId::FTL);
    CHECK_EQ(shuffle.bc.cards.drawPile.size(), 0);
}

void scrape() {
    Fight f(0);
    f.toDraw(CardId::ANGER);    // cost 0
    f.toDraw(CardId::DEFEND_RED);
    f.toDraw(CardId::WOUND);
    f.toDraw(CardId::WHIRLWIND);  // X costs -1, which isn't 0
    f.play(CardId::SCRAPE);
    CHECK_EQ(f.damageTaken(), 7);
    CHECK_EQ(f.bc.cards.cardsInHand, 1);
    CHECK(f.bc.cards.hand[0].getId() == CardId::ANGER);
    CHECK_EQ(f.bc.cards.discardPile.size(), 4);  // the three, then Scrape
    CHECK(f.bc.cards.discardPile[0].getId() == CardId::WHIRLWIND);
    CHECK(f.bc.cards.discardPile[3].getId() == CardId::SCRAPE);
    CHECK_EQ(f.bc.player.cardsDiscardedThisTurn, 3);

    // What's drawn before a shuffle counts with what's drawn after it.
    Fight shuffle(0);
    shuffle.toDraw(CardId::DEFEND_RED);
    shuffle.toDiscard(CardId::ANGER);
    shuffle.toDiscard(CardId::DEFEND_RED);
    shuffle.play(CardId::SCRAPE, true);
    CHECK_EQ(shuffle.bc.cards.cardsInHand, 1);
    CHECK(shuffle.bc.cards.hand[0].getId() == CardId::ANGER);
    CHECK_EQ(shuffle.bc.cards.discardPile.size(), 3);
}

void allForOne() {
    Fight f(0);
    f.toDiscard(CardId::ANGER);
    f.toDiscard(CardId::DEFEND_RED);
    f.toDiscard(CardId::CLAW);
    f.play(CardId::ALL_FOR_ONE);
    CHECK_EQ(f.damageTaken(), 10);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);
    CHECK(f.bc.cards.hand[0].getId() == CardId::ANGER);  // from the start of the pile
    CHECK(f.bc.cards.hand[1].getId() == CardId::CLAW);
    CHECK_EQ(f.bc.cards.discardPile.size(), 2);  // All For One landed before they moved

    Fight full(0);
    for (int i = 0; i < 9; ++i) {
        full.toHand(CardId::WOUND);
    }
    full.toDiscard(CardId::ANGER);
    full.toDiscard(CardId::ANGER);
    full.play(CardId::ALL_FOR_ONE);
    CHECK_EQ(full.bc.cards.cardsInHand, 10);
    CHECK_EQ(full.bc.cards.discardPile.size(), 2);  // one Anger stays, and All For One
}

void simpleAttacks() {
    Fight beam(0);
    beam.play(CardId::BEAM_CELL, true);
    CHECK_EQ(beam.damageTaken(), 4);
    CHECK_EQ(beam.monster().getStatus<MS::VULNERABLE>(), 2);

    Fight lock(0);
    lock.play(CardId::BULLSEYE);
    CHECK_EQ(lock.damageTaken(), 8);
    CHECK_EQ(lock.monster().getStatus<MS::LOCK_ON>(), 2);

    Fight melter(0);
    melter.monster().block = 50;
    melter.play(CardId::MELTER);
    CHECK_EQ(melter.monster().block, 0);
    CHECK_EQ(melter.damageTaken(), 10);

    Fight surge(0);
    surge.play(CardId::CORE_SURGE);
    CHECK_EQ(surge.damageTaken(), 11);
    CHECK_EQ(surge.bc.player.artifact, 1);
    CHECK_EQ(surge.bc.cards.exhaustPile.size(), 1);

    Fight hyper(0);
    hyper.bc.player.focus = 5;
    hyper.play(CardId::HYPERBEAM);
    CHECK_EQ(hyper.damageTaken(), 26);
    CHECK_EQ(hyper.bc.player.focus, 2);

    Fight blocked(0);
    blocked.bc.player.artifact = 1;
    blocked.play(CardId::HYPERBEAM);
    CHECK_EQ(blocked.bc.player.focus, 0);  // negative Focus is a debuff
    CHECK_EQ(blocked.bc.player.artifact, 0);

    Fight sweep(0, MonsterEncounter::THREE_CULTIST);
    sweep.toDraw(CardId::WOUND);
    sweep.play(CardId::SWEEPING_BEAM);
    CHECK_EQ(sweep.damageTaken(1), 6);
    CHECK_EQ(sweep.bc.cards.cardsInHand, 1);

    Fight eyes(0, MonsterEncounter::JAW_WORM);
    eyes.play(CardId::GO_FOR_THE_EYES);  // Jaw Worm opens with an attack
    CHECK_EQ(eyes.monster().getStatus<MS::WEAK>(), 1);

    Fight notAttacking(0);
    notAttacking.play(CardId::GO_FOR_THE_EYES);  // Cultist opens with Incantation
    CHECK_EQ(notAttacking.monster().getStatus<MS::WEAK>(), 0);
}

// ******** Skills ********

void simpleBlock() {
    Fight leap(0);
    leap.play(CardId::LEAP, true);
    CHECK_EQ(leap.bc.player.block, 12);

    Fight boot(0);
    boot.play(CardId::BOOT_SEQUENCE);
    CHECK_EQ(boot.bc.player.block, 10);
    CHECK_EQ(boot.bc.cards.exhaustPile.size(), 1);

    Fight shields(0);
    shields.play(CardId::AUTO_SHIELDS);
    CHECK_EQ(shields.bc.player.block, 11);
    shields.play(CardId::AUTO_SHIELDS);
    CHECK_EQ(shields.bc.player.block, 11);

    Fight stack(0);
    stack.toDiscard(CardId::WOUND);
    stack.toDiscard(CardId::WOUND);
    stack.play(CardId::STACK, true);
    CHECK_EQ(stack.bc.player.block, 5);

    Fight field(0);
    field.play(CardId::FORCE_FIELD);
    CHECK_EQ(field.bc.player.block, 12);
    CHECK_EQ(field.bc.player.energy, 6);

    Fight undo(0);
    undo.play(CardId::EQUILIBRIUM);
    CHECK_EQ(undo.bc.player.block, 13);
    CHECK_EQ(undo.bc.player.getStatus<PS::EQUILIBRIUM>(), 1);

    Fight battery(0);
    battery.play(CardId::CHARGE_BATTERY);
    CHECK_EQ(battery.bc.player.block, 7);
    battery.toDraw(CardId::WOUND);
    battery.endTurn();
    CHECK_EQ(battery.bc.player.energy, battery.bc.player.energyPerTurn + 1);
    CHECK(!battery.bc.player.hasStatus<PS::ENERGIZED_BLUE>());

    Fight body(0);
    body.bc.player.energy = 2;
    body.play(CardId::REINFORCED_BODY);
    CHECK_EQ(body.bc.player.block, 14);
    CHECK_EQ(body.bc.player.energy, 0);
}

void equilibriumKeepsTheHand() {
    Fight f(0);
    f.toHand(CardId::WOUND);
    f.toHand(CardId::DAZED);  // ethereal, so it isn't kept
    f.toDraw(CardId::DEFEND_RED);
    f.toDraw(CardId::DEFEND_RED);
    f.play(CardId::EQUILIBRIUM);
    f.endTurn();
    CHECK(!f.bc.player.hasStatus<PS::EQUILIBRIUM>());
    CHECK(f.bc.cards.hand[0].getId() == CardId::WOUND);
    CHECK_EQ(f.bc.cards.exhaustPile.size(), 1);
}

void steamBarrierLosesBlock() {
    Fight f(0);
    f.play(CardId::STEAM_BARRIER);
    CHECK_EQ(f.bc.player.block, 6);
    auto &c = f.bc.cards.discardPile[0];
    CHECK_EQ(c.specialData, 1);
    const auto again = c;
    f.bc.cards.discardPile.clear();
    f.bc.cards.moveToHand(again);
    f.playIdx(0);
    CHECK_EQ(f.bc.player.block, 11);

    // baseBlock goes below 0, and Dexterity starts from there.
    Fight deep(0);
    deep.bc.player.dexterity = 3;
    auto low = deep.card(CardId::STEAM_BARRIER);
    low.specialData = 8;
    deep.bc.cards.moveToHand(low);
    deep.playIdx(0);
    CHECK_EQ(deep.bc.player.block, 1);
    Fight none(0);
    auto zero = none.card(CardId::STEAM_BARRIER);
    zero.specialData = 8;
    none.bc.cards.moveToHand(zero);
    none.playIdx(0);
    CHECK_EQ(none.bc.player.block, 0);
}

void geneticAlgorithmPersists() {
    Fight f(0);
    f.gc.deck.obtainRaw(Card(CardId::GENETIC_ALGORITHM));
    const int deckIdx = f.gc.deck.size() - 1;
    CHECK_EQ(f.gc.deck.cards[deckIdx].misc, 1);
    auto c = CardInstance(f.gc.deck.cards[deckIdx]);
    c.setUniqueId(deckIdx);
    f.bc.cards.moveToHand(c);
    f.toHand(CardId::WOUND);
    f.playIdx(0);
    CHECK_EQ(f.bc.player.block, 1);  // the block from before the increase
    CHECK_EQ(f.bc.cards.exhaustPile[0].specialData, 3);

    Fight up(0);
    up.play(CardId::GENETIC_ALGORITHM, true);
    CHECK_EQ(up.bc.player.block, 1);  // an upgrade only changes the increase
    CHECK_EQ(up.bc.cards.exhaustPile[0].specialData, 4);
}

void energyCards() {
    Fight turbo(0);
    turbo.bc.player.energy = 0;
    turbo.play(CardId::TURBO, true);
    CHECK_EQ(turbo.bc.player.energy, 3);
    CHECK(turbo.bc.cards.discardPile[0].getId() == CardId::VOID);

    Fight dbl(0);
    dbl.bc.player.energy = 3;
    dbl.play(CardId::DOUBLE_ENERGY);
    CHECK_EQ(dbl.bc.player.energy, 4);  // pays 1 first

    Fight agg(0);
    agg.bc.player.energy = 1;
    for (int i = 0; i < 9; ++i) {
        agg.toDraw(CardId::WOUND);
    }
    agg.play(CardId::AGGREGATE);
    CHECK_EQ(agg.bc.player.energy, 2);

    Fight aggUp(0);
    aggUp.bc.player.energy = 1;
    for (int i = 0; i < 9; ++i) {
        aggUp.toDraw(CardId::WOUND);
    }
    aggUp.play(CardId::AGGREGATE, true);
    CHECK_EQ(aggUp.bc.player.energy, 3);

    Fight over(0);
    over.toDraw(CardId::WOUND);
    over.toDraw(CardId::WOUND);
    over.play(CardId::OVERCLOCK);
    CHECK_EQ(over.bc.cards.cardsInHand, 2);
    CHECK(over.bc.cards.discardPile[0].getId() == CardId::BURN);

    Fight skim(0);
    for (int i = 0; i < 4; ++i) {
        skim.toDraw(CardId::WOUND);
    }
    skim.play(CardId::SKIM);
    CHECK_EQ(skim.bc.cards.cardsInHand, 3);
}

void recycle() {
    Fight one(0);
    one.toHand(CardId::BLUDGEON);
    one.bc.player.energy = 1;
    one.play(CardId::RECYCLE, false);  // Bludgeon is the only card left, so no screen
    CHECK_EQ(one.bc.player.energy, 3);
    CHECK_EQ(one.bc.cards.exhaustPile.size(), 1);

    Fight screen(0);
    screen.toHand(CardId::WHIRLWIND);
    screen.toHand(CardId::WOUND);
    screen.bc.player.energy = 2;
    screen.play(CardId::RECYCLE);
    CHECK(screen.bc.inputState == InputState::CARD_SELECT);
    CHECK(screen.bc.cardSelectInfo.cardSelectTask == CardSelectTask::RECYCLE);
    CHECK_EQ(SAction::enumerateCardSelectActions(screen.bc).size(), 2);
    screen.select(0);  // X cost: gains the energy left
    CHECK_EQ(screen.bc.player.energy, 2);
    CHECK_EQ(screen.bc.cards.cardsInHand, 1);

    Fight wound(0);
    wound.toHand(CardId::WOUND);
    wound.bc.player.energy = 1;
    wound.play(CardId::RECYCLE);
    CHECK_EQ(wound.bc.player.energy, 0);  // unplayable gains nothing
}

void hologramAndSeek() {
    Fight holo(0);
    holo.toDiscard(CardId::WOUND);
    holo.toDiscard(CardId::ANGER);
    holo.play(CardId::HOLOGRAM);
    CHECK_EQ(holo.bc.player.block, 3);
    CHECK(holo.bc.cardSelectInfo.cardSelectTask == CardSelectTask::HOLOGRAM);
    CHECK_EQ(SAction::enumerateCardSelectActions(holo.bc).size(), 2);
    holo.select(1);
    CHECK(holo.bc.cards.hand[0].getId() == CardId::ANGER);
    CHECK_EQ(holo.bc.cards.exhaustPile.size(), 1);

    Fight seek(0);
    seek.toDraw(CardId::WOUND);
    seek.toDraw(CardId::ANGER);
    seek.toDraw(CardId::DEFEND_RED);
    seek.play(CardId::SEEK, true);
    CHECK(seek.bc.inputState == InputState::CARD_SELECT);
    CHECK_EQ(SAction::enumerateCardSelectActions(seek.bc).size(), 3);
    seek.select(1);
    CHECK(seek.bc.inputState == InputState::CARD_SELECT);
    CHECK_EQ(SAction::enumerateCardSelectActions(seek.bc).size(), 2);
    seek.select(0);
    CHECK(seek.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK_EQ(seek.bc.cards.cardsInHand, 2);
    CHECK(seek.bc.cards.hand[0].getId() == CardId::ANGER);
    CHECK(seek.bc.cards.hand[1].getId() == CardId::WOUND);
    CHECK_EQ(seek.bc.cards.drawPile.size(), 1);

    // Not more cards than it takes: no screen, and they come from the bottom of the pile first.
    Fight all(0);
    all.toDraw(CardId::WOUND);
    all.toDraw(CardId::ANGER);
    all.play(CardId::SEEK, true);
    CHECK(all.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK(all.bc.cards.hand[0].getId() == CardId::WOUND);
    CHECK(all.bc.cards.hand[1].getId() == CardId::ANGER);
}

void rebootMovesEverything() {
    Fight f(0);
    f.toHand(CardId::WOUND);
    f.toHand(CardId::DAZED);
    f.toDiscard(CardId::ANGER);
    f.toDraw(CardId::DEFEND_RED);
    Random shuffleRng = f.bc.shuffleRng;
    const int cardRolls = f.bc.cardRandomRng.counter;
    const int shuffleRolls = shuffleRng.counter;
    f.play(CardId::REBOOT);
    CHECK_EQ(f.bc.cards.cardsInHand, 4);
    CHECK_EQ(f.bc.cards.drawPile.size(), 0);
    CHECK_EQ(f.bc.cards.discardPile.size(), 0);
    CHECK_EQ(f.bc.cards.exhaustPile.size(), 1);
    CHECK_EQ(f.bc.cardRandomRng.counter - cardRolls, 2);  // one for each card in hand
    CHECK_EQ(f.bc.shuffleRng.counter - shuffleRolls, 2);  // the discard pile, then the draw pile
}

void reprogramAndOthers() {
    Fight f(0);
    f.bc.player.focus = 3;
    f.play(CardId::REPROGRAM, true);
    CHECK_EQ(f.bc.player.focus, 1);
    CHECK_EQ(f.bc.player.strength, 2);
    CHECK_EQ(f.bc.player.dexterity, 2);

    Fight noise(0);
    noise.play(CardId::WHITE_NOISE);
    CHECK_EQ(noise.bc.cards.cardsInHand, 1);
    const auto &c = noise.bc.cards.hand[0];
    CHECK(c.getType() == CardType::POWER);
    CHECK(getCardColor(c.getId()) == CardColor::RED);  // the class's own pool, Prismatic Shard or not
    CHECK_EQ(static_cast<int>(c.costForTurn), 0);
    CHECK_EQ(noise.bc.cards.exhaustPile.size(), 1);
}

// ******** Powers ********

void reboundSendsTheNextCardBack() {
    Fight f(0);
    f.play(CardId::REBOUND);
    CHECK_EQ(f.damageTaken(), 9);
    CHECK(f.bc.cards.discardPile[0].getId() == CardId::REBOUND);  // not itself
    CHECK(f.bc.player.hasStatus<PS::REBOUND>());
    f.play(CardId::ANGER);
    CHECK(f.bc.cards.drawPile.back().getId() == CardId::ANGER);
    CHECK(!f.bc.player.hasStatus<PS::REBOUND>());
    f.play(CardId::ANGER);
    CHECK(f.bc.cards.discardPile.back().getId() == CardId::ANGER);

    // A second Rebound goes back itself and leaves the power on for the next card.
    Fight two(0);
    two.play(CardId::REBOUND);
    two.play(CardId::REBOUND);
    CHECK(two.bc.cards.drawPile.back().getId() == CardId::REBOUND);
    CHECK_EQ(two.bc.player.getStatus<PS::REBOUND>(), 1);

    // A Power uses it up without being sent back.
    Fight power(0);
    power.play(CardId::REBOUND);
    power.play(CardId::DEFRAGMENT);
    CHECK(!power.bc.player.hasStatus<PS::REBOUND>());
    CHECK_EQ(power.bc.cards.drawPile.size(), 0);

    // Gone at the end of the turn.
    Fight turn(0);
    turn.play(CardId::REBOUND);
    turn.toDraw(CardId::WOUND);
    turn.endTurn();
    CHECK(!turn.bc.player.hasStatus<PS::REBOUND>());
}

void amplifyPlaysPowersTwice() {
    Fight f(0);
    f.play(CardId::AMPLIFY);
    f.bc.player.energy = 5;
    f.play(CardId::DEFRAGMENT);
    CHECK_EQ(f.bc.player.focus, 2);
    CHECK_EQ(f.bc.player.energy, 4);  // only the first one is paid for
    CHECK(!f.bc.player.hasStatus<PS::AMPLIFY>());

    Fight skill(0);
    skill.play(CardId::AMPLIFY);
    skill.play(CardId::LEAP);
    CHECK_EQ(skill.bc.player.block, 9);
    CHECK(skill.bc.player.hasStatus<PS::AMPLIFY>());
    skill.toDraw(CardId::WOUND);
    skill.endTurn();
    CHECK(!skill.bc.player.hasStatus<PS::AMPLIFY>());
}

void powersOnPlayingPowers() {
    Fight storm(2);
    storm.toDraw(CardId::WOUND);  // a fight with no cards left counts as lost
    storm.play(CardId::STORM);
    CHECK_EQ(storm.bc.player.filledOrbCount(), 0);  // Storm doesn't see itself
    storm.play(CardId::DEFRAGMENT);
    CHECK_EQ(storm.bc.player.filledOrbCount(), 1);
    CHECK(storm.bc.player.orbs[0] == Orb::LIGHTNING);

    Fight sinks(0);
    sinks.toDraw(CardId::WOUND);
    sinks.toDraw(CardId::WOUND);
    sinks.play(CardId::HEATSINKS, true);
    CHECK_EQ(sinks.bc.cards.cardsInHand, 0);
    sinks.play(CardId::DEFRAGMENT);
    CHECK_EQ(sinks.bc.cards.cardsInHand, 2);

    Fight field(0);
    field.toDiscard(CardId::FORCE_FIELD);
    field.toDraw(CardId::FORCE_FIELD);
    field.toHand(CardId::FORCE_FIELD);
    field.play(CardId::DEFRAGMENT);
    CHECK_EQ(static_cast<int>(field.bc.cards.hand[0].costForTurn), 3);
    CHECK_EQ(static_cast<int>(field.bc.cards.discardPile[0].cost), 3);
    CHECK_EQ(static_cast<int>(field.bc.cards.drawPile[0].cost), 3);
    CHECK_EQ(static_cast<int>(field.bc.player.powersPlayedThisCombat), 1);
}

void forceFieldKeepsItsDiscount() {
    GameContext gc(CharacterClass::IRONCLAD, 77, 0);
    gc.powersPlayedLastCombat = 2;
    gc.deck.obtain(gc, Card(CardId::FORCE_FIELD), 1);
    const auto &deckCard = gc.deck.cards.back();
    CHECK_EQ(deckCard.misc, 2);
    const CardInstance c(deckCard);
    CHECK_EQ(static_cast<int>(c.cost), 2);
    CHECK_EQ(static_cast<int>(c.costForTurn), 2);
    CHECK_EQ(c.specialData, 0);
}

void staticDischarge() {
    Fight f(1, MonsterEncounter::JAW_WORM);
    f.bc.player.buff<PS::STATIC_DISCHARGE>(1);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK(f.bc.player.orbs[0] == Orb::LIGHTNING);

    Fight blocked(1, MonsterEncounter::JAW_WORM);
    blocked.bc.player.buff<PS::STATIC_DISCHARGE>(1);
    blocked.bc.player.block = 100;
    blocked.toDraw(CardId::WOUND);
    blocked.endTurn();
    CHECK(!blocked.bc.player.hasOrb());
}

void loop() {
    Fight f(1);
    f.channel(Orb::LIGHTNING);
    f.toDraw(CardId::WOUND);
    f.play(CardId::LOOP);
    f.endTurn();
    CHECK_EQ(f.damageTaken(), 6);  // the end of this turn, and Loop at the start of the next

    Fight plasma(1);
    plasma.channel(Orb::PLASMA);
    plasma.toDraw(CardId::WOUND);
    plasma.play(CardId::LOOP, true);
    plasma.endTurn();
    CHECK_EQ(plasma.bc.player.energy, plasma.bc.player.energyPerTurn + 3);
}

void biasedCognition() {
    Fight f(0);
    f.toDraw(CardId::WOUND);
    f.play(CardId::BIASED_COGNITION);
    CHECK_EQ(f.bc.player.focus, 4);
    CHECK(f.bc.player.hasStatus<PS::BIAS>());
    f.endTurn();
    CHECK_EQ(f.bc.player.focus, 3);

    Fight artifact(0);
    artifact.bc.player.artifact = 1;
    artifact.play(CardId::BIASED_COGNITION);
    CHECK(!artifact.bc.player.hasStatus<PS::BIAS>());
    CHECK_EQ(artifact.bc.player.artifact, 0);

    Fight lost(0);
    lost.bc.player.focus = -2;
    lost.bc.player.removeDebuffs();
    CHECK_EQ(lost.bc.player.focus, 0);
}

void cardsMadeEachTurn() {
    Fight hello(0);
    hello.toDraw(CardId::WOUND);
    hello.play(CardId::HELLO_WORLD);
    hello.endTurn();
    CHECK_EQ(hello.bc.cards.cardsInHand, 2);
    CHECK(getCardColor(hello.bc.cards.hand[0].getId()) == CardColor::RED);
    CHECK(getCardRarity(hello.bc.cards.hand[0].getId()) == CardRarity::COMMON);

    Fight ai(0);
    ai.toDraw(CardId::WOUND);
    ai.play(CardId::CREATIVE_AI);
    ai.endTurn();
    CHECK_EQ(ai.bc.cards.cardsInHand, 2);
    CHECK(ai.bc.cards.hand[0].getType() == CardType::POWER);
    CHECK(getCardColor(ai.bc.cards.hand[0].getId()) == CardColor::RED);
}

void otherPowers() {
    Fight ml(0);
    const int draw = ml.bc.player.cardDrawPerTurn;
    for (int i = 0; i < 10; ++i) {
        ml.toDraw(CardId::WOUND);
    }
    ml.play(CardId::MACHINE_LEARNING);
    CHECK_EQ(static_cast<int>(ml.bc.player.cardDrawPerTurn), draw + 1);
    ml.endTurn();
    CHECK_EQ(ml.bc.cards.cardsInHand, draw + 1);

    Fight buffer(0);
    buffer.play(CardId::BUFFER, true);
    CHECK_EQ(buffer.bc.player.getStatus<PS::BUFFER>(), 2);

    Fight echo(0);
    echo.toDraw(CardId::WOUND);
    echo.play(CardId::ECHO_FORM);
    echo.play(CardId::LEAP);
    CHECK_EQ(echo.bc.player.block, 9);  // Echo Form was this turn's first card
    echo.endTurn();
    echo.play(CardId::LEAP);
    CHECK_EQ(echo.bc.player.block, 18);
    echo.play(CardId::LEAP);
    CHECK_EQ(echo.bc.player.block, 27);

    Fight defrag(0);
    defrag.play(CardId::DEFRAGMENT, true);
    CHECK_EQ(defrag.bc.player.focus, 2);

    Fight repair(0);
    repair.toDraw(CardId::WOUND);
    repair.play(CardId::SELF_REPAIR);
    CHECK_EQ(repair.bc.player.getStatus<PS::SELF_REPAIR>(), 7);
    repair.gc.curHp = 50;
    repair.bc.player.curHp = 50;
    repair.monster().curHp = 1;
    repair.play(CardId::ZAP);  // no slot, so it does nothing
    repair.play(CardId::STRIKE_RED);
    CHECK(repair.bc.outcome == Outcome::PLAYER_VICTORY);
    repair.gc.curHp = repair.bc.player.curHp;
    repair.bc.updateRelicsOnExit(repair.gc);
    CHECK_EQ(repair.gc.curHp, 50 + 6 + 7);  // Burning Blood, then the power
}

void spireShieldCanLowerFocus() {
    Fight f(1);
    CHECK_EQ(static_cast<int>(f.bc.player.orbSlots), 1);
    f.bc.player.debuff<PS::FOCUS>(-1);
    CHECK_EQ(f.bc.player.focus, -1);
    CHECK_EQ(f.bc.player.getStatus<PS::FOCUS>(), -1);
}

}  // namespace

int main() {
    orbSlots();
    channelWhenFullEvokesFirst();
    focusChangesOrbs();
    endOfTurnPassives();
    plasmaAtStartOfTurn();
    lockOnRoundsDown();
    darkHitsLowestHp();
    electrodynamicsHitsAll();
    evokes();
    tempest();
    recursionKeepsDarkCharge();
    chaosRollsInOrbListOrder();
    simpleChannels();
    consumeAndCapacitor();
    fission();
    attacksThatCountOrbs();
    clawGrows();
    streamlineGetsCheaper();
    sunder();
    ftl();
    scrape();
    allForOne();
    simpleAttacks();
    simpleBlock();
    equilibriumKeepsTheHand();
    steamBarrierLosesBlock();
    geneticAlgorithmPersists();
    energyCards();
    recycle();
    hologramAndSeek();
    rebootMovesEverything();
    reprogramAndOthers();
    reboundSendsTheNextCardBack();
    amplifyPlaysPowersTwice();
    powersOnPlayingPowers();
    forceFieldKeepsItsDiscount();
    staticDischarge();
    loop();
    biasedCognition();
    cardsMadeEachTurn();
    otherPowers();
    spireShieldCanLowerFocus();

    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
