#include "Logic.h"
#include "Rando/StaticData/StaticData.h"

namespace Rando {

namespace Logic {

// if we keep adding more groups we should probably make this an enum or somthing
static constexpr int CONFINEMENT_GROUP_SONGS = DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE_BOSS + 1;

int SceneIdToDungeon(SceneId sceneId) {
    switch (sceneId) {
        case SCENE_MITURIN:
        case SCENE_MITURIN_BS:
            return DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE;
        case SCENE_HAKUGIN:
        case SCENE_HAKUGIN_BS:
            return DUNGEON_SCENE_INDEX_SNOWHEAD_TEMPLE;
        case SCENE_SEA:
        case SCENE_SEA_BS:
            return DUNGEON_SCENE_INDEX_GREAT_BAY_TEMPLE;
        case SCENE_INISIE_N:
        case SCENE_INISIE_R:
        case SCENE_INISIE_BS:
            return DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE;
        default:
            return -1;
    }
}

int DungeonItemToDungeon(RandoItemId itemId) {
    int dungeon = -1;
    switch (itemId) {
        case RI_WOODFALL_SMALL_KEY:
        case RI_WOODFALL_BOSS_KEY:
        case RI_WOODFALL_STRAY_FAIRY:
        case RI_REMAINS_ODOLWA:
            dungeon = DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE;
            break;
        case RI_SNOWHEAD_SMALL_KEY:
        case RI_SNOWHEAD_BOSS_KEY:
        case RI_SNOWHEAD_STRAY_FAIRY:
        case RI_REMAINS_GOHT:
            dungeon = DUNGEON_SCENE_INDEX_SNOWHEAD_TEMPLE;
            break;
        case RI_GREAT_BAY_SMALL_KEY:
        case RI_GREAT_BAY_BOSS_KEY:
        case RI_GREAT_BAY_STRAY_FAIRY:
        case RI_REMAINS_GYORG:
            dungeon = DUNGEON_SCENE_INDEX_GREAT_BAY_TEMPLE;
            break;
        case RI_STONE_TOWER_SMALL_KEY:
        case RI_STONE_TOWER_BOSS_KEY:
        case RI_STONE_TOWER_STRAY_FAIRY:
        case RI_REMAINS_TWINMOLD:
            dungeon = DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE;
            break;
        default:
            return -1;
    }
    return dungeon;
}

RandoOptionId DungeonItemPlacementOption(RandoItemId itemId) {
    if (DungeonItemToDungeon(itemId) < 0 && itemId != RI_CLOCK_TOWN_STRAY_FAIRY) {
        return RO_MAX;
    }
    switch (Rando::StaticData::Items[itemId].randoItemType) {
        case RITYPE_SMALL_KEY:
            return RO_PLACEMENT_SMALL_KEYS;
        case RITYPE_BOSS_KEY:
            return RO_PLACEMENT_BOSS_KEYS;
        case RITYPE_STRAY_FAIRY:
            return RO_PLACEMENT_STRAY_FAIRIES;
        default:
            return RO_MAX;
    }
}

bool StaysAtVanillaCheck(RandoItemId itemId, const RandoSaveInfo& saveInfo) {
    RandoOptionId placementOption = DungeonItemPlacementOption(itemId);
    if (placementOption == RO_MAX) {
        return false;
    }
    switch (saveInfo.randoSaveOptions[placementOption]) {
        case RO_DUNGEON_ITEM_VANILLA:
            return true;
        case RO_DUNGEON_ITEM_OWN_DUNGEON:
            return DungeonItemToDungeon(itemId) < 0;
        default:
            return false;
    }
}

int RandoItemIdToDungeon(RandoItemId itemId) {
    switch (itemId) {
        case RI_REMAINS_ODOLWA:
        case RI_REMAINS_GOHT:
        case RI_REMAINS_GYORG:
        case RI_REMAINS_TWINMOLD:
            return RANDO_SAVE_OPTIONS[RO_SHUFFLE_BOSS_REMAINS] == RO_REMAINS_SHUFFLE_OWN_DUNGEON
                       ? DungeonItemToDungeon(itemId)
                       : -1;
        default:
            break;
    }

    RandoOptionId placementOption = DungeonItemPlacementOption(itemId);
    if (placementOption == RO_MAX || RANDO_SAVE_OPTIONS[placementOption] != RO_DUNGEON_ITEM_OWN_DUNGEON) {
        return -1;
    }
    return DungeonItemToDungeon(itemId);
}

bool IsSongLocationItem(RandoItemId itemId) {
    static const std::array<bool, RI_MAX> songLocationItems = [] {
        std::array<bool, RI_MAX> items{};
        for (auto& [randoCheckId, randoStaticCheck] : Rando::StaticData::Checks) {
            if (randoStaticCheck.randoCheckType == RCTYPE_SONG) {
                items[randoStaticCheck.randoItemId] = true;
            }
        }
        return items;
    }();
    return itemId < RI_MAX && songLocationItems[itemId];
}

int CheckIdToConfinementGroup(RandoCheckId checkId) {
    auto& randoStaticCheck = Rando::StaticData::Checks[checkId];
    if (RANDO_SAVE_OPTIONS[RO_SHUFFLE_SONGS] == RO_SONG_SHUFFLE_SONG_LOCATIONS &&
        randoStaticCheck.randoCheckType == RCTYPE_SONG) {
        return CONFINEMENT_GROUP_SONGS;
    }
    return SceneIdToDungeon(randoStaticCheck.sceneId);
}

int RandoItemIdToConfinementGroup(RandoItemId itemId) {
    if (RANDO_SAVE_OPTIONS[RO_SHUFFLE_SONGS] == RO_SONG_SHUFFLE_SONG_LOCATIONS && IsSongLocationItem(itemId)) {
        return CONFINEMENT_GROUP_SONGS;
    }
    return RandoItemIdToDungeon(itemId);
}

bool IsMajorRandoItem(RandoItemId itemId) {
    if (itemId == RI_HEART_CONTAINER) {
        return true; // the 4 boss rewards, not Heart Pieces
    }
    RandoItemType itemType = Rando::StaticData::Items[itemId].randoItemType;
    return itemType == RITYPE_MAJOR || itemType == RITYPE_MASK;
}

bool IsMajorRandoCheck(RandoCheckId checkId) {
    // A check's tier is whatever its own vanilla item's tier is.
    return IsMajorRandoItem(Rando::StaticData::Checks[checkId].randoItemId);
}

// Whether itemId's tier (major/minor) matches checkId's tier, when the Major/Minor Split option is on. Junk is exempt
// (there's nothing about it worth protecting a tier for), and the check is skipped entirely when the option is off.
static bool MatchesMajorMinorSplit(RandoItemId itemId, RandoCheckId checkId) {
    if (RANDO_SAVE_OPTIONS[RO_MAJOR_MINOR_SPLIT] != RO_GENERIC_YES ||
        Rando::StaticData::Items[itemId].randoItemType == RITYPE_JUNK) {
        return true;
    }
    return IsMajorRandoItem(itemId) == IsMajorRandoCheck(checkId);
}

bool IsItemAllowedAtCheck(RandoItemId itemId, RandoCheckId checkId) {
    int confinedGroup = RandoItemIdToConfinementGroup(itemId);
    return confinedGroup < 0 || confinedGroup == CheckIdToConfinementGroup(checkId);
}

size_t SelectItemForCheck(const std::vector<RandoItemId>& itemPool, const std::vector<RandoCheckId>& checkPool,
                          RandoCheckId checkId) {
    int group = CheckIdToConfinementGroup(checkId);
    if (group >= 0) {
        int confinedItems = 0;
        for (RandoItemId itemId : itemPool) {
            if (RandoItemIdToConfinementGroup(itemId) == group) {
                confinedItems++;
            }
        }
        if (confinedItems > 0) {
            int groupChecks = 1; // this check was already removed from checkPool
            for (RandoCheckId other : checkPool) {
                if (CheckIdToConfinementGroup(other) == group) {
                    groupChecks++;
                }
            }
            if (confinedItems >= groupChecks) {
                for (size_t i = itemPool.size(); i-- > 0;) {
                    if (RandoItemIdToConfinementGroup(itemPool[i]) == group &&
                        MatchesMajorMinorSplit(itemPool[i], checkId)) {
                        return i;
                    }
                }
                // No confined item here also matches this check's tier: fall through. If the split truly cannot be
                // honored (not enough of this tier left for the checks that need it), the loops below will either
                // find a tier-appropriate item anyway or, failing that, make generation retry rather than place a
                // mismatched item silently.
            }
        }
    }

    // Major/Minor Split: prefer an item of this check's own tier, but only while there is still enough of that tier
    // left in the pool for every check of that tier that still needs one (counting this one). Once that stops being
    // true the split can't be honored for every remaining check of that tier, so the preference is dropped here
    // rather than risk failing generation over an unavoidable, often option-driven imbalance (e.g. extra Enemy Souls
    // with no matching new checks).
    if (RANDO_SAVE_OPTIONS[RO_MAJOR_MINOR_SPLIT] == RO_GENERIC_YES) {
        bool wantMajor = IsMajorRandoCheck(checkId);
        int tierItems = 0;
        for (RandoItemId itemId : itemPool) {
            if (Rando::StaticData::Items[itemId].randoItemType != RITYPE_JUNK &&
                IsMajorRandoItem(itemId) == wantMajor) {
                tierItems++;
            }
        }
        if (tierItems > 0) {
            int tierChecks = 1; // this check
            for (RandoCheckId other : checkPool) {
                if (IsMajorRandoCheck(other) == wantMajor) {
                    tierChecks++;
                }
            }
            if (tierItems >= tierChecks) {
                for (size_t i = itemPool.size(); i-- > 0;) {
                    if (Rando::StaticData::Items[itemPool[i]].randoItemType != RITYPE_JUNK &&
                        IsMajorRandoItem(itemPool[i]) == wantMajor && IsItemAllowedAtCheck(itemPool[i], checkId)) {
                        return i;
                    }
                }
            }
        }
    }

    for (size_t i = itemPool.size(); i-- > 0;) {
        if (IsItemAllowedAtCheck(itemPool[i], checkId)) {
            return i;
        }
    }
    return itemPool.size();
}

void PreplaceConfinedItems(std::vector<RandoCheckId>& checkPool, std::vector<RandoItemId>& itemPool) {
    std::map<int, std::vector<RandoCheckId>> groupChecks;
    for (RandoCheckId checkId : checkPool) {
        int group = CheckIdToConfinementGroup(checkId);
        if (group >= 0) {
            groupChecks[group].push_back(checkId);
        }
    }

    std::set<RandoCheckId> placedChecks;
    std::set<size_t> placedItems;
    for (size_t i = 0; i < itemPool.size(); i++) {
        int group = RandoItemIdToConfinementGroup(itemPool[i]);
        if (group < 0) {
            continue; // not confined to a dungeon
        }
        auto& checks = groupChecks[group];
        if (checks.empty()) {
            continue; // dungeon out of room: leave it for the general pool
        }

        // Prefer a check whose tier matches this item's, but settle for any check in the group rather than leave the
        // item unplaced (PreplaceConfinedItems only runs for No Logic / Nearly No Logic, which never fail generation).
        std::vector<size_t> matchingTier;
        for (size_t c = 0; c < checks.size(); c++) {
            if (MatchesMajorMinorSplit(itemPool[i], checks[c])) {
                matchingTier.push_back(c);
            }
        }
        size_t pick =
            matchingTier.empty() ? Ship_Random(0, checks.size()) : matchingTier[Ship_Random(0, matchingTier.size())];
        RandoCheckId checkId = checks[pick];
        checks.erase(checks.begin() + pick);

        RANDO_SAVE_CHECKS[checkId].shuffled = true;
        RANDO_SAVE_CHECKS[checkId].randoItemId = itemPool[i];
        placedChecks.insert(checkId);
        placedItems.insert(i);
    }

    // Remove the placed items and checks from the pools.
    std::vector<RandoItemId> remainingItems;
    for (size_t i = 0; i < itemPool.size(); i++) {
        if (placedItems.find(i) == placedItems.end()) {
            remainingItems.push_back(itemPool[i]);
        }
    }
    itemPool = std::move(remainingItems);

    std::vector<RandoCheckId> remainingChecks;
    for (RandoCheckId checkId : checkPool) {
        if (placedChecks.find(checkId) == placedChecks.end()) {
            remainingChecks.push_back(checkId);
        }
    }
    checkPool = std::move(remainingChecks);
}

void FillChecksRandomly(std::vector<RandoCheckId>& checkPool, std::vector<RandoItemId>& itemPool) {
    for (size_t i = 0; i < itemPool.size(); i++) {
        std::swap(itemPool[i], itemPool[Ship_Random(0, itemPool.size())]);
    }

    if (RANDO_SAVE_OPTIONS[RO_MAJOR_MINOR_SPLIT] != RO_GENERIC_YES) {
        for (RandoCheckId checkId : checkPool) {
            if (checkId == RC_UNKNOWN) {
                continue;
            }
            RANDO_SAVE_CHECKS[checkId].shuffled = true;
            RANDO_SAVE_CHECKS[checkId].randoItemId = itemPool.back();
            itemPool.pop_back();
        }
        checkPool.clear();
        itemPool.clear();
        return;
    }

    std::vector<RandoItemId> majorItems, minorItems, junkItems;
    for (RandoItemId itemId : itemPool) {
        if (Rando::StaticData::Items[itemId].randoItemType == RITYPE_JUNK) {
            junkItems.push_back(itemId);
        } else if (IsMajorRandoItem(itemId)) {
            majorItems.push_back(itemId);
        } else {
            minorItems.push_back(itemId);
        }
    }

    std::vector<RandoCheckId> majorChecks, minorChecks;
    for (RandoCheckId checkId : checkPool) {
        if (checkId == RC_UNKNOWN) {
            continue;
        }
        if (IsMajorRandoCheck(checkId)) {
            majorChecks.push_back(checkId);
        } else {
            minorChecks.push_back(checkId);
        }
    }

    // Junk is tier-agnostic filler, so use it to cover a shortfall on either side before resorting to a mismatch.
    auto topUp = [&](std::vector<RandoItemId>& items, size_t target) {
        while (items.size() < target && !junkItems.empty()) {
            items.push_back(junkItems.back());
            junkItems.pop_back();
        }
    };
    topUp(majorItems, majorChecks.size());
    topUp(minorItems, minorChecks.size());

    // Any remaining shortfall is a genuine imbalance (e.g. more major items than major checks, which some option
    // combinations can produce) that junk alone can't fix. These logic modes never fail generation, so fill it from
    // the other tier's surplus as a last resort, keeping every check filled.
    auto assign = [&](std::vector<RandoCheckId>& checks, std::vector<RandoItemId>& items,
                      std::vector<RandoItemId>& other) {
        for (RandoCheckId checkId : checks) {
            RandoItemId itemId;
            if (!items.empty()) {
                itemId = items.back();
                items.pop_back();
            } else if (!junkItems.empty()) {
                itemId = junkItems.back();
                junkItems.pop_back();
            } else if (!other.empty()) {
                itemId = other.back();
                other.pop_back();
            } else {
                continue; // nothing left; shouldn't happen since the pools were balanced to equal sizes beforehand
            }
            RANDO_SAVE_CHECKS[checkId].shuffled = true;
            RANDO_SAVE_CHECKS[checkId].randoItemId = itemId;
        }
    };
    assign(majorChecks, majorItems, minorItems);
    assign(minorChecks, minorItems, majorItems);

    checkPool.clear();
    itemPool.clear();
}

} // namespace Logic

} // namespace Rando
