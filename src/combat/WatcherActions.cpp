//
// The Watcher's actions, stances, Mantra and Scry. Ported with hewzhew/SpireSim (MIT) and klottick/spire-gym (MIT) as references
// and checked against the game's bytecode, which wins where they differ.
//

#include <algorithm>

#include "combat/Actions.h"
#include "combat/BattleContext.h"
#include "game/Game.h"

using namespace sts;

// MakeTempCardInHandAction and the others, and the ShowCardAnd... effects that DiscoveryAction and the like use directly.
void BattleContext::upgradeIfMasterReality(CardInstance &c) const {
    if (player.hasStatus<PS::MASTER_REALITY>() && c.getType() != CardType::CURSE && c.getType() != CardType::STATUS) {
        c.upgrade();
    }
}

// ChangeStanceAction: nothing happens, and nothing triggers, if the stance doesn't change.
void BattleContext::changeStance(Stance newStance) {
    const Stance oldStance = player.stance;
    if (oldStance == newStance) {
        return;
    }

    for (int i = 0; i < player.powerOrderCount; ++i) { // AbstractPower.onChangeStance, in the order of the powers
        const PlayerStatus s = player.powerOrder[i];
        if (s == PS::MENTAL_FORTRESS) {
            addToBot( Actions::GainBlock(player.getStatus<PS::MENTAL_FORTRESS>()) );
        } else if (s == PS::RUSHDOWN && newStance == Stance::WRATH) {
            addToBot( Actions::DrawCards(player.getStatus<PS::RUSHDOWN>()) );
        }
    }

    if (player.hasRelic<R::VIOLET_LOTUS>() && oldStance == Stance::CALM) {
        addToBot( Actions::GainEnergy(1) );
    }

    if (oldStance == Stance::CALM) { // CalmStance.onExitStance
        addToBot( Actions::GainEnergy(2) );
    }

    player.stance = newStance;

    if (newStance == Stance::DIVINITY) { // DivinityStance.onEnterStance
        addToBot( Actions::GainEnergy(3) );
    }

    for (const auto &c : cards.discardPile) { // triggerExhaustedCardsOnStanceChange, which only looks at the discard pile
        if (c.getId() == CardId::FLURRY_OF_BLOWS) {
            addToBot( Actions::DiscardToHandAction(c.getUniqueId()) );
        }
    }
}

// MantraPower: the first one is added as it comes, and only stackPower looks for 10.
void BattleContext::applyMantra(int amount) {
    if (!player.hasStatus<PS::MANTRA>()) {
        player.buff<PS::MANTRA>(amount);
        return;
    }
    int total = player.getStatus<PS::MANTRA>() + amount;
    if (total >= 10) {
        addToTop( Actions::ChangeStance(Stance::DIVINITY) );
        total -= 10;
    }
    if (total > 0) {
        player.setStatusValueNoChecks<PS::MANTRA>(total);
    } else {
        player.removeStatus<PS::MANTRA>();
    }
}

void BattleContext::queueMantra(int amount) {
    player.mantraGained = static_cast<std::int16_t>(player.mantraGained + amount);
    addToBot( Actions::ApplyMantra(amount) );
}

// ScryAction: Nirvana goes off even with nothing to look at, and Weave only comes back if the screen opened.
void BattleContext::scry(int amount) {
    if (player.hasRelic<R::GOLDEN_EYE>()) {
        amount += 2;
    }
    if (monsters.areMonstersBasicallyDead()) {
        return;
    }
    if (player.hasStatus<PS::NIRVANA>()) {
        addToBot( Actions::GainBlock(player.getStatus<PS::NIRVANA>()) );
    }
    if (cards.drawPile.empty()) {
        return;
    }
    openSimpleCardSelectScreen(CardSelectTask::SCRY, std::min(amount, static_cast<int>(cards.drawPile.size())));
    cardSelectInfo.canPickZero = true;
    cardSelectInfo.canPickAnyNumber = true;
    cardSelectInfo.scry_Passed() = 0;
}

// The game moves the picks when its screen closes, and triggerOnScry goes through the discard pile after that. It runs every
// frame the action lasts, but the extra DiscardToHandActions find the card gone.
void BattleContext::finishScry() {
    for (const auto &c : cards.discardPile) {
        if (c.getId() == CardId::WEAVE) {
            addToBot( Actions::DiscardToHandAction(c.getUniqueId()) );
        }
    }
}

void BattleContext::chooseScryCard(int offsetFromTop) {
    if (offsetFromTop == CardSelectInfo::SCRY_DONE_IDX) {
        finishScry();
        return;
    }
    const int onScreen = cardSelectInfo.pickCount - 1;
    const int drawIdx = static_cast<int>(cards.drawPile.size()) - 1 - offsetFromTop;
    const CardInstance c = cards.drawPile[drawIdx];
    cards.removeFromDrawPileAtIdx(drawIdx);
    cards.moveToDiscardPile(c);
    if (offsetFromTop < onScreen) { // the next card down has moved up to this offset
        addToTop( {[=] (BattleContext &bc) {
            bc.openSimpleCardSelectScreen(CardSelectTask::SCRY, onScreen);
            bc.cardSelectInfo.canPickZero = true;
            bc.cardSelectInfo.canPickAnyNumber = true;
            bc.cardSelectInfo.scry_Passed() = offsetFromTop;
        }} );
    } else {
        finishScry();
    }
}

// MeditateAction: a pick is only marked to stay if there's room for it in the hand.
void BattleContext::chooseMeditateCard(int discardIdx) {
    const int picksLeft = cardSelectInfo.pickCount - 1;
    if (cards.cardsInHand < CardManager::MAX_HAND_SIZE) {
        CardInstance c = cards.discardPile[discardIdx];
        cards.removeFromDiscard(discardIdx);
        c.retain = true;
        cards.moveToHand(c);
    }
    if (picksLeft > 0) {
        addToTop( Actions::MeditateAction(picksLeft) );
    }
}

// OmniscienceAction: the card leaves the pile and is played at a random target, then copies of it are.
void BattleContext::chooseOmniscienceCard(int drawIdx) {
    const int playCount = cardSelectInfo.omniscience_PlayCount();
    const CardInstance c = cards.drawPile[drawIdx];
    cards.removeFromDrawPileAtIdx(drawIdx);

    // makeStatEquivalentCopy gives each copy its own uuid, unlike Double Tap's.
    CardQueueItem item(c, -1, player.energy);
    item.randomTarget = true;
    item.autoplay = true;
    item.freeToPlay = true;
    item.exhaustOnUse = true;
    addToBotCard(item);

    for (int i = 0; i < playCount - 1; ++i) {
        CardInstance copied = c;
        copied.setUniqueId(cards.nextUniqueCardId++);
        CardQueueItem copy(copied, -1, player.energy);
        copy.randomTarget = true;
        copy.autoplay = true;
        copy.freeToPlay = true;
        copy.purgeOnUse = true;
        addToBotCard(copy);
    }
}

void BattleContext::chooseForeignInfluenceCard(int choice) {
    CardInstance c(cardSelectInfo.cards[choice]);
    upgradeIfMasterReality(c);
    if (c.getId() == CardId::BLOOD_FOR_BLOOD) {  // Masterful Stab's makeCopy is a plain new card
        c.tookDamageTimes(player.timesDamagedThisCombat);
    }
    if (cardSelectInfo.foreignInfluence_Upgraded()) {
        c.setCostForTurn(0);
    }
    if (cards.cardsInHand < CardManager::MAX_HAND_SIZE) {
        cards.createTempCardInHand(c);
    } else {
        cards.createTempCardInDiscard(c);
    }
}

// ChooseWrath, then ChooseCalm. A potion's choice is the same screen as Wish's.
void BattleContext::chooseStanceOption(int choice) {
    addToBot( Actions::ChangeStance(choice == 0 ? Stance::WRATH : Stance::CALM) );
}

// The three option cards' onChoseThisOption. Their stats are on the option cards, which Wish+ upgrades.
void BattleContext::chooseWishOption(int choice) {
    const bool upgraded = cardSelectInfo.wish_Upgraded();
    switch (choice) {
        case 0:
            addToBot( Actions::BuffPlayer<PS::STRENGTH>(upgraded ? 4 : 3) );
            break;
        case 1: {
            const int gold = upgraded ? 30 : 25;
            addToBot( {[=] (BattleContext &bc) { bc.player.gainGold(bc, gold); }} );
            break;
        }
        default:
            addToBot( Actions::BuffPlayer<PS::PLATED_ARMOR>(upgraded ? 8 : 6) );
            break;
    }
}

namespace {
    // The attacks with the HEALING tag, which CardLibrary.getAnyColorCard(type, rarity) leaves out and these pools don't.
    bool isHealingAttack(CardId id) {
        return id == CardId::BITE || id == CardId::FEED || id == CardId::LESSON_LEARNED || id == CardId::REAPER;
    }

    bool canOfferAttack(CardId id) {
        return getCardType(id) == CardType::ATTACK && !isHealingAttack(id);
    }

    // CardLibrary.getAnyColorCard(type, rarity): its shuffle rolls cardRandomRng and changes nothing, since getRandomCard sorts
    // the list by id and picks with cardRng. The pools here hold every type, in that order.
    CardId anyColorAttack(Random &cardRandomRng, Random &cardRng, CardRarity rarity) {
        const CardId *pool = AnyColorTypeCardPool::commonCards;
        int poolSize = AnyColorTypeCardPool::commonPoolSize;
        if (rarity == CardRarity::UNCOMMON) {
            pool = AnyColorTypeCardPool::uncommonCards;
            poolSize = AnyColorTypeCardPool::uncommonPoolSize;
        } else if (rarity == CardRarity::RARE) {
            pool = AnyColorTypeCardPool::rareCards;
            poolSize = AnyColorTypeCardPool::rarePoolSize;
        }

        int attackCount = 0;
        for (int i = 0; i < poolSize; ++i) {
            if (canOfferAttack(pool[i])) {
                ++attackCount;
            }
        }

        cardRandomRng.randomLong();
        int pick = cardRng.random(attackCount - 1);
        for (int i = 0; i < poolSize; ++i) {
            if (canOfferAttack(pool[i]) && pick-- == 0) {
                return pool[i];
            }
        }
        return CardId::INVALID;
    }
}

Action Actions::ApplyMantra(int amount) {
    return {[=] (BattleContext &bc) {
        bc.applyMantra(amount);
    }};
}

Action Actions::ScryAction(int amount) {
    return {[=] (BattleContext &bc) {
        bc.scry(amount);
    }};
}

Action Actions::PressEndTurnButton() {
    return {[] (BattleContext &bc) {
        bc.callEndTurnEarlySequence(true);  // the card's UseCardAction is still in the queue
    }};
}

Action Actions::SkipEnemiesTurn() {
    return {[] (BattleContext &bc) {
        bc.skipMonsterTurn = true;
    }};
}

Action Actions::LoseEnergy(int amount) {
    return {[=] (BattleContext &bc) {
        bc.player.energy = std::max(0, bc.player.energy - amount);
    }};
}

Action Actions::CollectAction(int effect, bool useEnergy) {
    return {[=] (BattleContext &bc) {
        const int total = effect + (bc.player.hasRelic<R::CHEMICAL_X>() ? 2 : 0);
        if (total > 0) {
            bc.addToBot( Actions::BuffPlayer<PS::COLLECT>(total) );
            if (useEnergy) {
                bc.player.useEnergy(bc.player.energy);
            }
        }
    }};
}

// There's no check for 0 here, so with no energy it still makes an Expunger that hits no times.
Action Actions::ConjureBladeAction(int effect, bool useEnergy) {
    return {[=] (BattleContext &bc) {
        CardInstance expunger(CardId::EXPUNGER);
        expunger.specialData = static_cast<std::int16_t>(effect + (bc.player.hasRelic<R::CHEMICAL_X>() ? 2 : 0));
        bc.addToBot( Actions::MakeTempCardInDrawPile(expunger, 1, true) );
        if (useEnergy) {
            bc.player.useEnergy(bc.player.energy);
        }
    }};
}

Action Actions::CrushJointsAction(int idx, int amount) {
    return {[=] (BattleContext &bc) {
        if (bc.player.previousCardTypePlayed == CardType::SKILL) {
            bc.addToTop( Actions::DebuffEnemy<MS::VULNERABLE>(idx, amount, false) );
        }
    }};
}

// DevaPower starts its energy at 1 and each one stacked adds another.
Action Actions::DevaFormAction() {
    return {[] (BattleContext &bc) {
        bc.player.devaFormEnergyPerTurn = static_cast<std::int16_t>(bc.player.hasStatus<PS::DEVA>() ? bc.player.devaFormEnergyPerTurn + 1 : 1);
        bc.player.buff<PS::DEVA>(1);
    }};
}

Action Actions::EndTurnDeathAction() {
    return {[] (BattleContext &bc) {
        if (!bc.player.hasStatus<PS::BLASPHEMER>()) {
            bc.player.buff<PS::BLASPHEMER>(1);
        }
    }};
}

Action Actions::EstablishmentAction(int amount) {
    return {[=] (BattleContext &bc) {
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            auto &c = bc.cards.hand[i];
            if (c.hasSelfRetain() || c.retain) {
                c.modifyCostForCombat(-amount);
            }
        }
    }};
}

Action Actions::FearNoEvilAction(int idx, int damage) {
    return {[=] (BattleContext &bc) {
        if (bc.monsters.arr[idx].isAttacking()) {
            bc.addToTop( Actions::ChangeStance(Stance::CALM) );
        }
        bc.addToTop( Actions::AttackEnemy(idx, damage) );
    }};
}

Action Actions::FollowUpAction() {
    return {[] (BattleContext &bc) {
        if (bc.player.previousCardTypePlayed == CardType::ATTACK) {
            bc.addToTop( Actions::GainEnergy(1) );
        }
    }};
}

Action Actions::ForeignInfluenceAction(bool upgraded) {
    return {[=] (BattleContext &bc) {
        std::array<CardId, 3> choices {CardId::INVALID, CardId::INVALID, CardId::INVALID};
        int count = 0;
        while (count < 3) {
            const int roll = bc.cardRandomRng.random(99);
            const CardRarity rarity = roll < 55 ? CardRarity::COMMON : (roll < 85 ? CardRarity::UNCOMMON : CardRarity::RARE);
            const CardId id = anyColorAttack(bc.cardRandomRng, bc.cardRng, rarity);
            if (std::find(choices.begin(), choices.begin() + count, id) == choices.begin() + count) {
                choices[count++] = id;
            }
        }
        bc.openSimpleCardSelectScreen(CardSelectTask::FOREIGN_INFLUENCE, 1);
        bc.cardSelectInfo.cards = choices;
        bc.cardSelectInfo.foreignInfluence_Upgraded() = upgraded ? 1 : 0;
    }};
}

Action Actions::HaltAction(int block, int additional) {
    return {[=] (BattleContext &bc) {
        bc.addToTop( Actions::GainBlock(bc.player.stance == Stance::WRATH ? block + additional : block) );
    }, false};
}

Action Actions::HeadStompAction(int idx, int amount) {
    return {[=] (BattleContext &bc) {
        if (bc.player.previousCardTypePlayed == CardType::ATTACK) {
            bc.addToTop( Actions::DebuffEnemy<MS::WEAK>(idx, amount, false) );
        }
    }};
}

Action Actions::IndignationAction(int amount) {
    return {[=] (BattleContext &bc) {
        if (bc.player.stance == Stance::WRATH) {
            for (int i = 0; i < bc.monsters.monsterCount; ++i) {
                bc.addToBot( Actions::DebuffEnemy<MS::VULNERABLE>(i, amount, false) );
            }
        } else {
            bc.addToBot( Actions::ChangeStance(Stance::WRATH) );
        }
    }};
}

Action Actions::InnerPeaceAction(int amount) {
    return {[=] (BattleContext &bc) {
        if (bc.player.stance == Stance::CALM) {
            bc.addToTop( Actions::DrawCards(amount) );
        } else {
            bc.addToTop( Actions::ChangeStance(Stance::CALM) );
        }
    }};
}

// JudgementAction queues an InstantKillAction, which sets the HP to 0 before any damage, so nothing like Invincible can stop it.
Action Actions::JudgementAction(int idx, int cutoff) {
    return {[=] (BattleContext &bc) {
        if (bc.monsters.arr[idx].curHp <= cutoff) {
            bc.addToTop( {[=] (BattleContext &inner) {
                auto &m = inner.monsters.arr[idx];
                if (!m.isDeadOrEscaped()) {
                    m.curHp = 0;
                    m.die(inner);
                    inner.checkCombat();
                }
            }} );
        }
    }};
}

// The roll is on miscRng, among the master deck's cards that can still be upgraded, and only the master deck's copy changes.
Action Actions::LessonLearnedAction(int idx, int damage) {
    return {[=] (BattleContext &bc) {
        auto &m = bc.monsters.arr[idx];
        if (m.isDeadOrEscaped()) {
            return;
        }
        m.attacked(bc, damage);

        const bool killed = !m.hasStatus<MS::MINION>()
                && !m.isAlive()
                && !m.isHalfDead()
                && !(m.hasStatus<MS::REGROW>() && bc.monsters.monstersAlive > 0);
        if (killed && !bc.upgradableDeckIdxs.empty()) {
            const int pick = bc.miscRng.random(0, static_cast<int>(bc.upgradableDeckIdxs.size()) - 1);
            const std::int16_t entry = bc.upgradableDeckIdxs[pick];
            bc.lessonLearnedUpgrades.push_back(static_cast<std::int16_t>(entry & ~BattleContext::SEARING_BLOW_DECK_IDX));
            if (!(entry & BattleContext::SEARING_BLOW_DECK_IDX)) { // Searing Blow can always be upgraded again
                bc.upgradableDeckIdxs.erase(bc.upgradableDeckIdxs.begin() + pick);
            }
        }
        bc.checkCombat();
    }};
}

// With no more cards than picks there's no screen: all of them come back and are marked, even those a full hand leaves behind.
Action Actions::MeditateAction(int count) {
    return {[=] (BattleContext &bc) {
        auto &discard = bc.cards.discardPile;
        if (discard.empty() || count <= 0) {
            return;
        }
        if (static_cast<int>(discard.size()) > count) {
            bc.openSimpleCardSelectScreen(CardSelectTask::MEDITATE, count);
            return;
        }
        int i = 0;
        while (i < static_cast<int>(discard.size())) {
            discard[i].retain = true;
            if (bc.cards.cardsInHand < CardManager::MAX_HAND_SIZE) {
                const CardInstance c = discard[i];
                bc.cards.removeFromDiscard(i);
                bc.cards.moveToHand(c);
            } else {
                ++i;
            }
        }
    }};
}

Action Actions::OmniscienceAction(int playCount) {
    return {[=] (BattleContext &bc) {
        if (bc.cards.drawPile.empty()) {
            return;
        }
        bc.openSimpleCardSelectScreen(CardSelectTask::OMNISCIENCE, 1);
        bc.cardSelectInfo.omniscience_PlayCount() = playCount;
    }};
}

Action Actions::SanctityAction(int draw) {
    return {[=] (BattleContext &bc) {
        if (bc.player.previousCardTypePlayed == CardType::SKILL) {
            bc.addToTop( Actions::DrawCards(draw) );
        }
    }};
}

// Every monster's MarkPower.triggerMarks queues a LoseHPAction behind this card's UseCardAction.
Action Actions::TriggerMarksAction() {
    return {[] (BattleContext &bc) {
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (m.hasStatus<MS::MARK>()) {
                bc.addToBot( Actions::MonsterLoseHp(i, m.getStatus<MS::MARK>()) );
            }
        }
    }};
}

// lastDamageTaken is what the hit took from the monster's HP, which stops at the HP it had.
Action Actions::WallopAction(int idx, int damage) {
    return {[=] (BattleContext &bc) {
        auto &m = bc.monsters.arr[idx];
        if (m.isDeadOrEscaped()) {
            return;
        }
        const int hpBefore = m.curHp;
        m.attacked(bc, damage);
        const int taken = hpBefore - m.curHp;
        if (taken > 0) {
            bc.addToTop( Actions::GainBlock(taken) );
        }
        bc.checkCombat();
    }};
}

Action Actions::ChooseStanceAction() {
    return {[] (BattleContext &bc) {
        bc.openSimpleCardSelectScreen(CardSelectTask::CHOOSE_STANCE, 1);
    }};
}

Action Actions::WishAction(bool upgraded) {
    return {[=] (BattleContext &bc) {
        bc.openSimpleCardSelectScreen(CardSelectTask::WISH, 1);
        bc.cardSelectInfo.cards = {CardId::BECOME_ALMIGHTY, CardId::FAME_AND_FORTUNE, CardId::LIVE_FOREVER};
        bc.cardSelectInfo.wish_Upgraded() = upgraded ? 1 : 0;
    }};
}
