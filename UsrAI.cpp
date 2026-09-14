#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

namespace
{
enum GatherKind
{
    GATHER_NONE,
    GATHER_FOOD,
    GATHER_WOOD,
    GATHER_STONE,
    GATHER_GOLD
};

struct AiState
{
    int lastFrame;
    int lastBuildFrame;
    int nextBuildPos;
    bool bronzeStarted;
    bool bronzeDone;
    bool towerTechStarted;
    bool towerTechDone;
    bool clubUpgradeStarted;
    bool clubUpgradeDone;
    bool woodTechStarted;
    bool woodTechDone;
    bool wheelStarted;
    bool wheelDone;
    bool compositeStarted;
    bool compositeDone;

    AiState()
        : lastFrame(-10),
          lastBuildFrame(-80),
          nextBuildPos(0),
          bronzeStarted(false),
          bronzeDone(false),
          towerTechStarted(false),
          towerTechDone(false),
          clubUpgradeStarted(false),
          clubUpgradeDone(false),
          woodTechStarted(false),
          woodTechDone(false),
          wheelStarted(false),
          wheelDone(false),
          compositeStarted(false),
          compositeDone(false)
    {
    }
};

GatherKind resourceKind(int type)
{
    if (type == RESOURCE_BUSH || type == RESOURCE_GAZELLE || type == RESOURCE_ELEPHANT)
        return GATHER_FOOD;
    if (type == RESOURCE_TREE)
        return GATHER_WOOD;
    if (type == RESOURCE_STONE)
        return GATHER_STONE;
    if (type == RESOURCE_GOLD)
        return GATHER_GOLD;
    return GATHER_NONE;
}

int dist2(int aDR, int aUR, int bDR, int bUR)
{
    const int dr = aDR - bDR;
    const int ur = aUR - bUR;
    return dr * dr + ur * ur;
}

int dist2(const tagObj &a, const tagObj &b)
{
    return dist2(a.BlockDR, a.BlockUR, b.BlockDR, b.BlockUR);
}

int findNearestResource(const tagInfo &info, const tagFarmer &farmer, GatherKind wanted)
{
    int bestSN = -1;
    double bestDistance = 0.0;

    for (const tagResource &res : info.resources) {
        if (resourceKind(res.Type) != wanted)
            continue;
        if (res.Blood <= 0 && res.Cnt <= 0)
            continue;

        const double dr = farmer.DR - res.DR;
        const double ur = farmer.UR - res.UR;
        const double d = dr * dr + ur * ur;
        if (bestSN == -1 || d < bestDistance) {
            bestSN = res.SN;
            bestDistance = d;
        }
    }

    return bestSN;
}

int findNearestFarm(const tagInfo &info, const tagFarmer &farmer)
{
    int bestSN = -1;
    int bestDistance = 0;

    for (const tagBuilding &building : info.buildings) {
        if (building.Type != BUILDING_FARM || building.Percent < 100 || building.Cnt <= 0)
            continue;

        const int d = dist2(farmer.BlockDR, farmer.BlockUR, building.BlockDR, building.BlockUR);
        if (bestSN == -1 || d < bestDistance) {
            bestSN = building.SN;
            bestDistance = d;
        }
    }

    return bestSN;
}

bool isBusyBuilding(const tagInfo &info, const tagFarmer &farmer)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Percent < 100 && farmer.WorkObjectSN == building.SN)
            return true;
    }
    return false;
}

const tagBuilding *findBuilding(const tagInfo &info, int type)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == type && building.Percent >= 100)
            return &building;
    }
    return nullptr;
}

const tagBuilding *findEnemyBuilding(const tagInfo &info, int type)
{
    for (const tagBuilding &building : info.enemy_buildings) {
        if (building.Type == type && building.Blood > 0)
            return &building;
    }
    return nullptr;
}

const tagBuilding *chooseEnemyAssaultBuilding(const tagInfo &info, const tagArmy &army)
{
    const tagBuilding *best = nullptr;
    int bestScore = INT_MAX;

    for (const tagBuilding &building : info.enemy_buildings) {
        if (building.Blood <= 0 || building.Type == BUILDING_SIEGE)
            continue;

        int score = dist2(army.BlockDR, army.BlockUR, building.BlockDR, building.BlockUR);
        if (building.Type == BUILDING_ARROWTOWER)
            score -= 10000;
        if (best == nullptr || score < bestScore) {
            best = &building;
            bestScore = score;
        }
    }

    return best;
}

int countBuilding(const tagInfo &info, int type)
{
    int count = 0;
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == type && building.Percent >= 100)
            ++count;
    }
    return count;
}

bool hasBuildingInProgress(const tagInfo &info, int type)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == type && building.Percent < 100)
            return true;
    }
    return false;
}

bool hasAnyBuildingInProgress(const tagInfo &info)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Percent < 100)
            return true;
    }
    return false;
}

int countArmy(const tagInfo &info, int type)
{
    int count = 0;
    for (const tagArmy &army : info.armies) {
        if (army.Sort == type && army.Blood > 0)
            ++count;
    }
    return count;
}

int countFarms(const tagInfo &info)
{
    int count = 0;
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == BUILDING_FARM)
            ++count;
    }
    return count;
}

GatherKind chooseJob(int foodWorkers,
                     int woodWorkers,
                     int stoneWorkers,
                     int goldWorkers,
                     int foodWeight,
                     int woodWeight,
                     int stoneWeight,
                     int goldWeight,
                     bool hasFood,
                     bool hasWood,
                     bool hasStone,
                     bool hasGold)
{
    GatherKind result = GATHER_NONE;
    double bestLoad = 0.0;

    if (hasFood) {
        result = GATHER_FOOD;
        bestLoad = static_cast<double>(foodWorkers) / foodWeight;
    }
    if (hasWood) {
        const double load = static_cast<double>(woodWorkers) / woodWeight;
        if (result == GATHER_NONE || load < bestLoad) {
            result = GATHER_WOOD;
            bestLoad = load;
        }
    }
    if (hasStone) {
        const double load = static_cast<double>(stoneWorkers) / stoneWeight;
        if (result == GATHER_NONE || load < bestLoad) {
            result = GATHER_STONE;
            bestLoad = load;
        }
    }
    if (hasGold) {
        const double load = static_cast<double>(goldWorkers) / goldWeight;
        if (result == GATHER_NONE || load < bestLoad)
            result = GATHER_GOLD;
    }

    return result;
}

int chooseBuilder(const tagInfo &info, int buildDR, int buildUR)
{
    int bestSN = -1;
    int bestDistance = 0;

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;
        if (isBusyBuilding(info, farmer))
            continue;

        const int d = dist2(farmer.BlockDR, farmer.BlockUR, buildDR, buildUR);
        if (bestSN == -1 || d < bestDistance) {
            bestSN = farmer.SN;
            bestDistance = d;
        }
    }

    return bestSN;
}

bool tryBuild(UsrAI *ai,
              const tagInfo &info,
              AiState &state,
              const tagBuilding &center,
              int buildingType)
{
    if (info.GameFrame - state.lastBuildFrame < 60)
        return false;

    static const int offsets[][2] = {
        {5, 0}, {-5, 0}, {0, 5}, {0, -5},
        {5, 5}, {-5, 5}, {5, -5}, {-5, -5},
        {9, 0}, {-9, 0}, {0, 9}, {0, -9},
        {9, 5}, {-9, 5}, {9, -5}, {-9, -5},
        {13, 0}, {-13, 0}, {0, 13}, {0, -13}
    };

    const int offsetCount = static_cast<int>(sizeof(offsets) / sizeof(offsets[0]));
    for (int i = 0; i < offsetCount; ++i) {
        const int pos = (state.nextBuildPos + i) % offsetCount;
        const int buildDR = center.BlockDR + offsets[pos][0];
        const int buildUR = center.BlockUR + offsets[pos][1];
        const int builderSN = chooseBuilder(info, buildDR, buildUR);
        if (builderSN == -1)
            return false;

        ai->HumanBuild(builderSN, buildingType, buildDR, buildUR);
        state.lastBuildFrame = info.GameFrame;
        state.nextBuildPos = pos + 1;
        return true;
    }

    return false;
}

bool startedAndFinished(bool &started, bool &done, const tagBuilding *building, int project)
{
    if (done || building == nullptr)
        return done;
    if (building->Project == project) {
        started = true;
    } else if (started && building->Project == ACT_NULL) {
        done = true;
    }
    return done;
}

const tagArmy *chooseEnemyTarget(const tagInfo &info,
                                const tagArmy &army,
                                const tagBuilding &center,
                                const map<int, int> &targetLoads)
{
    const tagArmy *best = nullptr;
    int bestScore = INT_MAX;

    for (const tagArmy &enemy : info.enemy_armies) {
        if (enemy.Blood <= 0)
            continue;
        const int centerLimit = 26;
        const int armyLimit = 14;
        if (dist2(enemy, center) > centerLimit * centerLimit &&
            dist2(enemy, army) > armyLimit * armyLimit)
            continue;

        const map<int, int>::const_iterator load = targetLoads.find(enemy.SN);
        const int assignedCount = load == targetLoads.end() ? 0 : load->second;
        int score = dist2(army, enemy);
        if (info.GameFrame >= 20000)
            score += assignedCount * 100000;
        if (enemy.Sort == AT_STONE_THROWER) {
            score -= info.GameFrame >= 20000 ? 20000 : 10000;
        } else if (enemy.Sort == AT_CHARIOT_ARCHER ||
                   enemy.Sort == AT_COMPOSITE_BOWMAN || enemy.Sort == AT_BOWMAN) {
            score -= info.GameFrame >= 20000 ? 5000 : 120;
        }
        if (best == nullptr || score < bestScore) {
            best = &enemy;
            bestScore = score;
        }
    }

    return best;
}
}

/* ============================== 主入口 ============================== */
void UsrAI::processData()
{
    const tagInfo info = getInfo();

    static AiState state;
    if (info.GameFrame < state.lastFrame)
        state = AiState();
    if (info.GameFrame - state.lastFrame < 10)
        return;
    state.lastFrame = info.GameFrame;

    const tagBuilding *center = findBuilding(info, BUILDING_CENTER);
    if (center == nullptr)
        return;

    const tagBuilding *granary = findBuilding(info, BUILDING_GRANARY);
    const tagBuilding *stock = findBuilding(info, BUILDING_STOCK);
    const tagBuilding *camp = findBuilding(info, BUILDING_ARMYCAMP);
    const tagBuilding *market = findBuilding(info, BUILDING_MARKET);
    const tagBuilding *range = findBuilding(info, BUILDING_RANGE);

    const bool toolDone = info.civilizationStage >= CIVILIZATION_TOOLAGE;
    state.bronzeDone = info.civilizationStage >= CIVILIZATION_BRONZEAGE;
    startedAndFinished(state.towerTechStarted, state.towerTechDone, granary, BUILDING_GRANARY_ARROWTOWER);
    startedAndFinished(state.clubUpgradeStarted, state.clubUpgradeDone, camp, BUILDING_ARMYCAMP_UPGRADE_CLUBMAN);
    startedAndFinished(state.woodTechStarted, state.woodTechDone, market, BUILDING_MARKET_WOOD_UPGRADE);
    startedAndFinished(state.wheelStarted, state.wheelDone, market, BUILDING_MARKET_WHEEL_UPGRADE);
    startedAndFinished(state.compositeStarted, state.compositeDone, range, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);

    const int farmerCount = static_cast<int>(info.farmers.size());
    const int slingerCount = countArmy(info, AT_SLINGER);
    const int bowCount = countArmy(info, AT_BOWMAN);
    const int rangeCount = countBuilding(info, BUILDING_RANGE);
    const int chariotArcherCount = countArmy(info, AT_CHARIOT_ARCHER);
    const int compositeCount = countArmy(info, AT_COMPOSITE_BOWMAN);
    const int fightingCount = static_cast<int>(info.armies.size()) - countArmy(info, AT_PRIEST);
    const int towerCount = countBuilding(info, BUILDING_ARROWTOWER);
    const int farmCount = countFarms(info);
    const int farmTarget = info.GameFrame >= 18000 ? 10 : 5;

    // 快速升级必须分两步：石器先升工具，工具时代才能再升铜器。
    // 原先把第一次升级也记成“铜器升级已开始”，会导致复合弓永远无法解锁。
    const int farmerTarget =
        fightingCount < 14 ? 10 :
        (fightingCount < 18 ? 12 : (state.bronzeDone ? 30 : 14));
    if (farmerCount < farmerTarget &&
        center->Project == ACT_NULL &&
        info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD &&
        info.Human_Num < info.Human_MaxNum) {
        BuildingAction(center->SN, BUILDING_CENTER_CREATEFARMER);
    } else if (!toolDone &&
               center->Project == ACT_NULL &&
               info.Meat >= BUILDING_CENTER_UPGRADE_TOOLAGE_FOOD) {
        BuildingAction(center->SN, BUILDING_CENTER_UPGRADE);
    } else if (toolDone && !state.bronzeDone &&
               !state.bronzeStarted &&
               fightingCount >= 10 &&
               farmCount >= 4 &&
               center->Project == ACT_NULL &&
               info.Meat >= BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD) {
        BuildingAction(center->SN, BUILDING_CENTER_UPGRADE);
        state.bronzeStarted = true;
    }

    // 房屋优先，避免人口卡住；其它建筑一次只造一个，更稳。
    const bool needHouse = info.Human_MaxNum - info.Human_Num <= 3.0;
    if (needHouse && !hasBuildingInProgress(info, BUILDING_HOME) && info.Wood >= BUILD_HOUSE_WOOD) {
        tryBuild(this, info, state, *center, BUILDING_HOME);
    } else if (!hasAnyBuildingInProgress(info)) {
        if (camp == nullptr && info.Wood >= BUILD_ARMYCAMP_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_ARMYCAMP);
        } else if (granary == nullptr && info.Wood >= BUILD_GRANARY_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_GRANARY);
        } else if (state.towerTechDone &&
                   towerCount < (info.GameFrame >= 18500 && fightingCount >= 20 ? 3 : 1) &&
                   info.Stone >= BUILD_ARROWTOWER_STONE) {
            tryBuild(this, info, state, *center, BUILDING_ARROWTOWER);
        } else if (range == nullptr && camp != nullptr && info.Wood >= BUILD_RANGE_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_RANGE);
        } else if (market == nullptr && granary != nullptr && info.Wood >= BUILD_MARKET_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_MARKET);
        } else if (market != nullptr && farmCount < farmTarget && info.Wood >= BUILD_FARM_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_FARM);
        } else if (rangeCount < 2 && farmCount >= 3 && info.Wood >= BUILD_RANGE_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_RANGE);
        } else if (stock == nullptr && info.Wood >= BUILD_STOCK_WOOD) {
            tryBuild(this, info, state, *center, BUILDING_STOCK);
        }
    }

    // 关键科技：箭塔守家，斧兵撑前两波，木材/车轮/复合弓服务后续造兵。
    if (granary != nullptr && granary->Project == ACT_NULL &&
        !state.towerTechStarted && !state.towerTechDone &&
        info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
        BuildingAction(granary->SN, BUILDING_GRANARY_ARROWTOWER);
    }
    if (camp != nullptr && camp->Project == ACT_NULL &&
        !state.clubUpgradeStarted && !state.clubUpgradeDone &&
        fightingCount >= 6 &&
        info.Meat >= 200) {
        BuildingAction(camp->SN, BUILDING_ARMYCAMP_UPGRADE_CLUBMAN);
    }
    if (market != nullptr && market->Project == ACT_NULL &&
        !state.woodTechStarted && !state.woodTechDone &&
        farmCount >= 3 &&
        info.Meat >= 200 &&
        info.Wood >= BUILDING_MARKET_WOOD_UPGRADE_WOOD) {
        BuildingAction(market->SN, BUILDING_MARKET_WOOD_UPGRADE);
    }
    if (state.bronzeDone && market != nullptr && market->Project == ACT_NULL &&
        !state.wheelStarted && !state.wheelDone &&
        info.Meat >= BUILDING_MARKET_WHEEL_UPGRADE_FOOD &&
        info.Wood >= BUILDING_MARKET_WHEEL_UPGRADE_WOOD) {
        BuildingAction(market->SN, BUILDING_MARKET_WHEEL_UPGRADE);
    }
    if (state.bronzeDone && range != nullptr && range->Project == ACT_NULL &&
        !state.compositeStarted && !state.compositeDone &&
        info.Meat >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD &&
        info.Wood >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD) {
        BuildingAction(range->SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
    }
    // 造兵：兵营先保命；靶场建好后持续出远程，铜器后转战车弓/复合弓。
    if (camp != nullptr && camp->Percent >= 100 && camp->Project == ACT_NULL &&
        info.Human_Num < info.Human_MaxNum) {
        if (fightingCount >= 4 && slingerCount < 1 &&
            info.Meat >= BUILDING_ARMYCAMP_CREATE_SLINGER_FOOD &&
            info.Stone >= BUILDING_ARMYCAMP_CREATE_SLINGER_STONE) {
            BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_SLINGER);
        } else if (fightingCount < (state.bronzeDone ? 28 : 24) &&
                   info.Meat >= BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD) {
            BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
        }
    }
    for (const tagBuilding &oneRange : info.buildings) {
        if (oneRange.Type != BUILDING_RANGE ||
            oneRange.Percent < 100 ||
            oneRange.Project != ACT_NULL ||
            info.Human_Num >= info.Human_MaxNum)
            continue;
        // 先把市场和农田建起来，避免靶场吃光木材后经济永久停摆。
        if (info.GameFrame < 15000 && (market == nullptr || farmCount < 3))
            continue;
        if (info.GameFrame < 15000 && fightingCount < 10)
            continue;

        if (state.wheelDone && chariotArcherCount < 14 &&
            info.Meat >= BUILDING_RANGE_CREATE_CHARIOT_ARCHER_FOOD &&
            info.Wood >= BUILDING_RANGE_CREATE_CHARIOT_ARCHER_WOOD) {
            BuildingAction(oneRange.SN, BUILDING_RANGE_CREATE_CHARIOT_ARCHER);
        } else if (state.compositeDone && compositeCount < 12 &&
                   info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD &&
                   info.Gold >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD) {
            BuildingAction(oneRange.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
        } else if (bowCount < 32 &&
                   info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD &&
                   info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
            BuildingAction(oneRange.SN, BUILDING_RANGE_CREATE_BOWMAN);
        }
    }

    // 守家：敌人靠近基地就集火，优先处理投石车和远程兵。
    const tagArmy *baseThreat = nullptr;
    int baseThreatScore = INT_MAX;
    for (const tagArmy &enemy : info.enemy_armies) {
        if (enemy.Blood <= 0 || dist2(enemy, *center) > 26 * 26)
            continue;

        int score = dist2(enemy, *center);
        if (enemy.Sort == AT_STONE_THROWER)
            score -= 10000;
        else if (enemy.Sort == AT_CHARIOT_ARCHER ||
                 enemy.Sort == AT_COMPOSITE_BOWMAN ||
                 enemy.Sort == AT_BOWMAN)
            score -= 200;

        if (baseThreat == nullptr || score < baseThreatScore) {
            baseThreat = &enemy;
            baseThreatScore = score;
        }
    }

    // 箭塔不会自动选择目标，和守军一样优先射击当前最危险的敌人。
    if (baseThreat != nullptr) {
        for (const tagBuilding &building : info.buildings) {
            if (building.Type == BUILDING_ARROWTOWER &&
                building.Percent >= 100 && building.Project != baseThreat->SN) {
                HumanAction(building.SN, baseThreat->SN);
            }
        }
    }

    const bool assaultMode = info.GameFrame > 25500 &&
                             info.enemy_armies.empty() &&
                             fightingCount >= 30;

    const tagArmy *priest = nullptr;
    for (const tagArmy &army : info.armies) {
        if (army.Sort == AT_PRIEST && army.Blood > 0) {
            priest = &army;
            break;
        }
    }

    // 祭司只负责转化和保命，不把他当普通近战单位使用。
    if (priest != nullptr) {
        const tagArmy *escapeThreat = baseThreat;
        int escapeDistance = escapeThreat == nullptr ? INT_MAX : dist2(*priest, *escapeThreat);
        if (info.GameFrame >= 20000) {
            for (const tagArmy &enemy : info.enemy_armies) {
                const int distance = dist2(*priest, enemy);
                if (enemy.Blood > 0 && distance < escapeDistance) {
                    escapeThreat = &enemy;
                    escapeDistance = distance;
                }
            }
        }

        if (baseThreat != nullptr && priest->ConvertCooldown == 0) {
            if (priest->WorkObjectSN != baseThreat->SN)
                HumanAction(priest->SN, baseThreat->SN);
        } else if (escapeThreat != nullptr &&
                   escapeDistance <= (info.GameFrame >= 20000 ? 12 * 12 : 8 * 8)) {
            const double block = static_cast<double>(BLOCKSIDELENGTH);
            int fleeDR;
            int fleeUR;
            if (info.GameFrame >= 20000) {
                // 第三波敌人多，沿威胁侧面的方向绕基地移动，避免直线折返穿过敌群。
                fleeDR = center->BlockDR +
                          (escapeThreat->BlockUR >= center->BlockUR ? -12 : 12);
                fleeUR = center->BlockUR +
                          (escapeThreat->BlockDR >= center->BlockDR ? 12 : -12);
            } else {
                fleeDR = center->BlockDR +
                          (center->BlockDR >= escapeThreat->BlockDR ? 8 : -8);
                fleeUR = center->BlockUR +
                          (center->BlockUR >= escapeThreat->BlockUR ? 8 : -8);
            }
            HumanMove(priest->SN,
                      (fleeDR + 0.5) * block,
                      (fleeUR + 0.5) * block);
        } else if (assaultMode) {
            // 清完三波以后，祭司先到攻城厂边缘；看见目标就直接转化。
            const tagBuilding *enemySiege = findEnemyBuilding(info, BUILDING_SIEGE);
            if (enemySiege != nullptr && priest->ConvertCooldown == 0 &&
                priest->WorkObjectSN != enemySiege->SN) {
                HumanAction(priest->SN, enemySiege->SN);
            } else if (dist2(priest->BlockDR, priest->BlockUR, 23, 86) > 9) {
                const double block = static_cast<double>(BLOCKSIDELENGTH);
                HumanMove(priest->SN, (23 + 0.5) * block, (86 + 0.5) * block);
            }
        } else if (info.GameFrame > 23500) {
            // 三波进攻结束后，转化敌方攻城器械厂才算真正通关。
            const tagBuilding *enemySiege = findEnemyBuilding(info, BUILDING_SIEGE);
            if (enemySiege != nullptr && priest->ConvertCooldown == 0 &&
                priest->WorkObjectSN != enemySiege->SN) {
                HumanAction(priest->SN, enemySiege->SN);
            }
        } else if (priest->NowState == HUMAN_STATE_IDLE &&
                   dist2(priest->BlockDR, priest->BlockUR,
                         center->BlockDR - 3, center->BlockUR - 3) > 9) {
            const double block = static_cast<double>(BLOCKSIDELENGTH);
            HumanMove(priest->SN,
                      (center->BlockDR - 3 + 0.5) * block,
                      (center->BlockUR - 3 + 0.5) * block);
        }
    }

    map<int, int> targetLoads;
    for (const tagArmy &army : info.armies) {
        if (army.Blood <= 0 || army.Sort == AT_SHIP)
            continue;

        if (army.Sort == AT_PRIEST)
            continue;

        const tagArmy *target = chooseEnemyTarget(info, army, *center, targetLoads);
        if (target != nullptr) {
            ++targetLoads[target->SN];
            if (army.WorkObjectSN != target->SN)
                HumanAction(army.SN, target->SN);
            continue;
        }

        if (assaultMode) {
            const tagBuilding *buildingTarget = chooseEnemyAssaultBuilding(info, army);
            if (buildingTarget != nullptr) {
                if (army.WorkObjectSN != buildingTarget->SN)
                    HumanAction(army.SN, buildingTarget->SN);
                continue;
            }

            const int slot = army.SN % 9;
            const int assaultDR = 18 + slot % 3;
            const int assaultUR = 84 + slot / 3;
            if (dist2(army.BlockDR, army.BlockUR, assaultDR, assaultUR) > 9) {
                const double block = static_cast<double>(BLOCKSIDELENGTH);
                HumanMove(army.SN,
                          (assaultDR + 0.5) * block,
                          (assaultUR + 0.5) * block);
            }
            continue;
        }

        // 没敌人时回防，不追太远，避免第二、三波来时家里没人。
        if (army.Sort != AT_PRIEST && army.NowState == HUMAN_STATE_IDLE) {
            const int slot = army.SN % 6;
            const int rallyDR = center->BlockDR + (slot % 3 - 1) * 4;
            const int rallyUR = center->BlockUR + (slot / 3 == 0 ? -5 : 5);
            if (dist2(army.BlockDR, army.BlockUR, rallyDR, rallyUR) > 16) {
                const double block = static_cast<double>(BLOCKSIDELENGTH);
                HumanMove(army.SN, (rallyDR + 0.5) * block, (rallyUR + 0.5) * block);
            }
        }
    }

    // 资源分配：先冲食物升铜，升铜后提高木材和黄金，保证靶场不断兵。
    int foodWorkers = 0;
    int woodWorkers = 0;
    int stoneWorkers = 0;
    int goldWorkers = 0;
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;
        for (const tagResource &res : info.resources) {
            if (farmer.WorkObjectSN != res.SN)
                continue;
            const GatherKind kind = resourceKind(res.Type);
            if (kind == GATHER_FOOD)
                ++foodWorkers;
            else if (kind == GATHER_WOOD)
                ++woodWorkers;
            else if (kind == GATHER_STONE)
                ++stoneWorkers;
            else if (kind == GATHER_GOLD)
                ++goldWorkers;
        }
        for (const tagBuilding &building : info.buildings) {
            if (building.Type == BUILDING_FARM && farmer.WorkObjectSN == building.SN)
                ++foodWorkers;
        }
    }

    const int foodWeight = info.Meat < 120 ? 10 : (info.Meat > 300 ? 3 : 6);
    const int woodWeight = info.Wood < 120 ? 5 : (state.bronzeDone ? 5 : 3);
    const bool needStone = towerCount < 1 ||
                           (info.GameFrame >= 17500 &&
                            towerCount < 3 &&
                            info.Stone < BUILD_ARROWTOWER_STONE);
    const int stoneWeight = 1;
    const int goldWeight = state.bronzeDone ? 3 : 1;

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER || isBusyBuilding(info, farmer))
            continue;

        int foodSN = findNearestResource(info, farmer, GATHER_FOOD);
        if (foodSN == -1)
            foodSN = findNearestFarm(info, farmer);
        const int woodSN = findNearestResource(info, farmer, GATHER_WOOD);
        const int stoneSN = findNearestResource(info, farmer, GATHER_STONE);
        const int goldSN = findNearestResource(info, farmer, GATHER_GOLD);

        bool gatheringStone = false;
        for (const tagResource &res : info.resources) {
            if (farmer.WorkObjectSN == res.SN &&
                resourceKind(res.Type) == GATHER_STONE) {
                gatheringStone = true;
                break;
            }
        }

        // 食物见底时主动调回少量伐木工，避免所有造兵和补农民同时停摆。
        if (info.Meat < 80 && foodWorkers < 6 && foodSN != -1) {
            HumanAction(farmer.SN, foodSN);
            ++foodWorkers;
            continue;
        }
        if (!needStone && gatheringStone && (woodSN != -1 || foodSN != -1)) {
            if (woodSN != -1) {
                HumanAction(farmer.SN, woodSN);
                ++woodWorkers;
            } else {
                HumanAction(farmer.SN, foodSN);
                ++foodWorkers;
            }
            continue;
        }

        if (farmer.NowState != HUMAN_STATE_IDLE)
            continue;

        const GatherKind kind = chooseJob(foodWorkers, woodWorkers, stoneWorkers, goldWorkers,
                                          foodWeight, woodWeight, stoneWeight, goldWeight,
                                          foodSN != -1, woodSN != -1,
                                          needStone && stoneSN != -1,
                                          state.bronzeDone && goldSN != -1);

        int targetSN = -1;
        if (kind == GATHER_FOOD) {
            targetSN = foodSN;
            ++foodWorkers;
        } else if (kind == GATHER_WOOD) {
            targetSN = woodSN;
            ++woodWorkers;
        } else if (kind == GATHER_STONE) {
            targetSN = stoneSN;
            ++stoneWorkers;
        } else if (kind == GATHER_GOLD) {
            targetSN = goldSN;
            ++goldWorkers;
        }

        if (targetSN != -1)
            HumanAction(farmer.SN, targetSN);
    }
}
