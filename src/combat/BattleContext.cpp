//
// Created by gamerpuppy on 7/4/2021.
//

#include "combat/BattleContext.h"
#include "game/GameContext.h"
#include "game/Game.h"

using namespace sts;

namespace sts {
    thread_local BattleContext *g_debug_bc;
}


// assume all bc fields have just been initialized by in class member initializers
void BattleContext::init(const GameContext &gc) {
    init(gc, gc.info.encounter);
}

void BattleContext::init(const GameContext &gc, MonsterEncounter encounterToInit) {
    undefinedBehaviorEvoked = false;
    haveUsedDiscoveryAction = false;
    seed = gc.seed;
    floorNum = gc.floorNum;
    encounter = encounterToInit;

    auto startRandom = Random(gc.seed+gc.floorNum);
    aiRng = startRandom;
    monsterHpRng = startRandom;
    shuffleRng = startRandom;
    cardRandomRng = startRandom;
    if (gc.curRoom == Room::EVENT && gc.curEvent == Event::COLOSSEUM && gc.info.eventData == 1) {
        // Its second fight is in the same room, so the streams carry on from the first.
        aiRng = gc.aiRng;
        monsterHpRng = gc.monsterHpRng;
        shuffleRng = gc.shuffleRng;
        cardRandomRng = gc.cardRandomRng;
        shuffleRng.randomLong(); // Colosseum.reopen calls preBattlePrep, which shuffles a deck nothing uses
    }
    miscRng = gc.miscRng;
    potionRng = gc.potionRng;
    cardRng = gc.cardRng;

    upgradableDeckIdxs.clear();
    lessonLearnedUpgrades.clear();
    const bool hasLessonLearned = std::any_of(gc.deck.cards.begin(), gc.deck.cards.end(),
                                              [](const Card &c) { return c.getId() == CardId::LESSON_LEARNED; });
    if (hasLessonLearned) {
        for (int i = 0; i < gc.deck.size(); ++i) {
            if (gc.deck.cards[i].canUpgrade()) {
                const bool searingBlow = gc.deck.cards[i].getId() == CardId::SEARING_BLOW;
                upgradableDeckIdxs.push_back(static_cast<std::int16_t>(searingBlow ? i | SEARING_BLOW_DECK_IDX : i));
            }
        }
    }

    ascension = gc.ascension;
    outcome = Outcome::UNDECIDED;
    inputState = InputState::EXECUTING_ACTIONS;
    miscBits.reset();

    monsterTurnIdx = 6;
    skipMonsterTurn = false;
    turnHasEnded = false;
    isBattleOver = false;

    actionQueue.clear();
    cardQueue.clear();

    potionCount = gc.potionCount;
    potionCapacity = gc.potionCapacity;
    potions = gc.potions;

    player.cc = gc.cc;  // FIX: was never initialised -> Entropic Brew / Dead Branch used a garbage class pool
    player.curHp = gc.curHp;
    player.maxHp = gc.maxHp;
    player.gold = gc.gold;

    monsters.init(*this, encounterToInit);
    if (gc.map->burningEliteX == gc.curMapNodeX && gc.map->burningEliteY == gc.curMapNodeY) {
        monsters.applyEmeraldEliteBuff(*this, gc.map->burningEliteBuff, gc.act);
    }

    player.cardDrawPerTurn = 5;
    if (gc.hasRelic(R::SNECKO_EYE)) {
        player.cardDrawPerTurn += 2;
    }
    if (gc.relics.has(R::RING_OF_THE_SERPENT)) {
        player.cardDrawPerTurn += 1;
    }
    //addToBot(Actions::DrawCards(player.cardDrawPerTurn));

    player.orbSlots = gc.masterMaxOrbs; // AbstractPlayer.preBattlePrep

    cards.init(gc, *this);

    initRelics(gc);
    player.energy += player.energyPerTurn;

    executeActions();
}

// this doesnt apply powers in order, so if that matters in the future all relics will have to be sorted
void BattleContext::initRelics(const GameContext &gc) {
    player.relicBits0 = gc.relics.relicBits0;
    player.relicBits1 = gc.relics.relicBits1;

    fixed_list<RelicId, 4> atBattleStartPreDraw;
    fixed_list<RelicId, 8> atBattleStart;
    fixed_list<RelicId, 2> atTurnStartPostDraw;

    auto room = gc.curRoom;
    // The relics below check the room's eliteTrigger. Dead Adventurer's fight and Colosseum's second fight set it too.
    const bool eliteTrigger = room == Room::ELITE
            || (room == Room::EVENT && (gc.curEvent == Event::DEAD_ADVENTURER || encounter == MonsterEncounter::COLOSSEUM_EVENT_NOBS));
    // These check for a monster of type BOSS, not the room, and Mind Bloom's fight is against one.
    const bool bossMonster = room == Room::BOSS || (room == Room::EVENT && gc.curEvent == Event::MINDBLOOM);

    auto &p = player;

    for (const auto &r : gc.relics.relics) {
        switch (r.id) {

            case R::HOLY_WATER:
            case R::NINJA_SCROLL:
            case R::PURE_WATER:
            case R::TOOLBOX:
                atBattleStartPreDraw.push_back(r.id);
                break;

            case R::BAG_OF_MARBLES:
            case R::BAG_OF_PREPARATION:
            case R::CLOCKWORK_SOUVENIR:
            case R::GREMLIN_VISAGE:
            case R::RED_MASK:
            case R::RING_OF_THE_SNAKE:
            case R::TWISTED_FUNNEL:
                atBattleStart.push_back(r.id);
                break;

            case R::MARK_OF_PAIN:
                ++p.energyPerTurn;
                atBattleStart.push_back(r.id);
                break;

            case R::GAMBLING_CHIP:
            case R::WARPED_TONGS:
                atTurnStartPostDraw.push_back(r.id);
                break;

            case R::AKABEKO:
                p.buff<PS::VIGOR>(8);
                break;

            case R::BRIMSTONE:
                p.buff<PS::STRENGTH>(2);
                for (int i = 0; i < monsters.monsterCount; ++i) {
                    Monster &m = monsters.arr[i];
                    if (m.isTargetable()) {
                        m.buff<MS::STRENGTH>(1);
                    }
                }
                break;

            case R::ECTOPLASM:
                p.energyPerTurn++;
                break;

            case R::ENCHIRIDION: {
                const auto cardId = getTrulyRandomCardInCombat(cardRandomRng, p.cc, CardType::POWER);
                CardInstance c(cardId);
                c.setCostForTurn(0);
                addToBot( Actions::MakeTempCardInHand(c) );
                break;
            }

            case R::HAPPY_FLOWER:
                player.happyFlowerCounter = r.data + 1;
                if (player.happyFlowerCounter == 3) {
                    ++player.energy;
                    player.happyFlowerCounter = 0;
                }
                break;

            case R::INCENSE_BURNER:
                p.incenseBurnerCounter = r.data;
                if (++p.incenseBurnerCounter == 6) {
                    p.incenseBurnerCounter = 0;
                    p.buff<PS::INTANGIBLE>(1);
                }
                break;

            case R::INK_BOTTLE:
                p.inkBottleCounter = r.data;
                break;

            case R::INSERTER:
                if (r.data) {
                    p.inserterCounter = 0;
                    p.increaseOrbSlots(1);
                } else {
                    p.inserterCounter = 1;
                }
                break;

            case R::LIZARD_TAIL:
                p.setHasRelic<R::LIZARD_TAIL>(r.data);
                break;

            case R::NUNCHAKU:
                p.nunchakuCounter = r.data;
                break;

            case R::OMAMORI:
                p.setHasRelic<R::OMAMORI>(r.data);
                break;

            case R::PEN_NIB:
                if (r.data == 9) {
                    p.buff<PS::PEN_NIB>(1);
                    p.penNibCounter = -1;
                } else {
                    p.penNibCounter = r.data;
                }
                break;

            case R::PHILOSOPHERS_STONE:
                for (int i = 0; i < monsters.monsterCount; ++i) {
                    auto &m = monsters.arr[i];
                    if (m.isTargetable()) { // not the empty slots Bronze Automaton's orbs will use
                        m.buff<MS::STRENGTH>(1);
                    }
                }
                p.energyPerTurn++;
                break;

            case R::RUNIC_DOME:
                p.energyPerTurn++;
                break;

            case R::SNECKO_EYE:
                p.debuff<PS::CONFUSED>(1);
                break;

            case R::SOZU:
                p.energyPerTurn++;
                break;

            case R::SUNDIAL:
                p.sundialCounter = r.data;
                break;

            case R::VELVET_CHOKER:
                p.energyPerTurn++;
                break;

            case R::ANCHOR:
                p.block += 10;
                break;

            case R::ANCIENT_TEA_SET:
                if (gc.relics.getRelicValue(R::ANCIENT_TEA_SET) == -2) {
                    p.gainEnergy(2);
                }
                break;

            case R::BLOOD_VIAL:
                p.heal(2);
                break;

            case R::BRONZE_SCALES:
                p.buff<PS::THORNS>(3);
                break;

            case R::BUSTED_CROWN:
                p.energyPerTurn++;
                break;

            case R::COFFEE_DRIPPER:
                p.energyPerTurn++;
                break;

            case R::CRACKED_CORE:
                p.channelOrb(*this, Orb::LIGHTNING);
                break;

            case R::CURSED_KEY:
                p.energyPerTurn++;
                break;

            case R::DAMARU:
                queueMantra(1);
                break;

            case R::DATA_DISK:
                p.buff<PS::FOCUS>(1);
                break;

            case R::DU_VU_DOLL: {
                int curses = 0;
                for (const auto &c : gc.deck.cards) {
                    if (c.getType() == CardType::CURSE) {
                        ++curses;
                    }
                }
                p.buff<PS::STRENGTH>(curses);
                break;
            }

            case R::FOSSILIZED_HELIX:
                p.buff<PS::BUFFER>(1);
                break;

            case R::FUSION_HAMMER:
                p.energyPerTurn++;
                break;

            case R::GIRYA:
                p.buff<PS::STRENGTH>(r.data);
                break;

            case R::LANTERN:
                p.gainEnergy(1);
                break;

            case R::MUTAGENIC_STRENGTH: // this appears to be applied before clockwork if it was acquired first
                p.buff<PS::STRENGTH>(3);
                p.debuff<PS::LOSE_STRENGTH>(3);
                break;

            case R::NEOWS_LAMENT: // remember to decrement somewhere else
                if (r.data > 0) {
                    for (int i = 0; i < monsters.monsterCount; ++i) {
                        Monster &m = monsters.arr[i];
                        if (m.idx == -1) continue;  // FIX: reserved (not yet spawned) slots are not monsters; giving them
                                                    // 1 HP made them targetable and "killable" (monstersAlive went negative)
                        m.curHp = 1;
                    }
                }
                break;

            case R::NUCLEAR_BATTERY:
                p.channelOrb(*this, Orb::PLASMA);
                break;

            case R::ODDLY_SMOOTH_STONE:
                p.buff<PS::DEXTERITY>(1);
                break;

            case R::PANTOGRAPH:
                if (bossMonster) {
                    p.heal(25);
                }
                break;

            case R::PRESERVED_INSECT:
                if (eliteTrigger) {
                    for (int i = 0; i < monsters.monsterCount; ++i) {
                        Monster &m = monsters.arr[i];
                        m.curHp = static_cast<int>(m.maxHp * .75);
                    }
                }
                break;

            case R::RING_OF_THE_SERPENT:
                // now handled in battlecontext init
//                p.cardDrawPerTurn++;
                break;

            case R::RUNIC_CAPACITOR:
                p.increaseOrbSlots(3);
                break;

            case R::SLAVERS_COLLAR:
                if (eliteTrigger || bossMonster) {
                    p.energyPerTurn++;
                }
                break;

            case R::SLING_OF_COURAGE:
                if (eliteTrigger) {
                    p.buff<PS::STRENGTH>(2);
                }
                break;

            case R::SYMBIOTIC_VIRUS:
                p.channelOrb(*this, Orb::DARK);
                break;

            case R::TEARDROP_LOCKET:
                p.changeStance<Stance::CALM>();
                break;

            case R::THREAD_AND_NEEDLE:
                p.buff<PS::PLATED_ARMOR>(4);
                break;

            case R::VAJRA:
                p.buff<PS::STRENGTH>(1);
                break;

            default:
                break;
        }
    }

    // todo maybe move this to proper place -nvm drawCards is added below this
    for (auto r : atBattleStartPreDraw) {
        switch (r) {
            case R::HOLY_WATER:
                addToBot(Actions::MakeTempCardInHand(CardId::MIRACLE, false, 3));
                break;

            case R::NINJA_SCROLL:
                addToBot(Actions::MakeTempCardInHand(CardId::SHIV, false, 3) );
                break;

            case R::PURE_WATER:
                addToBot(Actions::MakeTempCardInHand(CardId::MIRACLE, false, 1));
                break;

            case R::TOOLBOX:
                addToBot( Actions::ToolboxAction() );
                break;

            default:
                break;
        }
    }

    addToBot( Actions::DrawCards(p.cardDrawPerTurn) );

    for (auto r : atBattleStart) {
        switch (r) {
            case R::BAG_OF_MARBLES:
                addToBot( Actions::DebuffAllEnemy<MS::VULNERABLE>(1, false) );
                break;

            case R::BAG_OF_PREPARATION:
                addToBot( Actions::DrawCards(2) );
                break;

            case R::CLOCKWORK_SOUVENIR:
                addToBot( Actions::BuffPlayer<PS::ARTIFACT>(1) );
                break;

            case R::GREMLIN_VISAGE:
                p.debuff<PS::WEAK>(1, false);
                break;

            case R::MARK_OF_PAIN:
                addToBot( Actions::MakeTempCardInDrawPile( {CardId::WOUND}, 2, true) );
                break;

            case R::RED_MASK:
                addToBot( Actions::DebuffAllEnemy<MS::WEAK>(1, false) );
                break;

            case R::RING_OF_THE_SNAKE:
                addToBot( Actions::DrawCards(2) );
                break;

            case R::TWISTED_FUNNEL:
                addToBot( Actions::DebuffAllEnemy<MS::POISON>(4) );
                break;

            default:
                break;
        }
    }

    if (gc.hasRelic(R::MERCURY_HOURGLASS)) {
        addToBot( Actions::DamageAllEnemy(3) );
    }

    if (gc.hasRelic(R::RED_SKULL) && p.curHp <= p.maxHp / 2) { // after Blood Vial's heal
        p.buff<PS::STRENGTH>(3);
        p.redSkullActive = true;
    }

    for (auto r : atTurnStartPostDraw) {
        switch (r) {
            case R::GAMBLING_CHIP:
                addToBot( Actions::GambleAction() );
                break;

            case R::WARPED_TONGS:
                addToBot( Actions::UpgradeRandomCardAction() );
                break;

            default:
                break;
        }
    }

    // ** OnStartOfTurn ** ORBS todo
    //RelicCables -> OnStartOfTurn again for orb 0
}

void BattleContext::exitBattle(GameContext &g) const {
    // do this first so that darkstone periapt is overridden by curHp and maxHp are set afterwards
    const auto &m = monsters.arr[0];
    if (m.id == MonsterId::WRITHING_MASS && m.miscInfo) {
        if (player.hasRelic<R::OMAMORI>()) {
            --g.relics.getRelicValueRef(RelicId::OMAMORI);
        } else {
            g.deck.obtain(g, CardId::PARASITE);

        }
    }

    g.potionCount = potionCount;
    g.potions = potions;

    // not sure its really necessary to sync these every time, (i believe colosseum is the only time two battles occur on the same floor)
    g.aiRng = aiRng;
    g.cardRandomRng = cardRandomRng;
    g.miscRng = miscRng;
    g.monsterHpRng = monsterHpRng;
    g.potionRng = potionRng;
    g.shuffleRng = shuffleRng;
    g.cardRng = cardRng;

    g.curHp = player.curHp;
    g.maxHp = player.maxHp;
    g.gold = player.gold;
    g.powersPlayedLastCombat = player.powersPlayedThisCombat;


    // todo lesson learned bitset

    // relic counters
    updateRelicsOnExit(g);

    // cards
    updateCardsOnExit(g.deck);
    for (const auto deckIdx : lessonLearnedUpgrades) {
        g.deck.upgrade(deckIdx);
    }

    g.info.stolenGold = 0;
    g.info.allMonstersEscaped = monsters.monsterCount > 0;
    for (int i = 0; i < monsters.monsterCount; ++i) {
        const auto &m = monsters.arr[i];
        if (m.id == MonsterId::INVALID) {
            continue;
        }

        const bool canHaveStolenGold = m.id == MonsterId::LOOTER || m.id == MonsterId::MUGGER;
        const bool escaped = m.curHp > 0 && (m.moveHistory[0] == MMID::LOOTER_ESCAPE ||
                                             m.moveHistory[0] == MMID::MUGGER_ESCAPE);

        if (requiresStolenGoldCheck() && canHaveStolenGold && !escaped) {
            g.info.stolenGold += m.miscInfo;
        }
        // Not tied to the stolen gold check: a thief that stole nothing (no gold to take) can still escape.
        g.info.allMonstersEscaped = g.info.allMonstersEscaped && canHaveStolenGold && escaped;
    }

    if (outcome == Outcome::PLAYER_LOSS) {
        g.outcome = GameOutcome::PLAYER_LOSS;
    } else if (miscBits.test(1)) {
        g.afterEscape();  // Smoke Bomb: straight back to the map, no rewards
    } else {
        // player victory
        g.regainControl();
    }

    BattleContext::sum += g.curHp + g.maxHp + g.gold + g.act
            + g.ascension + g.floorNum + potionRng.counter + cardRandomRng.counter;
}

void BattleContext::updateRelicsOnExit(GameContext &g) const {
    // Victory relics heal while the room's phase is still COMBAT, so Magic Flower counts.
    const auto victoryHeal = [&g](int amount) { return g.hasRelic(RelicId::MAGIC_FLOWER) ? (amount * 3 + 1) / 2 : amount; };
    // AbstractRoom.endBattle triggers it ahead of every other relic's onVictory, so Burning Blood can't lift the HP past half first.
    if (outcome == Outcome::PLAYER_VICTORY && g.hasRelic(RelicId::MEAT_ON_THE_BONE) && g.curHp <= g.maxHp / 2) {
        g.playerHeal(victoryHeal(12));
    }
    for (auto &r : g.relics.relics) {
        switch (r.id) {
            case RelicId::ANCIENT_TEA_SET:
                r.data = 0;
                break;

            case RelicId::HAPPY_FLOWER:
                r.data = player.happyFlowerCounter;
                break;

            case RelicId::INCENSE_BURNER:
                r.data = player.incenseBurnerCounter;
                break;

            case RelicId::INK_BOTTLE:
                r.data = player.inkBottleCounter;
                break;

            case RelicId::INSERTER:
                r.data = player.inserterCounter;
                break;

            case RelicId::NEOWS_LAMENT:
                if (r.data > 0) {
                    --r.data;
                }
                break;

            case RelicId::NUNCHAKU:
                r.data = player.nunchakuCounter;
                break;

            case RelicId::PEN_NIB:
                // possible bug
                if (player.penNibCounter == -1) {
                    r.data = 9;
                } else {
                    r.data = player.penNibCounter;
                }
                break;

            case RelicId::SUNDIAL:
                r.data = player.sundialCounter;
                break;

            case RelicId::LIZARD_TAIL:
                if (!player.hasRelic<R::LIZARD_TAIL>()) {
                    r.data = 0;
                }
                break;

            case RelicId::BURNING_BLOOD:
                if (outcome == Outcome::PLAYER_VICTORY) {
                    g.playerHeal(victoryHeal(6));
                }
                break;

            case RelicId::BLACK_BLOOD:
                if (outcome == Outcome::PLAYER_VICTORY) {
                    g.playerHeal(victoryHeal(12));
                }
                break;

            default:
                break;
        }

    }

    // AbstractPlayer.onVictory goes through the relics and then the powers.
    if (outcome == Outcome::PLAYER_VICTORY && player.hasStatus<PS::SELF_REPAIR>()) {
        g.playerHeal(victoryHeal(player.getStatus<PS::SELF_REPAIR>()));
    }
}

void cardOnExit(const CardInstance &c, Deck &deck) {
    const auto isDeckCard = c.uniqueId < deck.size();
    if (!isDeckCard) {
        return;
    }

    const auto deckIdx = c.uniqueId;
    auto &deckCard = deck.cards[deckIdx];

    if (c.getId() == CardId::RITUAL_DAGGER) {
        deckCard.misc = c.specialData;
    }

    // IncreaseMiscAction also raises the deck's copy.
    if (c.getId() == CardId::GENETIC_ALGORITHM) {
        deckCard.misc = c.specialData;
    }
}

void BattleContext::updateCardsOnExit(Deck &deck) const {
    for (const auto &c : cards.drawPile) {
        cardOnExit(c, deck);
    }

    for (const auto &c : cards.discardPile) {
        cardOnExit(c, deck);
    }

    for (const auto &c : cards.exhaustPile) {
        cardOnExit(c, deck);
    }

    for (int i = 0; i < cards.cardsInHand; ++i) {
        cardOnExit(cards.hand[i], deck);
    }

//    if (curCardQueueItem)

}


void BattleContext::setRequiresStolenGoldCheck(bool value) {
    miscBits.set(0, value);
}

bool BattleContext::requiresStolenGoldCheck() const {
    return miscBits.test(0);
}

int BattleContext::getMonsterTurnNumber() const {
    return turn+1; // todo;
}

void BattleContext::setState(InputState s) {
    inputState = s;
}

void BattleContext::addToTop(const Action &a) {
    actionQueue.pushFront(a);
}

void BattleContext::addToBot(const Action &a) {
    actionQueue.pushBack(a);
}

void BattleContext::addToTop(Action &&a) {  // PERF: move overload, no std::function copy
    actionQueue.pushFront(std::move(a));
}

void BattleContext::addToBot(Action &&a) {  // PERF: move overload, no std::function copy
    actionQueue.pushBack(std::move(a));
}

// todo remove or replace other method that does this
void BattleContext::addToTopCard(CardQueueItem item) {
    cardQueue.pushFront(item);
}

void BattleContext::addToBotCard(CardQueueItem item) {
    cardQueue.pushBack(item);
}

void BattleContext::checkCombat() {
    // a minion leader's death ends the fight, but its minions only die in actions to come, so the game keeps the queue
    if (outcome == Outcome::PLAYER_VICTORY && monsters.areMonstersBasicallyDead()) {
        clearPostCombatActions();
    }
}

void BattleContext::clearPostCombatActions() {
    // todo?
    cleanCardQueue(); // this is actually done when monsters are damaged but we can do here?
//    actionQueue.clearOnCombatVictory();

    int curIdx = actionQueue.front;
    int placeIdx = actionQueue.front;

    int oldsize = actionQueue.size;
    for (int i = 0; i < oldsize; ++i) {
        if (curIdx >= actionQueue.getCapacity()) {
            curIdx = 0;
        }
        const bool shouldClear = actionQueue.bits[curIdx];

        if (shouldClear) {
            --actionQueue.size;
        } else {
            if (placeIdx >= actionQueue.getCapacity()) {
                placeIdx = 0;
            }

            actionQueue.arr[placeIdx] = actionQueue.arr[curIdx];
            actionQueue.bits[placeIdx] = actionQueue.bits[curIdx];
            ++placeIdx;
        }
        ++curIdx;
    }
}

void BattleContext::cleanCardQueue() {
    // todo
    // not sure where this matters, as we don't queue more than 1 item at a time in the hand
}

bool BattleContext::isCardPlayAllowed() const {
    if (player.hasRelic<R::VELVET_CHOKER>() && player.cardsPlayedThisTurn >= 6) {
        return false;
    }

    if (cards.handNormalityCount && player.cardsPlayedThisTurn >= 3) {
        return false;
    }

    return true;
}

void BattleContext::executeActions() {
    // todo find a place for checking where card queue is empty and player doesn't have control for calling onEndingTurn
    ++sum;
    g_debug_bc = this;

    while (true)
    {
        if (++loopCount > 1000000 || monsters.monstersAlive < 0 || turn > 500) {
            // something went wrong
            if (turn > 500) {
                outcome = Outcome::PLAYER_LOSS;
                break;
            }

            std::cerr << seed << std::endl;
            std::cout << *this << '\n';
            assert(false);
        }

        if (inputState != InputState::EXECUTING_ACTIONS) {
            break;
        }

        if (outcome == Outcome::PLAYER_LOSS) {
            break;
        }

        if (!actionQueue.isEmpty()) {
            // do a action
            auto a = std::move(actionQueue.popFront());
            a(*this);
            continue;
        }

        if (outcome != Outcome::UNDECIDED) {
            break;
        }

        if (!cardQueue.isEmpty()) {
            // play a card queue item
            auto item = cardQueue.popFront();
            playCardQueueItem(item);
            continue;
        }

        // can't win check
        if (cards.cardsInHand + cards.discardPile.size() + cards.drawPile.size() == 0) {
            bool hasDamageWithoutCards = player.hasStatus<PS::OMEGA>() ||
                    player.hasStatus<PS::THORNS>() ||
                    player.hasOrb() ||
                    player.hasStatus<PS::HELLO_WORLD>() ||
                    player.hasStatus<PS::CREATIVE_AI>() ||
                    player.hasStatus<PS::INFINITE_BLADES>() ||
                    player.hasStatus<PS::NOXIOUS_FUMES>() ||
                    player.nightmareCount > 0 ||
                    player.bomb1 ||
                    player.bomb2 ||
                    player.bomb3;

            // Poison still ticks with no cards left.
            for (int i = 0; i < monsters.monsterCount && !hasDamageWithoutCards; ++i) {
                hasDamageWithoutCards = monsters.arr[i].hasStatus<MS::POISON>();
            }

            if (!hasDamageWithoutCards && monsters.arr[0].id != MonsterId::TRANSIENT) {
                outcome = Outcome::PLAYER_LOSS;
                break;
            }
        }

        if (outcome != Outcome::UNDECIDED) {
            break;
        }

        if (monsterTurnIdx < monsters.monsterCount) {
            // do a monster turn
            monsters.doMonsterTurn(*this);
            continue;
        }
        monsters.skipTurn.reset();

        if (outcome != Outcome::UNDECIDED) {
            break;
        }

        if (turnHasEnded) {
            // after all monster turns
            afterMonsterTurns();
            continue;

        }

        if (endTurnQueued) {
            endTurnQueued = false;
            onTurnEnding();
            continue;

        }

        if (player.hasRelic<R::UNCEASING_TOP>()) {
            // turn cannot have ended here
#ifdef sts_asserts
            assert(!endTurnQueued);
            assert(actionQueue.isEmpty());
            assert(cardQueue.isEmpty());
#endif

            if (cards.cardsInHand == 0) {
                drawCards(1);
                // an empty draw pile only queues the shuffle and the draw; Evolve and the like queue theirs too
                if (!actionQueue.isEmpty()) {
                    continue;
                }
            }
        }

        setState(InputState::PLAYER_NORMAL);
        break;
    }
}

void BattleContext::playCardQueueItem(CardQueueItem playItem) {
    // if c is null callEndOfTurnActions()
    // if cardQueueSize is 1 and carditem is endTurnAutoplay diable unceasing top

    curCardQueueItem = playItem;
    auto &item = curCardQueueItem;
    auto &c = item.card;

    if (item.isEndTurn) {
        // the game removes this card from limbo - don't think necessary
        callEndOfTurnActions();
        return;
    }


    // if cardQueueItem random target, assign a target
    if (item.randomTarget) {
        item.target = monsters.getRandomMonsterIdx(cardRandomRng);
    }

//    bool canPlayCard = false; // not really sure what this is used for
    // Copies go through canUse too: only the energy check knows about autoplay, so Velvet Choker can stop a Double Tap copy.
    const bool canUseCard = item.triggerOnUse && isCardPlayAllowed() && c.canUse(*this, item.target, item.autoplay)
            && (!c.requiresTarget() || monsters.arr[item.target].isTargetable());
    if (canUseCard) { // not sure if this is correct,
//        canPlayCard = true; // what is this for......

        if (c.isFreeToPlay(*this)) { // what the fuck
            c.freeToPlayOnce = true;
        }

        if (c.requiresTarget() && !item.autoplay) {
            player.lastTargetedMonster = item.target;
        }

        if (!c.requiresTarget() || monsters.arr[item.target].isTargetable()) { // this is redundant right???? -> no i think echo form abilities can queue a card with invalid target
            useCard();
        }

    }

    if (!item.triggerOnUse) {
        useNoTriggerCard(); // for burn, decay, doubt, regret and shame,

    } else if (!canUseCard && item.autoplay) {
        // GameActionManager sends it straight to UseCardAction without triggers, so Havoc still exhausts it.
        item.exhaustOnUse |= c.doesExhaust();
        item.triggerOnUse = false;
        addToBot(Actions::OnAfterCardUsed());
    }
}

void BattleContext::useCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    item.exhaustOnUse |= c.doesExhaust();
    ++player.cardsPlayedThisTurn;
    noteCardTypePlayed(c.getType());

    switch (c.getType()) {
        case CardType::ATTACK:
            useAttackCard();
            onUseAttackCard();
            break;

        case CardType::SKILL:
            useSkillCard();
            onUseSkillCard();
            if (player.hasStatus<PS::CORRUPTION>()) {
                item.exhaustOnUse = true;
            }
            break;

        case CardType::POWER:
            usePowerCard();
            onUsePowerCard();
            break;

        case CardType::STATUS:
        case CardType::CURSE:
            onUseStatusOrCurseCard();
            break;

        default:
            // unreachable
            break;
    }

    // ChokePower.onUseCard comes last in UseCardAction's constructor, with the other monster powers.
    for (int i = 0; i < monsters.monsterCount; ++i) {
        const auto &m = monsters.arr[i];
        if (m.hasStatus<MS::CHOKED>() && !m.isDeadOrEscaped()) {
            addToBot( Actions::MonsterLoseHp(i, m.getStatus<MS::CHOKED>()) );
        }
    }

    addToBot(Actions::OnAfterCardUsed());
    triggerOnOtherCardPlayed(c);

    if (!item.purgeOnUse) { // todo change to checking the card queue item
        cards.removeFromHandById(c.uniqueId);
        if (c.costForTurn > 0 && !c.isFreeToPlay(*this) && !item.autoplay && !(player.hasStatus<PS::CORRUPTION>() && c.getType() == CardType::SKILL)) {
            player.useEnergy(c.costForTurn);
        }
    }
}

void BattleContext::noteCardTypePlayed(CardType type) {
    player.previousCardTypePlayed = player.lastCardTypePlayed;
    player.lastCardTypePlayed = type;
}

void BattleContext::useNoTriggerCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    switch (c.id) {
        case CardId::BURN:
            addToTop( Actions::DamagePlayer(c.isUpgraded() ? 4 : 2, true));
            break;

        case CardId::DECAY:
            addToTop( Actions::DamagePlayer(2, true));
            break;

        case CardId::DOUBT:
            player.debuff<PS::WEAK>(1, true);
            break;

        case CardId::REGRET:
            addToTop( Actions::PlayerLoseHp(item.regretCardCount, true));
            break;

        case CardId::SHAME:
            // todo this is fixed just test -> this and doubt are bugged if you are already weak i think. because the apply power action does not create a new power and justapplied is not set to true
            player.debuff<PS::FRAIL>(1, true);
            break;


        default:
            // this can actually be called on any card now because of time warp power

//#ifdef sts_asserts
//            assert(false);
//#endif // sts_asserts
            break;
    }

    cards.removeFromHandById(c.uniqueId);
    addToBot(Actions::DiscardNoTriggerCard()); // todo what if havoc plays one of these
}

void BattleContext::useAttackCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    const auto t = item.target;
    const bool up = c.isUpgraded();

    // todo test vigor with multi attacks and necro/double tap
    switch (c.getId()) {
        case CardId::STRIKE_RED:
        case CardId::STRIKE_BLUE:
        case CardId::STRIKE_GREEN:
        case CardId::STRIKE_PURPLE: {
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 9 : 6)) );
            break;
        }

        case CardId::ANGER:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 8 : 6)) );
            addToBot( Actions::MakeTempCardInDiscard(c, 1) );  // makeStatEquivalentCopy keeps a changed cost
            break;

        case CardId::BASH:
            // technically calculate attack damage is called first, keep note if we optimize addToBot later
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 8)) );
            addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(t, up ? 3 : 2, false) );
            break;

        case CardId::BITE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 8 : 7)) );
            addToBot( Actions::HealPlayer(up ? 3 : 2) );
            break;

        case CardId::BODY_SLAM:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, player.block)) );
            break;

        case CardId::BLOOD_FOR_BLOOD:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 22 : 18)) );
            break;

        case CardId::BLUDGEON:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 42 : 32)) );
            break;

        case CardId::CARNAGE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 28 : 20)) );
            break;

        case CardId::CLASH:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 18 : 14)) );
            break;

        case CardId::CLEAVE:  {
            int baseDamage = (up ? 11 : 8) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::AttackAllEnemy(baseDamage) );
            break;
        }

        case CardId::CLOTHESLINE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 14 : 12)) );
            addToBot( Actions::DebuffEnemy<MS::WEAK>(t, up ? 3 : 2, false) );
            break;

        case CardId::DRAMATIC_ENTRANCE: {
            int baseDamage = (up ? 12 : 8) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::AttackAllEnemy(baseDamage) );
            break;
        }

        case CardId::DROPKICK:
            addToBot( Actions::DropkickAction(t, calculateCardDamage(c, t, up ? 8 : 5)) );
            break;

        case CardId::FEED:
            addToBot( Actions::FeedAction(t, calculateCardDamage(c, t, up ? 12 : 10), up) );
            break;

        case CardId::FIEND_FIRE:
            addToBot( Actions::FiendFireAction(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            break;

        case CardId::FLASH_OF_STEEL:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 6 : 3)) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::HAND_OF_GREED:
            addToBot( Actions::HandOfGreedAction(t, calculateCardDamage(c, t, up ? 25 : 20), up) );
            break;

        case CardId::HEADBUTT:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 12 : 9)) );
            addToBot( Actions::HeadbuttAction() );
            break;

        case CardId::HEAVY_BLADE: {
            const int dmg1 = 14 + ((up ? 4 : 2) * player.getStatus<PS::STRENGTH>());
            const int dmg2 = calculateCardDamage(c, t, dmg1);
            addToBot( Actions::AttackEnemy(t, dmg2) );
            break;
        }

        case CardId::HEMOKINESIS:
            //  attack enemy should recalculate damage, because we can lose hp and therefore gain strength before the dmg, need to test
           // actually no i dont think that is true ^^
            addToBot( Actions::PlayerLoseHp(2, true) );
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 20 : 15)) );
            break;

        case CardId::IMMOLATE: {
            int baseDamage = (up ? 28 : 21) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::AttackAllEnemy(baseDamage) );
            addToBot( Actions::MakeTempCardInDiscard({CardId::BURN}, 1) );
            break;
        }

        case CardId::IRON_WAVE: {
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 7 : 5)) );  // FIX: block modifiers were applied twice
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 7 : 5)) );
            break;
        }

        case CardId::MIND_BLAST: {
            // PlayTopCardAction runs applyPowers as it takes the card, while the cards after it are still in the pile
            const bool offTheTop = curCardQueueItem.autoplay && c.specialData > 0;
            const auto baseDamage = offTheTop ? c.specialData - 1 : static_cast<int>(cards.drawPile.size());
            const auto damage = calculateCardDamage(c, t, baseDamage);
            addToBot( Actions::AttackEnemy(t, damage) );
            break;
        }

        case CardId::PERFECTED_STRIKE: {
            // hack because we calculate strikeCount while non purge cards are still in hand.
            // A card played off the top of the draw pile is in limbo in the game, where countCards doesn't look.
            const int strikeDmg = (cards.strikeCount - (item.autoplay && !item.purgeOnUse ? 1 : 0)) * (up ? 3 : 2);
            const int baseDamage = 6 + strikeDmg;
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, baseDamage)) );
            break;
        }

        case CardId::POMMEL_STRIKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 9)) );
            addToBot( Actions::DrawCards(up  ? 2 : 1) );
            break;

        case CardId::PUMMEL: {
            const auto attackCount = up ? 5 : 4;
            const auto damage = calculateCardDamage(c, t, 2);
            for (int i = 0; i < attackCount; ++i) {
                addToBot( Actions::AttackEnemy(t, damage) );
            }
            break;
        }

        case CardId::RAMPAGE: { // todo doubled card damage is changed by the first
            const int damage = calculateCardDamage(c, t, 8+c.specialData);
            addToBot( Actions::AttackEnemy(t, damage) );

            if (item.purgeOnUse) {
                cards.findAndUpgradeSpecialData(c.uniqueId, up ? 8 : 5);
            }
            c.specialData += up ? 8 : 5;

            break;
        }

        case CardId::REAPER: {
            const auto baseDamage = (up ? 5 : 4) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::ReaperAction(baseDamage) );
            break;
        }

        case CardId::RECKLESS_CHARGE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            addToBot( Actions::MakeTempCardInDrawPile({CardId::DAZED}, 1, true) );
            break;

        case CardId::RITUAL_DAGGER:
            addToBot( Actions::RitualDaggerAction(t, calculateCardDamage(c, t, c.specialData)) );
            break;

        case CardId::SEARING_BLOW: {
            const int n = c.getUpgradeCount();
            const int baseDmg = n * (n+7) / 2 + 12;
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, baseDmg)) );
            break;
        }

        case CardId::SEVER_SOUL: // another example of damage being calculated after an action, maybe need a new action to calculate damage later...
            addToBot( Actions::SeverSoulExhaustAction() );
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 22 : 16)) );
            break;

        case CardId::SWIFT_STRIKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            break;

        case CardId::SWORD_BOOMERANG:
            for (int i = 0; i < (up ? 4 : 3); ++i) {
                addToBot(Actions::SwordBoomerangAction(3+player.getStatus<PS::VIGOR>())); // vigor is removed afterwards so this is a necessary (maybe not 100% accurate) hack
            }
            break;

        case CardId::THUNDERCLAP: {
            int baseDamage = (up ? 7 : 4) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::AttackAllEnemy(baseDamage));
            addToBot( Actions::DebuffAllEnemy<MS::VULNERABLE>(1, false) );
            break;
        }

        case CardId::TWIN_STRIKE: { // todo test with breaking block on first hit and drill relic
            const int dmg = calculateCardDamage(c, t, up ? 7 : 5);
            addToBot(Actions::AttackEnemy(t, dmg));
            addToBot(Actions::AttackEnemy(t, dmg));
            break;
        }

        case CardId::UPPERCUT:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, 13)) );
            addToBot( Actions::DebuffEnemy<MS::WEAK>(t, up ? 2 : 1, false) );
            addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(t, up ? 2 : 1, false) );
            break;

        case CardId::WHIRLWIND: {
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            const auto baseDamage = (up ? 8 : 5) + player.getStatus<PS::VIGOR>();
            addToBot( Actions::WhirlwindAction(baseDamage, item.energyOnUse, !(item.freeToPlay || c.freeToPlayOnce)));
            break;
        }

        case CardId::WILD_STRIKE:
            addToBot(Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 17 : 12)));
            addToBot( Actions::MakeTempCardInDrawPile( CardInstance(CardId::WOUND), 1, true) );
            break;


        // ************************* Defect, ported from Attemory/sts_lightspeed and checked against the game *************************

        case CardId::ALL_FOR_ONE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 14 : 10)) );
            addToBot( Actions::AllCostToHandAction(0) );
            break;

        case CardId::BALL_LIGHTNING:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            addToBot( Actions::ChannelOrb(Orb::LIGHTNING) );
            break;

        case CardId::BARRAGE:
            addToBot( Actions::BarrageAction(t, calculateCardDamage(c, t, up ? 6 : 4)) );
            break;

        case CardId::BEAM_CELL:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 4 : 3)) );
            addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(t, up ? 2 : 1, false) );
            break;

        case CardId::BLIZZARD:
            addToBot( Actions::AttackAllEnemy((up ? 3 : 2) * player.frostChanneled + player.getStatus<PS::VIGOR>()) );
            break;

        case CardId::BULLSEYE: // the game's Lock-On
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 11 : 8)) );
            addToBot( Actions::DebuffEnemy<MS::LOCK_ON>(t, up ? 3 : 2, false) );
            break;

        case CardId::CLAW:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, (up ? 5 : 3) + c.specialData)) );
            addToBot( Actions::GashAction(2) );
            break;

        case CardId::COLD_SNAP:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 9 : 6)) );
            addToBot( Actions::ChannelOrb(Orb::FROST) );
            break;

        case CardId::COMPILE_DRIVER:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            addToBot( Actions::CompileDriverAction(1) );
            break;

        case CardId::CORE_SURGE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 15 : 11)) );
            addToBot( Actions::BuffPlayer<PS::ARTIFACT>(1) );
            break;

        case CardId::DOOM_AND_GLOOM:
            addToBot( Actions::AttackAllEnemy((up ? 14 : 10) + player.getStatus<PS::VIGOR>()) );
            addToBot( Actions::ChannelOrb(Orb::DARK) );
            break;

        case CardId::FTL:
            addToBot( Actions::FtlAction(t, calculateCardDamage(c, t, up ? 6 : 5), up ? 4 : 3) );
            break;

        case CardId::GO_FOR_THE_EYES:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 4 : 3)) );
            addToBot( Actions::ForTheEyesAction(t, up ? 2 : 1) );
            break;

        case CardId::HYPERBEAM:
            addToBot( Actions::AttackAllEnemy((up ? 34 : 26) + player.getStatus<PS::VIGOR>()) );
            addToBot( Actions::DebuffPlayer<PS::FOCUS>(-3, false) );
            break;

        case CardId::MELTER:
            addToBot( Actions::RemoveAllBlock(t) );
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 14 : 10)) );
            break;

        case CardId::METEOR_STRIKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 30 : 24)) );
            addToBot( Actions::ChannelOrbs(Orb::PLASMA, 3) );
            break;

        case CardId::REBOUND:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 12 : 9)) );
            addToBot( Actions::BuffPlayer<PS::REBOUND>(1) );
            break;

        case CardId::RIP_AND_TEAR:
            addToBot( Actions::AttackRandomEnemy((up ? 9 : 7) + player.getStatus<PS::VIGOR>(), 2) );
            break;

        case CardId::SCRAPE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 7)) );
            addToBot( Actions::ScrapeAction(up ? 5 : 4) );
            break;

        case CardId::STREAMLINE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 20 : 15)) );
            addToBot( Actions::ReduceCostAction(c.getUniqueId()) );
            break;

        case CardId::SUNDER:
            addToBot( Actions::SunderAction(t, calculateCardDamage(c, t, up ? 32 : 24), 3) );
            break;

        case CardId::SWEEPING_BEAM:
            addToBot( Actions::AttackAllEnemy((up ? 9 : 6) + player.getStatus<PS::VIGOR>()) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::THUNDER_STRIKE:
            addToBot( Actions::AttackRandomEnemy((up ? 9 : 7) + player.getStatus<PS::VIGOR>(), player.lightningChanneled) );
            break;

        // ************************* Silent, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::ALL_OUT_ATTACK:
            addToBot( Actions::AttackAllEnemy(calculateMultiDamage(c, up ? 14 : 10)) );
            addToBot( Actions::DiscardAction(1, true) );
            break;

        case CardId::BACKSTAB:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 15 : 11)) );
            break;

        case CardId::BANE: {
            const int damage = calculateCardDamage(c, t, up ? 10 : 7);
            addToBot( Actions::AttackEnemy(t, damage) );
            addToBot( Actions::BaneAction(t, damage) );
            break;
        }

        case CardId::CHOKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, 12)) );
            addToBot( Actions::DebuffEnemy<MS::CHOKED>(t, up ? 5 : 3, false) );
            break;

        case CardId::DAGGER_SPRAY: { // both hits use the damage worked out when it's played
            const auto damage = calculateMultiDamage(c, up ? 6 : 4);
            addToBot( Actions::AttackAllEnemy(damage) );
            addToBot( Actions::AttackAllEnemy(damage) );
            break;
        }

        case CardId::DAGGER_THROW:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 12 : 9)) );
            addToBot( Actions::DrawCards(1) );
            addToBot( Actions::DiscardAction(1) );
            break;

        case CardId::DASH:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 13 : 10)) );
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 13 : 10)) );
            break;

        case CardId::DIE_DIE_DIE:
            addToBot( Actions::AttackAllEnemy(calculateMultiDamage(c, up ? 17 : 13)) );
            break;

        case CardId::ENDLESS_AGONY:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 6 : 4)) );
            break;

        case CardId::EVISCERATE: {
            const int damage = calculateCardDamage(c, t, up ? 9 : 7);
            for (int i = 0; i < 3; ++i) {
                addToBot( Actions::AttackEnemy(t, damage) );
            }
            break;
        }

        case CardId::FINISHER:
            addToBot( Actions::FinisherAction(t, calculateCardDamage(c, t, up ? 8 : 6)) );
            break;

        case CardId::FLECHETTES:
            addToBot( Actions::FlechetteAction(t, calculateCardDamage(c, t, up ? 6 : 4)) );
            break;

        case CardId::FLYING_KNEE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 11 : 8)) );
            addToBot( Actions::BuffPlayer<PS::ENERGIZED>(1) );
            break;

        case CardId::GLASS_KNIFE: {
            const int damage = calculateCardDamage(c, t, (up ? 12 : 8) - c.specialData);
            addToBot( Actions::AttackEnemy(t, damage) );
            addToBot( Actions::AttackEnemy(t, damage) );
            addToBot( Actions::GlassKnifeAction(c.getUniqueId()) );
            break;
        }

        case CardId::GRAND_FINALE:
            addToBot( Actions::AttackAllEnemy(calculateMultiDamage(c, up ? 60 : 50)) );
            break;

        case CardId::HEEL_HOOK:
            addToBot( Actions::HeelHookAction(t, calculateCardDamage(c, t, up ? 8 : 5)) );
            break;

        case CardId::MASTERFUL_STAB:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 16 : 12)) );
            break;

        case CardId::NEUTRALIZE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 4 : 3)) );
            addToBot( Actions::DebuffEnemy<MS::WEAK>(t, up ? 2 : 1, false) );
            break;

        case CardId::POISONED_STAB:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 8 : 6)) );
            addToBot( Actions::DebuffEnemy<MS::POISON>(t, up ? 4 : 3) );
            break;

        case CardId::PREDATOR:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 20 : 15)) );
            addToBot( Actions::BuffPlayer<PS::DRAW_CARD_NEXT_TURN>(2) );
            break;

        case CardId::QUICK_SLASH:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 12 : 8)) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::RIDDLE_WITH_HOLES: {
            const int damage = calculateCardDamage(c, t, up ? 4 : 3);
            for (int i = 0; i < 5; ++i) {
                addToBot( Actions::AttackEnemy(t, damage) );
            }
            break;
        }

        case CardId::SHIV: // Accuracy writes this into the card's baseDamage, wherever the card is
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, (up ? 6 : 4) + player.getStatus<PS::ACCURACY>())) );
            break;

        case CardId::SKEWER:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::SkewerAction(t, calculateCardDamage(c, t, up ? 10 : 7), item.energyOnUse,
                                            !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::SLICE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 9 : 6)) );
            break;

        case CardId::SNEAKY_STRIKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 16 : 12)) );
            addToBot( Actions::GainEnergyIfDiscard(2) );
            break;

        case CardId::SUCKER_PUNCH:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 9 : 7)) );
            addToBot( Actions::DebuffEnemy<MS::WEAK>(t, up ? 2 : 1, false) );
            break;

        case CardId::UNLOAD:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 18 : 14)) );
            addToBot( Actions::UnloadAction() );
            break;

        // ************************* Watcher, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::BOWLING_BASH: { // one hit for each monster alive when it's played
            int alive = 0;
            for (int i = 0; i < monsters.monsterCount; ++i) {
                if (!monsters.arr[i].isDeadOrEscaped()) {
                    ++alive;
                }
            }
            addToBot( Actions::AttackEnemyTimes(t, calculateCardDamage(c, t, up ? 10 : 7), alive) );
            break;
        }

        case CardId::BRILLIANCE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, (up ? 16 : 12) + player.mantraGained)) );
            break;

        case CardId::CARVE_REALITY:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 6)) );
            addToBot( Actions::MakeTempCardInHand(CardId::SMITE) );
            break;

        case CardId::CONCLUDE:
            addToBot( Actions::AttackAllEnemy(calculateMultiDamage(c, up ? 16 : 12)) );
            addToBot( Actions::PressEndTurnButton() );
            break;

        case CardId::CONSECRATE:
            addToBot( Actions::AttackAllEnemy(calculateMultiDamage(c, up ? 8 : 5)) );
            break;

        case CardId::CRUSH_JOINTS:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 8)) );
            addToBot( Actions::CrushJointsAction(t, up ? 2 : 1) );
            break;

        case CardId::CUT_THROUGH_FATE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 9 : 7)) );
            addToBot( Actions::ScryAction(up ? 3 : 2) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::EMPTY_FIST:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 14 : 9)) );
            addToBot( Actions::ChangeStance(Stance::NEUTRAL) );
            break;

        case CardId::ERUPTION:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, 9)) );
            addToBot( Actions::ChangeStance(Stance::WRATH) );
            break;

        case CardId::EXPUNGER:
            addToBot( Actions::AttackEnemyTimes(t, calculateCardDamage(c, t, up ? 15 : 9), c.specialData) );
            break;

        case CardId::FEAR_NO_EVIL:
            addToBot( Actions::FearNoEvilAction(t, calculateCardDamage(c, t, up ? 11 : 8)) );
            break;

        case CardId::FLURRY_OF_BLOWS:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 6 : 4)) );
            break;

        case CardId::FLYING_SLEEVES: {
            const int damage = calculateCardDamage(c, t, up ? 6 : 4);
            addToBot( Actions::AttackEnemy(t, damage) );
            addToBot( Actions::AttackEnemy(t, damage) );
            break;
        }

        case CardId::FOLLOW_UP:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 11 : 7)) );
            addToBot( Actions::FollowUpAction() );
            break;

        case CardId::JUST_LUCKY:
            addToBot( Actions::ScryAction(up ? 2 : 1) );
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 3 : 2)) );
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 4 : 3)) );
            break;

        case CardId::LESSON_LEARNED:
            addToBot( Actions::LessonLearnedAction(t, calculateCardDamage(c, t, up ? 13 : 10)) );
            break;

        case CardId::RAGNAROK: // AttackDamageRandomEnemyAction works its damage out when it runs, with Vigor still there
            addToBot( Actions::AttackRandomEnemy((up ? 6 : 5) + player.getStatus<PS::VIGOR>(), up ? 6 : 5) );
            break;

        case CardId::REACH_HEAVEN:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 15 : 10)) );
            addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::THROUGH_VIOLENCE), 1, true) );
            break;

        case CardId::SANDS_OF_TIME:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 26 : 20)) );
            break;

        case CardId::SASH_WHIP:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 10 : 8)) );
            addToBot( Actions::HeadStompAction(t, up ? 2 : 1) );
            break;

        case CardId::SIGNATURE_MOVE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 40 : 30)) );
            break;

        case CardId::SMITE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 16 : 12)) );
            break;

        case CardId::TALK_TO_THE_HAND:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 7 : 5)) );
            addToBot( Actions::DebuffEnemy<MS::BLOCK_RETURN>(t, up ? 3 : 2, false) );
            break;

        case CardId::TANTRUM: {
            const int damage = calculateCardDamage(c, t, 3);
            addToBot( Actions::AttackEnemyTimes(t, damage, up ? 4 : 3) );
            addToBot( Actions::ChangeStance(Stance::WRATH) );
            break;
        }

        case CardId::THROUGH_VIOLENCE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 30 : 20)) );
            break;

        case CardId::WALLOP:
            addToBot( Actions::WallopAction(t, calculateCardDamage(c, t, up ? 12 : 9)) );
            break;

        case CardId::WEAVE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 6 : 4)) );
            break;

        case CardId::WHEEL_KICK:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, up ? 20 : 15)) );
            addToBot( Actions::DrawCards(2) );
            break;

        case CardId::WINDMILL_STRIKE:
            addToBot( Actions::AttackEnemy(t, calculateCardDamage(c, t, (up ? 10 : 7) + c.specialData)) );
            break;

        default:
#ifdef sts_asserts
            std::cerr << "attempted to use unimplemented card: " << c.getName() << std::endl;
            assert(false);
#endif
            break;
    }
}

// todo a lot of things can be done immediately
void BattleContext::useSkillCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;
    const auto t = item.target;
    const bool up = c.isUpgraded();

    switch (c.getId()) {
        case CardId::DEFEND_RED:
        case CardId::DEFEND_BLUE:
        case CardId::DEFEND_GREEN:
        case CardId::DEFEND_PURPLE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 8 : 5)) );
            break;

        case CardId::ARMAMENTS:
            addToBot(Actions::GainBlock(calculateCardBlock(5)) );
            if (up) {
                addToBot(Actions::UpgradeAllCardsInHand());
            } else {
                addToBot(Actions::ArmamentsAction());
            }
            break;

        case CardId::APOTHEOSIS:
            addToBot( Actions::ApotheosisAction() );
            break;

        case CardId::APPARITION:
            addToBot( Actions::BuffPlayer<PS::INTANGIBLE>(1) );
            break;

        case CardId::BANDAGE_UP:
            addToBot( Actions::HealPlayer(up ? 6 : 4) );
            break;

        case CardId::BATTLE_TRANCE:
            addToBot( Actions::DrawCards(up ? 4 : 3) );
            addToBot( Actions::DebuffPlayer<PS::NO_DRAW>() );
            break;

        case CardId::BLIND:
            if (up) {
                addToBot( Actions::DebuffAllEnemy<MS::WEAK>(2, false) );
            } else {
                addToBot( Actions::DebuffEnemy<MS::WEAK>(t, 2, false) );
            }
            break;

        case CardId::BLOODLETTING:
            addToBot( Actions::PlayerLoseHp(3, true) );
            addToBot( Actions::GainEnergy(up ? 3 : 2) );
            break;

        case CardId::BURNING_PACT:
            addToBot( Actions::ChooseExhaustOne() );
            addToBot( Actions::DrawCards(up ? 3 : 2) );
            break;

        case CardId::CHRYSALIS:
            addToBot( Actions::PutRandomCardsInDrawPile(CardType::SKILL, up ? 5 : 3) );
            break;

        case CardId::DARK_SHACKLES:
            addToBot( Actions::DebuffEnemy<MS::STRENGTH>(t, up ? -15 : -9, false) );  // FIX: sign was positive (gave strength)
            if (!monsters.arr[t].hasStatus<MS::ARTIFACT>()) {  // FIX: condition was inverted (real game: GainStrengthPower only if no Artifact)
                addToBot( Actions::BuffEnemy<MS::SHACKLED>(t, up ? 15 : 9) );
            }
            break;

        case CardId::DEEP_BREATH:
            if (!cards.discardPile.empty()) {
                onShuffle();
                addToBot( Actions::EmptyDeckShuffle() );
                addToBot( Actions::ShuffleDrawPile() );
            }
            addToBot( Actions::DrawCards(up ? 2 : 1) );
            break;

        case CardId::DISARM:
            addToBot( Actions::DebuffEnemy<MS::STRENGTH>(t, up ? -3 : -2, false) );  // FIX: upgrade value
            break;

        case CardId::DISCOVERY:
            undefinedBehaviorEvoked = true;
            addToBot(Actions::DiscoveryAction(CardType::INVALID, 1));
            break;

        case CardId::DOUBLE_TAP:
            addToBot(Actions::BuffPlayer<PS::DOUBLE_TAP>(up ? 2 : 1));
            break;

        case CardId::DUAL_WIELD:
            addToBot( Actions::DualWieldAction(up ? 2 : 1) );
            break;

        case CardId::ENLIGHTENMENT:
            addToBot( Actions::EnlightenmentAction(up) );
            break;

        case CardId::ENTRENCH:
            addToBot(Actions::EntrenchAction());
            break;

        case CardId::EXHUME:
            addToBot( Actions::ExhumeAction() );
            break;

        case CardId::FINESSE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 4 : 2)) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::FORETHOUGHT:
            addToBot( Actions::ForethoughtAction(up) );
            break;

        case CardId::FLAME_BARRIER:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 16 : 12)) );
            addToBot( Actions::BuffPlayer<PS::FLAME_BARRIER>(up ? 6 : 4) );
            break;

        case CardId::FLEX:
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(up ? 4 : 2) );
            addToBot( Actions::DebuffPlayer<PS::LOSE_STRENGTH>(up ? 4 : 2) );
            break;

        case CardId::GHOSTLY_ARMOR:
            addToBot( Actions::GainBlock(calculateCardBlock(up  ?  13 : 10)) );
            break;

        case CardId::GOOD_INSTINCTS:
            addToBot( Actions::GainBlock(calculateCardBlock(up  ?  9 : 6)) );
            break;

        case CardId::HAVOC:
            addToBot( Actions::PlayTopCard(monsters.getRandomMonsterIdx(cardRandomRng, true), true) );
            break;

        case CardId::IMPATIENCE: {
            bool hasAttack = false;
            for (int i = 0; i < cards.cardsInHand; ++i) {
                if (cards.hand[i].getType() == CardType::ATTACK) {
                    hasAttack = true;  // FIX: was `false` -> card always drew
                    break;
                }
            }
            if (!hasAttack) {
                addToBot(Actions::DrawCards(up ? 3 : 2));
            }
            break;
        }

        case CardId::IMPERVIOUS:
            addToBot( Actions::GainBlock(calculateCardBlock(up  ?  40 : 30)) );
            break;

        case CardId::INFERNAL_BLADE:
            addToBot( Actions::InfernalBladeAction() );
            break;

        case CardId::INTIMIDATE:
            addToBot( Actions::DebuffAllEnemy<MS::WEAK>(up ? 2 : 1, false) ); // game justs adds one for each enemy in order
            break;

        case CardId::JACK_OF_ALL_TRADES: // the game decides the random cards here and adds a MakeTempCard for each
            addToBot( Actions::MakeTempCardInHand(getTrulyRandomColorlessCardInCombat(cardRandomRng)) );
            if (up) {
                addToBot( Actions::MakeTempCardInHand(getTrulyRandomColorlessCardInCombat(cardRandomRng)) );
            }
            break;

        case CardId::JAX:
            addToBot( Actions::PlayerLoseHp(3, true) );
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(up ? 3 : 2) );
            break;

        case CardId::LIMIT_BREAK:
            addToBot( Actions::LimitBreakAction() );
            break;

        case CardId::MADNESS:
            addToBot( Actions::MadnessAction() );
            break;

        case CardId::MASTER_OF_STRATEGY:
            addToBot( Actions::DrawCards(up ? 4 : 3) );
            break;

        case CardId::METAMORPHOSIS:
            addToBot( Actions::PutRandomCardsInDrawPile(CardType::ATTACK, up ? 5 : 3) );
            break;

        case CardId::OFFERING:
            addToBot( Actions::PlayerLoseHp(6, true) );
            addToBot( Actions::GainEnergy(2) );
            addToBot( Actions::DrawCards(up ? 5 : 3) );
            break;

        case CardId::PANACEA:
            addToBot( Actions::BuffPlayer<PS::ARTIFACT>(up ? 2 : 1) );
            break;

        case CardId::PANIC_BUTTON:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 40 : 30)) );
            addToBot( Actions::DebuffPlayer<PS::NO_BLOCK>(2) );
            break;

        case CardId::POWER_THROUGH:
            addToBot( Actions::MakeTempCardInHand(CardId::WOUND, false, 2) );
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 20 : 15)) );
            break;

        case CardId::PURITY:
            addToBot( Actions::ExhaustMany(up ? 5 : 3) );
            break;

        case CardId::RAGE:
            addToBot( Actions::BuffPlayer<PS::RAGE>(up ? 5 : 3) );
            break;

        case CardId::SECRET_TECHNIQUE:
            addToBot( Actions::DrawToHandAction(CardSelectTask::SECRET_TECHNIQUE, CardType::SKILL) );
            break;

        case CardId::SECRET_WEAPON:
            addToBot( Actions::DrawToHandAction(CardSelectTask::SECRET_WEAPON, CardType::ATTACK) );
            break;

        case CardId::SECOND_WIND:
            addToBot( Actions::SecondWindAction(calculateCardBlock(up ? 7 : 5)) );
            break;

        case CardId::SEEING_RED:
            addToBot( Actions::GainEnergy(2) );
            break;

        case CardId::SENTINEL:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 8 : 5)) );
            break;

        case CardId::SHOCKWAVE:
            addToBot( Actions::DebuffAllEnemy<MS::WEAK>(up ? 5 : 3, false) );
            addToBot( Actions::DebuffAllEnemy<MS::VULNERABLE>(up ? 5 : 3, false) );
            break;

        case CardId::SHRUG_IT_OFF:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 11 : 8)) );
            addToBot( Actions::DrawCards(1) );
            break;

        case CardId::SPOT_WEAKNESS:
            addToBot( Actions::SpotWeaknessAction(t, up ? 4 : 3) );
            break;

        case CardId::THE_BOMB:
            addToBot( Actions::BuffPlayer<PS::THE_BOMB>(up ? 50 : 40) );
            break;

        case CardId::THINKING_AHEAD: // same as upgraded warcry
            addToBot( Actions::DrawCards(2) );
            addToBot( Actions::WarcryAction() );
            break;

        case CardId::TRANSMUTATION:
            if (player.energy > item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }

            addToBot( Actions::TransmutationAction(up, item.energyOnUse, !(item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::TRIP: // maybe fixed --- todo this doesn't work properly because it only requires a target when not upgraded, also the trip card doesn't uses its own implementation of debuff all enemy
            if (up) {
                addToBot( Actions::DebuffAllEnemy<MS::VULNERABLE>(2, false) );
            } else {
                addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(t, 2, false) );
            }
            break;

        case CardId::TRUE_GRIT:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 9 : 7)) );
            if (up) {
                addToBot( Actions::ChooseExhaustOne() );
            } else {
                addToBot( Actions::ExhaustRandomCardInHand(1) );
            }
            break;

        case CardId::VIOLENCE:
            addToBot( Actions::ViolenceAction(up ? 4 : 3) );
            break;

        case CardId::WARCRY:
            addToBot( Actions::DrawCards(up ? 2 : 1) );
            addToBot( Actions::WarcryAction() );
            break;

        // ************************* Defect, ported from Attemory/sts_lightspeed and checked against the game *************************

        case CardId::AGGREGATE: {
            const int divide = up ? 3 : 4;
            addToBot( {[=] (BattleContext &bc) {
                bc.player.gainEnergy(static_cast<int>(bc.cards.drawPile.size()) / divide);
            }} );
            break;
        }

        case CardId::AMPLIFY:
            addToBot( Actions::BuffPlayer<PS::AMPLIFY>(up ? 2 : 1) );
            break;

        case CardId::AUTO_SHIELDS:
            if (player.block == 0) { // checked when played, not when the action runs
                addToBot( Actions::GainBlock(calculateCardBlock(up ? 15 : 11)) );
            }
            break;

        case CardId::BOOT_SEQUENCE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 13 : 10)) );
            break;

        case CardId::CHAOS: { // AbstractOrb.getRandomOrb, rolled when the card is played
            static constexpr Orb orbList[] {Orb::DARK, Orb::FROST, Orb::LIGHTNING, Orb::PLASMA};
            if (up) {
                addToBot( Actions::ChannelOrb(orbList[cardRandomRng.random(3)]) );
            }
            addToBot( Actions::ChannelOrb(orbList[cardRandomRng.random(3)]) );
            break;
        }

        case CardId::CHARGE_BATTERY: // the game's Conserve Battery
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 10 : 7)) );
            addToBot( Actions::BuffPlayer<PS::ENERGIZED_BLUE>(1) );
            break;

        case CardId::CHILL:
            for (int i = 0; i < monsters.monsterCount; ++i) {
                if (!monsters.arr[i].isDeadOrEscaped()) {
                    addToBot( Actions::ChannelOrb(Orb::FROST) );
                }
            }
            break;

        case CardId::CONSUME:
            addToBot( Actions::BuffPlayer<PS::FOCUS>(up ? 3 : 2) );
            addToBot( Actions::DecreaseOrbSlots(1) );
            break;

        case CardId::COOLHEADED:
            addToBot( Actions::ChannelOrb(Orb::FROST) );
            addToBot( Actions::DrawCards(up ? 2 : 1) );
            break;

        case CardId::DARKNESS:
            addToBot( Actions::ChannelOrb(Orb::DARK) );
            if (up) {
                addToBot( Actions::DarkImpulseAction() );
            }
            break;

        case CardId::DOUBLE_ENERGY:
            addToBot( {[] (BattleContext &bc) {
                bc.player.gainEnergy(bc.player.energy);
            }} );
            break;

        case CardId::DUALCAST:
            addToBot( Actions::EvokeWithoutRemovingOrb() );
            addToBot( Actions::EvokeOrb() );
            break;

        case CardId::EQUILIBRIUM:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 16 : 13)) );
            addToBot( Actions::BuffPlayer<PS::EQUILIBRIUM>(1) );
            break;

        case CardId::FISSION:
            addToBot( Actions::FissionAction(up) );
            break;

        case CardId::FORCE_FIELD:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 16 : 12)) );
            break;

        case CardId::FUSION:
            addToBot( Actions::ChannelOrb(Orb::PLASMA) );
            break;

        case CardId::GENETIC_ALGORITHM: // the block is taken from before the increase, which an upgrade makes 3
            addToBot( Actions::IncreaseMiscAction(c.getUniqueId(), up ? 3 : 2) );
            addToBot( Actions::GainBlock(calculateCardBlock(c.specialData)) );
            break;

        case CardId::GLACIER:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 10 : 7)) );
            addToBot( Actions::ChannelOrbs(Orb::FROST, 2) );
            break;

        case CardId::HOLOGRAM:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 5 : 3)) );
            addToBot( Actions::BetterDiscardPileToHandAction(1, CardSelectTask::HOLOGRAM) );
            break;

        case CardId::LEAP:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 12 : 9)) );
            break;

        case CardId::MULTI_CAST:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::MulticastAction(item.energyOnUse, up, !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::OVERCLOCK: // the game's Steam Power
            addToBot( Actions::DrawCards(up ? 3 : 2) );
            addToBot( Actions::MakeTempCardInDiscard(CardInstance(CardId::BURN)) );
            break;

        case CardId::RAINBOW:
            addToBot( Actions::ChannelOrb(Orb::LIGHTNING) );
            addToBot( Actions::ChannelOrb(Orb::FROST) );
            addToBot( Actions::ChannelOrb(Orb::DARK) );
            break;

        case CardId::REBOOT:
            onShuffle(); // ShuffleAllAction's constructor
            addToBot( Actions::RebootShuffleAll() );
            addToBot( Actions::ShuffleDrawPile() );
            addToBot( Actions::DrawCards(up ? 6 : 4) );
            break;

        case CardId::RECURSION:
            addToBot( Actions::RecursionAction() );
            break;

        case CardId::RECYCLE:
            addToBot( Actions::RecycleAction() );
            break;

        case CardId::REINFORCED_BODY:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::ReinforcedBodyAction(calculateCardBlock(up ? 9 : 7), item.energyOnUse,
                                                    !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::REPROGRAM:
            addToBot( Actions::DebuffPlayer<PS::FOCUS>(up ? -2 : -1, false) );
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(up ? 2 : 1) );
            addToBot( Actions::BuffPlayer<PS::DEXTERITY>(up ? 2 : 1) );
            break;

        case CardId::SEEK:
            addToBot( Actions::SeekAction(up ? 2 : 1) );
            break;

        case CardId::SKIM:
            addToBot( Actions::DrawCards(up ? 4 : 3) );
            break;

        case CardId::STACK: { // Stack.applyPowers works its base block out from the discard pile
            // PlayTopCardAction runs applyPowers as it takes the card, before the cards taken ahead of it have been played
            const bool offTheTop = curCardQueueItem.autoplay && c.specialData > 0;
            const int discarded = offTheTop ? c.specialData - 1 : static_cast<int>(cards.discardPile.size());
            addToBot( Actions::GainBlock(calculateCardBlock(discarded + (up ? 3 : 0))) );
            break;
        }

        case CardId::STEAM_BARRIER: // ModifyBlockAction takes one from baseBlock, which can go below 0 and then Dexterity starts from there
            addToBot( Actions::GainBlock(calculateCardBlock((up ? 8 : 6) - c.specialData)) );
            addToBot( Actions::ModifyBlockAction(c.getUniqueId()) );
            break;

        case CardId::TEMPEST:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::TempestAction(item.energyOnUse, up, !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::TURBO:
            addToBot( Actions::GainEnergy(up ? 3 : 2) );
            addToBot( Actions::MakeTempCardInDiscard(CardInstance(CardId::VOID)) );
            break;

        case CardId::WHITE_NOISE: { // rolled when the card is played, from the class's own pool as for every card made in combat
            CardInstance power(getTrulyRandomCardInCombat(cardRandomRng, player.cc, CardType::POWER));
            power.setCostForTurn(0);
            addToBot( Actions::MakeTempCardInHand(power) );
            break;
        }

        case CardId::ZAP:
            addToBot( Actions::ChannelOrb(Orb::LIGHTNING) );
            break;

        // ************************* Silent, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::ACROBATICS:
            addToBot( Actions::DrawCards(up ? 4 : 3) );
            addToBot( Actions::DiscardAction(1) );
            break;

        case CardId::ADRENALINE:
            addToBot( Actions::GainEnergy(up ? 2 : 1) );
            addToBot( Actions::DrawCards(2) );
            break;

        case CardId::ALCHEMIZE: // rolled when the card is played
            addToBot( Actions::ObtainPotion(returnRandomPotion(potionRng, player.cc, true)) );
            break;

        case CardId::BACKFLIP:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 8 : 5)) );
            addToBot( Actions::DrawCards(2) );
            break;

        case CardId::BLADE_DANCE:
            addToBot( Actions::MakeTempCardInHand(CardId::SHIV, false, up ? 4 : 3) );
            break;

        case CardId::BLUR:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 8 : 5)) );
            addToBot( Actions::BuffPlayer<PS::BLUR>(1) );
            break;

        case CardId::BOUNCING_FLASK: // the first target is rolled when the card is played
            addToBot( Actions::BouncingFlaskAction(monsters.getRandomMonsterIdx(cardRandomRng, true), 3, up ? 4 : 3) );
            break;

        case CardId::BULLET_TIME:
            addToBot( Actions::DebuffPlayer<PS::NO_DRAW>(1, false) );
            addToBot( Actions::BulletTimeAction() );
            break;

        case CardId::BURST:
            addToBot( Actions::BuffPlayer<PS::BURST>(up ? 2 : 1) );
            break;

        case CardId::CALCULATED_GAMBLE:
            addToBot( Actions::CalculatedGambleAction() );
            break;

        case CardId::CATALYST:
            addToBot( Actions::CatalystAction(t, up ? 3 : 2) );
            break;

        case CardId::CLOAK_AND_DAGGER:
            addToBot( Actions::GainBlock(calculateCardBlock(6)) );
            addToBot( Actions::MakeTempCardInHand(CardId::SHIV, false, up ? 2 : 1) );
            break;

        case CardId::CONCENTRATE:
            addToBot( Actions::DiscardAction(up ? 2 : 3) );
            addToBot( Actions::GainEnergy(2) );
            break;

        case CardId::CORPSE_EXPLOSION:
            addToBot( Actions::DebuffEnemy<MS::POISON>(t, up ? 9 : 6) );
            addToBot( Actions::DebuffEnemy<MS::CORPSE_EXPLOSION>(t, 1) );
            break;

        case CardId::CRIPPLING_CLOUD: // the game's Crippling Poison
            if (!monsters.areMonstersBasicallyDead()) {
                for (int i = 0; i < monsters.monsterCount; ++i) {
                    if (!monsters.arr[i].isDying()) {
                        addToBot( Actions::DebuffEnemy<MS::POISON>(i, up ? 7 : 4) );
                        addToBot( Actions::DebuffEnemy<MS::WEAK>(i, 2, false) );
                    }
                }
            }
            break;

        case CardId::DEADLY_POISON:
            addToBot( Actions::DebuffEnemy<MS::POISON>(t, up ? 7 : 5) );
            break;

        case CardId::DEFLECT:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 7 : 4)) );
            break;

        case CardId::DISTRACTION: { // rolled when the card is played, from the class's own pool as for every card made in combat
            CardInstance skill(getTrulyRandomCardInCombat(cardRandomRng, player.cc, CardType::SKILL));
            skill.setCostForTurn(0);
            addToBot( Actions::MakeTempCardInHand(skill) );
            break;
        }

        case CardId::DODGE_AND_ROLL: {
            const int block = calculateCardBlock(up ? 6 : 4);
            addToBot( Actions::GainBlock(block) );
            addToBot( Actions::BuffPlayer<PS::NEXT_TURN_BLOCK>(block) );
            break;
        }

        case CardId::DOPPELGANGER:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::DoppelgangerAction(item.energyOnUse, up, !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::ESCAPE_PLAN:
            addToBot( Actions::EscapePlanDraw(calculateCardBlock(up ? 5 : 3)) );
            break;

        case CardId::EXPERTISE:
            addToBot( Actions::ExpertiseAction(up ? 7 : 6) );
            break;

        case CardId::LEG_SWEEP:
            addToBot( Actions::DebuffEnemy<MS::WEAK>(t, up ? 3 : 2, false) );
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 14 : 11)) );
            break;

        case CardId::MALAISE:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::MalaiseAction(t, item.energyOnUse, up, !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::NIGHTMARE:
            addToBot( Actions::NightmareAction() );
            break;

        case CardId::OUTMANEUVER:
            addToBot( Actions::BuffPlayer<PS::ENERGIZED>(up ? 3 : 2) );
            break;

        case CardId::PHANTASMAL_KILLER:
            addToBot( Actions::BuffPlayer<PS::PHANTASMAL>(1) );
            break;

        case CardId::PIERCING_WAIL: { // Artifact is looked at when the card is played, before any of this has used it
            const int amount = up ? 8 : 6;
            for (int i = 0; i < monsters.monsterCount; ++i) {
                addToBot( Actions::DebuffEnemy<MS::STRENGTH>(i, -amount, false) );
            }
            for (int i = 0; i < monsters.monsterCount; ++i) {
                if (!monsters.arr[i].hasStatus<MS::ARTIFACT>()) {
                    addToBot( Actions::BuffEnemy<MS::SHACKLED>(i, amount) );
                }
            }
            break;
        }

        case CardId::PREPARED:
            addToBot( Actions::DrawCards(up ? 2 : 1) );
            addToBot( Actions::DiscardAction(up ? 2 : 1) );
            break;

        case CardId::REFLEX: // can't be played, but the game's use is there
            addToBot( Actions::DrawCards(up ? 3 : 2) );
            break;

        case CardId::SETUP:
            addToBot( Actions::SetupAction() );
            break;

        case CardId::STORM_OF_STEEL:
            addToBot( Actions::StormOfSteelAction(up) );
            break;

        case CardId::SURVIVOR:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 11 : 8)) );
            addToBot( Actions::DiscardAction(1) );
            break;

        case CardId::TACTICIAN: // can't be played, and its use is empty
            break;

        case CardId::TERROR:
            addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(t, 99, false) );
            break;

        // ************************* Watcher, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::ALPHA:
            addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::BETA), 1, true) );
            break;

        case CardId::BETA:
            addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::OMEGA), 1, true) );
            break;

        case CardId::BLASPHEMY:
            addToBot( Actions::ChangeStance(Stance::DIVINITY) );
            addToBot( Actions::EndTurnDeathAction() );
            break;

        case CardId::COLLECT:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::CollectAction(item.energyOnUse + (up ? 1 : 0), !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::CONJURE_BLADE:
            if (!item.ignoreEnergyTotal && player.energy < item.energyOnUse) {
                item.energyOnUse = player.energy;
            }
            addToBot( Actions::ConjureBladeAction(item.energyOnUse + (up ? 1 : 0), !(item.autoplay || item.freeToPlay || c.freeToPlayOnce)) );
            break;

        case CardId::CRESCENDO:
            addToBot( Actions::ChangeStance(Stance::WRATH) );
            break;

        case CardId::DECEIVE_REALITY:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 7 : 4)) );
            addToBot( Actions::MakeTempCardInHand(CardId::SAFETY) );
            break;

        case CardId::DEUS_EX_MACHINA: // can't be played, and its use is empty
            break;

        case CardId::EMPTY_BODY:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 10 : 7)) );
            addToBot( Actions::ChangeStance(Stance::NEUTRAL) );
            break;

        case CardId::EMPTY_MIND:
            addToBot( Actions::DrawCards(up ? 3 : 2) );
            addToBot( Actions::ChangeStance(Stance::NEUTRAL) );
            break;

        case CardId::EVALUATE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 10 : 6)) );
            addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::INSIGHT), 1, true) );
            break;

        case CardId::FOREIGN_INFLUENCE:
            addToBot( Actions::ForeignInfluenceAction(up) );
            break;

        case CardId::HALT: { // Halt.applyPowers runs Dexterity and Frail on the two amounts apart
            addToBot( Actions::HaltAction(calculateCardBlock(up ? 4 : 3), calculateCardBlock(up ? 14 : 9)) );
            break;
        }

        case CardId::INDIGNATION:
            addToBot( Actions::IndignationAction(up ? 5 : 3) );
            break;

        case CardId::INNER_PEACE:
            addToBot( Actions::InnerPeaceAction(up ? 4 : 3) );
            break;

        case CardId::INSIGHT:
            addToBot( Actions::DrawCards(up ? 3 : 2) );
            break;

        case CardId::JUDGMENT:
            addToBot( Actions::JudgementAction(t, up ? 40 : 30) );
            break;

        case CardId::MEDITATE:
            addToBot( Actions::MeditateAction(up ? 2 : 1) );
            addToBot( Actions::ChangeStance(Stance::CALM) );
            addToBot( Actions::PressEndTurnButton() );
            break;

        case CardId::MIRACLE:
            addToBot( Actions::GainEnergy(up ? 2 : 1) );
            break;

        case CardId::OMNISCIENCE:
            addToBot( Actions::OmniscienceAction(2) );
            break;

        case CardId::PERSEVERANCE:
            addToBot( Actions::GainBlock(calculateCardBlock((up ? 7 : 5) + c.specialData)) );
            break;

        case CardId::PRAY:
            queueMantra(up ? 4 : 3);
            addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::INSIGHT), 1, true) );
            break;

        case CardId::PRESSURE_POINTS:
            addToBot( Actions::DebuffEnemy<MS::MARK>(t, up ? 11 : 8, false) );
            addToBot( Actions::TriggerMarksAction() );
            break;

        case CardId::PROSTRATE:
            queueMantra(up ? 3 : 2);
            addToBot( Actions::GainBlock(calculateCardBlock(4)) );
            break;

        case CardId::PROTECT:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 16 : 12)) );
            break;

        case CardId::SAFETY:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 16 : 12)) );
            break;

        case CardId::SANCTITY:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 9 : 6)) );
            addToBot( Actions::SanctityAction(2) );
            break;

        case CardId::SCRAWL:
            addToBot( Actions::ExpertiseAction(10) );
            break;

        case CardId::SIMMERING_FURY:
            addToBot( Actions::BuffPlayer<PS::WRATH_NEXT_TURN>(1) );
            addToBot( Actions::BuffPlayer<PS::DRAW_CARD_NEXT_TURN>(up ? 3 : 2) );
            break;

        case CardId::SPIRIT_SHIELD: { // every other card in hand. A copy's original has already left the hand.
            int others = 0;
            for (int i = 0; i < cards.cardsInHand; ++i) {
                if (cards.hand[i].getUniqueId() != c.getUniqueId()) {
                    ++others;
                }
            }
            addToBot( Actions::GainBlock(calculateCardBlock(others * (up ? 4 : 3))) );
            break;
        }

        case CardId::SWIVEL:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 11 : 8)) );
            addToBot( Actions::BuffPlayer<PS::FREE_ATTACK_POWER>(1) );
            break;

        case CardId::THIRD_EYE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 9 : 7)) );
            addToBot( Actions::ScryAction(up ? 5 : 3) );
            break;

        case CardId::TRANQUILITY:
            addToBot( Actions::ChangeStance(Stance::CALM) );
            break;

        case CardId::VAULT:
            addToBot( Actions::SkipEnemiesTurn() );
            addToBot( Actions::PressEndTurnButton() );
            break;

        case CardId::VIGILANCE:
            addToBot( Actions::GainBlock(calculateCardBlock(up ? 12 : 8)) );
            addToBot( Actions::ChangeStance(Stance::CALM) );
            break;

        case CardId::WAVE_OF_THE_HAND:
            addToBot( Actions::BuffPlayer<PS::WAVE_OF_THE_HAND>(up ? 2 : 1) );
            break;

        case CardId::WISH:
            addToBot( Actions::WishAction(up) );
            break;

        case CardId::WORSHIP:
            queueMantra(5);
            break;

        case CardId::WREATH_OF_FLAME:
            addToBot( Actions::BuffPlayer<PS::VIGOR>(up ? 8 : 5) );
            break;

        default:
#ifdef sts_asserts
            std::cerr << "attempted to use unimplemented card: " << c.getName() << std::endl;
            assert(false);
#endif
            break;
    }
}

void BattleContext::usePowerCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    const auto t = item.target;
    const bool up = c.isUpgraded();

    switch (c.getId()) {

        case CardId::BARRICADE:
            player.setHasStatus<PS::BARRICADE>(true);
            break;

        case CardId::BERSERK:
            ++player.energyPerTurn;
            addToBot( Actions::DebuffPlayer<PS::VULNERABLE>(up ? 1 : 2, false) );
            break;

        case CardId::BRUTALITY:
            addToBot( Actions::BuffPlayer<PS::BRUTALITY>(1) );
            break;

        case CardId::CORRUPTION:
            // ApplyPowerAction's constructor changes the costs, so Mummified Hand, which runs after use, already sees them.
            cards.onBuffCorruption();
            addToBot( Actions::BuffPlayer<PS::CORRUPTION>() );
            break;

        case CardId::COMBUST:
            if (player.hasStatus<PS::NO_DRAW>() && !player.hasStatus<PS::COMBUST>()) {
                player.noDrawBeforeCombust = true;
            }
            addToBot( Actions::BuffPlayer<PS::COMBUST>(up ? 7 : 5) );
            break;

        case CardId::DEMON_FORM:
            addToBot( Actions::BuffPlayer<PS::DEMON_FORM>(up ? 3 : 2) );
            break;

        case CardId::DARK_EMBRACE:
            addToBot( Actions::BuffPlayer<PS::DARK_EMBRACE>(1) );
            break;

        case CardId::EVOLVE:
            addToBot( Actions::BuffPlayer<PS::EVOLVE>(up ? 2 : 1) );
            break;

        case CardId::FEEL_NO_PAIN:
            addToBot( Actions::BuffPlayer<PS::FEEL_NO_PAIN>(up ? 4 : 3) );
            break;

        case CardId::FIRE_BREATHING:
            addToBot( Actions::BuffPlayer<PS::FIRE_BREATHING>(up ? 10 : 6) );
            break;

        case CardId::INFLAME:
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(up ? 3 : 2) );
            break;

        case CardId::JUGGERNAUT:
            addToBot( Actions::BuffPlayer<PS::JUGGERNAUT>(up ? 7 : 5) );
            break;

        case CardId::MAGNETISM:
            addToBot( Actions::BuffPlayer<PS::MAGNETISM>(1) );
            break;

        case CardId::MAYHEM:
            addToBot( Actions::BuffPlayer<PS::MAYHEM>(1) );
            break;

        case CardId::METALLICIZE:
            addToBot( Actions::BuffPlayer<PS::METALLICIZE>(up ? 4 : 3) );
            break;

        case CardId::PANACHE:
            addToBot( Actions::BuffPlayer<PS::PANACHE>(up ? 14 : 10) );
            break;

        case CardId::RUPTURE:
            addToBot(Actions::BuffPlayer<PS::RUPTURE>(up ? 2 : 1));
            break;

        case CardId::SADISTIC_NATURE:
            addToBot( Actions::BuffPlayer<PS::SADISTIC>(up ? 7 : 5) );
            break;

        case CardId::WRAITH_FORM:
            addToBot( Actions::BuffPlayer<PS::INTANGIBLE>(up ? 3 : 2) );
            addToBot( Actions::DebuffPlayer<PS::WRAITH_FORM>(-1, false) );  // its amount is the Dexterity it takes each turn
            break;

        // ************************* Defect, ported from Attemory/sts_lightspeed and checked against the game *************************

        case CardId::BIASED_COGNITION:
            addToBot( Actions::BuffPlayer<PS::FOCUS>(up ? 5 : 4) );
            addToBot( Actions::DebuffPlayer<PS::BIAS>(1, false) );
            break;

        case CardId::BUFFER:
            addToBot( Actions::BuffPlayer<PS::BUFFER>(up ? 2 : 1) );
            break;

        case CardId::CAPACITOR:
            addToBot( Actions::IncreaseOrbSlots(up ? 3 : 2) );
            break;

        case CardId::CREATIVE_AI:
            addToBot( Actions::BuffPlayer<PS::CREATIVE_AI>(1) );
            break;

        case CardId::DEFRAGMENT:
            addToBot( Actions::BuffPlayer<PS::FOCUS>(up ? 2 : 1) );
            break;

        case CardId::ECHO_FORM:
            addToBot( Actions::BuffPlayer<PS::ECHO_FORM>(1) );
            break;

        case CardId::ELECTRODYNAMICS:
            if (!player.hasStatus<PS::ELECTRO>()) {
                addToBot( Actions::BuffPlayer<PS::ELECTRO>(1) );
            }
            addToBot( Actions::ChannelOrbs(Orb::LIGHTNING, up ? 3 : 2) );
            break;

        case CardId::HEATSINKS:
            addToBot( Actions::BuffPlayer<PS::HEATSINK>(up ? 2 : 1) );
            break;

        case CardId::HELLO_WORLD:
            addToBot( Actions::BuffPlayer<PS::HELLO_WORLD>(1) );
            break;

        case CardId::LOOP:
            addToBot( Actions::BuffPlayer<PS::LOOP>(up ? 2 : 1) );
            break;

        case CardId::MACHINE_LEARNING:
            addToBot( Actions::BuffPlayer<PS::MACHINE_LEARNING>(1) );
            break;

        case CardId::SELF_REPAIR:
            addToBot( Actions::BuffPlayer<PS::SELF_REPAIR>(up ? 10 : 7) );
            break;

        case CardId::STATIC_DISCHARGE:
            addToBot( Actions::BuffPlayer<PS::STATIC_DISCHARGE>(up ? 2 : 1) );
            break;

        case CardId::STORM:
            addToBot( Actions::BuffPlayer<PS::STORM>(1) );
            break;

        // ************************* Silent, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::A_THOUSAND_CUTS:
            addToBot( Actions::BuffPlayer<PS::THOUSAND_CUTS>(up ? 2 : 1) );
            break;

        case CardId::ACCURACY:
            addToBot( Actions::BuffPlayer<PS::ACCURACY>(up ? 6 : 4) );
            break;

        case CardId::AFTER_IMAGE:
            addToBot( Actions::BuffPlayer<PS::AFTER_IMAGE>(1) );
            break;

        case CardId::CALTROPS:
            addToBot( Actions::BuffPlayer<PS::THORNS>(up ? 5 : 3) );
            break;

        case CardId::ENVENOM:
            addToBot( Actions::BuffPlayer<PS::ENVENOM>(1) );
            break;

        case CardId::FOOTWORK:
            addToBot( Actions::BuffPlayer<PS::DEXTERITY>(up ? 3 : 2) );
            break;

        case CardId::INFINITE_BLADES:
            addToBot( Actions::BuffPlayer<PS::INFINITE_BLADES>(1) );
            break;

        case CardId::NOXIOUS_FUMES:
            addToBot( Actions::BuffPlayer<PS::NOXIOUS_FUMES>(up ? 3 : 2) );
            break;

        case CardId::TOOLS_OF_THE_TRADE:
            addToBot( Actions::BuffPlayer<PS::TOOLS_OF_THE_TRADE>(1) );
            break;

        case CardId::WELL_LAID_PLANS:
            addToBot( Actions::BuffPlayer<PS::RETAIN_CARDS>(up ? 2 : 1) );
            break;

        // ************************* Watcher, ported with hewzhew/SpireSim as a reference and checked against the game *************************

        case CardId::BATTLE_HYMN:
            addToBot( Actions::BuffPlayer<PS::BATTLE_HYMN>(1) );
            break;

        case CardId::DEVA_FORM:
            addToBot( Actions::DevaFormAction() );
            break;

        case CardId::DEVOTION:
            addToBot( Actions::BuffPlayer<PS::DEVOTION>(up ? 3 : 2) );
            break;

        case CardId::ESTABLISHMENT:
            addToBot( Actions::BuffPlayer<PS::ESTABLISHMENT>(1) );
            break;

        case CardId::FASTING: // EnergyDownPower is a debuff, so Artifact stops it and not the two buffs
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(up ? 4 : 3) );
            addToBot( Actions::BuffPlayer<PS::DEXTERITY>(up ? 4 : 3) );
            addToBot( Actions::DebuffPlayer<PS::FASTING>(1, false) );
            break;

        case CardId::FORESIGHT:
            addToBot( Actions::BuffPlayer<PS::FORESIGHT>(up ? 4 : 3) );
            break;

        case CardId::LIKE_WATER:
            addToBot( Actions::BuffPlayer<PS::LIKE_WATER>(up ? 7 : 5) );
            break;

        case CardId::MASTER_REALITY:
            addToBot( Actions::BuffPlayer<PS::MASTER_REALITY>(1) );
            break;

        case CardId::MENTAL_FORTRESS:
            addToBot( Actions::BuffPlayer<PS::MENTAL_FORTRESS>(up ? 6 : 4) );
            break;

        case CardId::NIRVANA:
            addToBot( Actions::BuffPlayer<PS::NIRVANA>(up ? 4 : 3) );
            break;

        case CardId::OMEGA:
            addToBot( Actions::BuffPlayer<PS::OMEGA>(up ? 60 : 50) );
            break;

        case CardId::RUSHDOWN:
            addToBot( Actions::BuffPlayer<PS::RUSHDOWN>(2) );
            break;

        case CardId::STUDY:
            addToBot( Actions::BuffPlayer<PS::STUDY>(1) );
            break;

        default:
#ifdef sts_asserts
            std::cerr << "attempted to use unimplemented card: " << c.getName() << std::endl;
            assert(false);
#endif
            break;
    }

}


void BattleContext::onUseAttackCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    auto &p = player;
    ++p.attacksPlayedThisTurn;

    // ********* Powers onUseCard *********

    if (p.hasStatus<PS::AFTER_IMAGE>()) {
        addToBot(Actions::GainBlock(p.getStatus<PS::AFTER_IMAGE>()));
    }

    if (!item.purgeOnUse && p.hasStatus<PS::DOUBLE_TAP>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::DOUBLE_TAP>();
    }

    if (!item.purgeOnUse && p.hasStatus<PS::DUPLICATION>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::DUPLICATION>();
    }

    const auto echoForm = p.getStatus<PS::ECHO_FORM>();
    if (!item.purgeOnUse && echoForm) {
        const bool echoFormActive = player.cardsPlayedThisTurn - player.echoFormCardsDoubled <= echoForm;
        if (echoFormActive) {
            ++player.echoFormCardsDoubled;
            queuePurgeCard(c, item.target);
        }
    }

    if (p.hasStatus<PS::PANACHE>() && --p.panacheCounter <= 0) {
        p.panacheCounter = 5;
        addToBot( Actions::DamageAllEnemy(p.getStatus<PS::PANACHE>()) );
    }

    if (p.hasStatus<PS::RAGE>()) {
        addToBot( Actions::GainBlock(p.getStatus<PS::RAGE>()) );
    }

    if (p.hasStatus<PS::VIGOR>()) {
        p.removeStatus<PS::VIGOR>();
    }

    if (!item.purgeOnUse && p.hasStatus<PS::FREE_ATTACK_POWER>()) { // FreeAttackPower.onUseCard skips a copy
        p.decrementStatus<PS::FREE_ATTACK_POWER>();
    }

    if (p.hasStatus<PS::PEN_NIB>()) {
        // todo does this need to be added to bot?
        addToBot( Actions::RemoveStatus<PS::PEN_NIB>() );
    }

    // ********* Relics onUseCard *********
    // todo order of relics

    if (p.hasRelic<R::INK_BOTTLE>()) {
        p.inkBottleCounter++;
        if (p.inkBottleCounter == 10) {
            p.inkBottleCounter = 0;
            addToBot( Actions::DrawCards(1) );
        }
    }

    if (p.hasRelic<R::KUNAI>() && p.attacksPlayedThisTurn % 3 == 0) {
        addToBot( Actions::BuffPlayer<PS::DEXTERITY>(1) );
    }

    if (p.hasRelic<R::ORANGE_PELLETS>()) {
        p.orangePelletsCardTypesPlayed.set(static_cast<int>(CardType::ATTACK), true); // set bit 0 true
        if (p.orangePelletsCardTypesPlayed.all()) {
            p.orangePelletsCardTypesPlayed.reset();
            addToBot(Actions::RemovePlayerDebuffs());
        }
    }

    if (p.hasRelic<R::ORNAMENTAL_FAN>() && p.attacksPlayedThisTurn % 3 == 0) {
        addToBot( Actions::GainBlock(4) );
    }

    if (p.hasRelic<R::SHURIKEN>() && p.attacksPlayedThisTurn % 3 == 0) {
        addToBot( Actions::BuffPlayer<PS::STRENGTH>(1) );
    }

    if (p.hasRelic<R::NECRONOMICON>() && !p.haveUsedNecronomiconThisTurn &&
        (c.costForTurn >= 2 && !c.freeToPlayOnce || c.isXCost() && item.energyOnUse >= 2)) {
        // Unlike Double Tap's copy, this one isn't in limbo, so Rampage's ModifyDamageAction misses it.
        CardInstance copy = c;
        if (c.getId() == CardId::RAMPAGE && !item.purgeOnUse) {
            copy.specialData = static_cast<std::int16_t>(copy.specialData - (c.isUpgraded() ? 8 : 5));
        }
        queuePurgeCard(copy, item.target);
        p.haveUsedNecronomiconThisTurn = true;
    }

    if (p.hasRelic<R::PEN_NIB>()) {
        ++p.penNibCounter;
        if (p.penNibCounter == 9) {
            addToBot( Actions::BuffPlayer<PS::PEN_NIB>(1) );
            p.penNibCounter = -1; // take note of this
        }
    }

    if (p.hasRelic<R::DUALITY>()) {
        addToBot(Actions::DualityAction());
    }

    if (p.hasRelic<R::NUNCHAKU>()) {
        if (++p.nunchakuCounter >= 10) {
            addToBot(Actions::GainEnergy(1));
            p.nunchakuCounter = 0;
        }
    }

    /*
     *  for each card in hand : TriggerOnCardPlayed
     *  for each card in discardPile : TriggerOnCardPlayed
     *  for each card in drawPile : TriggerOnCardPlayed
     */

    // ********* Enemy Powers onUseCard *********

    // todo Choke Power

    auto &m = monsters.arr[0];
    if (m.hasStatus<MS::SHARP_HIDE>()) {
        addToBot( Actions::DamagePlayer(m.getStatus<MS::SHARP_HIDE>()));
    }

}

void BattleContext::onUseSkillCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;

    auto &p = player;
    ++p.skillsPlayedThisTurn;

    // ********* Powers onUseCard *********

    if (p.hasStatus<PS::AFTER_IMAGE>()) {
        addToBot(Actions::GainBlock(p.getStatus<PS::AFTER_IMAGE>()));
    }

    if (!item.purgeOnUse && p.hasStatus<PS::BURST>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::BURST>();
    }

    if (!item.purgeOnUse && p.hasStatus<PS::DUPLICATION>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::DUPLICATION>();
    }

    const auto echoForm = p.getStatus<PS::ECHO_FORM>();
    if (!item.purgeOnUse && echoForm) {
        const bool echoFormActive = player.cardsPlayedThisTurn - player.echoFormCardsDoubled <= echoForm;
        if (echoFormActive) {
            ++player.echoFormCardsDoubled;
            queuePurgeCard(c, item.target);
        }
    }

    if (p.hasStatus<PS::HEX>()) {
        addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::DAZED), 1, true) );
    }

    if (p.hasStatus<PS::PANACHE>() && --p.panacheCounter <= 0) {
        p.panacheCounter = 5;
        addToBot( Actions::DamageAllEnemy(p.getStatus<PS::PANACHE>()) );
    }

    // todo Storm
    // todo Heatsinks
    // todo BirdFacedUrn

    // ********* Relics onUseCard *********
    // todo ink bottle/ ornamental fan need to be ordered i believe

    if (p.hasRelic<R::INK_BOTTLE>()) {
        p.inkBottleCounter++;
        if (p.inkBottleCounter == 10) {
            p.inkBottleCounter = 0;
            addToBot( Actions::DrawCards(1) );
        }
    }

    if (p.hasRelic<R::ORANGE_PELLETS>()) {
        p.orangePelletsCardTypesPlayed.set(static_cast<int>(CardType::SKILL), true); // set bit 0 true
        if (p.orangePelletsCardTypesPlayed.all()) {
            p.orangePelletsCardTypesPlayed.reset();
            addToBot(Actions::RemovePlayerDebuffs());
        }
    }

    if (p.hasRelic<R::LETTER_OPENER>()) {
        if (p.skillsPlayedThisTurn >= 3 &&  p.skillsPlayedThisTurn % 3 == 0) {
            addToBot(Actions::DamageAllEnemy(5));
        }
    }

    if (p.hasRelic<R::MUMMIFIED_HAND>()) {
        // todo
    }

    /*
     *  for each card in hand : TriggerOnCardPlayed
     *  for each card in discardPile : TriggerOnCardPlayed
     *  for each card in drawPile : TriggerOnCardPlayed
     */

    // ********* Enemy Powers onUseCard *********
    // todo Choke Power

    // Colosseum's second fight has Gremlin Nob in the second slot.
    for (int i = 0; i < monsters.monsterCount; ++i) {
        auto &m = monsters.arr[i];
        if (m.hasStatus<MS::ENRAGE>()) {
            m.buff<MS::STRENGTH>(m.getStatus<MS::ENRAGE>());
        }
    }
}

void BattleContext::onUsePowerCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;
    auto &p = player;

    if (p.hasStatus<PS::AFTER_IMAGE>()) {
        addToBot(Actions::GainBlock(p.getStatus<PS::AFTER_IMAGE>()));
    }

    if (!item.purgeOnUse && p.hasStatus<PS::DUPLICATION>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::DUPLICATION>();
    }

    const auto echoForm = p.getStatus<PS::ECHO_FORM>();
    if (!item.purgeOnUse && echoForm) {
        const bool echoFormActive = player.cardsPlayedThisTurn - player.echoFormCardsDoubled <= echoForm;
        if (echoFormActive) {
            ++player.echoFormCardsDoubled;
            queuePurgeCard(c, item.target);
        }
    }

    if (p.hasStatus<PS::HEX>()) {
        addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::DAZED), 1, true) );
    }

    if (p.hasStatus<PS::PANACHE>() && --p.panacheCounter <= 0) {
        p.panacheCounter = 5;
        addToBot( Actions::DamageAllEnemy(p.getStatus<PS::PANACHE>()) );
    }

    if (!item.purgeOnUse && p.hasStatus<PS::AMPLIFY>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::AMPLIFY>();
    }

    if (p.hasStatus<PS::HEATSINK>()) {
        addToTop( Actions::DrawCards(p.getStatus<PS::HEATSINK>()) );
    }

    if (p.hasStatus<PS::STORM>()) {
        addToBot( Actions::ChannelOrbs(Orb::LIGHTNING, p.getStatus<PS::STORM>()) );
    }

    // Force Field's triggerOnCardPlayed, and its constructor counts cardsPlayedThisCombat for the deck's next copy.
    ++p.powersPlayedThisCombat;
    for (int i = 0; i < cards.cardsInHand; ++i) {
        if (cards.hand[i].getId() == CardId::FORCE_FIELD) {
            cards.hand[i].updateCost(-1);
        }
    }
    for (auto &card : cards.discardPile) {
        if (card.getId() == CardId::FORCE_FIELD) {
            card.updateCost(-1);
        }
    }
    for (auto &card : cards.drawPile) {
        if (card.getId() == CardId::FORCE_FIELD) {
            card.updateCost(-1);
        }
    }

    // ********* Relics onUseCard *********

    if (p.hasRelic<R::BIRD_FACED_URN>()) {
        p.heal(2);
    }

    if (p.hasRelic<R::INK_BOTTLE>()) {
        p.inkBottleCounter++;
        if (p.inkBottleCounter == 10) {
            p.inkBottleCounter = 0;
            addToBot( Actions::DrawCards(1) );
        }
    }

    if (p.hasRelic<R::ORANGE_PELLETS>()) {
        p.orangePelletsCardTypesPlayed.set(static_cast<int>(CardType::POWER), true); // set bit 0 true
        if (p.orangePelletsCardTypesPlayed.all()) {
            p.orangePelletsCardTypesPlayed.reset();
            addToBot(Actions::RemovePlayerDebuffs());
        }
    }

    if (p.hasRelic<R::MUMMIFIED_HAND>()) {
        mummifiedHandOnUsePower();
    }

    for (int i = 0; i < monsters.monsterCount; ++i) {
        auto &m = monsters.arr[i];
        if (m.hasStatus<MS::CURIOSITY>()) {
            m.buff<MS::STRENGTH>(m.getStatus<MS::CURIOSITY>());
        }
    }
}

void BattleContext::onUseStatusOrCurseCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;
    auto &p = player;

    if (p.hasStatus<PS::AFTER_IMAGE>()) {
        addToBot(Actions::GainBlock(p.getStatus<PS::AFTER_IMAGE>()));
    }

    if (!item.purgeOnUse && p.hasStatus<PS::DUPLICATION>()) {
        queuePurgeCard(c, item.target);
        p.decrementStatus<PS::DUPLICATION>();
    }

    const auto echoForm = p.getStatus<PS::ECHO_FORM>();
    if (!item.purgeOnUse && echoForm) {
        const bool echoFormActive = player.cardsPlayedThisTurn - player.echoFormCardsDoubled <= echoForm;
        if (echoFormActive) {
            ++player.echoFormCardsDoubled;
            queuePurgeCard(c, item.target);
        }
    }

    if (p.hasStatus<PS::HEX>()) {
        addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::DAZED), 1, true) );
    }

    if (p.hasStatus<PS::PANACHE>() && --p.panacheCounter <= 0) {
        p.panacheCounter = 5;
        addToBot( Actions::DamageAllEnemy(p.getStatus<PS::PANACHE>()) );
    }

    if (c.getType() == CardType::CURSE) {
        if (p.hasRelic<R::BLUE_CANDLE>()) {
            addToBot( Actions::PlayerLoseHp(1, true) );
            item.exhaustOnUse = true;
        }

    } else if (c.getType() == CardType::STATUS) {
        if (p.hasRelic<R::MEDICAL_KIT>()) {
            item.exhaustOnUse = true;
        }
    }

    if (p.hasRelic<R::INK_BOTTLE>()) {
        p.inkBottleCounter++;
        if (p.inkBottleCounter == 10) {
            p.inkBottleCounter = 0;
            addToBot( Actions::DrawCards(1) );
        }
    }

}

void BattleContext::onAfterUseCard() {
    auto &item = curCardQueueItem;
    auto &c = item.card;
    bool rebound = false;

    if (item.triggerOnUse) {
        // ReboundPower.onAfterUseCard runs for every card that triggers, including copies and powers, and skips the Rebound
        // that applied it. A Rebound played under another one is sent back like any other card.
        if (player.hasStatus<PS::REBOUND>()) {
            if (player.reboundJustApplied) {
                player.reboundJustApplied = false;
            } else {
                rebound = c.getType() != CardType::POWER;
                player.decrementStatus<PS::REBOUND>();
            }
        }

        auto &m = monsters.arr[0];
        if (m.hasStatus<MS::TIME_WARP>()) {
            auto timeWarp = m.getStatus<MS::TIME_WARP>();
            if (timeWarp == 11) {
                m.setStatus<MS::TIME_WARP>(0);
                m.buff<MS::STRENGTH>(2);
                callEndTurnEarlySequence();

            } else {
                m.setStatus<MS::TIME_WARP>(timeWarp + 1);
                ++timeWarp;
            }
        }
        if (m.hasStatus<MS::SLOW>()) {
            m.buff<MS::SLOW>(1);
        }
        if (m.hasStatus<MS::BEAT_OF_DEATH>()) {
            addToBot( Actions::DamagePlayer(m.getStatus<MS::BEAT_OF_DEATH>()) );
        }
    }

    if (item.purgeOnUse) {
        return;
    }

    c.freeToPlayOnce = false;

    if (c.getType() == CardType::POWER) {
        c.id = CardId::INVALID;
        return;
    }

    bool spoonProc = false;
    if (item.exhaustOnUse && player.hasRelic<R::STRANGE_SPOON>()) {
        spoonProc = cardRandomRng.randomBoolean();
    }

    if (item.exhaustOnUse && !spoonProc) {
        triggerAndMoveToExhaustPile(c);

    } else {
        // targetCard.exhaustOnUseOnce = false;
        // targetCard.dontTriggerOnUseCard = false;
        // this.addToBot(new HandCheckAction());

        if (rebound) { // these methods all call resetCardBeforeMoving but it looks unnecessary
            cards.moveToDrawPileTop(c);

        } else if (c.id == CardId::TANTRUM) {
            CardInstance landing = c;
            landing.setCostForTurn(landing.cost); // a card landing on the draw pile, as moveToDrawPileTop does
            cards.shuffleIntoDrawPile(cardRandomRng, landing);

        } else {
            cards.moveToDiscardPile(c);
        }
    }
    cards.onCardDrawOrDiscard(*this);
    // TODO these must be done in the cards method itself
    // todo make Accuracy part of calculateBaseDamage

    // this.targetCard.exhaustOnUseOnce = false;
    //            this.targetCard.dontTriggerOnUseCard = false;
    //            this.addToBot(new HandCheckAction());
}

// **********************

void BattleContext::endTurn() {
#ifdef sts_assert
    assert(!endTurnQueued);
#endif //sts_assert
    // todo probably dont need a card queue item for this
    energyWasted += player.energy;
    cardQueue.pushBack(CardQueueItem::endTurnItem());
    endTurnQueued = true;
}

void BattleContext::callEndOfTurnActions() {

    // ********************* Player Relics OnPlayerEndTurn *********************

    if (player.hasRelic<R::CLOAK_CLASP>()) {
        addToBot( Actions::GainBlock(cards.cardsInHand) );
    }

    if (player.hasRelic<R::FROZEN_CORE>()) {
        if (player.hasEmptyOrb()) {
            player.channelOrb(*this, Orb::FROST);
        }
    }

    if (player.hasRelic<R::NILRYS_CODEX>()) {
        addToBot(Actions::CodexAction());
    }

    if (player.hasRelic<R::ORICHALCUM>()) {
        if (player.block <= 0) {
            addToTop(Actions::GainBlock(6));
        }
    }

    if (player.hasRelic<R::STONE_CALENDAR>()) {
        if (turn == 6) {
            addToBot(Actions::DamageAllEnemy(52));
        }
    }

    // ********************* Player Powers AtEndOfTurnPreEndTurnCards *********************

    if (player.hasStatus<PS::METALLICIZE>()) {
        addToBot( Actions::GainBlock( player.getStatus<PS::METALLICIZE>()) );
    }

    if (player.hasStatus<PS::PLATED_ARMOR>()) {
        addToBot( Actions::GainBlock(player.getStatus<PS::PLATED_ARMOR>()) );
    }

    if (player.hasStatus<PS::LIKE_WATER>() && player.stance == Stance::CALM) {
        addToBot( Actions::GainBlock(player.getStatus<PS::LIKE_WATER>()) );
    }

    if (player.orbSlots) {
        addToBot(Actions::TriggerEndOfTurnOrbsAction());
    }

    // todo for cards in hand call triggerOnEndOfTurnForPlayingCard

    for (int i = 0; i < cards.cardsInHand; ++i) {

        const auto &c = cards.hand[i];

        switch (c.id) {
            case CardId::BURN:
            case CardId::DECAY:
            case CardId::DOUBT:
            case CardId::SHAME:
            case CardId::REGRET: {
                CardQueueItem item;
                item.triggerOnUse = false;
                item.regretCardCount = cards.cardsInHand;
                item.card = c;
                addToBotCard(item);
                break;
            }
            case CardId::PRIDE:
                addToBot( Actions::MakeTempCardInDrawPile(CardInstance(CardId::PRIDE), 1, false) );
                break;
            default:
                break;
        }
    }

    // todo stance onEndOfTurn
}

void BattleContext::onTurnEnding() {
    // AbstractRoom endTurn()

    // ********************* Player Powers atEndOfTurn *********************
    player.applyEndOfTurnPowers(*this);
    addToBot(Actions::ClearCardQueue());
    addToBot(Actions::DiscardAtEndOfTurn());

    // todo reset card attributes here in draw, discard and hand
    cards.resetAttributesAtEndOfTurn();

    addToBot(Actions::UnnamedEndOfTurnAction());
}

void BattleContext::callEndTurnEarlySequence(bool queueStillRunning) {
    while (!cardQueue.isEmpty()) {
        auto item = cardQueue.popFront();
        if (item.autoplay && !item.purgeOnUse) {
            addToBot( Actions::TimeEaterPlayCardQueueItem(item) );
        }
    }
    // Unceasing Top still sees the empty hand while the end of the turn is queued. Seen on the real game with Time Warp.
    // UnceasingTop.onRefreshHand needs an empty action queue, so it can't go off from inside a card's own actions.
    if (!queueStillRunning && player.hasRelic<R::UNCEASING_TOP>() && cards.cardsInHand == 0) {
        addToBot( Actions::DrawCards(1) );
    }
    addToTopCard(CardQueueItem::endTurnItem());
    endTurnQueued = true;
}

void BattleContext::applyEndOfRoundPowers() {
    for (int i = 0; i < monsters.monsterCount; i++) {
        auto &m = monsters.arr[i];
        if (m.isDying() || m.isEscaping()) {
            continue;
        }
        m.applyEndOfTurnTriggers(*this);
    }

    player.applyAtEndOfRoundPowers();

    for (int i = 0; i < monsters.monsterCount; i++) {
        auto &m = monsters.arr[i];
        if (m.isDying() || m.isEscaping()) {
            continue;
        }
        m.applyEndOfRoundPowers(*this);
    }
}

void BattleContext::afterMonsterTurns() {
    const bool roundEnded = !skipMonsterTurn;

    // ********* Enemy powers atEndOfRound *********
    if (!skipMonsterTurn) {
        applyEndOfRoundPowers();
    }

    ++turn;
    skipMonsterTurn = false;
    turnHasEnded = false;

    // player stance atStartOfTurn
    if (player.stance == Stance::DIVINITY) {
        addToBot(Actions::ChangeStance(Stance::NEUTRAL));
    }

    player.applyStartOfTurnRelics(*this);

    // player applyStartOfTurnPreDrawCards() // no cards implement this
    // player.applyStartOfTurnCards() // only Eviscerate implements this

    player.applyStartOfTurnPowers(*(this));

    // player.applyStartOfTurnOrbs()
    for (int i = 0; i < player.orbSlots; ++i) {
        player.orbOnStartOfTurn(*this, i);
    }
    if (player.hasRelic<R::GOLD_PLATED_CABLES>() && player.hasOrb()) {
        player.orbOnStartOfTurn(*this, 0);
    }

    if (player.hasStatus<PS::BARRICADE>() || player.hasStatus<PS::BLUR>()) {

    } else if (player.hasRelic<R::CALIPERS>()) {
        player.block = std::max(0, player.block-15);

    } else {
        player.block = 0;
    }

    // BlurPower.atEndOfRound queues its ReducePowerAction, so it's still there for the check above.
    if (roundEnded && player.hasStatus<PS::BLUR>()) {
        player.decrementStatus<PS::BLUR>();
    }

    if (isBattleOver) {
        return;
    }

    // DrawCardAction's constructor makes the PlayerTurnEffect that recharges energy, so Collect's Miracle is queued ahead of this draw.
    player.rechargeEnergy(*this);

    addToBot(Actions::DrawCards(player.cardDrawPerTurn));

    // DrawReductionPower.atEndOfRound, which Vault's skipped round doesn't reach.
    if (roundEnded && player.hasStatus<PS::DRAW_REDUCTION>()) {
        if (player.wasJustApplied<PS::DRAW_REDUCTION>()) {
            player.setJustApplied<PS::DRAW_REDUCTION>(false);
        } else {
            player.removeStatus<PS::DRAW_REDUCTION>();
            ++player.cardDrawPerTurn;
        }
    }

    player.applyStartOfTurnPostDrawRelics(*this);
    player.applyStartOfTurnPostDrawPowers(*this);

    // this has to be here because some relics check this info.
    player.cardsPlayedThisTurn = 0;
    player.attacksPlayedThisTurn = 0;
    player.skillsPlayedThisTurn = 0;
    player.cardsDiscardedThisTurn = 0;
}

void BattleContext::obtainPotion(Potion p) {
    if (potionCount == potionCapacity || player.hasRelic<R::SOZU>()) {
        return; // no empty potion slots
    }

    for (int i = 0; i < potionCapacity; ++i) {
        if (potions[i] == Potion::EMPTY_POTION_SLOT) {
            ++potionCount;
            potions[i] = p;
            return;
        }
    }

//  todo prevent this, do nothing for now
//    assert(false); // unreachable
}

void BattleContext::discardPotion(int idx) {
    potions[idx] = Potion::EMPTY_POTION_SLOT;
    --potionCount;
}

void BattleContext::drinkPotion(int idx, int target) {
    const bool hasBark = player.hasRelic<R::SACRED_BARK>();
    const Potion p = potions[idx];
    discardPotion(idx);
    if (potionRequiresTarget(p)) {
        player.lastTargetedMonster = target;
    }
    if (player.hasRelic<R::TOY_ORNITHOPTER>()) {  // FIX: relic had no implementation
        addToBot(Actions::HealPlayer(5));
    }

    // todo - dont need to add to bot because always will have nothing in actionQueue?

    switch (p) {
        case Potion::AMBROSIA:
            addToBot(Actions::ChangeStance(Stance::DIVINITY));
            break;

        case Potion::ANCIENT_POTION:
            addToBot(Actions::BuffPlayer<PS::ARTIFACT>(hasBark ? 2 : 1));
            break;

        case Potion::ATTACK_POTION:
            addToBot(Actions::DiscoveryAction(CardType::ATTACK, hasBark ? 2 : 1));
            break;

        case Potion::BLESSING_OF_THE_FORGE:
            addToBot(Actions::UpgradeAllCardsInHand());
            break;

        case Potion::BLOCK_POTION:
            addToBot(Actions::GainBlock(hasBark ? 24 : 12));
            break;


        case Potion::BLOOD_POTION: {
            int healAmt = static_cast<int>((static_cast<float>(player.maxHp * (hasBark ? 40 : 20)) / 100.0f));  // FIX: was inverted
            addToBot(Actions::HealPlayer(healAmt));
            break;
        }

        case Potion::BOTTLED_MIRACLE:
            addToBot(Actions::MakeTempCardInHand(CardId::MIRACLE, false, hasBark ? 4 : 2));
            break;

        case Potion::COLORLESS_POTION:
            addToBot( Actions::DiscoveryAction(CardType::STATUS, hasBark ? 2 : 1) ); // status card type is being used to indicate colorless
            break;

        case Potion::CULTIST_POTION:
            addToBot(Actions::BuffPlayer<PS::RITUAL>(hasBark ? 2 : 1));
            break;

        case Potion::CUNNING_POTION:
            addToBot(Actions::MakeTempCardInHand(CardId::SHIV, true, hasBark ? 6 : 3));
            break;

        case Potion::DEXTERITY_POTION:
            addToBot(Actions::BuffPlayer<PS::DEXTERITY>(hasBark ? 4 : 2));
            break;

        case Potion::DISTILLED_CHAOS: {
            const auto cardsToPlay = hasBark ? 6 : 3;
            for (int i = 0; i < cardsToPlay; ++i) {
                addToBot( Actions::PlayTopCard(monsters.getRandomMonsterIdx(cardRandomRng), false) );
            }
            break;
        }

        case Potion::DUPLICATION_POTION:
            addToBot(Actions::BuffPlayer<PS::DUPLICATION>(hasBark ? 2 : 1));
            break;

        case Potion::ELIXIR_POTION:
            addToBot( Actions::ExhaustMany(10) );
            break;

        case Potion::ENERGY_POTION:
            addToBot(Actions::GainEnergy(hasBark ? 4 : 2));
            break;

        case Potion::ENTROPIC_BREW: {
            for (int i = 0; i < potionCapacity; ++i) {
                Potion randomPotion = returnRandomPotion(potionRng, player.cc, true);
                obtainPotion(randomPotion);
            }
            break;
        }

        case Potion::ESSENCE_OF_DARKNESS:
            addToBot( Actions::EssenceOfDarkness(hasBark ? 2 : 1) );
            break;

        case Potion::ESSENCE_OF_STEEL:
            addToBot( Actions::BuffPlayer<PS::PLATED_ARMOR>(hasBark ? 8 : 4) );
            break;

        case Potion::EXPLOSIVE_POTION: {
            const auto damage = hasBark ? 20 : 10;
            addToBot( Actions::DamageAllEnemy(damage) ); // todo does not having player be owner here matter?
            break;
        }

        case Potion::FEAR_POTION:
            addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(target, hasBark ? 6 : 3, false) );
            break;

        case Potion::FIRE_POTION:
            addToBot( Actions::DamageEnemy(target, hasBark ? 40 : 20) );
            break;

        case Potion::FLEX_POTION:
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(hasBark ? 10 : 5) );
            addToBot( Actions::DebuffPlayer<PS::LOSE_STRENGTH>(hasBark ? 10 : 5) );
            break;

        case Potion::FOCUS_POTION:
            addToBot(Actions::BuffPlayer<PS::FOCUS>(hasBark ? 4 : 2));
            break;

        case Potion::FRUIT_JUICE:
            player.increaseMaxHp(hasBark ? 10 : 5);
            break;

        case Potion::GAMBLERS_BREW:
            addToBot( Actions::GambleAction() );
            break;

        case Potion::GHOST_IN_A_JAR:
            addToBot(Actions::BuffPlayer<PS::INTANGIBLE>(hasBark ? 2 : 1));
            break;

        case Potion::HEART_OF_IRON:
            addToBot(Actions::BuffPlayer<PS::METALLICIZE>(hasBark ? 12 : 6));
            break;

        case Potion::LIQUID_BRONZE:
            addToBot(Actions::BuffPlayer<PS::THORNS>(hasBark ? 6 : 3));
            break;

        case Potion::LIQUID_MEMORIES:
            addToBot( Actions::BetterDiscardPileToHandAction(hasBark ? 2 : 1, CardSelectTask::LIQUID_MEMORIES_POTION) );
            break;

        case Potion::POISON_POTION:
            addToBot( Actions::DebuffEnemy<MS::POISON>(target, hasBark ? 12 : 6) );
            break;

        case Potion::POTION_OF_CAPACITY:
            addToBot( Actions::IncreaseOrbSlots(hasBark ? 4 : 2) );
            break;

        case Potion::POWER_POTION:
            haveUsedDiscoveryAction = true;
            addToBot( Actions::DiscoveryAction(CardType::POWER, hasBark ? 2 : 1) );
            break;

        case Potion::REGEN_POTION:
            addToBot(Actions::BuffPlayer<PS::REGEN>(hasBark ? 10 : 5));
            break;

        case Potion::SKILL_POTION:
            addToBot(Actions::DiscoveryAction(CardType::SKILL, hasBark ? 2 : 1));
            break;

        case Potion::SMOKE_BOMB:
            // FIX (was a no-op): escape the fight (not usable against bosses), forfeiting rewards
            if (!isBossEncounter(encounter)) {
                miscBits.set(1);  // escaped flag, read by exitBattle
                cardQueue.clear();
                actionQueue.clear();
                if (player.hasRelic<R::TOY_ORNITHOPTER>()) {  // the game's queue still runs its heal
                    player.heal(5);
                }
                outcome = Outcome::PLAYER_VICTORY;
            }
            break;

        case Potion::SNECKO_OIL:
            addToBot(Actions::DrawCards(hasBark ? 10 : 5));
            addToBot(Actions::RandomizeHandCost());
            break;

        case Potion::SPEED_POTION:
            addToBot(Actions::BuffPlayer<PS::DEXTERITY>(hasBark ? 10 : 5));
            addToBot(Actions::DebuffPlayer<PS::LOSE_DEXTERITY>(hasBark ? 10 : 5));
            break;

        case Potion::STANCE_POTION:
            addToBot(Actions::ChooseStanceAction());
            break;

        case Potion::STRENGTH_POTION:
            addToBot(Actions::BuffPlayer<PS::STRENGTH>(hasBark ? 4 : 2));
            break;

        case Potion::SWIFT_POTION:
            addToBot(Actions::DrawCards(hasBark ? 6 : 3));
            break;

        case Potion::WEAK_POTION:
            addToBot(Actions::DebuffEnemy<MS::WEAK>(target, hasBark ? 6 : 3, false));
            break;

        case Potion::INVALID:
        case Potion::EMPTY_POTION_SLOT:
        case Potion::FAIRY_POTION:
        default:
            std::cerr << seed << "invalid drink potion: " << static_cast<int>(p) << std::endl;
            assert(false);
            break;
    }

}

void BattleContext::drawCards(int count) {
    if (count <= 0 ||
        player.hasStatus<PS::NO_DRAW>() ||
         cards.drawPile.size() + cards.discardPile.size() == 0 ||
         cards.cardsInHand == 10 ) {
        return;
    }

    int amountToDraw = std::min(10-cards.cardsInHand, count);

    if (cards.drawPile.size() < amountToDraw) {
        const auto temp = amountToDraw-static_cast<int>(cards.drawPile.size());
        addToTop( Actions::DrawCards(temp) );
        onShuffle();
        addToTop( Actions::EmptyDeckShuffle() );

        if (!cards.drawPile.empty()) {
            drawCards(static_cast<int>(cards.drawPile.size())); // the game adds this to top
        }
        return;
    }

    cardsDrawn += amountToDraw; // statistic for monte carlo search
    cards.draw(*this, amountToDraw);
}

void BattleContext::discardAtEndOfTurn() {
    int retainCount = 0;
    for (int i = 0; i < cards.cardsInHand; ++i) {
        auto c = cards.hand[i];
        if (c.hasSelfRetain() || c.retain) {
            cards.limbo[retainCount++] = c;
        }
    }
    if (retainCount > 0) {
        addToTop(Actions::RestoreRetainedCards(retainCount));

        int placeIdx = 0;
        for (int i = 0; i < cards.cardsInHand; ++i) {
            auto c = cards.hand[i];
            if (c.hasSelfRetain() || c.retain) {
                continue;
            } else {
                cards.hand[placeIdx++] = cards.hand[i];
            }
        }
        cards.cardsInHand -= retainCount;
    }

    if (!player.hasRelic<R::RUNIC_PYRAMID>() && !player.hasStatus<PS::EQUILIBRIUM>()) {
        addToTop(Actions::DiscardAtEndOfTurnHelper());
    }

    for (int i = 0; i < cards.cardsInHand; ++i) {
        const auto &c = cards.hand[i];
        if (c.isEthereal()) {
            addToTop(Actions::ExhaustSpecificCardInHand(i, c.uniqueId)); // c.triggerOnEndOfPlayerTurn
        }
    }

}

void BattleContext::discardAtEndOfTurnHelper() {
    if (outcome != Outcome::UNDECIDED) {
        return;
    }

    int temp = cards.cardsInHand;
    for (int i = temp-1; i >= 0; --i) {
        cards.notifyRemoveFromHand(cards.hand[i]);
        cards.moveToDiscardPile(cards.hand[i]);
        ++player.cardsDiscardedThisTurn;
    }
    cards.cardsInHand = 0;
}

void BattleContext::playTopCardInDrawPile(int monsterTargetIdx, bool exhausts) {
    if (cards.drawPile.empty()) {
        if (!cards.discardPile.empty()) {
            addToTop( Actions::PlayTopCard(monsterTargetIdx, exhausts) );
            onShuffle();
            addToTop( Actions::EmptyDeckShuffle() );
        }
        return;
    }

    auto top = cards.popFromDrawPile();
    if (top.getId() == CardId::MIND_BLAST) {
        top.specialData = static_cast<std::int16_t>(cards.drawPile.size() + 1);
    } else if (top.getId() == CardId::STACK) {
        top.specialData = static_cast<std::int16_t>(cards.discardPile.size() + 1);
    }
    CardQueueItem item(top, monsterTargetIdx, player.energy);
    item.exhaustOnUse = exhausts;
    item.autoplay = true;
    item.freeToPlay = true; // todo remove the autoplay boolean? added this instead
    addToBotCard(item);
}

void BattleContext::moveToHandHelper(CardInstance c) {
    if (cards.cardsInHand < 10) {
        if (player.hasStatus<PS::CORRUPTION>() && c.getType() == CardType::SKILL) {
            c.setCostForTurn(-9);
        }
        cards.moveToHand(c);
    } else {
        cards.moveToDiscardPile(c);
    }
}

void BattleContext::exhaustSpecificCardInHand(int idx, std::int16_t uniqueId) {

    int foundIdx = -1;

    if (idx < cards.cardsInHand && cards.hand[idx].uniqueId == uniqueId) {
        foundIdx = idx;
    } else {
        for (int i = 0; i < cards.cardsInHand; ++i) {
            if (cards.hand[i].uniqueId == uniqueId) {
               foundIdx = i;
               break;
            }
        }
    }

    if (foundIdx == -1) {
        std::cerr << "exhaustSpecificCardInHand: card not found in hand\n";
        return;
    }

    cards.notifyRemoveFromHand(cards.hand[foundIdx]);
    triggerAndMoveToExhaustPile(cards.hand[foundIdx]);
    --cards.cardsInHand;

    for (int i = foundIdx; i < cards.cardsInHand; ++i) {
        cards.hand[i] = cards.hand[i+1];
    } // todo fixed the cached variables in cardmanager
}

void BattleContext::restoreRetainedCards(int count) {
    for (int i = 0; i < count; ++i) {
        auto &c = cards.limbo[i];
        // check that c retained or self retained?
        c.retain = false;
        switch (c.getId()) { // AbstractCard.onRetained
            case CardId::PERSEVERANCE:
                c.specialData = static_cast<std::int16_t>(c.specialData + (c.isUpgraded() ? 3 : 2));
                break;
            case CardId::WINDMILL_STRIKE:
                c.specialData = static_cast<std::int16_t>(c.specialData + (c.isUpgraded() ? 5 : 4));
                break;
            case CardId::SANDS_OF_TIME:
                addToBot( Actions::ReduceCostAction(c.getUniqueId()) );
                break;
            default:
                break;
        }
        cards.hand[cards.cardsInHand++] = c;
    }
}

void BattleContext::exhaustTopCardInHand() {
    if (cards.cardsInHand <= 0) {
        std::cerr << "exhaustTopCardInHand: no cards in hand";
        return;
    }

    auto &c = cards.hand[--cards.cardsInHand];
    cards.notifyRemoveFromHand(c);
    triggerAndMoveToExhaustPile(c);
}

//void BattleContext::incrementDiscard() { // not for end of turn
//    player.cardsDiscardedThisTurn++;
//    if (!turnHasEnded) {
//        // todo
//    }
//
////    ++totalDiscardedThisTurn;
////    if (!AbstractDungeon.actionManager.turnHasEnded && !endOfTurn) {
////        AbstractDungeon.player.updateCardsOnDiscard();
////        Iterator var1 = AbstractDungeon.player.relics.iterator();
////
////        while(var1.hasNext()) {
////            AbstractRelic r = (AbstractRelic)var1.next();
////            r.onManualDiscard();
////        }
////    }
//}

void BattleContext::triggerOnEndOfTurnForPlayingCards() {
//
//    bool foundCurse;
//    do {
//        foundCurse = false;
//
//        for (int i = 0; i <= hand.size(); ++i) {
//            switch (hand[i].id) {
//                case CardId::DECAY:
//                    addToTop(Actions::DamagePlayer(2));
//                    foundCurse = true;
//                    break;
//
//                case CardId::DOUBT:
//                    addToTop( Actions::DebuffPlayer<PS::WEAK>(1) );
//                    foundCurse = true;
//                    break;
//
//                case CardId::SHAME:
//                    addToTop( Actions::DebuffPlayer<PS::FRAIL>(1) );
//                    foundCurse = true;
//                    break;
//
//                case CardId::REGRET:
//                    addToTop( Actions::PlayerLoseHp(hand.size(), true) );
//                    foundCurse = true;
//                    break;
//
//                case CardId::BURN:
//                    addToTop( Actions::DamagePlayer(hand[i].upgraded ? 4 : 2) );
//                    foundCurse = true;
//                    break;
//
//                default:
//                    break;
//            }
//
//            if (foundCurse) {
//                hand.removeCardAtIdx(i);
//            }
//
//        }
//
//    } while (foundCurse);

}

void BattleContext::triggerOnOtherCardPlayed(const CardInstance &usedCard) {
    int painCount = cards.handPainCount;
    if (usedCard.getId() == CardId::PAIN) {
        --painCount;
    }
    for (int i = 0; i < painCount; ++i) {
        addToTop(Actions::PlayerLoseHp(1, true));
    }

    const auto thousandCuts = player.getStatus<PS::THOUSAND_CUTS>();
    if (thousandCuts) {
        addToBot(Actions::DamageAllEnemy(thousandCuts));
    }
}

int BattleContext::calculateCardDamage(const CardInstance &card, int targetIdx, int baseDamage) const {

    auto damage = static_cast<float>(baseDamage);

    // ****** Player Relics AtDamageModify ******

    if (player.hasRelic<R::STRIKE_DUMMY>() && card.isStrikeCard()) {
        damage += 3;
    }

    if (player.hasRelic<R::WRIST_BLADE>() && (card.costForTurn == 0 || (card.freeToPlayOnce && card.cost != -1))) {
        damage += 4;
    }


    // ****** Player Powers AtDamageGive ******

    damage += static_cast<float>(player.getStatus<PS::STRENGTH>());

    if (player.hasStatus<PS::VIGOR>()) {
        damage += static_cast<float>(player.getStatus<PS::VIGOR>());
    }

    if (player.hasStatus<PS::DOUBLE_DAMAGE>()) {
        damage *= 2;
    }

    if (player.hasStatus<PS::PEN_NIB>()) {
        damage *= 2;
    }

    if (player.hasStatus<PS::WEAK>()) {
        damage *= .75f;
    }

    // ****** Stance AtDamageGive ******

    if (player.stance == Stance::WRATH) {
        damage *= 2;
    } else if (player.stance == Stance::DIVINITY) {
        damage *= 3;
    }

    // ****** Enemy Powers AtDamageReceive ******
    const Monster &monster = monsters.arr[targetIdx];

    if (monster.hasStatus<MS::SLOW>()) {
        damage *= 1 + static_cast<float>(monster.getStatus<MS::SLOW>()) * 0.1f;
    }

    if (monster.hasStatus<MS::VULNERABLE>()) {
        if (player.hasRelic<R::PAPER_PHROG>()) {
            damage *= 1.75f;
        } else {
            damage *= 1.5f;
        }
    }


    // ****** Player Powers AtDamageGiveFinal ****** (none ?)
    // ****** Monster Powers AtDamageReceiveFinal ******

    if (monster.hasStatus<MS::FLIGHT>()) {
        damage *= .5;
    }

    if (monster.hasStatus<MS::INTANGIBLE>()) {
        damage = std::min(damage, 1.0f);  // Body Slam with no block stays at 0
    }

    return std::max(0, static_cast<int>(damage));
}

int BattleContext::calculateCardBlock(int baseBlock) const {
    if (player.hasStatus<PS::NO_BLOCK>()) {
        return 0;
    }

    int block = baseBlock;
    if (player.hasStatus<PS::DEXTERITY>()) {
        block = std::max(0, block + player.getStatus<PS::DEXTERITY>());
    }

    block = std::max(0, block); // Steam Barrier's base block can go below 0

    if (player.hasStatus<PS::FRAIL>()) {
        return block * 3 / 4;
    }

    return block;
}

void BattleContext::queuePurgeCard(const CardInstance &c, int target) {
    CardQueueItem item;
    item.card = c;
    item.purgeOnUse = true;
    item.target = target;
    item.energyOnUse = curCardQueueItem.energyOnUse;
    item.ignoreEnergyTotal = true;
    item.autoplay = true;
    addPurgeCardToCardQueue(item);
}

void BattleContext::addPurgeCardToCardQueue(const CardQueueItem &item) {
    if (cardQueue.size > 0) {
        auto temp = cardQueue.front();
        cardQueue.front() = item;
        cardQueue.pushFront(temp);
    } else {
        cardQueue.pushFront(item);
    }

}

void BattleContext::noOpRollMove() {
    aiRng.random(99);
}

void BattleContext::onManualDiscard(const CardInstance &c) {
    if (c.getId() == CardId::TACTICIAN) {
        player.gainEnergy(c.isUpgraded() ? 2 : 1);
    } else if (c.getId() == CardId::REFLEX) {
        addToBot( Actions::DrawCards(c.isUpgraded() ? 3 : 2) );
    }
}

void BattleContext::discardFromHand(int handIdx, int flyingFrom) {
    const int firstFlying = flyingFrom < 0 ? static_cast<int>(cards.discardPile.size()) : flyingFrom;
    const CardInstance c = cards.hand[handIdx];
    cards.removeFromHandAtIdx(handIdx);
    cards.moveToDiscardPile(c);
    cards.onCardDrawOrDiscard(*this);
    onManualDiscard(c);

    ++player.cardsDiscardedThisTurn;
    if (!turnHasEnded) { // AbstractPlayer.updateCardsOnDiscard: only Eviscerate does anything with it
        auto didDiscard = [] (CardInstance &card) {
            if (card.getId() == CardId::EVISCERATE) {
                card.setCostForTurn(card.costForTurn - 1);
            }
        };
        for (int i = 0; i < cards.cardsInHand; ++i) {
            didDiscard(cards.hand[i]);
        }
        for (int i = 0; i < firstFlying; ++i) {
            didDiscard(cards.discardPile[i]);
        }
        for (auto &card : cards.drawPile) {
            didDiscard(card);
        }
    }
}

DamageMatrix BattleContext::calculateMultiDamage(const CardInstance &card, int baseDamage) const {
    DamageMatrix matrix {0};
    for (int i = 0; i < monsters.monsterCount; ++i) {
        if (!monsters.arr[i].isDeadOrEscaped()) {
            matrix[i] = static_cast<std::uint16_t>(std::min(static_cast<int>(std::numeric_limits<std::uint16_t>::max()), calculateCardDamage(card, i, baseDamage)));
        }
    }
    return matrix;
}

void BattleContext::onShuffle() {
    if (player.hasRelic<R::THE_ABACUS>()) {
        addToBot( Actions::GainBlock(6) );
    }

    if (player.hasRelic<R::MELANGE>()) {
        addToBot( Actions::ScryAction(3) );
    }

    if (player.hasRelic<R::SUNDIAL>()) {
        if (player.sundialCounter == 2) {
            player.sundialCounter = 0;
            addToBot( Actions::GainEnergy(2) );
        } else {
            ++player.sundialCounter;
        }
    }
}

void BattleContext::triggerAndMoveToExhaustPile(CardInstance c) {
    // player relics onExhaust
    // player powers onExhaust
    // (the card).triggerOnExhaust

    if (player.hasRelic<R::CHARONS_ASHES>()) {
        addToTop(Actions::DamageAllEnemy(3));
    }

    if (player.hasRelic<R::DEAD_BRANCH>()){
        CardInstance c(getTrulyRandomCardInCombat(cardRandomRng, player.cc));
        c.tookDamageTimes(player.timesDamagedThisCombat);
        addToBot(Actions::MakeTempCardInHand(c));
    }

    if (player.hasStatus<PS::DARK_EMBRACE>()) {
        addToBot(Actions::DrawCards(player.getStatus<PS::DARK_EMBRACE>()));
    }

    if (player.hasStatus<PS::FEEL_NO_PAIN>()) {
        addToBot(Actions::GainBlock(player.getStatus<PS::FEEL_NO_PAIN>()));
    }

    if (c.getId() == CardId::NECRONOMICURSE) {
        addToBot(Actions::MakeTempCardInHand(CardId::NECRONOMICURSE));
    }

    if (c.getId() == CardId::SENTINEL) {
        player.gainEnergy(c.isUpgraded() ? 3 : 2); // the game adds to bot here
    }

    cards.moveToExhaustPile(c);
}

void BattleContext::mummifiedHandOnUsePower() {
    fixed_list<int,10> matchingIdxList;

    for (int i = 0; i < cards.cardsInHand; ++i) {
        const auto &c = cards.hand[i];
        bool canPick = c.cost > 0 && c.costForTurn > 0 && !c.freeToPlayOnce;
        if (canPick) {
            matchingIdxList.push_back(i);
        }
    }

    if (matchingIdxList.empty()) {
        return;
    }

    for (int i = matchingIdxList.size()-1; i >= 0; --i) {
        // The game's queue still holds the card being played at this point, so that card is left out too.
        const auto uniqueId = cards.hand[matchingIdxList[i]].getUniqueId();
        if (cardQueue.containsCardWithId(uniqueId) || uniqueId == curCardQueueItem.card.getUniqueId()) {
            matchingIdxList.remove(i);
        }
    }

    if (matchingIdxList.empty()) {
        return;
    }

    const int selectedListIdx = cardRandomRng.random(0,matchingIdxList.size()-1);
    const int selectedHandIdx = matchingIdxList[selectedListIdx];
    cards.hand[selectedHandIdx].setCostForTurn(0);
}

void BattleContext::openDiscoveryScreen(std::array<CardId, 3> discoveryCards, int copyCount) {
    inputState = InputState::CARD_SELECT;
    cardSelectInfo.cardSelectTask = CardSelectTask::DISCOVERY;
    cardSelectInfo.pickCount = 1;
    cardSelectInfo.canPickAnyNumber = false;
    cardSelectInfo.canPickZero = false;
    cardSelectInfo.cards = discoveryCards;
    cardSelectInfo.discovery_CopyCount() = copyCount;
    cardSelectInfo.discoveryIsFree = true;
}

void BattleContext::openSimpleCardSelectScreen(CardSelectTask task, int count) {
    inputState = InputState::CARD_SELECT;
    cardSelectInfo.cardSelectTask = task;
    cardSelectInfo.pickCount = count;
    cardSelectInfo.canPickAnyNumber = false;
    cardSelectInfo.canPickZero = false;
}



void BattleContext::chooseArmamentsCard(int handIdx) {
    // todo cleaner solution

    fixed_list<CardInstance,10> validCards;
    fixed_list<CardInstance,10> invalidCards;
    for (int i = 0; i < cards.cardsInHand; ++i) {
        const auto &c = cards.hand[i];
        if (i == handIdx) {
            continue;
        }
        if (c.canUpgrade()) {
            validCards.push_back(c);
        } else {
            invalidCards.push_back(c);
        }
    }

    CardInstance cardToUpgrade = cards.hand[handIdx];
    cardToUpgrade.upgrade();

    int i = 0;
    for (auto c : validCards) {
        cards.hand[i++] = c;
    }
    cards.hand[i++] = cardToUpgrade;
    for (auto c : invalidCards) {
        cards.hand[i++] = c;
    }

}

// The game takes all the picks and moves them when its screen closes, in the order they were picked. Nothing can read the
// hand in between, as Reflex's draw is at the bottom of the queue, so one at a time comes to the same thing.
void BattleContext::chooseDiscardCard(int handIdx) {
    const int picksLeft = cardSelectInfo.pickCount - 1;
    const int flyingFrom = cardSelectInfo.discard_FlyingFrom();
    discardFromHand(handIdx, flyingFrom);
    if (picksLeft > 0) {
        addToTop( Actions::DiscardAction(picksLeft, false, flyingFrom) );
    }
}

// The screen takes the pick out of the hand, and NightmareAction puts it back on top.
void BattleContext::chooseNightmareCard(int handIdx) {
    const CardInstance c = cards.hand[handIdx];
    player.addNightmare(c);
    cards.removeFromHandAtIdx(handIdx);
    cards.moveToHand(c);
}

// RetainCardsAction adds each pick back on top of the hand, and an Ethereal card comes back without being retained.
void BattleContext::chooseRetainCard(int handIdx) {
    const int picksLeft = cardSelectInfo.pickCount - 1;
    const int selected = cardSelectInfo.retain_SelectedCount() + 1;
    if (handIdx == CardSelectInfo::RETAIN_DONE_IDX) {
        return;
    }
    CardInstance c = cards.hand[handIdx];
    if (!c.isEthereal()) {
        c.retain = true;
    }
    cards.removeFromHandAtIdx(handIdx);
    cards.moveToHand(c);
    if (picksLeft > 0) {
        addToTop( Actions::RetainCardsAction(picksLeft, selected) );
    }
}

void BattleContext::chooseSetupCard(int handIdx) {
    CardInstance c = cards.hand[handIdx];
    if (c.cost > 0) {
        c.freeToPlayOnce = true;
    }
    cards.removeFromHandAtIdx(handIdx);
    cards.moveToDrawPileTop(c);
}

void BattleContext::chooseCodexCard(CardId id) {
    CardInstance c(id);
    upgradeIfMasterReality(c);
    c.tookDamageTimes(player.timesDamagedThisCombat);
    c.uniqueId = static_cast<std::int16_t>(cards.nextUniqueCardId++);
    cards.notifyAddCardToCombat(c);
    cards.shuffleIntoDrawPile(cardRandomRng, c);
}

void BattleContext::chooseDualWieldCard(int handIdx) {

    // dual wield is so fucking buggy
    // if you dual wield a ritual dagger:
    // when there is no choice on which card to pick, the first one will change the card in the deck
    // when there **is** a choice on which card to pick, neither will change the card in the deck XDD

    const int copyCount = cardSelectInfo.dualWield_CopyCount();
    CardInstance dualWieldCard = cards.hand[handIdx];

    // todo cleaner solution

    fixed_list<CardInstance,10> validCards;
    fixed_list<CardInstance,10> invalidCards;
    for (int i = 0; i < cards.cardsInHand; ++i) {
        const auto &c = cards.hand[i];
        if (i == handIdx) {
            continue;
        }
        if (c.getType() == CardType::ATTACK || c.getType() == CardType::POWER) {
            validCards.push_back(c);
        } else {
            invalidCards.push_back(c);
        }
    }


    int i = 0;
    for (auto c : validCards) {
        cards.hand[i++] = c;
    }
    for (auto c : invalidCards) {
        cards.hand[i++] = c;
    }

    dualWieldCard.uniqueId = static_cast<std::int16_t>(cards.nextUniqueCardId++); // dual wield buggy
    cards.hand[i++] = dualWieldCard;
    upgradeIfMasterReality(dualWieldCard); // its copies go through the card effects, which upgrade them

    for (int x = 0; x < copyCount; ++x) {
        if (cards.cardsInHand + 1 <= CardManager::MAX_HAND_SIZE) {
            cards.createTempCardInHand(dualWieldCard);

        } else {
            cards.createTempCardInDiscard(dualWieldCard);

        }
    }

}

void BattleContext::chooseDiscardToHandCard(int discardIdx, bool forZeroCost) {
    CardInstance c = cards.discardPile[discardIdx];
    cards.removeFromDiscard(discardIdx);
    if (forZeroCost) {
        c.setCostForTurn(0);
    }
    moveToHandHelper(c);
}

void BattleContext::chooseDiscoveryCard(CardId id) {
    const auto discoveryAmount = cardSelectInfo.data0;
    CardInstance c(id);
    upgradeIfMasterReality(c);
    c.tookDamageTimes(player.timesDamagedThisCombat);
    if (cardSelectInfo.discoveryIsFree) {
        c.setCostForTurn(0);
    }

    for (int i = 0; i < discoveryAmount; ++i) {
        if (cards.cardsInHand + 1 <= CardManager::MAX_HAND_SIZE) {
            if (player.hasStatus<PS::CORRUPTION>() && c.getType() == CardType::SKILL) {
                c.setCostForTurn(-9);
            }
            cards.createTempCardInHand(c);

        } else {
            cards.createTempCardInDiscard(c);
        }
    }
}

void BattleContext::chooseExhaustOneCard(int handIdx) {
    auto c = cards.hand[handIdx];
    cards.removeFromHandAtIdx(handIdx);
    triggerAndMoveToExhaustPile(c);
}


void BattleContext::chooseExhumeCard(int exhaustIdx) {
    // todo game handles corruption here
    auto c = cards.exhaustPile[exhaustIdx];
    cards.removeFromExhaustPile(exhaustIdx);
    cards.notifyAddCardToCombat(c);

    moveToHandHelper(c);
}

void BattleContext::chooseForethoughtCard(int handIdx) {
    if (cards.hand[handIdx].cost > 0) {
        cards.hand[handIdx].freeToPlayOnce = true;
    }

    cards.insertToDrawPile(0, cards.hand[handIdx]);
    cards.removeFromHandAtIdx(handIdx);
}

void BattleContext::chooseHeadbuttCard(int discardIdx) {
#ifdef sts_asserts
    assert(discardIdx >= 0 && discardIdx < cards.discardPile.size());
#endif
    cards.moveToDrawPileTop(cards.discardPile[discardIdx]);
    cards.removeFromDiscard(discardIdx);
}

void BattleContext::chooseRecycleCard(int handIdx) {
    const CardInstance c = cards.hand[handIdx];
    if (c.costForTurn == -1) {
        addToTop( Actions::GainEnergy(player.energy) );
    } else if (c.costForTurn > 0) {
        addToTop( Actions::GainEnergy(c.costForTurn) );
    }
    cards.removeFromHandAtIdx(handIdx);
    triggerAndMoveToExhaustPile(c);
    cards.onCardDrawOrDiscard(*this);
}

void BattleContext::chooseWarcryCard(int handIdx) {
#ifdef sts_asserts
    assert(handIdx >= 0 && handIdx < cards.cardsInHand);
#endif
    cards.moveToDrawPileTop(cards.hand[handIdx]);
    cards.removeFromHandAtIdx(handIdx);
}

void BattleContext::chooseDrawToHandCards(const int *idxs, int cardCount) {
    for (int i = 0; i < cardCount; ++i) {
        const auto drawIdx = idxs[i];
        auto c = cards.drawPile[drawIdx];
        cards.removeFromDrawPileAtIdx(drawIdx);
        moveToHandHelper(c);
    }
}

void BattleContext::chooseExhaustCards(const fixed_list<int, 10> &idxs) {
    if (idxs.empty()) {
        return;
    }
    auto listCopy = idxs;
    std::sort(listCopy.begin(), listCopy.end(), [](auto a, auto b) { return b < a; });

    // assume idxs is sorted in descending order
    for (const auto handIdx : listCopy) {
        auto c = cards.hand[handIdx];
        cards.removeFromHandAtIdx(handIdx);
        triggerAndMoveToExhaustPile(c);
    }
}

void BattleContext::chooseGambleCards(const fixed_list<int, 10> &idxs) {
    if (idxs.empty()) {
        return;
    }
    auto listCopy = idxs;
    std::sort(listCopy.begin(), listCopy.end(), [](auto a, auto b) { return b < a; });

    // assume idxs is sorted in descending order
    addToTop( Actions::DrawCards(listCopy.size()) );
    for (const auto handIdx : listCopy) {
        discardFromHand(handIdx);
    }
}


namespace sts {


    void printRngCounters(std::ostream &os, const BattleContext &bc) {
        const std::string separator = " ";
        os << '\t';

        os << "aiRng: " << bc.aiRng.counter << separator;
        os << "cardRandomRng: " << bc.cardRandomRng.counter << separator;
        os << "shuffleRng: " << bc.shuffleRng.counter << separator;
        os << "miscRng: " << bc.miscRng.counter << separator;
        os << "monsterHpRng: " << bc.monsterHpRng.counter << separator;
        os << "potionRng: " << bc.potionRng.counter << separator;

        os << '\n';
    }

    void printPotions(std::ostream &os, const BattleContext &bc) {
        const auto s = "\n\t";
        os << "\t" << "potionCount: " << bc.potionCount;
        os << s << "potionCapacity: " << bc.potionCapacity;

        os << s << "{ ";
        for (int i = 0; i < bc.potionCapacity; ++i) {
            os << getPotionName(bc.potions[i]) << ", ";
        }
        os << "}\n";
    }

    std::ostream& operator<<(std::ostream &os, const BattleContext &bc) {
        os << "BattleContext: {\n";
        printPotions(os, bc);
        printRngCounters(os, bc);

        os << "\tactionQueueSize: " << bc.actionQueue.size
            << ", cardQueueSize: " << bc.cardQueue.size
            << ", turn: " << bc.turn
            << ", ascension " << bc.ascension
            << ", loopCount: " << bc.loopCount
            << ", sum: " << bc.sum
            << ", seed: " << bc.seed
            << "\n";

        os << bc.monsters;
        os << bc.player;
        os << bc.cards;
        os << "}\n";
        return os;
    }


}
