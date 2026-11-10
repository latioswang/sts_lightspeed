//
// Created by gamerpuppy on 7/4/2021.
//

#include <algorithm>
#include <cassert>

#include <constants/PlayerStatusEffects.h>
#include <combat/BattleContext.h>
#include <combat/Actions.h>
#include "combat/Player.h"
#include "game/Game.h"

using namespace sts;

namespace {

// AbstractPower.priority, where a power changes it. ApplyPowerAction sorts the list by it each time one is added, and
// the sort keeps the order gained among equals.
int powerPriority(PlayerStatus s) {
    constexpr int kDefaultPriority = 5;
    switch (s) {
        case PS::CONFUSED: return 0;
        case PS::DOUBLE_DAMAGE:
        case PS::PEN_NIB: return 6;
        case PS::FRAIL: return 10;
        case PS::DRAW_CARD_NEXT_TURN: return 20;
        case PS::ESTABLISHMENT:
        case PS::TOOLS_OF_THE_TRADE: return 25;
        case PS::INTANGIBLE: return 75;
        case PS::CONSTRICTED: return 105;
        case PS::WEAK: return 99;
        default: return kDefaultPriority;
    }
}

struct StatusList {
    std::array<PlayerStatus, 128> items{};
    int count = 0;
    void add(PlayerStatus s) { items[count++] = s; }
    [[nodiscard]] auto begin() const { return items.begin(); }
    [[nodiscard]] auto end() const { return items.begin() + count; }
};

// A Nightmare has no status, since each one is a power of its own, so it holds its place in the order with this.
constexpr PlayerStatus kNightmareInOrder = PS::INVALID;

// What's in statusMap, in the game's order. A status the order doesn't know about comes last.
StatusList powersInGameOrder(const Player &p, bool withNightmares = false) {
    StatusList list;
    const auto ordered = p.powerOrder.begin() + p.powerOrderCount;
    for (auto it = p.powerOrder.begin(); it != ordered; ++it) {
        if (p.statusMap.count(*it) || (withNightmares && *it == kNightmareInOrder)) list.add(*it);
    }
    for (const auto &entry : p.statusMap) {
        if (std::find(p.powerOrder.begin(), ordered, entry.first) == ordered) list.add(entry.first);
    }
    return list;
}

}  // namespace

void Player::notePowerGained(PlayerStatus s) {
    if (powerOrderCount >= kMaxOrderedPowers) return;
    int at = powerOrderCount;
    while (at > 0 && powerPriority(powerOrder[at - 1]) > powerPriority(s)) {
        powerOrder[at] = powerOrder[at - 1];
        --at;
    }
    powerOrder[at] = s;
    ++powerOrderCount;
}

bool Player::actsBefore(PlayerStatus a, PlayerStatus b) const {
    const auto ordered = powerOrder.begin() + powerOrderCount;
    const auto ia = std::find(powerOrder.begin(), ordered, a);
    const auto ib = std::find(powerOrder.begin(), ordered, b);
    if (ia != ordered || ib != ordered) {
        return ia < ib;
    }
    return a < b;  // statusMap's key order
}

void Player::notePowerLost(PlayerStatus s) {
    const auto end = powerOrder.begin() + powerOrderCount;
    const auto it = std::find(powerOrder.begin(), end, s);
    if (it == end) return;
    std::move(it + 1, end, it);
    --powerOrderCount;
}

bool Player::hasRelicRuntime(RelicId r) const {
    if ((int) r < 64) {
        return relicBits0 & (1ULL << (int)r);
    } else {
        return relicBits1 & (1ULL << ((int)r-64));
    }
}

bool Player::hasStatusRuntime(PlayerStatus s) const {
    switch (s) {
        case PS::ARTIFACT:
            return artifact;

        case PS::DEXTERITY:
            return dexterity;

        case PS::FOCUS:
            return focus;

        case PS::STRENGTH:
            return strength;

        default:
            break;
    }

    int idx = static_cast<int>(s);
    if (idx < 64) {
        return statusBits0 & (1ULL << idx);
    } else {
        return statusBits1 & (1ULL << (idx-64));
    }
}

int Player::getStatusRuntime(PlayerStatus s) const {
    switch (s) {
        case PS::ARTIFACT:
            return artifact;
        case PS::DEXTERITY:
            return dexterity;
        case PS::FOCUS:
            return focus;
        case PS::STRENGTH:
            return strength;
        default:
            if (hasStatusRuntime(s)) {
                return statusMap.at(s);
            } else {
                return 0;
            }
    }
}

void Player::gainBlock(BattleContext &bc, int amount) {
    if (amount <= 0) {
        return;
    }

    block += amount;

    if (hasStatus<PS::JUGGERNAUT>()) {
        bc.addToBot(Actions::DamageRandomEnemy(getStatus<PS::JUGGERNAUT>()));
    }

    if (hasStatus<PS::WAVE_OF_THE_HAND>()) {
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            bc.addToBot(Actions::DebuffEnemy<MS::WEAK>(i, getStatus<PS::WAVE_OF_THE_HAND>(), false));
        }
    }
}

void Player::gainGold(BattleContext &bc, int amount) {
#ifdef sts_asserts
    assert(amount > 0);
#endif

    if (hasRelic<R::ECTOPLASM>()) {
        return;
    }

    gold += amount;
    if (hasRelic<R::BLOODY_IDOL>()) {
        heal(5);
    }
}

void Player::useEnergy(int amount) {
#ifdef sts_asserts
    assert(energy >= amount);
#endif

    energy -= amount;
}

void Player::gainEnergy(int amount) {
    energy += amount;
}

// IncreaseMaxOrbAction calls increaseMaxOrbSlots once per slot, and that does nothing at exactly 10.
void Player::increaseOrbSlots(int amount) {
    for (int i = 0; i < amount && orbSlots != MAX_ORB_SLOTS; ++i) {
        ++orbSlots;
    }
}

// decreaseMaxOrbSlots removes the last slot, whatever it holds.
void Player::decreaseOrbSlots(int amount) {
    for (int i = 0; i < amount && orbSlots > 0; ++i) {
        --orbSlots;
        orbs[orbSlots] = Orb::EMPTY;
        darkOrbEvoke[orbSlots] = 0;
    }
}

void Player::channelOrb(BattleContext &bc, Orb orb, int darkEvoke) {
    if (orbSlots <= 0) {
        return;
    }

    for (int i = 0; i < orbSlots; ++i) {
        if (orbs[i] == Orb::EMPTY) {
            orbs[i] = orb;
            darkOrbEvoke[i] = static_cast<std::int16_t>(darkEvoke);
            if (orb == Orb::LIGHTNING) {
                ++lightningChanneled;
            } else if (orb == Orb::FROST) {
                ++frostChanneled;
            }
            return;
        }
    }

    // Full: AbstractPlayer.channelOrb queues the evoke and then the same channel on top.
    bc.addToTop( Actions::ChannelOrb(orb, darkEvoke) );
    bc.addToTop( Actions::EvokeOrb() );
}

void Player::evokeOrb(BattleContext &bc) {
    if (!hasOrb()) {
        return;
    }
    evokeWithoutLosingOrb(bc);
    removeNextOrb();
}

void Player::evokeWithoutLosingOrb(BattleContext &bc) {
    if (!hasOrb()) {
        return;
    }

    const int amount = orbEvokeAmount(0);
    switch (orbs[0]) {
        case Orb::LIGHTNING:
            bc.addToTop( Actions::LightningOrbDamage(amount, hasStatus<PS::ELECTRO>()) );
            break;

        case Orb::FROST:
            bc.addToTop( Actions::GainBlock(amount) );
            break;

        case Orb::DARK: {
            // DarkOrbEvokeAction's constructor takes the first monster with the lowest HP.
            int target = -1;
            for (int i = 0; i < bc.monsters.monsterCount; ++i) {
                const auto &m = bc.monsters.arr[i];
                if (!m.isDeadOrEscaped() && (target == -1 || m.curHp < bc.monsters.arr[target].curHp)) {
                    target = i;
                }
            }
            if (target != -1) {
                bc.addToTop( Actions::DarkOrbDamage(target, amount) );
            }
            break;
        }

        case Orb::PLASMA:
            bc.addToTop( Actions::GainEnergy(amount) );
            break;

        default:
            break;
    }
}

void Player::removeNextOrb() {
    if (!hasOrb()) {
        return;
    }
    for (int i = 1; i < orbSlots; ++i) {
        orbs[i-1] = orbs[i];
        darkOrbEvoke[i-1] = darkOrbEvoke[i];
    }
    orbs[orbSlots-1] = Orb::EMPTY;
    darkOrbEvoke[orbSlots-1] = 0;
}

void Player::orbOnStartOfTurn(BattleContext &bc, int slot) {
    if (orbs[slot] == Orb::PLASMA) {
        bc.addToBot( Actions::GainEnergy(orbPassiveAmount(slot)) );
    }
}

void Player::orbOnEndOfTurn(BattleContext &bc, int slot) {
    switch (orbs[slot]) {
        case Orb::LIGHTNING:
            bc.addToBot( Actions::LightningOrbDamage(orbPassiveAmount(slot), hasStatus<PS::ELECTRO>()) );
            break;

        case Orb::FROST:
            bc.addToBot( Actions::GainBlock(orbPassiveAmount(slot)) );
            break;

        case Orb::DARK:
            darkOrbEvoke[slot] = static_cast<std::int16_t>(darkOrbEvoke[slot] + orbPassiveAmount(slot));
            break;

        default:
            break;
    }
}

bool Player::hasEmptyOrb() const {
    return orbSlots > 0 && orbs[orbSlots-1] == Orb::EMPTY;
}

bool Player::hasOrb() const {
    return orbSlots > 0 && orbs[0] != Orb::EMPTY;
}

int Player::filledOrbCount() const {
    int count = 0;
    while (count < orbSlots && orbs[count] != Orb::EMPTY) {
        ++count;
    }
    return count;
}

// AbstractOrb.applyFocus and Dark's override. Focus changes reach every orb at once, so these can be worked out when read.
int Player::orbPassiveAmount(int slot) const {
    switch (orbs[slot]) {
        case Orb::LIGHTNING: return std::max(0, 3 + focus);
        case Orb::FROST: return std::max(0, 2 + focus);
        case Orb::DARK: return std::max(0, 6 + focus);
        case Orb::PLASMA: return 1;
        default: return 0;
    }
}

int Player::orbEvokeAmount(int slot) const {
    switch (orbs[slot]) {
        case Orb::LIGHTNING: return std::max(0, 8 + focus);
        case Orb::FROST: return std::max(0, 5 + focus);
        case Orb::DARK: return darkOrbEvoke[slot];
        case Orb::PLASMA: return 2;
        default: return 0;
    }
}

void Player::removeDebuffs() {
    if (getStatus<PS::STRENGTH>() < 0) {
        setStatusValueNoChecks<PS::STRENGTH>(0);
    }
    if (getStatus<PS::DEXTERITY>() < 0) {
        setStatusValueNoChecks<PS::DEXTERITY>(0);
    }
    if (focus < 0) {
        focus = 0;
    }

    removeStatus<PS::BIAS>();
    removeStatus<PS::CONFUSED>();
    removeStatus<PS::CONSTRICTED>();

    if (hasStatus<PS::DRAW_REDUCTION>()) {
        ++cardDrawPerTurn;
        removeStatus<PS::DRAW_REDUCTION>();
    }

    removeStatus<PS::ENTANGLED>();
    removeStatus<PS::FASTING>();
    removeStatus<PS::FRAIL>();
    removeStatus<PS::HEX>();
    removeStatus<PS::LOSE_DEXTERITY>();
    removeStatus<PS::LOSE_STRENGTH>();
    removeStatus<PS::NO_BLOCK>();
    removeStatus<PS::NO_DRAW>();
    removeStatus<PS::VULNERABLE>();
    removeStatus<PS::WEAK>();
    removeStatus<PS::WRAITH_FORM>();
}

void Player::increaseMaxHp(int amount) {
    maxHp += amount;
    heal(amount);
}

void Player::heal(int amount) {
    if (hasRelic<RelicId::MARK_OF_THE_BLOOM>()) {
        return;
    }

    if (hasRelic<RelicId::MAGIC_FLOWER>()) {
        amount = (amount * 3 + 1) / 2;  // MathUtils.round(amount * 1.5f)
    }

    curHp = std::min(static_cast<int>(maxHp), curHp + amount);

    if (curHp > maxHp/2 && redSkullActive) {
        debuff<PS::STRENGTH>(-3, false);
        redSkullActive = false;
    }
}

void Player::damage(BattleContext &bc, const int calculatedDamage, const bool selfDamage) {
#ifdef sts_asserts
    assert(calculatedDamage >= 0);
#endif

    int damage = calculatedDamage;

    if (damage > 0 && hasStatus<PS::INTANGIBLE>()) {
        damage = 1;
    }

    const auto savedBlock = block;
    block = std::max(0, block-damage);
    damage -= savedBlock;



    damage -= block;
    if (damage > 0) {
        block = 0;
    }

    if (damage > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        damage = 0;
    }

    if (damage > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        damage -= 1;
    }

    if (damage > 0) {
        hpWasLost(bc, damage, selfDamage);
    }
}

void Player::attacked(BattleContext &bc, int enemyIdx, int calculatedDamage) {
    assert(calculatedDamage >= 0);

    // assume intangible is already handled

    int damage = calculatedDamage;
    const auto savedBlock = block;
    block = std::max(0, block-damage);
    damage -= savedBlock;

    // buffer triggers before tungsten rod in the game's implementation
    // cases where tungsten rod would prevent damage // todo check if this is true
    if (damage > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        damage = 0;
    }

    if (hasStatus<PS::THORNS>()) {
        bc.addToTop( Actions::DamageEnemy(enemyIdx, getStatus<PS::THORNS>()) );
    }

    if (hasStatus<PS::FLAME_BARRIER>()) {
        bc.addToTop( Actions::DamageEnemy(enemyIdx, getStatus<PS::FLAME_BARRIER>()) );
    }

    // StaticDischargePower.onAttacked sees the damage after block and Buffer, before Torii and Tungsten Rod.
    if (damage > 0 && hasStatus<PS::STATIC_DISCHARGE>()) {
        for (int i = 0; i < getStatus<PS::STATIC_DISCHARGE>(); ++i) {
            bc.addToTop( Actions::ChannelOrb(Orb::LIGHTNING) );
        }
    }

    if (damage > 0 && damage <=5 && hasRelic<RelicId::TORII>()) {
        damage = 1;
    }

    if (damage > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        damage -= 1;
    }

    if (damage > 0) {
        lastAttackUnblockedDamage = damage;

        if (hasStatus<PS::PLATED_ARMOR>()) {
            decrementStatus<PS::PLATED_ARMOR>();
        }

        if (bc.monsters.arr[enemyIdx].hasStatus<MS::PAINFUL_STABS>()) {
            bc.addToBot( Actions::MakeTempCardInDiscard({CardId::WOUND}) );
        }

        hpWasLost(bc, damage, false);

    } else {
        lastAttackUnblockedDamage = 0;
    }
}

void Player::loseHp(BattleContext &bc, int amount, bool selfDamage) {
    if (hasStatus<PS::INTANGIBLE>()) {
        amount = 1;
    }

    // HP loss skips block, but BufferPower.onAttackedToChangeDamage still stops it.
    if (amount > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        return;
    }

    if (amount > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        amount -= 1;
        if (amount == 0) {
            return;
        }
    }

    hpWasLost(bc, amount, selfDamage);
}

void Player::hpWasLost(BattleContext &bc, int amount, bool selfDamage) {
    assert(amount > 0);

    bool wasBloodied = curHp <= maxHp/2;

    curHp = std::max(0, curHp-amount);

    if (selfDamage && hasStatus<PS::RUPTURE>()) {
        buff<PS::STRENGTH>(getStatus<PS::RUPTURE>());
    }

    // todo - does order acquired matter with centennial/runic?
    // relics wasHpLost
    // -centennial-puzzle
    // -emotion chip
    // -runic cube
    // -self forming clay

    if (hasRelic<RelicId::CENTENNIAL_PUZZLE>()) {
        setHasRelic<RelicId::CENTENNIAL_PUZZLE>(false);
        bc.addToTop( Actions::DrawCards(3) );
    }

    if (hasRelic<RelicId::EMOTION_CHIP>()) {
        emotionChipPulse = true;
    }

    if (hasRelic<RelicId::SELF_FORMING_CLAY>()) {
        buff<PS::NEXT_TURN_BLOCK>(3);
    }

    if (hasRelic<RelicId::RUNIC_CUBE>()) {
        bc.addToTop( Actions::DrawCards(1) );
    }

    if (hasRelic<RelicId::RED_SKULL>() && !redSkullActive && curHp <= maxHp/2) {
        buff<PS::STRENGTH>(3);
        redSkullActive = true;
    }

    bc.cards.onTookDamage();
    ++timesDamagedThisCombat;

    if (curHp <= 0) {
        wouldDie(bc);
    }
}

void Player::wouldDie(BattleContext &bc) {
    // assume fairy and lizard tail heal for greater than zero - max hp is not less than ~8
    curHp = 0;
    if  (!hasRelic<RelicId::MARK_OF_THE_BLOOM>()) {
        for (int i = 0; i < bc.potionCapacity; i++) {
            if (bc.potions[i] == Potion::FAIRY_POTION) {
                bc.discardPotion(i);
                const int healAmount = std::max(
                        1,
                        static_cast<int>(static_cast<float>(maxHp) * (hasRelic<R::SACRED_BARK>() ? 0.6f : 0.3f))
                );
                heal(healAmount);
                return;
            }
        }

        if (hasRelic<RelicId::LIZARD_TAIL>()) {
            setHasRelic<RelicId::LIZARD_TAIL>(false);
            heal(maxHp/2);
            return;
        }
    }

    bc.outcome = Outcome::PLAYER_LOSS;
}

void Player::addNightmare(const CardInstance &c) {
    if (nightmareCount < MAX_NIGHTMARES) {
        nightmareCards[nightmareCount] = c;
        nightmareCards[nightmareCount].setCostForTurn(c.cost); // NightmarePower runs resetAttributes on its copy
        ++nightmareCount;
        notePowerGained(kNightmareInOrder);
    }
}

void Player::applyEndOfTurnPowers(BattleContext &bc) {
    // Powers trigger in the order they were gained, with Constricted always last. It shows when No Draw comes off: HP loss
    // from Combust or Constricted draws (Runic Cube, Centennial Puzzle) only if No Draw has already gone.
    if (bomb1) {
        bc.addToBot( Actions::DamageAllEnemy(bomb1) );
    }
    bomb1 = bomb2;
    bomb2 = bomb3;
    bomb3 = 0;

    for (const PlayerStatus status : powersInGameOrder(*this)) {
        if (!hasStatusRuntime(status)) {
            continue;
        }
        const std::pair<PlayerStatus, std::int16_t> pair{status, statusMap.at(status)};

        switch (pair.first) {
            case PS::BURST:
                bc.addToBot(Actions::RemoveStatus<PS::BURST>());
                break;

            case PS::COMBUST:
                if (!bc.monsters.areMonstersBasicallyDead()) {
                    bc.addToBot(Actions::PlayerLoseHp(combustHpLoss, true)); // todo combust doesnt stack hp loss correctly
                    bc.addToBot(Actions::DamageAllEnemy(pair.second));
                }
                break;

            case PS::CONSTRICTED:
                bc.addToBot(Actions::DamagePlayer(pair.second));
                break;

            case PS::NO_DRAW:
                bc.addToBot(Actions::RemoveStatus<PS::NO_DRAW>());
                break;

            case PS::AMPLIFY:
                bc.addToBot(Actions::RemoveStatus<PS::AMPLIFY>());
                break;

            case PS::DOUBLE_TAP:
                bc.addToBot(Actions::RemoveStatus<PS::DOUBLE_TAP>());
                break;

            case PS::ENTANGLED:
                bc.addToBot(Actions::RemoveStatus<PS::ENTANGLED>());
                break;

            case PS::EQUILIBRIUM: // goes through limbo like any retained card, so onRetained runs
                for (int i = 0; i < bc.cards.cardsInHand; ++i) {
                    if (!bc.cards.hand[i].isEthereal()) {
                        bc.cards.hand[i].retain = true;
                    }
                }
                break;

            case PS::ESTABLISHMENT:
                bc.addToBot(Actions::EstablishmentAction(pair.second));
                break;

            case PS::STUDY:
                bc.addToBot(Actions::MakeTempCardInDrawPile(CardInstance(CardId::INSIGHT), pair.second, true));
                break;

            case PS::LOSE_DEXTERITY:
                bc.addToBot(Actions::DebuffPlayer<PS::DEXTERITY>(-pair.second));
                bc.addToBot(Actions::RemoveStatus<PS::LOSE_DEXTERITY>());
                break;

            case PS::LOSE_STRENGTH:
                bc.addToBot(Actions::DebuffPlayer<PS::STRENGTH>(-pair.second));
                bc.addToBot(Actions::RemoveStatus<PS::LOSE_STRENGTH>());
                break;

            case PS::OMEGA:
                bc.addToBot(Actions::DamageAllEnemy(pair.second));
                break;

            case PS::RAGE:
                removeStatus<PS::RAGE>();
                break;

            case PS::REBOUND:
                bc.addToBot(Actions::RemoveStatus<PS::REBOUND>());
                break;

            case PS::REGEN:
                bc.addToTop(Actions::HealPlayer(pair.second));
                bc.addToTop(Actions::DecrementStatus<PS::REGEN>());
                break;

            case PS::RETAIN_CARDS:
                if (bc.cards.cardsInHand > 0 && !hasRelic<R::RUNIC_PYRAMID>() && !hasStatus<PS::EQUILIBRIUM>()) {
                    bc.addToBot( Actions::RetainCardsAction(pair.second) );
                }
                break;

            case PS::RITUAL:
                bc.addToBot(Actions::BuffPlayer<PS::STRENGTH>(pair.second));
                break;
                // case TheBomb

            case PS::WRAITH_FORM: // an ApplyPowerAction with a negative DexterityPower, so Artifact stops it
                bc.addToBot(Actions::DebuffPlayer<PS::DEXTERITY>(pair.second, false));
                break;

            default:
                break;
        }
    }

}

void Player::applyAtEndOfRoundPowers() {
    if (hasStatus<PS::FRAIL>()) {
        decrementIfNotJustApplied<PS::FRAIL>();
    }

    if (hasStatus<PS::VULNERABLE>()) {
        decrementIfNotJustApplied<PS::VULNERABLE>();
    }

    if (hasStatus<PS::WEAK>()) {
        decrementIfNotJustApplied<PS::WEAK>();
    }

    // handling this later so it is not removed before block check
//    if (hasStatusInternal<PS::BLUR>()) {
//        decrementStatus<PS::BLUR>();
//    }

    if (hasStatus<PS::DOUBLE_DAMAGE>()) {
        decrementIfNotJustApplied<PS::DOUBLE_DAMAGE>();
    }

    // handle elsewhere
//    if (hasStatusInternal<PS::DRAW_REDUCTION>()) {
//        decrementStatus<PS::DRAW_REDUCTION>();
//    }

    if (hasStatus<PS::DUPLICATION>()) {
        decrementStatus<PS::DUPLICATION>();
    }

    if (hasStatus<PS::EQUILIBRIUM>()) {
        decrementStatus<PS::EQUILIBRIUM>();
    }

    if (hasStatus<PS::INTANGIBLE>()) {
        decrementStatus<PS::INTANGIBLE>();
    }

    if (hasStatus<PS::NO_BLOCK>()) {
        decrementStatus<PS::NO_BLOCK>();
    }

    if (hasStatus<PS::WAVE_OF_THE_HAND>()) { // the whole power goes, not one of its stacks
        removeStatus<PS::WAVE_OF_THE_HAND>();
    }
}

void Player::applyStartOfTurnRelics(BattleContext &bc) {
    // AbstractPlayer.applyStartOfTurnRelics runs each relic's atTurnStart in the order the relics were gained.
    for (const auto r : bc.turnStartRelics) {
        switch (r) {
            case R::ART_OF_WAR: {
                if (true) {
                    if (attacksPlayedThisTurn == 0) {
                        bc.addToBot(Actions::GainEnergy(1));
                    }
                }
                break;
            }
            case R::BRIMSTONE: {
                if (true) {
                    buff<PS::STRENGTH>(2);
                    for (int i = 0; i < bc.monsters.monsterCount; i++) {
                        if (bc.monsters.arr[i].isTargetable()) {
                            bc.monsters.arr[i].buff<MS::STRENGTH>(1);
                        }
                    }
                }
                break;
            }
            case R::CAPTAINS_WHEEL: {
                if (true) {
                    if (bc.turn == 2) {
                        bc.addToBot( Actions::GainBlock(18) );
                    }
                }
                break;
            }
            case R::DAMARU: {
                if (true) {
                    bc.queueMantra(1);
                }
                break;
            }
            case R::EMOTION_CHIP: {
                if (true && emotionChipPulse) {
                    emotionChipPulse = false;
                    bc.addToBot( Actions::ImpulseAction() );
                }
                break;
            }
            case R::HAPPY_FLOWER: {
                if (true) {
                    if (++happyFlowerCounter == 3) {
                        happyFlowerCounter = 0;
                        bc.addToBot( Actions::GainEnergy(1) );
                    }
                }
                break;
            }
            case R::HORN_CLEAT: {
                if (true) {
                    if (bc.turn == 1) {
                        bc.addToBot( Actions::GainBlock(14) );
                    }
                }
                break;
            }
            case R::HOVERING_KITE: {
                if (true) {
                    kiteTriggeredThisTurn = false;
                }
                break;
            }
            case R::INCENSE_BURNER: {
                if (true) {
                    if (++incenseBurnerCounter == 6) {
                        incenseBurnerCounter = 0;
                        bc.addToBot( Actions::BuffPlayer<PS::INTANGIBLE>(1) );
                    }
                }
                break;
            }
            case R::INSERTER: {
                if (true) {
                    if (++inserterCounter == 2) {
                        inserterCounter = 0; // todo
                        bc.addToBot( {[=](BattleContext &bc) {
                            bc.player.increaseOrbSlots(1);
                        }});
                    }
                }
                break;
            }
            case R::MERCURY_HOURGLASS: {
                if (true) {
                    bc.addToBot( Actions::DamageAllEnemy(3) );
                }
                break;
            }
            case R::NECRONOMICON: {
                if (true) {
                    haveUsedNecronomiconThisTurn = false;
                }
                break;
            }
            case R::ORANGE_PELLETS: {
                if (true) {
                    orangePelletsCardTypesPlayed.reset();
                }
                break;
            }
            default:
                break;
        }
    }
}

void Player::applyStartOfTurnPowers(BattleContext &bc) {
    // ****** Player powers atStartOfTurn ******
    int nightmaresDone = 0;
    const auto nextNightmare = [&]() { // MakeTempCardInHandAction makes a stat equivalent copy of the power's card
        bc.addToBot( Actions::MakeTempCardInHand(nightmareCards[nightmaresDone++], NIGHTMARE_COPIES) );
    };
    for (const PlayerStatus status : powersInGameOrder(*this, true)) {
        if (status == kNightmareInOrder) {
            nextNightmare();
            continue;
        }
        if (!hasStatusRuntime(status)) {
            continue;
        }
        const std::pair<PlayerStatus, std::int16_t> pair{status, statusMap.at(status)};

        switch (pair.first) {
            case PS::BATTLE_HYMN:
                if (!bc.monsters.areMonstersBasicallyDead()) {
                    bc.addToBot(Actions::MakeTempCardInHand(CardId::SMITE, false, pair.second) );
                }
                break;

            case PS::BIAS: // an ApplyPowerAction with a negative FocusPower, so Artifact stops it
                bc.addToBot( Actions::DebuffPlayer<PS::FOCUS>(-pair.second, false) );
                break;

            case PS::CREATIVE_AI: // The game's combat pools stay the class's own, with or without Prismatic Shard.
                for (int i = 0; i < pair.second; ++i) {
                    bc.addToBot( Actions::MakeTempCardInHand(getTrulyRandomCardInCombat(bc.cardRandomRng, cc, CardType::POWER)) );
                }
                break;

            case PS::ECHO_FORM:
                echoFormCardsDoubled = 0;
                break;

            case PS::BLASPHEMER: // HP loss: Intangible holds it to 1 and Buffer stops it, and the power goes anyway
                removeStatus<PS::BLASPHEMER>();
                bc.addToBot( Actions::PlayerLoseHp(99999, true) );
                break;

            case PS::FASTING: // the game's EnergyDownPower. The energy for the turn has been set by the time this runs.
                bc.addToBot( Actions::LoseEnergy(pair.second) );
                break;

            case PS::FORESIGHT:
                if (bc.cards.drawPile.empty()) { // EmptyDeckShuffleAction's constructor runs the relics' onShuffle
                    bc.onShuffle();
                    bc.addToTop( Actions::EmptyDeckShuffle() );
                }
                bc.addToBot( Actions::ScryAction(pair.second) );
                break;

            case PS::FLAME_BARRIER:
                removeStatus<PS::FLAME_BARRIER>();
                break;

            case PS::HELLO_WORLD:
                if (!bc.monsters.areMonstersBasicallyDead()) {
                    for (int i = 0; i < pair.second; ++i) {
                        bc.addToBot( Actions::MakeTempCardInHand(getRandomClassCardOfRarity(bc.cardRandomRng, cc, CardRarity::COMMON)) );
                    }
                }
                break;

            case PS::INFINITE_BLADES:
                if (!bc.monsters.areMonstersBasicallyDead()) {
                    bc.addToBot(Actions::MakeTempCardInHand(CardId::SHIV, hasStatus<PS::MASTER_REALITY>(), pair.second) );
                }
                break;

            case PS::LOOP:
                if (orbSlots > 0) {
                    for (int i = 0; i < pair.second; ++i) {
                        orbOnStartOfTurn(bc, 0);
                        orbOnEndOfTurn(bc, 0);
                    }
                }
                break;

            case PS::MAGNETISM:
                for (int i = 0; i < pair.second; ++i) {
                    bc.addToBot( Actions::MakeTempCardInHand(sts::getTrulyRandomColorlessCardInCombat(bc.cardRandomRng)) );
                }
                break;

            case PS::MAYHEM:
                // MayhemPower queues an action that queues PlayTopCardAction, so the card is played after the turn's draw.
                for (int i = 0; i < pair.second; i++) {
                    bc.addToBot( Actions::PlayTopCardDelayed() );
                }
                break;

            case PS::NEXT_TURN_BLOCK:
                bc.addToBot( Actions::GainBlock(pair.second) );
                removeStatus<PS::NEXT_TURN_BLOCK>();
                break;

                // todo  case NightMarePower: this.bc.addToBot(new MakeTempCardInHandAction(this.card, this.amount)); bc.addToBot remove power

            case PS::PANACHE:
                panacheCounter = 5;
                break;

            case PS::PHANTASMAL:
                decrementStatus<PS::PHANTASMAL>();
                bc.addToBot( Actions::BuffPlayer<PS::DOUBLE_DAMAGE>() );
                break;

                // time maze not used in standard modes

            case PS::WRATH_NEXT_TURN:
                removeStatus<PS::WRATH_NEXT_TURN>();
                bc.addToBot( Actions::ChangeStance(Stance::WRATH) );
                break;

            default:
                break;
        }

    }

    while (nightmaresDone < nightmareCount) { // the order was full when it came
        nextNightmare();
    }
    for (int i = 0; i < nightmareCount; ++i) {
        notePowerLost(kNightmareInOrder);
    }
    nightmareCount = 0;
}

void Player::applyStartOfTurnPostDrawRelics(BattleContext &bc) {
    // ****** Player Relics AtTurnStartPostDraw, in the order gained ******
    const bool tongsFirst = bc.miscBits.test(3);
    if (tongsFirst && hasRelic<R::WARPED_TONGS>()) {
        bc.addToBot(Actions::UpgradeRandomCardAction());
    }

    if (hasRelic<R::POCKETWATCH>()) {
        if (cardsPlayedThisTurn <= 3) {
            bc.addToBot(Actions::DrawCards(3));
        }
    }

    if (!tongsFirst && hasRelic<R::WARPED_TONGS>()) {
        bc.addToBot(Actions::UpgradeRandomCardAction());
    }
}

void Player::applyStartOfTurnPostDrawPowers(BattleContext &bc) {
    // ****** Player Powers AtStartOfTurnPostDraw ******
    for (const PlayerStatus status : powersInGameOrder(*this)) {
        if (!hasStatusRuntime(status)) {
            continue;
        }
        const std::pair<PlayerStatus, std::int16_t> pair{status, statusMap.at(status)};

        switch (pair.first) {
            case PS::BRUTALITY:
                bc.addToBot( Actions::DrawCards(pair.second) );
                bc.addToBot( Actions::PlayerLoseHp(pair.second, true) );
                break;

            case PS::DEMON_FORM:
                bc.addToBot( Actions::BuffPlayer<PS::STRENGTH>(pair.second) );
                break;

            case PS::DEVOTION: // with no Mantra yet and 10 or more to give, it goes straight to Divinity and gives no Mantra
                if (!hasStatus<PS::MANTRA>() && pair.second >= 10) {
                    bc.addToBot( Actions::ChangeStance(Stance::DIVINITY) );
                } else {
                    bc.queueMantra(pair.second);
                }
                break;

            case PS::DRAW_CARD_NEXT_TURN:
                bc.addToBot( Actions::DrawCards(pair.second) );
                removeStatus<PS::DRAW_CARD_NEXT_TURN>();
                break;

            case PS::NOXIOUS_FUMES:
                bc.addToBot( Actions::DebuffAllEnemy<MS::POISON>(pair.second) );
                break;

            case PS::TOOLS_OF_THE_TRADE:
                bc.addToBot( Actions::DrawCards(pair.second) );
                bc.addToBot( Actions::DiscardAction(pair.second) );
                break;

            default:
                break;
        }
    }
}

void Player::rechargeEnergy(BattleContext &bc) {
    if (hasRelic<R::ICE_CREAM>()) {
        gainEnergy(energyPerTurn);
    } else {
        energy = energyPerTurn;
    }

    // ****** Player powers onEnergyRecharge ******
    if (hasStatus<PS::COLLECT>()) {
        decrementStatus<PS::COLLECT>();
        bc.addToBot(Actions::MakeTempCardInHand(CardId::MIRACLE, true) );
    }

    if (hasStatus<PS::DEVA>()) {
        gainEnergy(devaFormEnergyPerTurn);
        devaFormEnergyPerTurn += getStatus<PS::DEVA>();
    }

    if (hasStatus<PS::ENERGIZED>()) {
        gainEnergy(getStatus<PS::ENERGIZED>());
        removeStatus<PS::ENERGIZED>();
    }

    if (hasStatus<PS::ENERGIZED_BLUE>()) {
        gainEnergy(getStatus<PS::ENERGIZED_BLUE>());
        removeStatus<PS::ENERGIZED_BLUE>();
    }
}

std::ostream& printIfHaveStatus(const Player &p, std::ostream &os, PlayerStatus s) {
    if (!p.hasStatusRuntime(s)){
        return os;
    }

    std::string desc = playerStatusStrings[(int)s];
    return os << "(" << desc << "," << p.getStatusRuntime(s) << ")" << ", ";
}

namespace sts {

//    void printRelicCounters(std::ostream &os, const Player &p) {
//        const std::string s = " ";
//
//        if (p.hasRelic<R::INK_BOTTLE>()) {
//            os << s << "inkBottleCounter: " << p.inkBottleCounter;
//        }
//
//        if (p.hasRelic<R::INSERTER>()) {
//            os << "inserterCounter: " << p.inserterCounter << " ";
//        }
//
//        if (p.hasRelic<R::NUNCHAKU>()) {
//            os << "nunchakuCounter: " << p.nunchakuCounter << " ";
//        }
//
//        if (p.hasRelic<R::LETTER_OPENER>()) {
//            os << "skillsPlayedThisTurn: " << p.skillsPlayedThisTurn << " ";
//        }
//
//    }
//
//    void printStatusCounters(std::ostream &os, const Player &p) {
//
//        if (p.hasStatusInternal<PS::COMBUST>()) {
//            os << "combustHpLoss: " << p.combustHpLoss << " ";
//        }
//
//        if (p.hasStatusInternal<PS::DEVA>()) {
//            os << "devaFormEnergyPerTurn: " << p.devaFormEnergyPerTurn << " ";
//        }
//
//        if (p.hasStatusInternal<PS::ECHO_FORM>()) {
//            os << "panacheCounter: " << p.panacheCounter << " ";
//        }
//
//
//        if (p.hasStatusInternal<PS::PANACHE>()) {
//            os << "panacheCounter: " << p.panacheCounter << " ";
//        }
//    }

    void printRelics(std::ostream &os, const Player &p) {
        os << "\t" << "Relics: ";

        // this is slow and stupid
        int i = static_cast<int>(R::AKABEKO);
        while (i <= static_cast<int>(R::WRIST_BLADE)) {
            auto r = static_cast<RelicId>(i);

            if (p.hasRelicRuntime(r)) {
                os << getRelicName(r) << ", ";
            }

            ++i;
        }
        os << "\n";
    }

    void printAllInfos(std::ostream &os, const Player &p) {
        const std::string s = ", ";

        os << "\t" << "Relic Counters: "
            << "" << "happyFlowerCounter: " << static_cast<int>(p.happyFlowerCounter)
            << s << "incenseBurnerCounter: " << static_cast<int>(p.incenseBurnerCounter)
            << s << "inkBottleCounter: " << static_cast<int>(p.inkBottleCounter)
            << s << "inserterCounter: " << static_cast<int>(p.inserterCounter)
            << s << "nunchakuCounter: " << static_cast<int>(p.nunchakuCounter)
            << s << "penNibCounter: " << static_cast<int>(p.penNibCounter)
            << s << "sundialCounter: " << static_cast<int>(p.sundialCounter)

            << "\n\t" << "Status Counters: "
            << "" << "combustHpLoss: " << static_cast<int>(p.combustHpLoss)
            << s << "devaFormEnergyPerTurn: " << static_cast<int>(p.devaFormEnergyPerTurn)
            << s << "echoFormCardsDoubled: " << static_cast<int>(p.echoFormCardsDoubled)
            << s << "panacheCounter: " << static_cast<int>(p.panacheCounter)
            << s << "TheBomb: { 1=" << static_cast<int>(p.bomb1)
                << ", 2=" << static_cast<int>(p.bomb2)
                << ", 3=" << static_cast<int>(p.bomb3)
                << " }"

            << "\n\t" << "Misc: "
            << "" << "cardsPlayedThisTurn: " << static_cast<int>(p.cardsPlayedThisTurn)
            << s << "attacksPlayedThisTurn: " << static_cast<int>(p.attacksPlayedThisTurn)
            << s << "skillsPlayedThisTurn: " << static_cast<int>(p.skillsPlayedThisTurn);


            os << s << "orangePelletsCardTypesPlayed: {"
                << (p.orangePelletsCardTypesPlayed.test(0) ? "Attack," : "")
                << (p.orangePelletsCardTypesPlayed.test(1) ? "Skill," : "")
                << (p.orangePelletsCardTypesPlayed.test(2) ? "Power," : "")
                << "}";

            os << s << "cardsDiscardedThisTurn: " << static_cast<int>(p.cardsDiscardedThisTurn);
            os << s << "gold: " << static_cast<int>(p.gold) << '\n';
    }


    void printStatusEffects(std::ostream &os, const Player &p) {
        os << "\tStatusEffects: {";
        if (p.hasStatus<PS::CONFUSED>()) {
            os << "(Confused), ";
        }
        if (p.hasStatus<PS::CORRUPTION>()) {
            os << "(Corruption), ";
        }
        if (p.hasStatus<PS::BARRICADE>()) {
            os << "(Barricade), ";
        }

        printIfHaveStatus(p, os, PS::ARTIFACT);
        printIfHaveStatus(p, os, PS::DEXTERITY);
        printIfHaveStatus(p, os, PS::FOCUS);
        printIfHaveStatus(p, os, PS::STRENGTH);
        for (auto pair : p.statusMap) {
            printIfHaveStatus(p, os, pair.first);
        }
        os << "}\n";
    }

    std::ostream& operator<<(std::ostream &os, const Player &p) {
        os << "Player: {\n";

        os << "\t" << "hp:(" << p.curHp << "/" << p.maxHp << ")"
            << " energy:(" << p.energy << "/" << static_cast<int>(p.energyPerTurn)
        << ") block:(" << p.block << ")\n";

        if (p.orbSlots > 0) {
            static constexpr const char* const orbNames[] {"-", "Dark", "Frost", "Plasma", "Lightning"};
            os << "\t" << "orbs: {";
            for (int i = 0; i < p.orbSlots; ++i) {
                os << orbNames[static_cast<int>(p.orbs[i])] << "(" << p.orbPassiveAmount(i) << "," << p.orbEvokeAmount(i) << ") ";
            }
            os << "}\n";
        }

        printStatusEffects(os, p);
        printRelics(os, p);
        printAllInfos(os, p);

        os << "}\n";
        return os;
    }



}
