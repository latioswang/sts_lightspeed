// Plays each Silent card in a set-up fight and checks the result against what the game's bytecode does, then plays random
// fights with every green card, and runs the search on them. The checks encode a reading of the bytecode, so this catches
// regressions and crashes and doesn't show the simulator matches the game. Mirror mode does that.
// Build target: test_silent_cards.

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
    // The monster's next move, so a turn's end doesn't hurt.
    void calmPlayer() { bc.player.curHp = bc.player.maxHp = 1000; }
};

// ******** Numbers ********

void simpleAttacks() {
    struct Row { CardId id; int base; int up; };
    for (const Row &r : std::initializer_list<Row>{
            {CardId::STRIKE_GREEN, 6, 9}, {CardId::BACKSTAB, 11, 15}, {CardId::DASH, 10, 13}, {CardId::FLYING_KNEE, 8, 11},
            {CardId::QUICK_SLASH, 8, 12}, {CardId::SLICE, 6, 9}, {CardId::SUCKER_PUNCH, 7, 9}, {CardId::NEUTRALIZE, 3, 4},
            {CardId::POISONED_STAB, 6, 8}, {CardId::SNEAKY_STRIKE, 12, 16}, {CardId::CHOKE, 12, 12}, {CardId::PREDATOR, 15, 20},
            {CardId::ENDLESS_AGONY, 4, 6}, {CardId::MASTERFUL_STAB, 12, 16}, {CardId::UNLOAD, 14, 18}, {CardId::SHIV, 4, 6},
            {CardId::HEEL_HOOK, 5, 8}, {CardId::DAGGER_THROW, 9, 12}}) {
        for (const bool up : {false, true}) {
            Fight f;
            f.toDraw(CardId::WOUND);
            f.toDraw(CardId::WOUND);
            f.toHand(CardId::WOUND);
            f.toHand(CardId::WOUND);
            f.play(r.id, up);
            if (f.selecting(CardSelectTask::DISCARD)) {
                f.select(0);
            }
            CHECK_EQ(f.damageTaken(), up ? r.up : r.base);
        }
    }

    for (const bool up : {false, true}) {
        Fight multi;
        multi.play(CardId::EVISCERATE, up);
        CHECK_EQ(multi.damageTaken(), 3 * (up ? 9 : 7));
        Fight riddle;
        riddle.play(CardId::RIDDLE_WITH_HOLES, up);
        CHECK_EQ(riddle.damageTaken(), 5 * (up ? 4 : 3));
        Fight knife;
        knife.play(CardId::GLASS_KNIFE, up);
        CHECK_EQ(knife.damageTaken(), 2 * (up ? 12 : 8));
    }
}

void aoeAttacks() {
    Fight die(MonsterEncounter::TWO_LOUSE);
    die.toDraw(CardId::WOUND);
    die.play(CardId::DIE_DIE_DIE, false, 0);
    CHECK_EQ(die.damageTaken(0), 13);
    CHECK_EQ(die.damageTaken(1), 13);
    CHECK_EQ(static_cast<int>(die.bc.cards.exhaustPile.size()), 1);

    Fight spray(MonsterEncounter::TWO_LOUSE);
    spray.monster(1).addDebuff<MS::VULNERABLE>(1, false);
    spray.play(CardId::DAGGER_SPRAY, true);
    CHECK_EQ(spray.damageTaken(0), 12);
    CHECK_EQ(spray.damageTaken(1), 18);

    // Both hits use the damage from when it was played, so the Byrd's Flight running out in between changes nothing.
    Fight allOut(MonsterEncounter::TWO_LOUSE);
    const int discardMe = allOut.toHand(CardId::WOUND);
    (void)discardMe;
    allOut.play(CardId::ALL_OUT_ATTACK);
    CHECK_EQ(allOut.damageTaken(0), 10);
    CHECK_EQ(static_cast<int>(allOut.bc.cards.discardPile.size()), 2);  // the Wound, with no roll, then the card
    CHECK_EQ(allOut.bc.cardRandomRng.counter, Fight(MonsterEncounter::TWO_LOUSE).bc.cardRandomRng.counter);

    Fight allOutRoll;
    allOutRoll.toHand(CardId::WOUND);
    allOutRoll.toHand(CardId::DAZED);
    const auto rollsBefore = allOutRoll.bc.cardRandomRng.counter;
    allOutRoll.play(CardId::ALL_OUT_ATTACK, true);
    CHECK_EQ(allOutRoll.damageTaken(), 14);
    CHECK_EQ(allOutRoll.bc.cardRandomRng.counter, rollsBefore + 1);
    CHECK_EQ(allOutRoll.bc.cards.cardsInHand, 1);

    Fight finale;
    finale.toDraw(CardId::WOUND);
    finale.toHand(CardId::GRAND_FINALE);
    CHECK(!finale.bc.cards.hand[0].canUse(finale.bc, 0, false));  // the draw pile has a card
    finale.bc.cards.drawPile.clear();
    CHECK(finale.bc.cards.hand[0].canUse(finale.bc, 0, false));
    finale.playIdx(0);
    CHECK_EQ(finale.damageTaken(), 50);
    Fight finaleUp;
    finaleUp.play(CardId::GRAND_FINALE, true);
    CHECK_EQ(finaleUp.damageTaken(), 60);
}

void simpleBlock() {
    struct Row { CardId id; int base; int up; };
    for (const Row &r : std::initializer_list<Row>{
            {CardId::DEFEND_GREEN, 5, 8}, {CardId::DEFLECT, 4, 7}, {CardId::BACKFLIP, 5, 8}, {CardId::BLUR, 5, 8},
            {CardId::CLOAK_AND_DAGGER, 6, 6}, {CardId::DODGE_AND_ROLL, 4, 6}, {CardId::LEG_SWEEP, 11, 14}, {CardId::SURVIVOR, 8, 11}}) {
        for (const bool up : {false, true}) {
            Fight f;
            f.toDraw(CardId::WOUND);
            f.toDraw(CardId::WOUND);
            f.toHand(CardId::WOUND);
            f.play(r.id, up);
            CHECK_EQ(f.bc.player.block, up ? r.up : r.base);
        }
    }

    Fight dex;
    dex.bc.player.buff<PS::DEXTERITY>(2);
    dex.play(CardId::DODGE_AND_ROLL);
    CHECK_EQ(dex.bc.player.block, 6);
    CHECK_EQ(dex.bc.player.getStatus<PS::NEXT_TURN_BLOCK>(), 6);

    Fight noBlock;
    noBlock.bc.player.debuff<PS::NO_BLOCK>(2);
    noBlock.play(CardId::DODGE_AND_ROLL);
    CHECK(noBlock.bc.player.hasStatus<PS::NEXT_TURN_BLOCK>());  // 0 still applies the power
    CHECK_EQ(noBlock.bc.player.getStatus<PS::NEXT_TURN_BLOCK>(), 0);
}

// ******** Poison ********

void poisonTicks() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::DEADLY_POISON);
    CHECK_EQ(f.monster().getStatus<MS::POISON>(), 5);
    CHECK_EQ(f.damageTaken(), 0);
    f.monster().block = 20;
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.damageTaken(), 5);  // HP_LOSS skips block
    CHECK_EQ(f.monster().getStatus<MS::POISON>(), 4);
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.damageTaken(), 9);
    CHECK_EQ(f.monster().getStatus<MS::POISON>(), 3);

    Fight last;
    last.calmPlayer();
    last.monster().addDebuff<MS::POISON>(1);
    last.toDraw(CardId::WOUND);
    last.endTurn();
    CHECK(!last.monster().hasStatus<MS::POISON>());

    Fight kill;
    kill.calmPlayer();
    kill.monster().curHp = 3;
    kill.monster().addDebuff<MS::POISON>(3);
    kill.endTurn();
    CHECK(kill.bc.outcome == Outcome::PLAYER_VICTORY);

    Fight intangible;  // a monster's Intangible only changes damage worked out from a card or an attack
    intangible.calmPlayer();
    intangible.monster().buff<MS::INTANGIBLE>(2);
    intangible.monster().addDebuff<MS::POISON>(7);
    intangible.toDraw(CardId::WOUND);
    intangible.endTurn();
    CHECK_EQ(intangible.damageTaken(), 7);

    Fight artifact;
    artifact.monster().buff<MS::ARTIFACT>(1);
    artifact.play(CardId::DEADLY_POISON);
    CHECK(!artifact.monster().hasStatus<MS::POISON>());
    CHECK(!artifact.monster().hasStatus<MS::ARTIFACT>());

    Fight skull;
    skull.bc.player.setHasRelic<R::SNECKO_SKULL>(true);
    skull.play(CardId::DEADLY_POISON);
    CHECK_EQ(skull.monster().getStatus<MS::POISON>(), 6);
    skull.play(CardId::CATALYST);
    CHECK_EQ(skull.monster().getStatus<MS::POISON>(), 13);  // Snake Skull adds to Catalyst's power too
}

void catalyst() {
    Fight none;
    none.play(CardId::CATALYST);
    CHECK(!none.monster().hasStatus<MS::POISON>());
    CHECK_EQ(static_cast<int>(none.bc.cards.exhaustPile.size()), 1);

    Fight twice;
    twice.monster().addDebuff<MS::POISON>(7);
    twice.play(CardId::CATALYST);
    CHECK_EQ(twice.monster().getStatus<MS::POISON>(), 14);

    Fight three;
    three.monster().addDebuff<MS::POISON>(7);
    three.play(CardId::CATALYST, true);
    CHECK_EQ(three.monster().getStatus<MS::POISON>(), 21);
}

void poisonCards() {
    Fight stab;
    stab.play(CardId::POISONED_STAB, true);
    CHECK_EQ(stab.monster().getStatus<MS::POISON>(), 4);

    Fight corpse;
    corpse.play(CardId::CORPSE_EXPLOSION, true);
    CHECK_EQ(corpse.monster().getStatus<MS::POISON>(), 9);
    CHECK_EQ(corpse.monster().getStatus<MS::CORPSE_EXPLOSION>(), 1);

    Fight cloud(MonsterEncounter::TWO_LOUSE);
    cloud.play(CardId::CRIPPLING_CLOUD, true);
    CHECK_EQ(cloud.monster(0).getStatus<MS::POISON>(), 7);
    CHECK_EQ(cloud.monster(1).getStatus<MS::POISON>(), 7);
    CHECK_EQ(cloud.monster(0).getStatus<MS::WEAK>(), 2);
    CHECK_EQ(cloud.monster(1).getStatus<MS::WEAK>(), 2);
    CHECK_EQ(static_cast<int>(cloud.bc.cards.exhaustPile.size()), 1);

    Fight bane;
    bane.play(CardId::BANE);
    CHECK_EQ(bane.damageTaken(), 7);  // no poison, so one hit
    bane.monster().addDebuff<MS::POISON>(1);
    bane.play(CardId::BANE, true);
    CHECK_EQ(bane.damageTaken(), 7 + 20);

    Fight fumes(MonsterEncounter::TWO_LOUSE);
    fumes.calmPlayer();
    fumes.play(CardId::NOXIOUS_FUMES, true);
    CHECK_EQ(fumes.bc.player.getStatus<PS::NOXIOUS_FUMES>(), 3);
    CHECK(!fumes.monster(0).hasStatus<MS::POISON>());
    fumes.toDraw(CardId::WOUND);
    fumes.endTurn();
    CHECK_EQ(fumes.monster(0).getStatus<MS::POISON>(), 3);  // after the draw, so it hasn't ticked yet
    CHECK_EQ(fumes.monster(1).getStatus<MS::POISON>(), 3);
    CHECK_EQ(fumes.damageTaken(0), 0);

    Fight envenom;
    envenom.play(CardId::ENVENOM);
    envenom.play(CardId::STRIKE_GREEN);
    CHECK_EQ(envenom.monster().getStatus<MS::POISON>(), 1);
    envenom.monster().block = 100;
    envenom.play(CardId::STRIKE_GREEN);
    CHECK_EQ(envenom.monster().getStatus<MS::POISON>(), 1);  // all blocked
    envenom.monster().block = 0;
    envenom.play(CardId::RIDDLE_WITH_HOLES);
    CHECK_EQ(envenom.monster().getStatus<MS::POISON>(), 6);  // every hit
    envenom.bc.player.buff<PS::THOUSAND_CUTS>(1);
    envenom.play(CardId::DEFLECT);
    CHECK_EQ(envenom.monster().getStatus<MS::POISON>(), 6);  // not for THORNS damage
}

void bouncingFlask() {
    Fight f(MonsterEncounter::TWO_LOUSE);
    const auto rolls = f.bc.cardRandomRng.counter;
    f.play(CardId::BOUNCING_FLASK);
    CHECK_EQ(f.bc.cardRandomRng.counter, rolls + 3);  // one when played and one for each bounce after the first
    CHECK_EQ(f.monster(0).getStatus<MS::POISON>() + f.monster(1).getStatus<MS::POISON>(), 9);

    Fight up(MonsterEncounter::TWO_LOUSE);
    const auto rollsUp = up.bc.cardRandomRng.counter;
    up.play(CardId::BOUNCING_FLASK, true);
    CHECK_EQ(up.bc.cardRandomRng.counter, rollsUp + 4);
    CHECK_EQ(up.monster(0).getStatus<MS::POISON>() + up.monster(1).getStatus<MS::POISON>(), 12);
}

void choke() {
    Fight f;
    f.play(CardId::CHOKE);
    CHECK_EQ(f.damageTaken(), 12);
    CHECK_EQ(f.monster().getStatus<MS::CHOKED>(), 3);
    f.monster().block = 100;
    f.play(CardId::DEFLECT);
    CHECK_EQ(f.damageTaken(), 15);  // HP loss for any card, and block doesn't stop it
    f.play(CardId::CHOKE, true);
    CHECK_EQ(f.damageTaken(), 18);  // a Choke played under Choke loses to the old amount only; its power comes after
    CHECK_EQ(f.monster().getStatus<MS::CHOKED>(), 8);
    f.calmPlayer();
    f.toDraw(CardId::WOUND);
    f.endTurn();
    CHECK(!f.monster().hasStatus<MS::CHOKED>());
    CHECK_EQ(f.monster().getStatus<MS::CHOKED>(), 0);
    f.monster().addDebuff<MS::CHOKED>(1);
    CHECK_EQ(f.monster().getStatus<MS::CHOKED>(), 1);  // it starts over
}

void corpseExplosion() {
    Fight f(MonsterEncounter::TWO_LOUSE);
    f.monster(0).maxHp = 20;
    f.monster(0).curHp = 1;
    f.play(CardId::CORPSE_EXPLOSION, false, 0);
    f.play(CardId::STRIKE_GREEN, false, 0);
    CHECK(f.monster(0).isDying());
    CHECK_EQ(f.damageTaken(1), 20);

    Fight last;
    last.monster().curHp = 1;
    last.play(CardId::CORPSE_EXPLOSION);
    last.play(CardId::STRIKE_GREEN);
    CHECK(last.bc.outcome == Outcome::PLAYER_VICTORY);
}

// ******** Discards ********

void discardScreen() {
    Fight f;
    f.toHand(CardId::STRIKE_GREEN);
    f.toHand(CardId::DEFEND_GREEN);
    f.toDraw(CardId::SLICE);
    f.play(CardId::ACROBATICS);  // draws one here and asks for one
    CHECK(f.selecting(CardSelectTask::DISCARD));
    CHECK_EQ(f.bc.cards.cardsInHand, 3);
    f.select(1);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);
    CHECK(f.bc.cards.discardPile[0].getId() == CardId::DEFEND_GREEN);
    CHECK_EQ(f.bc.player.cardsDiscardedThisTurn, 1);
    CHECK(f.bc.inputState == InputState::PLAYER_NORMAL);

    Fight one;  // no choice when the hand has no more cards than it needs
    one.toHand(CardId::STRIKE_GREEN);
    one.play(CardId::SURVIVOR);
    CHECK(one.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK_EQ(one.bc.cards.cardsInHand, 0);

    Fight none;
    none.play(CardId::SURVIVOR);
    CHECK(none.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK_EQ(none.bc.player.cardsDiscardedThisTurn, 0);

    Fight two;
    two.toHand(CardId::STRIKE_GREEN);
    two.toHand(CardId::DEFEND_GREEN);
    two.toHand(CardId::SLICE);
    two.toHand(CardId::BANE);
    two.toDraw(CardId::WOUND);
    two.toDraw(CardId::WOUND);
    two.play(CardId::PREPARED, true);
    CHECK(two.selecting(CardSelectTask::DISCARD));
    CHECK_EQ(two.bc.cardSelectInfo.pickCount, 2);
    CHECK_EQ(two.bc.cards.cardsInHand, 6);
    two.select(2);  // Slice
    CHECK(two.selecting(CardSelectTask::DISCARD));
    CHECK_EQ(two.bc.cardSelectInfo.pickCount, 1);
    two.select(0);  // Strike
    CHECK(two.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK(two.bc.cards.discardPile[0].getId() == CardId::SLICE);  // in the order picked
    CHECK(two.bc.cards.discardPile[1].getId() == CardId::STRIKE_GREEN);
    CHECK_EQ(two.bc.player.cardsDiscardedThisTurn, 2);

    Fight concentrate;
    concentrate.toHand(CardId::STRIKE_GREEN);
    concentrate.toHand(CardId::DEFEND_GREEN);
    concentrate.toHand(CardId::SLICE);
    concentrate.toHand(CardId::BANE);
    const int energy = concentrate.bc.player.energy;
    concentrate.play(CardId::CONCENTRATE);
    CHECK(concentrate.selecting(CardSelectTask::DISCARD));
    CHECK_EQ(concentrate.bc.player.energy, energy);  // comes after the screen
    concentrate.select(0);
    concentrate.select(0);
    concentrate.select(0);
    CHECK_EQ(concentrate.bc.player.energy, energy + 2);
    CHECK_EQ(concentrate.bc.cards.cardsInHand, 1);

    Fight concentrateUp;
    concentrateUp.toHand(CardId::STRIKE_GREEN);
    concentrateUp.toHand(CardId::DEFEND_GREEN);
    concentrateUp.play(CardId::CONCENTRATE, true);
    CHECK(concentrateUp.bc.inputState == InputState::PLAYER_NORMAL);  // exactly enough
    CHECK_EQ(concentrateUp.bc.cards.cardsInHand, 0);
    CHECK(concentrateUp.bc.cards.discardPile[0].getId() == CardId::DEFEND_GREEN);  // from the top of the hand
    CHECK(concentrateUp.bc.cards.discardPile[1].getId() == CardId::STRIKE_GREEN);
}

void discardTriggers() {
    Fight tactician;
    tactician.toHand(CardId::TACTICIAN, true);
    const int energy = tactician.bc.player.energy;
    tactician.play(CardId::SURVIVOR);
    CHECK_EQ(tactician.bc.player.energy, energy - 1 + 2);

    Fight reflex;
    reflex.toHand(CardId::REFLEX);
    reflex.toDraw(CardId::SLICE);
    reflex.toDraw(CardId::BANE);
    reflex.toDraw(CardId::DASH);
    reflex.play(CardId::SURVIVOR);
    CHECK_EQ(reflex.bc.cards.cardsInHand, 2);  // the Reflex went, and two came

    Fight unplayable;
    unplayable.toHand(CardId::REFLEX);
    unplayable.toHand(CardId::TACTICIAN);
    CHECK(!unplayable.bc.cards.hand[0].canUse(unplayable.bc, 0, false));
    CHECK(!unplayable.bc.cards.hand[1].canUse(unplayable.bc, 0, false));

    // End of turn isn't a manual discard.
    Fight endOfTurn;
    endOfTurn.calmPlayer();
    endOfTurn.toHand(CardId::TACTICIAN);
    endOfTurn.toHand(CardId::REFLEX);
    endOfTurn.toDraw(CardId::WOUND);
    endOfTurn.endTurn();
    CHECK_EQ(endOfTurn.bc.player.energy, endOfTurn.bc.player.energyPerTurn);
    CHECK_EQ(endOfTurn.bc.player.cardsDiscardedThisTurn, 0);

    Fight sneaky;
    sneaky.play(CardId::SNEAKY_STRIKE);
    CHECK_EQ(sneaky.bc.player.energy, 8);
    sneaky.toHand(CardId::WOUND);
    sneaky.play(CardId::SURVIVOR);
    sneaky.play(CardId::SNEAKY_STRIKE);
    CHECK_EQ(sneaky.bc.player.energy, 8 - 1 - 2 + 2);
}

void eviscerate() {
    Fight hand;
    hand.toHand(CardId::EVISCERATE);
    hand.toHand(CardId::WOUND);
    hand.toHand(CardId::DAZED);
    CHECK_EQ(static_cast<int>(hand.bc.cards.hand[0].costForTurn), 3);
    hand.play(CardId::CALCULATED_GAMBLE);
    // It's discarded with the rest and lands at its full cost (it's in the discard pile).
    Fight one;
    one.toHand(CardId::EVISCERATE);
    one.toHand(CardId::WOUND);
    one.toHand(CardId::DAZED);
    one.play(CardId::SURVIVOR);
    one.select(1);
    CHECK_EQ(static_cast<int>(one.bc.cards.hand[0].costForTurn), 2);
    CHECK_EQ(static_cast<int>(one.bc.cards.hand[0].cost), 3);

    Fight drawn;
    drawn.toHand(CardId::WOUND);
    drawn.toHand(CardId::DAZED);
    drawn.toDraw(CardId::EVISCERATE);
    drawn.play(CardId::SURVIVOR);
    drawn.select(0);
    drawn.play(CardId::ADRENALINE);  // draws it after one discard this turn
    bool found = false;
    for (int i = 0; i < drawn.bc.cards.cardsInHand; ++i) {
        if (drawn.bc.cards.hand[i].getId() == CardId::EVISCERATE) {
            CHECK_EQ(static_cast<int>(drawn.bc.cards.hand[i].costForTurn), 2);
            found = true;
        }
    }
    CHECK(found);

    // One that leaves with the action lands at its full cost, whatever else goes with it. One already in a pile keeps the discount.
    Fight landing;
    landing.toDiscard(CardId::EVISCERATE);
    landing.toHand(CardId::EVISCERATE);
    landing.toHand(CardId::WOUND);
    landing.play(CardId::CONCENTRATE, true);
    CHECK_EQ(static_cast<int>(landing.bc.cards.discardPile[0].costForTurn), 1);
    CHECK(landing.bc.cards.discardPile[1].getId() == CardId::WOUND);  // from the top of the hand
    CHECK(landing.bc.cards.discardPile[2].getId() == CardId::EVISCERATE);
    CHECK_EQ(static_cast<int>(landing.bc.cards.discardPile[2].costForTurn), 3);

    // Next turn starts the count again.
    Fight nextTurn;
    nextTurn.calmPlayer();
    nextTurn.toHand(CardId::EVISCERATE);
    nextTurn.bc.cards.hand[0].retain = true;
    nextTurn.toHand(CardId::WOUND);
    nextTurn.toHand(CardId::DAZED);
    nextTurn.play(CardId::SURVIVOR);
    nextTurn.select(1);
    CHECK_EQ(static_cast<int>(nextTurn.bc.cards.hand[0].costForTurn), 2);
    nextTurn.endTurn();
    CHECK_EQ(static_cast<int>(nextTurn.bc.cards.hand[0].costForTurn), 3);
    CHECK_EQ(nextTurn.bc.player.cardsDiscardedThisTurn, 0);
}

void endlessAgony() {
    Fight f;
    f.toDraw(CardId::ENDLESS_AGONY, true);
    f.play(CardId::QUICK_SLASH);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);
    CHECK(f.bc.cards.hand[0].getId() == CardId::ENDLESS_AGONY);
    CHECK(f.bc.cards.hand[1].getId() == CardId::ENDLESS_AGONY);
    CHECK(f.bc.cards.hand[1].isUpgraded());
    CHECK(f.bc.cards.hand[0].getUniqueId() != f.bc.cards.hand[1].getUniqueId());

    // The hand fills with the draw, so the copy lands in the discard pile.
    Fight full;
    for (int i = 0; i < 9; ++i) {
        full.toHand(CardId::WOUND);
    }
    full.toDraw(CardId::ENDLESS_AGONY);
    full.play(CardId::QUICK_SLASH);
    CHECK_EQ(full.bc.cards.cardsInHand, 10);
    CHECK(full.bc.cards.discardPile[0].getId() == CardId::ENDLESS_AGONY);  // ahead of Quick Slash's UseCardAction
    CHECK(full.bc.cards.discardPile[1].getId() == CardId::QUICK_SLASH);
}

void calculatedGambleAndExpertise() {
    Fight f;
    f.toHand(CardId::STRIKE_GREEN);
    f.toHand(CardId::DEFEND_GREEN);
    for (int i = 0; i < 4; ++i) {
        f.toDraw(CardId::SLICE, i == 0);
    }
    const auto rolls = f.bc.cardRandomRng.counter;
    f.play(CardId::CALCULATED_GAMBLE, true);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);  // an upgrade doesn't draw one more: the card always passes false to its action
    CHECK_EQ(f.bc.cardRandomRng.counter, rolls);  // discarding the whole hand rolls nothing
    CHECK_EQ(f.bc.player.cardsDiscardedThisTurn, 2);
    CHECK_EQ(static_cast<int>(f.bc.cards.exhaustPile.size()), 0);  // upgraded

    Fight base;
    base.play(CardId::CALCULATED_GAMBLE);
    CHECK_EQ(static_cast<int>(base.bc.cards.exhaustPile.size()), 1);

    Fight expertise;
    for (int i = 0; i < 10; ++i) {
        expertise.toDraw(CardId::SLICE);
    }
    expertise.toHand(CardId::WOUND);
    expertise.toHand(CardId::WOUND);
    expertise.play(CardId::EXPERTISE);
    CHECK_EQ(expertise.bc.cards.cardsInHand, 6);
    Fight expertiseUp;
    for (int i = 0; i < 10; ++i) {
        expertiseUp.toDraw(CardId::SLICE);
    }
    expertiseUp.play(CardId::EXPERTISE, true);
    CHECK_EQ(expertiseUp.bc.cards.cardsInHand, 7);
}

void stormOfSteelAndUnload() {
    Fight f;
    f.toHand(CardId::STRIKE_GREEN);
    f.toHand(CardId::DEFEND_GREEN);
    f.toHand(CardId::WOUND);
    f.play(CardId::STORM_OF_STEEL, true);
    CHECK_EQ(f.bc.cards.cardsInHand, 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(f.bc.cards.hand[i].getId() == CardId::SHIV);
        CHECK(f.bc.cards.hand[i].isUpgraded());
    }
    CHECK(f.bc.cards.discardPile[0].getId() == CardId::WOUND);  // from the top of the hand
    CHECK_EQ(f.bc.player.cardsDiscardedThisTurn, 3);

    Fight unload;
    unload.toHand(CardId::STRIKE_GREEN);
    unload.toHand(CardId::DEFEND_GREEN);
    unload.toHand(CardId::SLICE);
    unload.toHand(CardId::WOUND);
    unload.toHand(CardId::DEFLECT);
    unload.play(CardId::UNLOAD);
    CHECK_EQ(unload.damageTaken(), 14);
    CHECK_EQ(unload.bc.cards.cardsInHand, 2);
    CHECK(unload.bc.cards.hand[0].getId() == CardId::STRIKE_GREEN);
    CHECK(unload.bc.cards.hand[1].getId() == CardId::SLICE);
    CHECK(unload.bc.cards.discardPile[0].getId() == CardId::DEFLECT);  // each new action goes on top, so last first
    CHECK(unload.bc.cards.discardPile[1].getId() == CardId::WOUND);
    CHECK(unload.bc.cards.discardPile[2].getId() == CardId::DEFEND_GREEN);
    CHECK(unload.bc.cards.discardPile[3].getId() == CardId::UNLOAD);
    CHECK_EQ(unload.bc.player.cardsDiscardedThisTurn, 3);
}

void toolsOfTheTrade() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::TOOLS_OF_THE_TRADE);
    for (int i = 0; i < 8; ++i) {
        f.toDraw(CardId::SLICE);
    }
    f.endTurn();
    CHECK(f.selecting(CardSelectTask::DISCARD));
    CHECK_EQ(f.bc.cards.cardsInHand, 6);
    f.select(0);
    CHECK_EQ(f.bc.cards.cardsInHand, 5);
    CHECK(f.bc.inputState == InputState::PLAYER_NORMAL);
}

// ******** Other select screens ********

void setup() {
    Fight one;
    one.toHand(CardId::DASH);
    one.play(CardId::SETUP);
    CHECK_EQ(one.bc.cards.cardsInHand, 0);
    CHECK(one.bc.cards.drawPile.back().getId() == CardId::DASH);
    CHECK(one.bc.cards.drawPile.back().freeToPlayOnce);

    Fight f;
    f.toHand(CardId::DASH);
    f.toHand(CardId::SLICE);
    f.play(CardId::SETUP, true);
    CHECK(f.selecting(CardSelectTask::SETUP));
    f.select(1);
    CHECK(f.bc.cards.drawPile.back().getId() == CardId::SLICE);
    CHECK(!f.bc.cards.drawPile.back().freeToPlayOnce);  // it costs 0 already
    CHECK_EQ(f.bc.player.energy, 10);
}

void nightmare() {
    Fight none;
    none.play(CardId::NIGHTMARE);
    CHECK_EQ(static_cast<int>(none.bc.player.nightmareCount), 0);

    Fight f;
    f.calmPlayer();
    f.toHand(CardId::DASH, true);
    f.toHand(CardId::SLICE);
    f.toHand(CardId::BANE);
    f.play(CardId::NIGHTMARE);
    CHECK(f.selecting(CardSelectTask::NIGHTMARE));
    f.select(0);
    CHECK_EQ(static_cast<int>(f.bc.player.nightmareCount), 1);
    CHECK_EQ(f.bc.cards.cardsInHand, 3);
    CHECK(f.bc.cards.hand[2].getId() == CardId::DASH);  // back on top of the hand
    CHECK(f.bc.cards.hand[0].getId() == CardId::SLICE);
    CHECK_EQ(static_cast<int>(f.bc.cards.exhaustPile.size()), 1);
    for (int i = 0; i < 5; ++i) {
        f.toDraw(CardId::WOUND);
    }
    f.endTurn();
    CHECK_EQ(static_cast<int>(f.bc.player.nightmareCount), 0);
    int dashes = 0;
    for (int i = 0; i < f.bc.cards.cardsInHand; ++i) {
        dashes += f.bc.cards.hand[i].getId() == CardId::DASH && f.bc.cards.hand[i].isUpgraded() ? 1 : 0;
    }
    CHECK_EQ(dashes, 3);

    Fight twoOfThem;  // they don't stack
    twoOfThem.calmPlayer();
    twoOfThem.toHand(CardId::SLICE);
    twoOfThem.play(CardId::NIGHTMARE);
    twoOfThem.toHand(CardId::BANE);
    twoOfThem.bc.player.energy = 10;
    twoOfThem.bc.cards.moveToHand(twoOfThem.card(CardId::NIGHTMARE));
    twoOfThem.playIdx(twoOfThem.bc.cards.cardsInHand - 1);
    CHECK(twoOfThem.selecting(CardSelectTask::NIGHTMARE));
    twoOfThem.select(1);
    CHECK_EQ(static_cast<int>(twoOfThem.bc.player.nightmareCount), 2);
    for (int i = 0; i < 4; ++i) {
        twoOfThem.toDraw(CardId::WOUND);
    }
    twoOfThem.endTurn();
    CHECK_EQ(twoOfThem.bc.cards.cardsInHand, 10);  // three of each, and what fits of the turn's draw
}

void wellLaidPlans() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::WELL_LAID_PLANS, true);
    CHECK_EQ(f.bc.player.getStatus<PS::RETAIN_CARDS>(), 2);
    f.toHand(CardId::STRIKE_GREEN);
    f.toHand(CardId::DEFEND_GREEN);
    f.toHand(CardId::SLICE);
    f.toHand(CardId::BANE);
    for (int i = 0; i < 5; ++i) {
        f.toDraw(CardId::WOUND);
    }
    f.endTurn();
    CHECK(f.selecting(CardSelectTask::RETAIN));
    CHECK_EQ(f.bc.cardSelectInfo.pickCount, 2);
    auto options = SAction::enumerateCardSelectActions(f.bc);
    CHECK_EQ(static_cast<int>(options.size()), 5);  // four cards and closing the screen
    f.select(0);  // Strike
    CHECK(f.selecting(CardSelectTask::RETAIN));
    CHECK_EQ(f.bc.cardSelectInfo.pickCount, 1);
    options = SAction::enumerateCardSelectActions(f.bc);
    CHECK_EQ(static_cast<int>(options.size()), 4);  // three cards and closing the screen
    f.select(1);  // Slice, now at 1
    CHECK(f.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK_EQ(f.bc.cards.cardsInHand, 2 + 5);
    CHECK(f.bc.cards.hand[0].getId() == CardId::STRIKE_GREEN);
    CHECK(f.bc.cards.hand[1].getId() == CardId::SLICE);
    CHECK(!f.bc.cards.hand[0].retain);

    Fight skip;
    skip.calmPlayer();
    skip.play(CardId::WELL_LAID_PLANS);
    skip.toHand(CardId::STRIKE_GREEN);
    for (int i = 0; i < 5; ++i) {
        skip.toDraw(CardId::WOUND);
    }
    skip.endTurn();
    CHECK(skip.selecting(CardSelectTask::RETAIN));  // even with one card
    skip.select(CardSelectInfo::RETAIN_DONE_IDX);
    CHECK_EQ(skip.bc.cards.cardsInHand, 5);
    CHECK(skip.bc.cards.discardPile[0].getId() == CardId::STRIKE_GREEN);

    Fight empty;
    empty.calmPlayer();
    empty.play(CardId::WELL_LAID_PLANS);
    empty.endTurn();
    CHECK(empty.bc.inputState == InputState::PLAYER_NORMAL);

    Fight ethereal;  // it comes back to the hand without being retained
    ethereal.calmPlayer();
    ethereal.play(CardId::WELL_LAID_PLANS);
    ethereal.toHand(CardId::DAZED);
    ethereal.toHand(CardId::STRIKE_GREEN);
    for (int i = 0; i < 5; ++i) {
        ethereal.toDraw(CardId::WOUND);
    }
    ethereal.endTurn();
    ethereal.select(0);
    CHECK_EQ(ethereal.bc.cards.cardsInHand, 5);
    CHECK_EQ(static_cast<int>(ethereal.bc.cards.exhaustPile.size()), 1);

    Fight pyramid;
    pyramid.calmPlayer();
    pyramid.bc.player.setHasRelic<R::RUNIC_PYRAMID>(true);
    pyramid.play(CardId::WELL_LAID_PLANS);
    pyramid.toHand(CardId::STRIKE_GREEN);
    pyramid.endTurn();
    CHECK(pyramid.bc.inputState == InputState::PLAYER_NORMAL);
}

// ******** Shivs ********

void shivs() {
    Fight dance;
    dance.play(CardId::BLADE_DANCE);
    CHECK_EQ(dance.bc.cards.cardsInHand, 3);
    CHECK(dance.bc.cards.hand[0].getId() == CardId::SHIV);
    CHECK(!dance.bc.cards.hand[0].isUpgraded());
    Fight danceUp;
    danceUp.play(CardId::BLADE_DANCE, true);
    CHECK_EQ(danceUp.bc.cards.cardsInHand, 4);

    Fight cloak;
    cloak.play(CardId::CLOAK_AND_DAGGER, true);
    CHECK_EQ(cloak.bc.cards.cardsInHand, 2);

    Fight accuracy;
    accuracy.toHand(CardId::SHIV);
    accuracy.play(CardId::ACCURACY);
    CHECK_EQ(accuracy.bc.player.getStatus<PS::ACCURACY>(), 4);
    accuracy.playIdx(0);
    CHECK_EQ(accuracy.damageTaken(), 8);  // one already in hand gets it too
    CHECK_EQ(static_cast<int>(accuracy.bc.cards.exhaustPile.size()), 1);
    accuracy.play(CardId::ACCURACY, true);
    accuracy.bc.player.buff<PS::STRENGTH>(1);
    accuracy.play(CardId::SHIV, true);
    CHECK_EQ(accuracy.damageTaken(), 8 + 6 + 10 + 1);

    Fight blades;
    blades.calmPlayer();
    blades.play(CardId::INFINITE_BLADES);
    blades.endTurn();
    CHECK_EQ(blades.bc.cards.cardsInHand, 1);
    CHECK(blades.bc.cards.hand[0].getId() == CardId::SHIV);

    // A card made by Dual Wield or Nightmare keeps what Glass Knife has lost.
    Fight knife;
    knife.toHand(CardId::GLASS_KNIFE);
    knife.toHand(CardId::GLASS_KNIFE);
    knife.playIdx(0);
    CHECK_EQ(knife.damageTaken(), 16);
    CHECK_EQ(knife.bc.cards.discardPile.back().specialData, 2);
    CHECK_EQ(knife.bc.cards.hand[0].specialData, 0);  // another card, another uuid
    SAction(ActionType::CARD, 0, 0).execute(knife.bc);
    CHECK_EQ(knife.damageTaken(), 32);
}

void glassKnife() {
    Fight f;
    f.bc.cards.moveToDiscardPile(f.card(CardId::GLASS_KNIFE));
    CardInstance c = f.bc.cards.discardPile[0];
    f.bc.cards.removeFromDiscard(0);
    for (int i = 0, expected = 8; i < 6; ++i, expected = std::max(0, expected - 2)) {
        const int before = f.damageTaken();
        f.bc.cards.moveToHand(c);
        f.playIdx(0);
        CHECK_EQ(f.damageTaken() - before, 2 * expected);
        c = f.bc.cards.discardPile.back();
        f.bc.cards.removeFromDiscard(static_cast<int>(f.bc.cards.discardPile.size()) - 1);
    }
    CHECK_EQ(c.specialData, 8);  // baseDamage stopped at 0
    c.upgrade();
    const int before = f.damageTaken();
    f.bc.cards.moveToHand(c);
    f.playIdx(0);
    CHECK_EQ(f.damageTaken() - before, 8);  // upgradeDamage adds 4 to the 0 it had

    Fight doubled;
    doubled.bc.player.buff<PS::DOUBLE_TAP>(1);
    doubled.play(CardId::GLASS_KNIFE);
    CHECK_EQ(doubled.damageTaken(), 16 + 12);  // the copy in limbo loses it too
    CHECK_EQ(doubled.bc.cards.discardPile.back().specialData, 4);
}

// ******** Counting cards ********

void countingAttacks() {
    Fight finisher;
    finisher.play(CardId::FINISHER);
    CHECK_EQ(finisher.damageTaken(), 0);  // doesn't count itself
    finisher.play(CardId::STRIKE_GREEN);
    finisher.play(CardId::DEFLECT);
    finisher.play(CardId::STRIKE_GREEN, false);
    const int before = finisher.damageTaken();
    finisher.play(CardId::FINISHER, true);
    CHECK_EQ(finisher.damageTaken() - before, 3 * 8);  // the Finisher, two Strikes

    Fight flechettes;
    flechettes.toHand(CardId::DEFLECT);
    flechettes.toHand(CardId::STRIKE_GREEN);
    flechettes.toHand(CardId::SURVIVOR);
    flechettes.toHand(CardId::WOUND);
    flechettes.play(CardId::FLECHETTES);
    CHECK_EQ(flechettes.damageTaken(), 2 * 4);

    Fight heel;
    heel.toDraw(CardId::SLICE);
    heel.play(CardId::HEEL_HOOK);
    CHECK_EQ(heel.damageTaken(), 5);
    CHECK_EQ(heel.bc.player.energy, 9);
    CHECK_EQ(heel.bc.cards.cardsInHand, 0);
    heel.monster().addDebuff<MS::WEAK>(1, false);
    heel.play(CardId::HEEL_HOOK, true);
    CHECK_EQ(heel.damageTaken(), 5 + 8);
    CHECK_EQ(heel.bc.player.energy, 9);  // one spent, one back
    CHECK_EQ(heel.bc.cards.cardsInHand, 1);
}

// ******** X costs ********

void xCosts() {
    Fight skewer;
    skewer.bc.player.energy = 3;
    skewer.play(CardId::SKEWER);
    CHECK_EQ(skewer.damageTaken(), 21);
    CHECK_EQ(skewer.bc.player.energy, 0);

    Fight skewerX;
    skewerX.bc.player.setHasRelic<R::CHEMICAL_X>(true);
    skewerX.bc.player.energy = 1;
    skewerX.play(CardId::SKEWER, true);
    CHECK_EQ(skewerX.damageTaken(), 30);
    CHECK_EQ(skewerX.bc.player.energy, 0);

    Fight zero;
    zero.bc.player.energy = 0;
    zero.play(CardId::SKEWER);
    CHECK_EQ(zero.damageTaken(), 0);

    Fight malaise;
    malaise.bc.player.energy = 2;
    malaise.play(CardId::MALAISE);
    CHECK_EQ(malaise.monster().strength, -2);
    CHECK_EQ(malaise.monster().getStatus<MS::WEAK>(), 2);
    CHECK_EQ(malaise.bc.player.energy, 0);

    Fight malaiseUp;  // an upgrade works with no energy at all
    malaiseUp.bc.player.energy = 0;
    malaiseUp.play(CardId::MALAISE, true);
    CHECK_EQ(malaiseUp.monster().strength, -1);

    Fight doppel;
    doppel.calmPlayer();
    doppel.bc.player.energy = 2;
    doppel.play(CardId::DOPPELGANGER, true);
    CHECK_EQ(doppel.bc.player.getStatus<PS::ENERGIZED>(), 3);
    CHECK_EQ(doppel.bc.player.getStatus<PS::DRAW_CARD_NEXT_TURN>(), 3);
    CHECK_EQ(doppel.bc.player.energy, 0);
    for (int i = 0; i < 10; ++i) {
        doppel.toDraw(CardId::SLICE);
    }
    doppel.endTurn();
    CHECK_EQ(doppel.bc.player.energy, doppel.bc.player.energyPerTurn + 3);
    CHECK_EQ(doppel.bc.cards.cardsInHand, 8);

    // A Burst's copy of an X card is free and repeats the same amount.
    Fight burst;
    burst.bc.player.buff<PS::BURST>(1);
    burst.bc.player.energy = 2;
    burst.play(CardId::MALAISE);
    CHECK_EQ(burst.monster().strength, -4);
    CHECK_EQ(burst.bc.player.energy, 0);
}

void burst() {
    Fight f;
    f.play(CardId::BURST, true);
    CHECK_EQ(f.bc.player.getStatus<PS::BURST>(), 2);
    f.play(CardId::DEFLECT);
    CHECK_EQ(f.bc.player.block, 8);
    f.play(CardId::STRIKE_GREEN);
    CHECK_EQ(f.damageTaken(), 6);  // attacks don't use it
    CHECK_EQ(f.bc.player.getStatus<PS::BURST>(), 1);
    f.calmPlayer();
    f.bc.player.block = 0;
    f.endTurn();
    CHECK(!f.bc.player.hasStatus<PS::BURST>());  // gone at the end of the turn
}

// ******** Powers with timing ********

void phantasmalKiller() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::PHANTASMAL_KILLER);
    CHECK_EQ(f.bc.player.getStatus<PS::PHANTASMAL>(), 1);
    f.play(CardId::STRIKE_GREEN);
    CHECK_EQ(f.damageTaken(), 6);  // not this turn
    f.endTurn();
    CHECK(!f.bc.player.hasStatus<PS::PHANTASMAL>());
    CHECK_EQ(f.bc.player.getStatus<PS::DOUBLE_DAMAGE>(), 1);
    f.bc.player.energy = 3;
    f.play(CardId::STRIKE_GREEN);
    CHECK_EQ(f.damageTaken(), 6 + 12);
    f.endTurn();
    CHECK(!f.bc.player.hasStatus<PS::DOUBLE_DAMAGE>());  // one turn: DoubleDamagePower here starts with justApplied false
    f.bc.player.energy = 3;
    const int before = f.damageTaken();
    f.play(CardId::STRIKE_GREEN);
    CHECK_EQ(f.damageTaken() - before, 6);
}

void blur() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::BLUR);
    CHECK_EQ(f.bc.player.block, 5);
    CHECK_EQ(f.bc.player.getStatus<PS::BLUR>(), 1);
    f.monster().setMove(MMID::CULTIST_INCANTATION);
    f.endTurn();
    CHECK_EQ(f.bc.player.block, 5);  // kept
    CHECK(!f.bc.player.hasStatus<PS::BLUR>());

    Fight barricade;  // the round still takes Blur away
    barricade.calmPlayer();
    barricade.bc.player.setHasStatus<PS::BARRICADE>(true);
    barricade.play(CardId::BLUR);
    barricade.endTurn();
    CHECK(!barricade.bc.player.hasStatus<PS::BLUR>());
}

void wraithForm() {
    Fight f;
    f.calmPlayer();
    f.play(CardId::WRAITH_FORM);
    CHECK_EQ(f.bc.player.getStatus<PS::INTANGIBLE>(), 2);
    CHECK_EQ(f.bc.player.getStatus<PS::WRAITH_FORM>(), -1);
    f.endTurn();
    CHECK_EQ(f.bc.player.dexterity, -1);
    CHECK_EQ(f.bc.player.getStatus<PS::INTANGIBLE>(), 1);
    f.endTurn();
    CHECK_EQ(f.bc.player.dexterity, -2);
    CHECK(!f.bc.player.hasStatus<PS::INTANGIBLE>());

    Fight up;
    up.play(CardId::WRAITH_FORM, true);
    CHECK_EQ(up.bc.player.getStatus<PS::INTANGIBLE>(), 3);

    Fight artifact;  // Artifact stops the power and then the Dexterity loss
    artifact.calmPlayer();
    artifact.bc.player.buff<PS::ARTIFACT>(2);
    artifact.bc.player.debuff<PS::WRAITH_FORM>(-1, false);
    CHECK_EQ(artifact.bc.player.artifact, 1);
    artifact.bc.player.artifact = 0;
    artifact.bc.player.debuff<PS::WRAITH_FORM>(-1, false);
    artifact.bc.player.artifact = 1;
    artifact.endTurn();
    CHECK_EQ(artifact.bc.player.dexterity, 0);
    CHECK_EQ(artifact.bc.player.artifact, 0);
}

void bulletTime() {
    Fight f;
    f.toHand(CardId::DASH);
    f.toHand(CardId::SKEWER);
    f.toDraw(CardId::SLICE);
    f.play(CardId::BULLET_TIME);
    CHECK(f.bc.player.hasStatus<PS::NO_DRAW>());
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[0].costForTurn), 0);
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[0].cost), 2);
    CHECK_EQ(static_cast<int>(f.bc.cards.hand[1].costForTurn), -1);  // setCostForTurn leaves an X cost alone
    f.play(CardId::QUICK_SLASH);
    CHECK_EQ(f.bc.cards.cardsInHand, 2);

    // A second No Draw stops before Artifact sees it.
    Fight twice;
    twice.bc.player.buff<PS::ARTIFACT>(1);
    twice.bc.player.setHasStatus<PS::NO_DRAW>(true);
    twice.bc.player.setStatusValueNoChecks<PS::NO_DRAW>(1);
    twice.play(CardId::BULLET_TIME);
    CHECK_EQ(twice.bc.player.artifact, 1);
}

void energyCards() {
    Fight knee;
    knee.calmPlayer();
    knee.play(CardId::FLYING_KNEE);
    CHECK_EQ(knee.bc.player.getStatus<PS::ENERGIZED>(), 1);
    knee.toDraw(CardId::WOUND);
    knee.endTurn();
    CHECK_EQ(knee.bc.player.energy, knee.bc.player.energyPerTurn + 1);
    CHECK(!knee.bc.player.hasStatus<PS::ENERGIZED>());

    Fight outmaneuver;
    outmaneuver.play(CardId::OUTMANEUVER, true);
    CHECK_EQ(outmaneuver.bc.player.getStatus<PS::ENERGIZED>(), 3);
    Fight outmaneuverBase;
    outmaneuverBase.play(CardId::OUTMANEUVER);
    CHECK_EQ(outmaneuverBase.bc.player.getStatus<PS::ENERGIZED>(), 2);

    Fight adrenaline;
    adrenaline.toDraw(CardId::SLICE);
    adrenaline.toDraw(CardId::SLICE);
    adrenaline.play(CardId::ADRENALINE, true);
    CHECK_EQ(adrenaline.bc.player.energy, 12);
    CHECK_EQ(adrenaline.bc.cards.cardsInHand, 2);
    CHECK_EQ(static_cast<int>(adrenaline.bc.cards.exhaustPile.size()), 1);

    Fight predator;
    predator.calmPlayer();
    predator.play(CardId::PREDATOR);
    CHECK_EQ(predator.bc.player.getStatus<PS::DRAW_CARD_NEXT_TURN>(), 2);
    for (int i = 0; i < 10; ++i) {
        predator.toDraw(CardId::SLICE);
    }
    predator.endTurn();
    CHECK_EQ(predator.bc.cards.cardsInHand, 7);
    CHECK(!predator.bc.player.hasStatus<PS::DRAW_CARD_NEXT_TURN>());
}

void debuffCards() {
    Fight terror;
    terror.play(CardId::TERROR);
    CHECK_EQ(terror.monster().getStatus<MS::VULNERABLE>(), 99);
    CHECK_EQ(static_cast<int>(terror.bc.cards.exhaustPile.size()), 1);
    terror.play(CardId::STRIKE_GREEN);
    CHECK_EQ(terror.damageTaken(), 9);

    Fight wail(MonsterEncounter::TWO_LOUSE);
    wail.monster(1).buff<MS::ARTIFACT>(1);
    wail.play(CardId::PIERCING_WAIL);
    CHECK_EQ(wail.monster(0).strength, -6);
    CHECK_EQ(wail.monster(0).getStatus<MS::SHACKLED>(), 6);
    CHECK_EQ(wail.monster(1).strength, 0);
    CHECK(!wail.monster(1).hasStatus<MS::SHACKLED>());
    CHECK(!wail.monster(1).hasStatus<MS::ARTIFACT>());

    Fight neutralize;
    neutralize.play(CardId::NEUTRALIZE, true);
    CHECK_EQ(neutralize.monster().getStatus<MS::WEAK>(), 2);

    Fight sweep;
    sweep.play(CardId::LEG_SWEEP, true);
    CHECK_EQ(sweep.monster().getStatus<MS::WEAK>(), 3);

    Fight punch;
    punch.play(CardId::SUCKER_PUNCH, true);
    CHECK_EQ(punch.monster().getStatus<MS::WEAK>(), 2);
}

void escapePlan() {
    Fight skill;
    skill.toDraw(CardId::DEFLECT);
    skill.play(CardId::ESCAPE_PLAN);
    CHECK_EQ(skill.bc.player.block, 3);

    Fight attack;
    attack.toDraw(CardId::SLICE);
    attack.play(CardId::ESCAPE_PLAN, true);
    CHECK_EQ(attack.bc.player.block, 0);
    CHECK_EQ(attack.bc.cards.cardsInHand, 1);

    Fight shuffle;  // an empty pile shuffles, and the card it then draws counts
    shuffle.toDiscard(CardId::DEFLECT);
    shuffle.play(CardId::ESCAPE_PLAN, true);
    CHECK_EQ(shuffle.bc.player.block, 5);

    Fight noDraw;
    noDraw.bc.player.setHasStatus<PS::NO_DRAW>(true);
    noDraw.bc.player.setStatusValueNoChecks<PS::NO_DRAW>(1);
    noDraw.toDraw(CardId::DEFLECT);
    noDraw.play(CardId::ESCAPE_PLAN);
    CHECK_EQ(noDraw.bc.player.block, 0);
}

void distractionAndAlchemize() {
    Fight distraction;
    distraction.play(CardId::DISTRACTION);
    CHECK_EQ(distraction.bc.cards.cardsInHand, 1);
    CHECK(distraction.bc.cards.hand[0].getType() == CardType::SKILL);
    CHECK_EQ(static_cast<int>(distraction.bc.cards.hand[0].costForTurn), 0);
    CHECK(getCardColor(distraction.bc.cards.hand[0].getId()) == CardColor::RED);  // the class's own pool
    CHECK_EQ(static_cast<int>(distraction.bc.cards.exhaustPile.size()), 1);

    Fight potion;
    const int before = potion.bc.potionCount;
    const auto rolls = potion.bc.potionRng.counter;
    potion.play(CardId::ALCHEMIZE);
    CHECK_EQ(potion.bc.potionCount, before + 1);
    CHECK(potion.bc.potionRng.counter > rolls);

    Fight sozu;
    sozu.bc.player.setHasRelic<R::SOZU>(true);
    sozu.play(CardId::ALCHEMIZE);
    CHECK_EQ(sozu.bc.potionCount, before);
}

void powers() {
    Fight cuts(MonsterEncounter::TWO_LOUSE);
    cuts.play(CardId::A_THOUSAND_CUTS, true);
    CHECK_EQ(cuts.bc.player.getStatus<PS::THOUSAND_CUTS>(), 2);
    const int beforeCuts = cuts.damageTaken(1);
    cuts.play(CardId::DEFLECT);
    CHECK_EQ(cuts.damageTaken(1) - beforeCuts, 2);

    Fight image;
    image.play(CardId::AFTER_IMAGE);
    image.play(CardId::STRIKE_GREEN);
    CHECK_EQ(image.bc.player.block, 1);
    image.play(CardId::DEFLECT);
    CHECK_EQ(image.bc.player.block, 1 + 4 + 1);

    Fight caltrops;
    caltrops.play(CardId::CALTROPS, true);
    CHECK_EQ(caltrops.bc.player.getStatus<PS::THORNS>(), 5);

    Fight footwork;
    footwork.play(CardId::FOOTWORK, true);
    CHECK_EQ(footwork.bc.player.dexterity, 3);
    footwork.play(CardId::DEFEND_GREEN);
    CHECK_EQ(footwork.bc.player.block, 8);

    CHECK(isCardInnate(CardId::AFTER_IMAGE, true) && !isCardInnate(CardId::AFTER_IMAGE, false));
    CHECK(isCardInnate(CardId::INFINITE_BLADES, true) && !isCardInnate(CardId::INFINITE_BLADES, false));
    CHECK(isCardInnate(CardId::BACKSTAB, false));

    Fight stab;  // HP lost so far counts when a card is made in combat
    stab.toHand(CardId::MASTERFUL_STAB);
    stab.calmPlayer();
    stab.bc.player.damage(stab.bc, 3);
    CHECK_EQ(static_cast<int>(stab.bc.cards.hand[0].costForTurn), 1);
    stab.bc.player.damage(stab.bc, 3);
    CHECK_EQ(static_cast<int>(stab.bc.cards.hand[0].costForTurn), 2);
}

// ******** Every green card, played at random ********

const std::vector<CardId> &greenCards() {
    static const std::vector<CardId> cards = [] {
        std::vector<CardId> out;
        const int count = static_cast<int>(sizeof(cardStringIds) / sizeof(cardStringIds[0]));
        for (int i = 1; i < count; ++i) {
            const auto id = static_cast<CardId>(i);
            if (getCardColor(id) == CardColor::GREEN) {
                out.push_back(id);
            }
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

// Random fights with green-heavy decks. Each card comes up in the deck in turn, so none is left out.
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

    const auto &green = greenCards();
    for (int fight = 0; fight < fights; ++fight) {
        std::mt19937 rng(static_cast<unsigned>(fight) * 2654435761u + 7);
        GameContext gc(CharacterClass::IRONCLAD, static_cast<std::uint64_t>(fight) + 1000, fight % 2 == 0 ? 0 : 20);
        gc.curRoom = Room::MONSTER;
        gc.curHp = gc.maxHp = 300;
        gc.deck = Deck{};
        // A few runs get the relics these cards interact with.
        if (fight % 7 == 0) gc.relics.add(RelicInstance{RelicId::SNECKO_SKULL, 0});
        if (fight % 11 == 0) gc.relics.add(RelicInstance{RelicId::CHEMICAL_X, 0});
        if (fight % 13 == 0) gc.relics.add(RelicInstance{RelicId::RUNIC_PYRAMID, 0});
        if (fight % 17 == 0) gc.relics.add(RelicInstance{RelicId::SNECKO_EYE, 0});
        if (fight % 19 == 0) gc.relics.add(RelicInstance{RelicId::UNCEASING_TOP, 0});
        if (fight % 23 == 0) gc.relics.add(RelicInstance{RelicId::GAMBLING_CHIP, 0});

        for (int i = 0; i < 18; ++i) {
            const CardId id = i == 0 ? green[fight % green.size()] : green[rng() % green.size()];
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
            // End the turn less often than a card would be picked at random, so the hands get played out.
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
    const auto &green = greenCards();
    for (int fight = 0; fight < fights; ++fight) {
        GameContext gc(CharacterClass::IRONCLAD, static_cast<std::uint64_t>(fight) + 5000, 0);
        gc.curRoom = Room::MONSTER;
        gc.deck = Deck{};
        for (int i = 0; i < 12; ++i) {
            gc.deck.obtainRaw(Card(green[(fight * 12 + i) % green.size()], i % 2));
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

    simpleAttacks();
    aoeAttacks();
    simpleBlock();
    poisonTicks();
    catalyst();
    poisonCards();
    bouncingFlask();
    choke();
    corpseExplosion();
    discardScreen();
    discardTriggers();
    eviscerate();
    endlessAgony();
    calculatedGambleAndExpertise();
    stormOfSteelAndUnload();
    toolsOfTheTrade();
    setup();
    nightmare();
    wellLaidPlans();
    shivs();
    glassKnife();
    countingAttacks();
    xCosts();
    burst();
    phantasmalKiller();
    blur();
    wraithForm();
    bulletTime();
    energyCards();
    debuffCards();
    escapePlan();
    distractionAndAlchemize();
    powers();

    std::map<CardId, int> played;
    randomFights(randomFightCount, played);
    int missing = 0;
    for (const CardId id : greenCards()) {
        if (played[id] == 0 && id != CardId::REFLEX && id != CardId::TACTICIAN) {
            std::cerr << "never played at random: " << cardEnumStrings[static_cast<int>(id)] << "\n";
            ++missing;
        }
    }
    CHECK_EQ(missing, 0);
    searchRuns(25);

    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
