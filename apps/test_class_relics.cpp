// Checks each class's relics and potions against what the game's bytecode does, then plays random fights as each class with
// its relics. The checks encode a reading of the bytecode, so this catches regressions and crashes and doesn't show the
// simulator matches the game. Mirror mode does that.
// Build target: test_class_relics.

#include "combat/BattleContext.h"
#include "constants/RelicPools.h"
#include "game/GameContext.h"
#include "game/Game.h"
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <random>
#include <vector>

using namespace sts;
using SAction = search::Action;
using search::ActionType;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::cerr << __func__ << ":" << __LINE__ << " failed: " #cond "\n"; } } while (0)
#define CHECK_EQ(a, b) do { ++checks; const auto va = (a); const auto vb = (b); if (!(va == vb)) { ++failures; std::cerr << __func__ << ":" << __LINE__ << " " #a " is " << va << ", expected " << vb << "\n"; } } while (0)

constexpr int HIGH_HP = 500;

// A real start of combat, with the relics in the order given and a deck of plain cards that nothing here looks at.
struct Battle {
    GameContext gc;
    BattleContext bc;

    Battle(CharacterClass cc, std::initializer_list<RelicId> extra, MonsterEncounter encounter = MonsterEncounter::CULTIST)
            : gc(cc, 77, 0) {
        gc.curRoom = Room::MONSTER;
        gc.curHp = gc.maxHp = HIGH_HP;
        gc.deck = Deck{};
        for (int i = 0; i < 20; ++i) {
            gc.deck.obtainRaw(Card(CardId::STRIKE_RED));
        }
        for (const RelicId r : extra) {
            gc.relics.add(RelicInstance{r, 0});
        }
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, encounter);
    }

    void endTurn() { SAction(ActionType::END_TURN).execute(bc); }
    void hardenMonsters() {
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            bc.monsters.arr[i].curHp = bc.monsters.arr[i].maxHp = HIGH_HP;
        }
    }
};

// The same, with nothing left in the card piles and plenty of energy, for the relics that trigger during a turn.
struct Fight : Battle {
    Fight(CharacterClass cc, std::initializer_list<RelicId> extra, MonsterEncounter encounter = MonsterEncounter::CULTIST)
            : Battle(cc, extra, encounter) {
        while (bc.cards.cardsInHand > 0) {
            bc.cards.removeFromHandAtIdx(0);
        }
        bc.cards.drawPile.clear();
        bc.cards.discardPile.clear();
        bc.cards.exhaustPile.clear();
        bc.player.energy = 10;
        // With no card left anywhere a fight ends at once, unless something can still do damage. Thorns of 0 does nothing else.
        bc.player.setHasStatus<PS::THORNS>(true);
        bc.player.setStatusValueNoChecks<PS::THORNS>(0);
        hardenMonsters();
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
    void drink(Potion p, int target = 0) {
        bc.potions[0] = p;
        bc.potionCount = 1;
        SAction(ActionType::POTION, 0, target).execute(bc);
    }
    void run(const Action &a) {
        bc.addToBot(a);
        bc.inputState = InputState::EXECUTING_ACTIONS;
        bc.executeActions();
    }
    void discardOne() { run( Actions::DiscardAction(1, true) ); }
    void setOrbs(std::initializer_list<Orb> orbs) {
        int i = 0;
        for (const Orb o : orbs) {
            bc.player.orbs[i] = o;
            bc.player.darkOrbEvoke[i] = 6;
            ++i;
        }
    }
    int damageTaken(int i = 0) { return monster(i).maxHp - monster(i).curHp; }
    bool selecting(CardSelectTask task) const { return bc.inputState == InputState::CARD_SELECT && bc.cardSelectInfo.cardSelectTask == task; }
};

// ******** Silent ********

void hoveringKite() {
    Fight f(CharacterClass::SILENT, {RelicId::HOVERING_KITE});
    for (int i = 0; i < 6; ++i) {
        f.toHand(CardId::WOUND);
    }
    f.discardOne();
    CHECK_EQ(f.bc.player.energy, 11);  // the first discard of the turn
    f.discardOne();
    CHECK_EQ(f.bc.player.energy, 11);  // and only the first

    // Discarding at the end of the turn isn't a manual discard, and the new turn resets it.
    f.endTurn();
    CHECK_EQ(f.bc.player.energy, f.bc.player.energyPerTurn);
    CHECK(!f.bc.player.kiteTriggeredThisTurn);
    f.toHand(CardId::WOUND);
    f.toHand(CardId::WOUND);
    f.discardOne();
    CHECK_EQ(f.bc.player.energy, f.bc.player.energyPerTurn + 1);

    // A chosen discard counts, and so does a card's cost.
    Fight chosen(CharacterClass::SILENT, {RelicId::HOVERING_KITE});
    chosen.toHand(CardId::WOUND);
    chosen.toHand(CardId::WOUND);
    chosen.play(CardId::SURVIVOR);
    CHECK(chosen.selecting(CardSelectTask::DISCARD));
    chosen.select(0);
    CHECK_EQ(chosen.bc.player.energy, 10);  // Survivor cost 1

    // Without the relic there's nothing.
    Fight none(CharacterClass::SILENT, {});
    none.toHand(CardId::WOUND);
    none.toHand(CardId::WOUND);
    none.discardOne();
    CHECK_EQ(none.bc.player.energy, 10);
}

void tingsha() {
    Fight f(CharacterClass::SILENT, {RelicId::TINGSHA});
    for (int i = 0; i < 6; ++i) {
        f.toHand(CardId::WOUND);
    }
    f.discardOne();
    CHECK_EQ(f.damageTaken(), 3);
    f.discardOne();
    CHECK_EQ(f.damageTaken(), 6);

    // Thorns damage: no Strength or Vulnerable, and block stops it.
    f.bc.player.buff<PS::STRENGTH>(5);
    f.monster().addDebuff<MS::VULNERABLE>(3, false);
    f.discardOne();
    CHECK_EQ(f.damageTaken(), 9);
    f.monster().block = 2;
    f.discardOne();
    CHECK_EQ(f.damageTaken(), 10);
    CHECK_EQ(f.monster().block, 0);

    // The target rolls cardRandomRng when the action runs, once per card, and only the living are rolled for.
    Fight many(CharacterClass::SILENT, {RelicId::TINGSHA}, MonsterEncounter::TWO_LOUSE);
    for (int i = 0; i < 6; ++i) {
        many.toHand(CardId::WOUND);
    }
    const int rollsBefore = many.bc.cardRandomRng.counter;
    many.discardOne();  // the discard's own roll, and the damage's
    CHECK_EQ(many.bc.cardRandomRng.counter, rollsBefore + 2);
    CHECK_EQ(many.damageTaken(0) + many.damageTaken(1), 3);

    // Not at the end of the turn.
    Fight end(CharacterClass::SILENT, {RelicId::TINGSHA});
    end.toHand(CardId::WOUND);
    end.endTurn();
    CHECK_EQ(end.damageTaken(), 0);
}

void toughBandages() {
    Fight f(CharacterClass::SILENT, {RelicId::TOUGH_BANDAGES});
    for (int i = 0; i < 6; ++i) {
        f.toHand(CardId::WOUND);
    }
    f.discardOne();
    CHECK_EQ(f.bc.player.block, 3);

    // A flat 3: GainBlockAction doesn't go through Dexterity or Frail.
    f.bc.player.buff<PS::DEXTERITY>(5);
    f.bc.player.debuff<PS::FRAIL>(2, false);
    f.discardOne();
    CHECK_EQ(f.bc.player.block, 6);

    // Three discarded at once are three triggers. Calculated Gamble's way of doing it counts too.
    Fight gamble(CharacterClass::SILENT, {RelicId::TOUGH_BANDAGES});
    gamble.toHand(CardId::WOUND);
    gamble.toHand(CardId::WOUND);
    gamble.toHand(CardId::WOUND);
    gamble.toDraw(CardId::WOUND);
    gamble.toDraw(CardId::WOUND);
    gamble.toDraw(CardId::WOUND);
    gamble.play(CardId::CALCULATED_GAMBLE);
    CHECK_EQ(gamble.bc.player.block, 9);

    Fight end(CharacterClass::SILENT, {RelicId::TOUGH_BANDAGES});
    end.toHand(CardId::WOUND);
    end.bc.player.buff<PS::BARRICADE>();
    end.endTurn();
    CHECK_EQ(end.bc.player.block, 0);
}

void discardTriggersEverywhere() {
    // All three trigger from each way a card can be discarded by a card or a potion.
    Fight unload(CharacterClass::SILENT, {RelicId::TOUGH_BANDAGES});
    unload.toHand(CardId::WOUND);
    unload.toHand(CardId::DEFEND_GREEN);
    unload.play(CardId::UNLOAD);
    CHECK_EQ(unload.bc.player.block, 6);

    Fight storm(CharacterClass::SILENT, {RelicId::TINGSHA});
    storm.toHand(CardId::WOUND);
    storm.toHand(CardId::WOUND);
    storm.play(CardId::STORM_OF_STEEL);
    CHECK_EQ(storm.damageTaken(), 6);

    // Gambler's Brew is GamblingChipAction: it's a screen, and each card picked is a discard.
    Fight brew(CharacterClass::SILENT, {RelicId::TOUGH_BANDAGES});
    brew.toHand(CardId::WOUND);
    brew.toHand(CardId::WOUND);
    brew.toHand(CardId::WOUND);
    brew.toDraw(CardId::WOUND);
    brew.toDraw(CardId::WOUND);
    brew.drink(Potion::GAMBLERS_BREW);
    CHECK(brew.selecting(CardSelectTask::GAMBLE));
    brew.bc.chooseGambleCards({0, 1});
    brew.bc.inputState = InputState::EXECUTING_ACTIONS;
    brew.bc.executeActions();
    CHECK_EQ(brew.bc.player.block, 6);
    CHECK_EQ(brew.bc.cards.cardsInHand, 3);
}

void theSpecimen() {
    // The poison moves to a random living monster, and it isn't Snake Skull's business until the new power is applied.
    Fight f(CharacterClass::SILENT, {RelicId::THE_SPECIMEN}, MonsterEncounter::TWO_LOUSE);
    f.monster(0).curHp = 2;
    f.monster(0).block = 0;
    f.monster(0).addDebuff<MS::POISON>(9, false);
    const int rollsBefore = f.bc.cardRandomRng.counter;
    f.run( Actions::DamageEnemy(0, 5) );
    CHECK(f.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK(f.monster(0).isDeadOrEscaped());
    CHECK_EQ(f.monster(1).getStatus<MS::POISON>(), 9);
    CHECK_EQ(f.bc.cardRandomRng.counter, rollsBefore + 1);

    Fight skull(CharacterClass::SILENT, {RelicId::THE_SPECIMEN, RelicId::SNECKO_SKULL}, MonsterEncounter::TWO_LOUSE);
    skull.monster(0).curHp = 2;
    skull.monster(0).addDebuff<MS::POISON>(9, false);
    skull.monster(1).addDebuff<MS::POISON>(1, false);
    skull.run( Actions::DamageEnemy(0, 5) );
    CHECK_EQ(skull.monster(1).getStatus<MS::POISON>(), 11);

    // No poison, no roll.
    Fight clean(CharacterClass::SILENT, {RelicId::THE_SPECIMEN}, MonsterEncounter::TWO_LOUSE);
    clean.monster(0).curHp = 2;
    const int cleanRolls = clean.bc.cardRandomRng.counter;
    clean.run( Actions::DamageEnemy(0, 5) );
    CHECK(clean.bc.inputState == InputState::PLAYER_NORMAL);
    CHECK(!clean.monster(1).hasStatus<MS::POISON>());
    CHECK_EQ(clean.bc.cardRandomRng.counter, cleanRolls);

    // The last monster has nobody to pass it to.
    Fight last(CharacterClass::SILENT, {RelicId::THE_SPECIMEN});
    last.monster().curHp = 2;
    last.monster().addDebuff<MS::POISON>(9, false);
    last.run( Actions::DamageEnemy(0, 5) );
    CHECK(last.bc.outcome == Outcome::PLAYER_VICTORY);

    // Poison killing its own monster passes on what's left after the tick.
    Fight tick(CharacterClass::SILENT, {RelicId::THE_SPECIMEN}, MonsterEncounter::TWO_LOUSE);
    tick.monster(0).curHp = 1;
    tick.monster(0).addDebuff<MS::POISON>(4, false);
    tick.endTurn();
    CHECK(tick.monster(0).isDeadOrEscaped());
    CHECK(tick.bc.outcome == Outcome::UNDECIDED);
}

void ringsAndScroll() {
    Battle plain(CharacterClass::SILENT, {});
    CHECK_EQ(plain.bc.cards.cardsInHand, 7);  // Ring of the Snake
    CHECK_EQ(static_cast<int>(plain.bc.player.cardDrawPerTurn), 5);

    Battle scroll(CharacterClass::SILENT, {RelicId::NINJA_SCROLL});
    CHECK_EQ(scroll.bc.cards.cardsInHand, 10);  // 3 Shivs ahead of the draw, so the Wounds fill up the rest
    int shivs = 0;
    for (int i = 0; i < scroll.bc.cards.cardsInHand; ++i) {
        shivs += scroll.bc.cards.hand[i].getId() == CardId::SHIV ? 1 : 0;
    }
    CHECK_EQ(shivs, 3);
    CHECK_EQ(scroll.bc.cards.hand[0].getId(), CardId::SHIV);

    // Paper Krane and Weak: 0.6 for a monster's damage, the player's own Weak is unchanged.
    Fight weak(CharacterClass::SILENT, {});
    weak.monster().addDebuff<MS::WEAK>(2, false);
    CHECK_EQ(weak.monster().calculateDamageToPlayer(weak.bc, 10), 7);
    Fight krane(CharacterClass::SILENT, {RelicId::PAPER_KRANE});
    krane.monster().addDebuff<MS::WEAK>(2, false);
    CHECK_EQ(krane.monster().calculateDamageToPlayer(krane.bc, 10), 6);
    krane.bc.player.debuff<PS::WEAK>(2, false);
    krane.play(CardId::STRIKE_GREEN);
    CHECK_EQ(krane.damageTaken(), 4);
}

void twistedFunnelAndWristBlade() {
    Battle funnel(CharacterClass::SILENT, {RelicId::TWISTED_FUNNEL}, MonsterEncounter::TWO_LOUSE);
    CHECK_EQ(funnel.bc.monsters.arr[0].getStatus<MS::POISON>(), 4);
    CHECK_EQ(funnel.bc.monsters.arr[1].getStatus<MS::POISON>(), 4);

    Battle skull(CharacterClass::SILENT, {RelicId::TWISTED_FUNNEL, RelicId::SNECKO_SKULL}, MonsterEncounter::TWO_LOUSE);
    CHECK_EQ(skull.bc.monsters.arr[0].getStatus<MS::POISON>(), 5);

    // Wrist Blade: a cost of 0 for the turn, or free to play once and not an X cost.
    Fight blade(CharacterClass::SILENT, {RelicId::WRIST_BLADE});
    blade.play(CardId::SHIV);
    CHECK_EQ(blade.damageTaken(), 8);
    blade.play(CardId::STRIKE_GREEN);
    CHECK_EQ(blade.damageTaken(), 14);

    Fight zeroed(CharacterClass::SILENT, {RelicId::WRIST_BLADE});
    int idx = zeroed.toHand(CardId::STRIKE_GREEN);
    zeroed.bc.cards.hand[idx].setCostForTurn(0);
    zeroed.playIdx(idx);
    CHECK_EQ(zeroed.damageTaken(), 10);

    Fight free(CharacterClass::SILENT, {RelicId::WRIST_BLADE});
    idx = free.toHand(CardId::STRIKE_GREEN);
    free.bc.cards.hand[idx].freeToPlayOnce = true;
    free.playIdx(idx);
    CHECK_EQ(free.damageTaken(), 10);

    Fight x(CharacterClass::SILENT, {RelicId::WRIST_BLADE});
    x.bc.player.energy = 1;
    idx = x.toHand(CardId::SKEWER);
    x.bc.cards.hand[idx].freeToPlayOnce = true;
    CHECK_EQ(x.bc.calculateCardDamage(x.bc.cards.hand[idx], 0, 7), 7);
}

void starterSwaps() {
    {
        GameContext gc(CharacterClass::SILENT, 5, 0);
        gc.relics.add(RelicInstance{RelicId::VAJRA, 0});
        gc.obtainRelic(RelicId::RING_OF_THE_SERPENT);
        CHECK(!gc.relics.has(RelicId::RING_OF_THE_SNAKE));
        CHECK(gc.relics.has(RelicId::RING_OF_THE_SERPENT));
        CHECK(gc.relics.relics[0].id == RelicId::RING_OF_THE_SERPENT);
        CHECK_EQ(gc.relics.size(), 2);
        gc.curRoom = Room::MONSTER;
        gc.deck = Deck{};
        for (int i = 0; i < 20; ++i) {
            gc.deck.obtainRaw(Card(CardId::STRIKE_RED));
        }
        BattleContext bc;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, MonsterEncounter::CULTIST);
        CHECK_EQ(bc.cards.cardsInHand, 6);  // no 2 from the Snake any more
        CHECK_EQ(static_cast<int>(bc.player.cardDrawPerTurn), 6);
    }
    {
        GameContext gc(CharacterClass::DEFECT, 5, 0);
        gc.obtainRelic(RelicId::FROZEN_CORE);
        CHECK(!gc.relics.has(RelicId::CRACKED_CORE));
        CHECK(gc.relics.relics[0].id == RelicId::FROZEN_CORE);
        CHECK_EQ(gc.relics.size(), 1);
    }
    {
        GameContext gc(CharacterClass::WATCHER, 5, 0);
        gc.obtainRelic(RelicId::HOLY_WATER);
        CHECK(!gc.relics.has(RelicId::PURE_WATER));
        CHECK(gc.relics.relics[0].id == RelicId::HOLY_WATER);
        CHECK_EQ(gc.relics.size(), 1);
    }
    {
        GameContext gc(CharacterClass::IRONCLAD, 5, 0);
        gc.obtainRelic(RelicId::BLACK_BLOOD);
        CHECK(!gc.relics.has(RelicId::BURNING_BLOOD));
        CHECK(gc.relics.relics[0].id == RelicId::BLACK_BLOOD);
    }
}

// ******** Defect ********

void orbRelicsAtStart() {
    Battle core(CharacterClass::DEFECT, {});
    CHECK_EQ(static_cast<int>(core.bc.player.orbSlots), 3);
    CHECK(core.bc.player.orbs[0] == Orb::LIGHTNING);
    CHECK(core.bc.player.orbs[1] == Orb::EMPTY);
    CHECK_EQ(core.bc.player.energy, 3);

    // In the order the relics were gained, and Plasma pays on the first turn.
    Battle all(CharacterClass::DEFECT, {RelicId::SYMBIOTIC_VIRUS, RelicId::NUCLEAR_BATTERY});
    CHECK(all.bc.player.orbs[0] == Orb::LIGHTNING);
    CHECK(all.bc.player.orbs[1] == Orb::DARK);
    CHECK(all.bc.player.orbs[2] == Orb::PLASMA);
    CHECK_EQ(all.bc.player.energy, 4);

    // Cables doubles slot 0 only.
    Battle first(CharacterClass::DEFECT, {RelicId::GOLD_PLATED_CABLES});
    CHECK_EQ(first.bc.player.energy, 3);
    GameContext gc(CharacterClass::DEFECT, 77, 0);
    gc.relics.remove(RelicId::CRACKED_CORE);
    gc.relics.add(RelicInstance{RelicId::NUCLEAR_BATTERY, 0});
    gc.relics.add(RelicInstance{RelicId::GOLD_PLATED_CABLES, 0});
    gc.curRoom = Room::MONSTER;
    BattleContext bc;
    bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
    bc.init(gc, MonsterEncounter::CULTIST);
    CHECK(bc.player.orbs[0] == Orb::PLASMA);
    CHECK_EQ(bc.player.energy, 5);

    Battle disk(CharacterClass::DEFECT, {RelicId::DATA_DISK});
    CHECK_EQ(disk.bc.player.getStatus<PS::FOCUS>(), 1);

    Battle capacitor(CharacterClass::DEFECT, {RelicId::RUNIC_CAPACITOR});
    CHECK_EQ(static_cast<int>(capacitor.bc.player.orbSlots), 6);
    capacitor.hardenMonsters();
    capacitor.endTurn();
    CHECK_EQ(static_cast<int>(capacitor.bc.player.orbSlots), 6);  // the first turn only
}

void inserter() {
    Battle b(CharacterClass::DEFECT, {RelicId::INSERTER});
    b.hardenMonsters();
    CHECK_EQ(static_cast<int>(b.bc.player.orbSlots), 3);  // the counter goes 0 to 1 on the first turn
    CHECK_EQ(static_cast<int>(b.bc.player.inserterCounter), 1);
    b.endTurn();
    CHECK_EQ(static_cast<int>(b.bc.player.orbSlots), 4);
    CHECK_EQ(static_cast<int>(b.bc.player.inserterCounter), 0);
    b.endTurn();
    CHECK_EQ(static_cast<int>(b.bc.player.orbSlots), 4);
    b.endTurn();
    CHECK_EQ(static_cast<int>(b.bc.player.orbSlots), 5);

    // The counter carries over between fights.
    GameContext gc(CharacterClass::DEFECT, 77, 0);
    gc.relics.add(RelicInstance{RelicId::INSERTER, 1});
    gc.curRoom = Room::MONSTER;
    BattleContext bc;
    bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
    bc.init(gc, MonsterEncounter::CULTIST);
    CHECK_EQ(static_cast<int>(bc.player.orbSlots), 4);
    CHECK_EQ(static_cast<int>(bc.player.inserterCounter), 0);
}

void frozenCore() {
    GameContext gc(CharacterClass::DEFECT, 77, 0);
    gc.obtainRelic(RelicId::FROZEN_CORE);
    gc.curRoom = Room::MONSTER;
    gc.curHp = gc.maxHp = HIGH_HP;
    BattleContext bc;
    bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
    bc.init(gc, MonsterEncounter::CULTIST);
    CHECK(bc.player.orbs[0] == Orb::EMPTY);  // it doesn't start with Cracked Core's Lightning
    bc.monsters.arr[0].curHp = bc.monsters.arr[0].maxHp = HIGH_HP;
    SAction(ActionType::END_TURN).execute(bc);
    CHECK(bc.player.orbs[0] == Orb::FROST);
    CHECK(bc.player.orbs[1] == Orb::EMPTY);
    CHECK_EQ(bc.player.block, 2 - 2);  // the Frost it channeled gave its block in the turn it came, and the new turn cleared it

    // With no empty slot nothing gets evoked.
    Fight full(CharacterClass::DEFECT, {RelicId::FROZEN_CORE});
    full.setOrbs({Orb::DARK, Orb::DARK, Orb::DARK});
    full.endTurn();
    CHECK(full.bc.player.orbs[0] == Orb::DARK);
    CHECK(full.bc.player.orbs[2] == Orb::DARK);
    CHECK_EQ(full.damageTaken(), 0);
}

void emotionChip() {
    // No HP lost: nothing happens.
    Fight none(CharacterClass::DEFECT, {RelicId::EMOTION_CHIP});
    none.setOrbs({Orb::FROST, Orb::EMPTY, Orb::EMPTY});
    none.bc.player.buff<PS::BARRICADE>();
    none.endTurn();
    CHECK_EQ(none.bc.player.block, 2);  // the Frost's own end of turn

    // HP lost: every orb goes off at the start of the next turn, once.
    Fight f(CharacterClass::DEFECT, {RelicId::EMOTION_CHIP});
    f.setOrbs({Orb::FROST, Orb::EMPTY, Orb::EMPTY});
    f.bc.player.buff<PS::BARRICADE>();
    f.run( Actions::PlayerLoseHp(3, true) );
    CHECK(f.bc.player.emotionChipPulse);
    f.endTurn();
    CHECK(!f.bc.player.emotionChipPulse);
    CHECK_EQ(f.bc.player.block, 4);
    f.endTurn();
    CHECK(!f.bc.player.emotionChipPulse);  // the Cultist's first attack took 6 of block and no HP
    CHECK_EQ(f.bc.player.block, 0);

    // Plasma's start of turn counts too, and Cables does slot 0 again.
    Fight cables(CharacterClass::DEFECT, {RelicId::EMOTION_CHIP, RelicId::GOLD_PLATED_CABLES});
    cables.setOrbs({Orb::PLASMA, Orb::EMPTY, Orb::EMPTY});
    cables.run( Actions::PlayerLoseHp(3, true) );
    cables.endTurn();
    // 3 for the turn, then 2 from the turn's start of turn triggers (with Cables), then 2 from Impulse.
    CHECK_EQ(cables.bc.player.energy, 7);

    // Blocked damage isn't HP lost.
    Fight blocked(CharacterClass::DEFECT, {RelicId::EMOTION_CHIP});
    blocked.bc.player.block = 100;
    blocked.run( Actions::DamagePlayer(3) );
    CHECK(!blocked.bc.player.emotionChipPulse);
}

void cables() {
    Fight f(CharacterClass::DEFECT, {RelicId::GOLD_PLATED_CABLES});
    f.setOrbs({Orb::FROST, Orb::DARK, Orb::EMPTY});
    f.bc.player.buff<PS::BARRICADE>();
    f.endTurn();
    CHECK_EQ(f.bc.player.block, 4);
    CHECK_EQ(static_cast<int>(f.bc.player.darkOrbEvoke[1]), 12);

    Fight none(CharacterClass::DEFECT, {RelicId::GOLD_PLATED_CABLES});
    none.setOrbs({Orb::EMPTY, Orb::EMPTY, Orb::EMPTY});
    none.endTurn();
    CHECK_EQ(none.damageTaken(), 0);
}

// ******** Watcher ********

void watcherStart() {
    Battle pure(CharacterClass::WATCHER, {});
    int miracles = 0;
    for (int i = 0; i < pure.bc.cards.cardsInHand; ++i) {
        miracles += pure.bc.cards.hand[i].getId() == CardId::MIRACLE ? 1 : 0;
    }
    CHECK_EQ(miracles, 1);
    CHECK_EQ(pure.bc.cards.cardsInHand, 6);
    CHECK_EQ(pure.bc.cards.hand[0].getId(), CardId::MIRACLE);

    GameContext gc(CharacterClass::WATCHER, 77, 0);
    gc.obtainRelic(RelicId::HOLY_WATER);
    gc.curRoom = Room::MONSTER;
    BattleContext bc;
    bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
    bc.init(gc, MonsterEncounter::CULTIST);
    miracles = 0;
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        miracles += bc.cards.hand[i].getId() == CardId::MIRACLE ? 1 : 0;
    }
    CHECK_EQ(miracles, 3);

    Battle locket(CharacterClass::WATCHER, {RelicId::TEARDROP_LOCKET});
    CHECK(locket.bc.player.stance == Stance::CALM);
    CHECK_EQ(locket.bc.player.energy, 3);

    Battle lotus(CharacterClass::WATCHER, {RelicId::TEARDROP_LOCKET, RelicId::VIOLET_LOTUS});
    CHECK(lotus.bc.player.stance == Stance::CALM);
    CHECK_EQ(lotus.bc.player.energy, 3);
}

void damaru() {
    Battle b(CharacterClass::WATCHER, {RelicId::DAMARU});
    b.hardenMonsters();
    CHECK_EQ(b.bc.player.getStatus<PS::MANTRA>(), 1);  // atTurnStart runs for the first turn
    b.endTurn();
    CHECK_EQ(b.bc.player.getStatus<PS::MANTRA>(), 2);
    for (int i = 0; i < 7; ++i) {
        b.endTurn();
    }
    CHECK_EQ(b.bc.player.getStatus<PS::MANTRA>(), 9);
    b.endTurn();
    CHECK(b.bc.player.stance == Stance::DIVINITY);
    CHECK(!b.bc.player.hasStatus<PS::MANTRA>());
}

void cloakClasp() {
    Fight f(CharacterClass::WATCHER, {RelicId::CLOAK_CLASP});
    f.bc.player.buff<PS::BARRICADE>();
    f.toHand(CardId::WOUND);
    f.toHand(CardId::WOUND);
    f.toHand(CardId::WOUND);
    f.endTurn();
    CHECK_EQ(f.bc.player.block, 3);

    Fight empty(CharacterClass::WATCHER, {RelicId::CLOAK_CLASP});
    empty.bc.player.buff<PS::BARRICADE>();
    empty.endTurn();
    CHECK_EQ(empty.bc.player.block, 0);

    // A flat amount, past Dexterity and Frail.
    Fight dex(CharacterClass::WATCHER, {RelicId::CLOAK_CLASP});
    dex.bc.player.buff<PS::BARRICADE>();
    dex.bc.player.buff<PS::DEXTERITY>(3);
    dex.bc.player.debuff<PS::FRAIL>(3, false);
    dex.toHand(CardId::WOUND);
    dex.endTurn();
    CHECK_EQ(dex.bc.player.block, 1);
}

void duality() {
    Fight f(CharacterClass::WATCHER, {RelicId::DUALITY});
    f.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(f.bc.player.getStatus<PS::DEXTERITY>(), 1);
    CHECK_EQ(f.bc.player.getStatus<PS::LOSE_DEXTERITY>(), 1);
    f.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(f.bc.player.getStatus<PS::DEXTERITY>(), 2);
    f.endTurn();
    CHECK_EQ(f.bc.player.getStatus<PS::DEXTERITY>(), 0);
    CHECK(!f.bc.player.hasStatus<PS::LOSE_DEXTERITY>());

    // Attacks only.
    Fight skill(CharacterClass::WATCHER, {RelicId::DUALITY});
    skill.play(CardId::DEFEND_PURPLE);
    CHECK_EQ(skill.bc.player.getStatus<PS::DEXTERITY>(), 0);

    // LoseDexterityPower is a debuff, so Artifact stops it and the point stays.
    Fight artifact(CharacterClass::WATCHER, {RelicId::DUALITY});
    artifact.bc.player.buff<PS::ARTIFACT>(1);
    artifact.play(CardId::STRIKE_PURPLE);
    CHECK_EQ(artifact.bc.player.getStatus<PS::DEXTERITY>(), 1);
    CHECK(!artifact.bc.player.hasStatus<PS::LOSE_DEXTERITY>());
    CHECK_EQ(artifact.bc.player.getStatus<PS::ARTIFACT>(), 0);
}

void scryRelics() {
    Fight eye(CharacterClass::WATCHER, {RelicId::GOLDEN_EYE});
    for (int i = 0; i < 8; ++i) {
        eye.toDraw(CardId::WOUND);
    }
    eye.play(CardId::THIRD_EYE);
    CHECK(eye.selecting(CardSelectTask::SCRY));
    CHECK_EQ(eye.bc.cardSelectInfo.pickCount, 5);

    // Melange scries on a shuffle, and Golden Eye adds to that too.
    Fight melange(CharacterClass::WATCHER, {RelicId::MELANGE});
    for (int i = 0; i < 6; ++i) {
        melange.toDiscard(CardId::WOUND);
    }
    melange.run( Actions::DrawCards(1) );
    CHECK(melange.selecting(CardSelectTask::SCRY));
    CHECK_EQ(melange.bc.cardSelectInfo.pickCount, 3);

    Fight both(CharacterClass::WATCHER, {RelicId::MELANGE, RelicId::GOLDEN_EYE});
    for (int i = 0; i < 6; ++i) {
        both.toDiscard(CardId::WOUND);
    }
    both.run( Actions::DrawCards(1) );
    CHECK(both.selecting(CardSelectTask::SCRY));
    CHECK_EQ(both.bc.cardSelectInfo.pickCount, 5);
}

void violetLotus() {
    Fight f(CharacterClass::WATCHER, {RelicId::VIOLET_LOTUS});
    f.bc.player.stance = Stance::CALM;
    f.run( Actions::ChangeStance(Stance::WRATH) );
    CHECK_EQ(f.bc.player.energy, 13);  // Calm's 2 and the Lotus's 1

    // Leaving any other stance gives nothing, and staying in Calm isn't leaving it.
    f.run( Actions::ChangeStance(Stance::CALM) );
    CHECK_EQ(f.bc.player.energy, 13);
    f.run( Actions::ChangeStance(Stance::CALM) );
    CHECK_EQ(f.bc.player.energy, 13);
}

// ******** Potions ********

int handCount(const BattleContext &bc, CardId id, bool upgraded) {
    int count = 0;
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        count += bc.cards.hand[i].getId() == id && bc.cards.hand[i].isUpgraded() == upgraded ? 1 : 0;
    }
    return count;
}

void potions() {
    {
        Fight f(CharacterClass::WATCHER, {});
        f.drink(Potion::AMBROSIA);
        CHECK(f.bc.player.stance == Stance::DIVINITY);
        CHECK_EQ(f.bc.player.energy, 13);
    }
    for (const bool bark : {false, true}) {
        const RelicId r = bark ? RelicId::SACRED_BARK : RelicId::VAJRA;
        {
            Fight f(CharacterClass::WATCHER, {r});
            f.drink(Potion::BOTTLED_MIRACLE);
            CHECK_EQ(handCount(f.bc, CardId::MIRACLE, false), bark ? 4 : 2);
        }
        {
            Fight f(CharacterClass::SILENT, {r});
            f.drink(Potion::CUNNING_POTION);
            CHECK_EQ(handCount(f.bc, CardId::SHIV, true), bark ? 6 : 3);
        }
        {
            Fight f(CharacterClass::DEFECT, {r});
            f.drink(Potion::FOCUS_POTION);
            CHECK_EQ(f.bc.player.getStatus<PS::FOCUS>(), bark ? 4 : 2);
        }
        {
            Fight f(CharacterClass::SILENT, {r});
            f.drink(Potion::GHOST_IN_A_JAR);
            CHECK_EQ(f.bc.player.getStatus<PS::INTANGIBLE>(), bark ? 2 : 1);
        }
        {
            Fight f(CharacterClass::SILENT, {r}, MonsterEncounter::TWO_LOUSE);
            f.drink(Potion::POISON_POTION, 1);
            CHECK_EQ(f.monster(1).getStatus<MS::POISON>(), bark ? 12 : 6);
            CHECK(!f.monster(0).hasStatus<MS::POISON>());
        }
        {
            Fight f(CharacterClass::SILENT, {r, RelicId::SNECKO_SKULL});
            f.drink(Potion::POISON_POTION);
            CHECK_EQ(f.monster().getStatus<MS::POISON>(), bark ? 13 : 7);
        }
        {
            Fight f(CharacterClass::DEFECT, {r});
            f.drink(Potion::POTION_OF_CAPACITY);
            CHECK_EQ(static_cast<int>(f.bc.player.orbSlots), bark ? 7 : 5);
        }
    }
    {
        // One Dark per slot, and each channel past the first three pushes the oldest orb out.
        Fight f(CharacterClass::DEFECT, {});
        f.setOrbs({Orb::EMPTY, Orb::EMPTY, Orb::EMPTY});
        f.drink(Potion::ESSENCE_OF_DARKNESS);
        CHECK_EQ(f.bc.player.filledOrbCount(), 3);
        CHECK(f.bc.player.orbs[0] == Orb::DARK && f.bc.player.orbs[2] == Orb::DARK);
        CHECK_EQ(f.damageTaken(), 0);

        Fight bark(CharacterClass::DEFECT, {RelicId::SACRED_BARK});
        bark.setOrbs({Orb::EMPTY, Orb::EMPTY, Orb::EMPTY});
        bark.drink(Potion::ESSENCE_OF_DARKNESS);
        CHECK_EQ(bark.bc.player.filledOrbCount(), 3);
        CHECK_EQ(bark.damageTaken(), 18);  // three evoked, 6 each
    }
    {
        Fight f(CharacterClass::WATCHER, {});
        f.drink(Potion::STANCE_POTION);
        CHECK(f.selecting(CardSelectTask::CHOOSE_STANCE));
        f.select(0);
        CHECK(f.bc.player.stance == Stance::WRATH);

        Fight calm(CharacterClass::WATCHER, {});
        calm.drink(Potion::STANCE_POTION);
        calm.select(1);
        CHECK(calm.bc.player.stance == Stance::CALM);
    }
}

// ******** Pools and starts ********

void potionPools() {
    const Potion firstThree[4][3] {
        {Potion::BLOOD_POTION, Potion::ELIXIR_POTION, Potion::HEART_OF_IRON},
        {Potion::POISON_POTION, Potion::CUNNING_POTION, Potion::GHOST_IN_A_JAR},
        {Potion::FOCUS_POTION, Potion::POTION_OF_CAPACITY, Potion::ESSENCE_OF_DARKNESS},
        {Potion::BOTTLED_MIRACLE, Potion::STANCE_POTION, Potion::AMBROSIA},
    };
    for (int cc = 0; cc < 4; ++cc) {
        for (int i = 0; i < 3; ++i) {
            CHECK(PotionPool::getPotionForClass(static_cast<CharacterClass>(cc), i) == firstThree[cc][i]);
        }
        for (int i = 3; i < PotionPool::poolSize; ++i) {
            CHECK(PotionPool::getPotionForClass(static_cast<CharacterClass>(cc), i) == PotionPool::getPotionForClass(CharacterClass::IRONCLAD, i));
        }
    }
    CHECK(getPotionRarity(Potion::POISON_POTION) == PotionRarity::COMMON);
    CHECK(getPotionRarity(Potion::CUNNING_POTION) == PotionRarity::UNCOMMON);
    CHECK(getPotionRarity(Potion::GHOST_IN_A_JAR) == PotionRarity::RARE);
    CHECK(getPotionRarity(Potion::FOCUS_POTION) == PotionRarity::COMMON);
    CHECK(getPotionRarity(Potion::POTION_OF_CAPACITY) == PotionRarity::UNCOMMON);
    CHECK(getPotionRarity(Potion::ESSENCE_OF_DARKNESS) == PotionRarity::RARE);
    CHECK(getPotionRarity(Potion::BOTTLED_MIRACLE) == PotionRarity::COMMON);
    CHECK(getPotionRarity(Potion::STANCE_POTION) == PotionRarity::UNCOMMON);
    CHECK(getPotionRarity(Potion::AMBROSIA) == PotionRarity::RARE);
}

void classStarts() {
    struct Row { CharacterClass cc; int hp; int a14; RelicId relic; int orbs; };
    for (const Row &r : std::initializer_list<Row>{
            {CharacterClass::IRONCLAD, 80, 75, RelicId::BURNING_BLOOD, 0}, {CharacterClass::SILENT, 70, 66, RelicId::RING_OF_THE_SNAKE, 0},
            {CharacterClass::DEFECT, 75, 71, RelicId::CRACKED_CORE, 3}, {CharacterClass::WATCHER, 72, 68, RelicId::PURE_WATER, 0}}) {
        GameContext gc(r.cc, 1, 0);
        CHECK_EQ(gc.maxHp, r.hp);
        CHECK_EQ(gc.curHp, r.hp);
        CHECK_EQ(gc.gold, 99);
        CHECK_EQ(gc.masterMaxOrbs, r.orbs);
        CHECK(gc.relics.has(r.relic));
        GameContext high(r.cc, 1, 14);
        CHECK_EQ(high.maxHp, r.a14);
        CHECK_EQ(high.curHp, static_cast<int>(std::round(static_cast<float>(r.a14) * 0.9f)));
        CHECK(!gc.relics.relics.empty() && gc.relics.relics[0].id == r.relic);
    }
}

void codexUsesTheClass() {
    Fight f(CharacterClass::WATCHER, {RelicId::NILRYS_CODEX});
    f.endTurn();
    CHECK(f.selecting(CardSelectTask::CODEX));
    for (const CardId id : f.bc.cardSelectInfo.codexCards()) {
        CHECK(getCardColor(id) == CardColor::PURPLE);
    }
}

// ******** Random fights ********

std::vector<SAction> legalActions(const BattleContext &bc) {
    if (bc.inputState == InputState::CARD_SELECT) {
        std::vector<SAction> actions;
        for (const auto &a : search::Action::enumerateCardSelectActions(bc)) {
            actions.push_back(a);
        }
        return actions;
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
    for (int i = 0; i < bc.potionCapacity; ++i) {
        const Potion p = bc.potions[i];
        if (p == Potion::EMPTY_POTION_SLOT || p == Potion::FAIRY_POTION || p == Potion::SMOKE_BOMB) {
            continue;
        }
        if (potionRequiresTarget(p)) {
            for (int t = 0; t < bc.monsters.monsterCount; ++t) {
                if (bc.monsters.arr[t].isTargetable()) {
                    actions.emplace_back(ActionType::POTION, i, t);
                }
            }
        } else {
            actions.emplace_back(ActionType::POTION, i, 0);
        }
    }
    actions.emplace_back(ActionType::END_TURN);
    return actions;
}

const std::vector<MonsterEncounter> &encounterList() {
    static const std::vector<MonsterEncounter> encounters {
        MonsterEncounter::CULTIST, MonsterEncounter::JAW_WORM, MonsterEncounter::TWO_LOUSE, MonsterEncounter::SMALL_SLIMES,
        MonsterEncounter::GREMLIN_GANG, MonsterEncounter::GREMLIN_NOB, MonsterEncounter::LAGAVULIN, MonsterEncounter::THREE_SENTRIES,
        MonsterEncounter::SLIME_BOSS, MonsterEncounter::THE_GUARDIAN, MonsterEncounter::HEXAGHOST, MonsterEncounter::CHOSEN,
        MonsterEncounter::BOOK_OF_STABBING, MonsterEncounter::GREMLIN_LEADER, MonsterEncounter::CHAMP, MonsterEncounter::AUTOMATON,
        MonsterEncounter::COLLECTOR, MonsterEncounter::THREE_BYRDS, MonsterEncounter::REPTOMANCER, MonsterEncounter::TRANSIENT,
        MonsterEncounter::THREE_DARKLINGS, MonsterEncounter::AWAKENED_ONE, MonsterEncounter::TIME_EATER, MonsterEncounter::DONU_AND_DECA,
        MonsterEncounter::WRITHING_MASS, MonsterEncounter::MAW, MonsterEncounter::SHIELD_AND_SPEAR, MonsterEncounter::THE_HEART,
    };
    return encounters;
}

const std::vector<RelicId> &classRelics(CharacterClass cc) {
    static const std::vector<RelicId> silent {RelicId::HOVERING_KITE, RelicId::TINGSHA, RelicId::TOUGH_BANDAGES, RelicId::NINJA_SCROLL, RelicId::PAPER_KRANE,
                                              RelicId::SNECKO_SKULL, RelicId::THE_SPECIMEN, RelicId::TWISTED_FUNNEL, RelicId::WRIST_BLADE};
    static const std::vector<RelicId> defect {RelicId::DATA_DISK, RelicId::EMOTION_CHIP, RelicId::GOLD_PLATED_CABLES, RelicId::INSERTER, RelicId::NUCLEAR_BATTERY,
                                              RelicId::RUNIC_CAPACITOR, RelicId::SYMBIOTIC_VIRUS};
    static const std::vector<RelicId> watcher {RelicId::CLOAK_CLASP, RelicId::DAMARU, RelicId::DUALITY, RelicId::GOLDEN_EYE, RelicId::MELANGE,
                                               RelicId::TEARDROP_LOCKET, RelicId::VIOLET_LOTUS};
    static const std::vector<RelicId> none;
    switch (cc) {
        case CharacterClass::SILENT: return silent;
        case CharacterClass::DEFECT: return defect;
        case CharacterClass::WATCHER: return watcher;
        default: return none;
    }
}

const std::vector<CardId> &classCards(CharacterClass cc) {
    static std::map<CharacterClass, std::vector<CardId>> byClass;
    auto &cards = byClass[cc];
    if (cards.empty()) {
        const CardColor color = cc == CharacterClass::SILENT ? CardColor::GREEN : cc == CharacterClass::DEFECT ? CardColor::BLUE
                : cc == CharacterClass::WATCHER ? CardColor::PURPLE : CardColor::RED;
        const int count = static_cast<int>(sizeof(cardStringIds) / sizeof(cardStringIds[0]));
        for (int i = 1; i < count; ++i) {
            const auto id = static_cast<CardId>(i);
            if (getCardColor(id) == color) {
                cards.push_back(id);
            }
        }
    }
    return cards;
}

const std::vector<Potion> &classPotions(CharacterClass cc) {
    static const std::vector<Potion> potions[4] {
        {Potion::BLOOD_POTION, Potion::ELIXIR_POTION, Potion::HEART_OF_IRON},
        {Potion::POISON_POTION, Potion::CUNNING_POTION, Potion::GHOST_IN_A_JAR},
        {Potion::FOCUS_POTION, Potion::POTION_OF_CAPACITY, Potion::ESSENCE_OF_DARKNESS},
        {Potion::BOTTLED_MIRACLE, Potion::STANCE_POTION, Potion::AMBROSIA},
    };
    return potions[static_cast<int>(cc)];
}

void randomFights(int fights) {
    const auto &encounters = encounterList();
    for (int fight = 0; fight < fights; ++fight) {
        std::mt19937 rng(static_cast<unsigned>(fight) * 2654435761u + 13);
        const auto cc = static_cast<CharacterClass>(1 + fight % 3);
        GameContext gc(cc, static_cast<std::uint64_t>(fight) + 9000, fight % 2 == 0 ? 0 : 20);
        gc.curRoom = Room::MONSTER;
        gc.curHp = gc.maxHp = 300;
        gc.deck = Deck{};

        const auto &relics = classRelics(cc);
        for (const RelicId r : relics) {
            if (rng() % 2 == 0) {
                gc.relics.add(RelicInstance{r, 0});
            }
        }
        // The starter swaps, in a share of the fights.
        if (rng() % 3 == 0) {
            gc.obtainRelic(cc == CharacterClass::SILENT ? RelicId::RING_OF_THE_SERPENT
                           : cc == CharacterClass::DEFECT ? RelicId::FROZEN_CORE : RelicId::HOLY_WATER);
        }
        // Every potion gets drunk over the run of fights.
        gc.potionCapacity = 5;
        gc.potionCount = 3;
        for (int i = 0; i < 5; ++i) {
            gc.potions[i] = i < 3 ? classPotions(cc)[i] : Potion::EMPTY_POTION_SLOT;
        }
        if (rng() % 3 == 0) {
            gc.relics.add(RelicInstance{RelicId::SACRED_BARK, 0});
        }

        const auto &cards = classCards(cc);
        for (int i = 0; i < 18; ++i) {
            gc.deck.obtainRaw(Card(cards[rng() % cards.size()], static_cast<int>(rng() % 2)));
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
                ++failures;
                std::cerr << "fight " << fight << " stuck in input state " << static_cast<int>(bc.inputState) << "\n";
                break;
            }
            const auto actions = legalActions(bc);
            if (actions.empty()) {
                std::cerr << "no action in fight " << fight << " task " << static_cast<int>(bc.cardSelectInfo.cardSelectTask) << "\n";
                ++failures;
                break;
            }
            const SAction a = actions[rng() % actions.size()];
            CHECK(a.isValidAction(bc));
            a.execute(bc);

            CHECK(bc.cards.cardsInHand >= 0 && bc.cards.cardsInHand <= 10);
            CHECK(bc.player.orbSlots >= 0 && bc.player.orbSlots <= Player::MAX_ORB_SLOTS);
            CHECK(bc.player.energy >= 0);
        }
    }
}

void searchRuns(int fights) {
    for (int fight = 0; fight < fights; ++fight) {
        const auto cc = static_cast<CharacterClass>(1 + fight % 3);
        GameContext gc(cc, static_cast<std::uint64_t>(fight) + 5000, 0);
        gc.curRoom = Room::MONSTER;
        gc.deck = Deck{};
        for (const RelicId r : classRelics(cc)) {
            gc.relics.add(RelicInstance{r, 0});
        }
        const auto &cards = classCards(cc);
        for (int i = 0; i < 12; ++i) {
            gc.deck.obtainRaw(Card(cards[(fight * 12 + i) % cards.size()], i % 2));
        }
        BattleContext bc;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::INVALID;
        bc.init(gc, MonsterEncounter::TWO_LOUSE);
        bc.executeActions();
        search::BattleScumSearcher2 searcher(bc);
        searcher.search(200);
        CHECK(searcher.root.simulationCount > 0);
    }
}

}  // namespace

int main(int argc, char **argv) {
    const int randomFightCount = argc > 1 ? std::atoi(argv[1]) : 3000;

    hoveringKite();
    tingsha();
    toughBandages();
    discardTriggersEverywhere();
    theSpecimen();
    ringsAndScroll();
    twistedFunnelAndWristBlade();
    starterSwaps();

    orbRelicsAtStart();
    inserter();
    frozenCore();
    emotionChip();
    cables();

    watcherStart();
    damaru();
    cloakClasp();
    duality();
    scryRelics();
    violetLotus();

    potions();
    potionPools();
    classStarts();
    codexUsesTheClass();

    randomFights(randomFightCount);
    searchRuns(15);

    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
