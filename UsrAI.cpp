#include "UsrAI.h"
#include<set>
#include <iostream>
#include<unordered_map>
#include<list>
#include <cstdlib>

using namespace std;
tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

const int kThinkInterval = 10;
const int kThirdWaveFrame = 21000;

struct PendingBuild
{
    int instructionId = -1;
    int type = -1;
    int blockDR = -1;
    int blockUR = -1;
};

struct StrategyState
{
    PendingBuild pendingBuild;
    set<int> rejectedBuildSites;
    unordered_map<int, int> lastArmyOrder;
    int towerResearchId = -1;
    int towerResearchFrame = -1;
    int scoutWaypoint = 0;
    int finalAttackFrame = -1;
    bool printedStart = false;
    bool printedBronze = false;
};

struct ResourceBudget
{
    int wood;
    int food;
    int stone;
    int gold;

    explicit ResourceBudget(const tagInfo &info)
        : wood(info.Wood), food(info.Meat), stone(info.Stone), gold(info.Gold)
    {
    }

    bool spend(int needWood, int needFood, int needStone, int needGold)
    {
        if (wood < needWood || food < needFood ||
                stone < needStone || gold < needGold)
            return false;

        wood -= needWood;
        food -= needFood;
        stone -= needStone;
        gold -= needGold;
        return true;
    }
};

StrategyState state;

double distanceSquare(double dr1, double ur1, double dr2, double ur2)
{
    const double dr = dr1 - dr2;
    const double ur = ur1 - ur2;
    return dr * dr + ur * ur;
}

const tagBuilding *findBuilding(const tagInfo &info, int type, bool completedOnly = false)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == type && (!completedOnly || building.Percent >= 100))
            return &building;
    }
    return nullptr;
}

const tagBuilding *findEnemySiege(const tagInfo &info)
{
    for (const tagBuilding &building : info.enemy_buildings) {
        if (building.Type == BUILDING_SIEGE)
            return &building;
    }
    return nullptr;
}

const tagArmy *findPriest(const tagInfo &info)
{
    for (const tagArmy &army : info.armies) {
        if (army.Sort == AT_PRIEST)
            return &army;
    }
    return nullptr;
}

int countBuildings(const tagInfo &info, int type, bool completedOnly = false)
{
    int count = 0;
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == type && (!completedOnly || building.Percent >= 100))
            ++count;
    }
    return count;
}

int countArmy(const tagInfo &info, int type = -1)
{
    int count = 0;
    for (const tagArmy &army : info.armies) {
        if (army.Sort == AT_PRIEST || army.Sort == AT_SHIP)
            continue;
        if (type == -1 || army.Sort == type)
            ++count;
    }
    return count;
}

int countNormalFarmers(const tagInfo &info)
{
    int count = 0;
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort == FARMERTYPE_FARMER)
            ++count;
    }
    return count;
}

int buildingSize(int type)
{
    switch (type) {
    case BUILDING_HOME:
    case BUILDING_ARROWTOWER:
    case BUILDING_DOCK:
        return 2;
    default:
        return 3;
    }
}

void buildingCost(int type, int &wood, int &stone)
{
    wood = 0;
    stone = 0;

    switch (type) {
    case BUILDING_HOME:
        wood = BUILD_HOUSE_WOOD;
        break;
    case BUILDING_ARMYCAMP:
        wood = BUILD_ARMYCAMP_WOOD;
        break;
    case BUILDING_MARKET:
        wood = BUILD_MARKET_WOOD;
        break;
    case BUILDING_RANGE:
        wood = BUILD_RANGE_WOOD;
        break;
    case BUILDING_STABLE:
        wood = BUILD_STABLE_WOOD;
        break;
    case BUILDING_COLLAGE:
        wood = BUILD_COLLAGE_WOOD;
        break;
    case BUILDING_FARM:
        wood = BUILD_FARM_WOOD;
        break;
    case BUILDING_ARROWTOWER:
        stone = BUILD_ARROWTOWER_STONE;
        break;
    default:
        break;
    }
}

bool rectanglesOverlap(int x1, int y1, int size1, int x2, int y2, int size2)
{
    return x1 < x2 + size2 && x2 < x1 + size1 &&
            y1 < y2 + size2 && y2 < y1 + size1;
}

int buildSiteKey(int type, int dr, int ur)
{
    return type * MAP_L * MAP_U + dr * MAP_U + ur;
}

bool isBuildSiteFree(const tagInfo &info, int type, int dr, int ur)
{
    if (info.theMap == nullptr)
        return false;

    const int size = buildingSize(type);
    if (dr < 1 || ur < 1 || dr + size >= MAP_L || ur + size >= MAP_U)
        return false;
    if (state.rejectedBuildSites.count(buildSiteKey(type, dr, ur)) != 0)
        return false;

    const int height = (*info.theMap)[dr][ur].height;
    for (int x = dr; x < dr + size; ++x) {
        for (int y = ur; y < ur + size; ++y) {
            const tagTerrain &terrain = (*info.theMap)[x][y];
            if (terrain.type != MAPPATTERN_GRASS || terrain.height != height)
                return false;
        }
    }

    for (const tagBuilding &building : info.buildings) {
        if (rectanglesOverlap(dr, ur, size, building.BlockDR, building.BlockUR,
                              buildingSize(building.Type)))
            return false;
    }
    for (const tagBuilding &building : info.enemy_buildings) {
        if (rectanglesOverlap(dr, ur, size, building.BlockDR, building.BlockUR,
                              buildingSize(building.Type)))
            return false;
    }
    for (const tagResource &resource : info.resources) {
        if (resource.BlockDR >= dr - 1 && resource.BlockDR <= dr + size &&
                resource.BlockUR >= ur - 1 && resource.BlockUR <= ur + size)
            return false;
    }
    return true;
}

bool findBuildSite(const tagInfo &info, int type, int &dr, int &ur)
{
    const tagBuilding *center = findBuilding(info, BUILDING_CENTER);
    if (center == nullptr)
        return false;

    // 从基地外圈逐步向外找，建筑集中，村民也不会走太远。
    for (int radius = 4; radius <= 22; ++radius) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int side = -1; side <= 1; side += 2) {
                const int x = center->BlockDR + dx;
                const int y = center->BlockUR + side * radius;
                if (isBuildSiteFree(info, type, x, y)) {
                    dr = x;
                    ur = y;
                    return true;
                }
            }
        }
        for (int dy = -radius + 1; dy < radius; ++dy) {
            for (int side = -1; side <= 1; side += 2) {
                const int x = center->BlockDR + side * radius;
                const int y = center->BlockUR + dy;
                if (isBuildSiteFree(info, type, x, y)) {
                    dr = x;
                    ur = y;
                    return true;
                }
            }
        }
    }
    return false;
}

const tagFarmer *chooseBuilder(const tagInfo &info, const tagBuilding &center)
{
    const tagFarmer *best = nullptr;
    double bestDistance = numeric_limits<double>::max();

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;
        const double distance = distanceSquare(farmer.DR, farmer.UR,
                                               center.BlockDR, center.BlockUR);
        if (farmer.NowState == HUMAN_STATE_IDLE)
            return &farmer;
        if (distance < bestDistance) {
            best = &farmer;
            bestDistance = distance;
        }
    }
    return best;
}

bool hasUnfinishedBuilding(const tagInfo &info)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Percent < 100)
            return true;
    }
    return false;
}

void updatePendingOrders(const tagInfo &info)
{
    if (state.pendingBuild.instructionId != -1) {
        const auto result = info.ins_ret.find(state.pendingBuild.instructionId);
        if (result != info.ins_ret.end()) {
            if (result->second != ACTION_SUCCESS) {
                state.rejectedBuildSites.insert(buildSiteKey(state.pendingBuild.type,
                                                              state.pendingBuild.blockDR,
                                                              state.pendingBuild.blockUR));
            }
            state.pendingBuild = PendingBuild();
        }
    }

    if (state.towerResearchId != -1) {
        const auto result = info.ins_ret.find(state.towerResearchId);
        if (result != info.ins_ret.end()) {
            if (result->second != ACTION_SUCCESS) {
                state.towerResearchId = -1;
                state.towerResearchFrame = -1;
            }
            else {
                state.towerResearchId = -1;
            }
        }
    }
}

bool hasBronzePrerequisites(const tagInfo &info)
{
    int completedTypes = 0;
    if (findBuilding(info, BUILDING_MARKET, true) != nullptr)
        ++completedTypes;
    if (findBuilding(info, BUILDING_RANGE, true) != nullptr)
        ++completedTypes;
    if (findBuilding(info, BUILDING_STABLE, true) != nullptr)
        ++completedTypes;
    return completedTypes >= 2;
}

int chooseNextBuilding(const tagInfo &info)
{
    const int freePopulation = info.Human_MaxNum - static_cast<int>(ceil(info.Human_Num));
    if (freePopulation <= 2 && countBuildings(info, BUILDING_HOME) < 10)
        return BUILDING_HOME;
    if (countBuildings(info, BUILDING_ARMYCAMP) == 0)
        return BUILDING_ARMYCAMP;
    if (countBuildings(info, BUILDING_MARKET) == 0)
        return BUILDING_MARKET;
    if (countBuildings(info, BUILDING_RANGE) == 0)
        return BUILDING_RANGE;

    const bool towerReady = state.towerResearchFrame != -1 &&
            info.GameFrame - state.towerResearchFrame > 350;
    if (towerReady && countBuildings(info, BUILDING_ARROWTOWER) < 2 &&
            info.GameFrame < kThirdWaveFrame)
        return BUILDING_ARROWTOWER;

    if (info.civilizationStage >= CIVILIZATION_BRONZEAGE) {
        if (countBuildings(info, BUILDING_STABLE) == 0)
            return BUILDING_STABLE;
        if (countBuildings(info, BUILDING_COLLAGE) == 0)
            return BUILDING_COLLAGE;
    }

    const int farmTarget = info.civilizationStage >= CIVILIZATION_BRONZEAGE ? 4 : 2;
    if (countBuildings(info, BUILDING_FARM) < farmTarget)
        return BUILDING_FARM;
    return -1;
}

void tryBuild(UsrAI &ai, const tagInfo &info, ResourceBudget &budget)
{
    if (state.pendingBuild.instructionId != -1 || hasUnfinishedBuilding(info))
        return;

    const int type = chooseNextBuilding(info);
    if (type == -1)
        return;

    int wood = 0;
    int stone = 0;
    buildingCost(type, wood, stone);
    if (!budget.spend(wood, 0, stone, 0))
        return;

    const tagBuilding *center = findBuilding(info, BUILDING_CENTER);
    int dr = -1;
    int ur = -1;
    if (center == nullptr || !findBuildSite(info, type, dr, ur))
        return;

    const tagFarmer *builder = chooseBuilder(info, *center);
    if (builder == nullptr)
        return;

    state.pendingBuild.instructionId = ai.HumanBuild(builder->SN, type, dr, ur);
    state.pendingBuild.type = type;
    state.pendingBuild.blockDR = dr;
    state.pendingBuild.blockUR = ur;
}

void manageTechnology(UsrAI &ai, const tagInfo &info, ResourceBudget &budget)
{
    const tagBuilding *granary = findBuilding(info, BUILDING_GRANARY, true);
    if (granary != nullptr && granary->Project == -1 &&
            state.towerResearchFrame == -1 && state.towerResearchId == -1 &&
            budget.spend(0, BUILDING_GRANARY_ARROWTOWER_FOOD, 0, 0)) {
        state.towerResearchId = ai.BuildingAction(granary->SN,
                                                  BUILDING_GRANARY_ARROWTOWER);
        state.towerResearchFrame = info.GameFrame;
    }
}

void manageCenter(UsrAI &ai, const tagInfo &info, ResourceBudget &budget)
{
    const tagBuilding *center = findBuilding(info, BUILDING_CENTER, true);
    if (center == nullptr || center->Project != -1)
        return;

    if (info.civilizationStage == CIVILIZATION_TOOLAGE &&
            hasBronzePrerequisites(info) &&
            budget.spend(0, BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD,
                         0, BUILDING_CENTER_UPGRADE_BRONZEAGE_GOLD)) {
        ai.BuildingAction(center->SN, BUILDING_CENTER_UPGRADE);
        return;
    }

    const int targetFarmers = info.civilizationStage >= CIVILIZATION_BRONZEAGE ? 20 : 15;
    const bool savingForBronze = info.civilizationStage == CIVILIZATION_TOOLAGE &&
            hasBronzePrerequisites(info) && budget.food < 950;
    if (!savingForBronze && countNormalFarmers(info) < targetFarmers &&
            info.Human_Num + 1 <= info.Human_MaxNum &&
            budget.spend(0, BUILDING_CENTER_CREATEFARMER_FOOD, 0, 0)) {
        ai.BuildingAction(center->SN, BUILDING_CENTER_CREATEFARMER);
    }
}

void trainArmy(UsrAI &ai, const tagInfo &info, ResourceBudget &budget)
{
    const int freePopulation = info.Human_MaxNum - static_cast<int>(ceil(info.Human_Num));
    if (freePopulation <= 0)
        return;

    const int armyCount = countArmy(info);
    const int targetArmy = info.civilizationStage >= CIVILIZATION_BRONZEAGE ? 22 : 8;
    const bool savingForBronze = info.civilizationStage == CIVILIZATION_TOOLAGE &&
            hasBronzePrerequisites(info) && budget.food < 950;
    if (armyCount >= targetArmy || savingForBronze)
        return;

    int placesLeft = freePopulation;
    for (const tagBuilding &building : info.buildings) {
        if (placesLeft <= 0 || building.Percent < 100 || building.Project != -1)
            continue;

        if (building.Type == BUILDING_COLLAGE &&
                info.civilizationStage >= CIVILIZATION_BRONZEAGE &&
                budget.spend(0, BUILDING_COLLAGE_CREATE_HOPLITE_FOOD,
                             0, BUILDING_COLLAGE_CREATE_HOPLITE_GOLD)) {
            ai.BuildingAction(building.SN, BUILDING_COLLAGE_CREATE_HOPLITE);
            --placesLeft;
        }
        else if (building.Type == BUILDING_STABLE) {
            if (countArmy(info, AT_SCOUT) == 0 &&
                    budget.spend(0, BUILDING_STABLE_CREATE_SCOUT_FOOD, 0, 0)) {
                ai.BuildingAction(building.SN, BUILDING_STABLE_CREATE_SCOUT);
                --placesLeft;
            }
            else if (info.civilizationStage >= CIVILIZATION_BRONZEAGE &&
                     countArmy(info, AT_CAVALRY) < 4 &&
                     budget.spend(0, BUILDING_STABLE_CREATE_CAVALRY_FOOD,
                                  0, BUILDING_STABLE_CREATE_CAVALRY_GOLD)) {
                ai.BuildingAction(building.SN, BUILDING_STABLE_CREATE_CAVALRY);
                --placesLeft;
            }
        }
        else if (building.Type == BUILDING_RANGE &&
                 budget.spend(BUILDING_RANGE_CREATE_BOWMAN_WOOD,
                              BUILDING_RANGE_CREATE_BOWMAN_FOOD, 0, 0)) {
            ai.BuildingAction(building.SN, BUILDING_RANGE_CREATE_BOWMAN);
            --placesLeft;
        }
        else if (building.Type == BUILDING_ARMYCAMP) {
            if (budget.spend(0, BUILDING_ARMYCAMP_CREATE_SLINGER_FOOD,
                             BUILDING_ARMYCAMP_CREATE_SLINGER_STONE, 0)) {
                ai.BuildingAction(building.SN, BUILDING_ARMYCAMP_CREATE_SLINGER);
                --placesLeft;
            }
            else if (budget.spend(0, BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD, 0, 0)) {
                ai.BuildingAction(building.SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
                --placesLeft;
            }
        }
    }
}

int resourceGroup(const tagInfo &info, int workObjectSN)
{
    for (const tagResource &resource : info.resources) {
        if (resource.SN != workObjectSN)
            continue;
        if (resource.Type == RESOURCE_TREE)
            return 1;
        if (resource.Type == RESOURCE_STONE)
            return 2;
        if (resource.Type == RESOURCE_GOLD)
            return 3;
        if (resource.Type != RESOURCE_FISH)
            return 0;
    }
    for (const tagBuilding &building : info.buildings) {
        if (building.SN == workObjectSN && building.Type == BUILDING_FARM &&
                building.Percent >= 100 && building.Cnt > 0)
            return 0;
    }
    return -1;
}

int nearestResource(const tagInfo &info, const tagFarmer &farmer, int group)
{
    int bestSN = -1;
    double bestDistance = numeric_limits<double>::max();

    for (const tagResource &resource : info.resources) {
        if (resource.Cnt <= 0)
            continue;

        int resourceGroupNumber = 0;
        if (resource.Type == RESOURCE_TREE)
            resourceGroupNumber = 1;
        else if (resource.Type == RESOURCE_STONE)
            resourceGroupNumber = 2;
        else if (resource.Type == RESOURCE_GOLD)
            resourceGroupNumber = 3;
        else if (resource.Type == RESOURCE_FISH)
            continue;

        if (resourceGroupNumber != group)
            continue;
        const double distance = distanceSquare(farmer.DR, farmer.UR,
                                               resource.DR, resource.UR);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestSN = resource.SN;
        }
    }

    if (group == 0) {
        for (const tagBuilding &building : info.buildings) {
            if (building.Type != BUILDING_FARM || building.Percent < 100 || building.Cnt <= 0)
                continue;
            const double distance = distanceSquare(farmer.DR, farmer.UR,
                                                   building.BlockDR, building.BlockUR);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestSN = building.SN;
            }
        }
    }
    return bestSN;
}

int mostNeededResource(const int assigned[4], const int wanted[4])
{
    int result = 0;
    double largestShortage = -numeric_limits<double>::max();
    for (int group = 0; group < 4; ++group) {
        if (wanted[group] == 0)
            continue;
        const double shortage = static_cast<double>(wanted[group] - assigned[group]) /
                wanted[group];
        if (shortage > largestShortage) {
            largestShortage = shortage;
            result = group;
        }
    }
    return result;
}

void assignFarmers(UsrAI &ai, const tagInfo &info)
{
    const int farmerCount = countNormalFarmers(info);
    if (farmerCount == 0)
        return;

    int wanted[4] = {0, 0, 0, 0};
    wanted[1] = info.Wood < 500 ? (farmerCount * 45 + 99) / 100
                                : (farmerCount * 25 + 99) / 100;
    wanted[3] = info.civilizationStage >= CIVILIZATION_BRONZEAGE
            ? (farmerCount * 20 + 99) / 100 : (farmerCount >= 10 ? 1 : 0);
    wanted[2] = info.Stone < 150 ? 1 : 0;
    wanted[0] = max(1, farmerCount - wanted[1] - wanted[2] - wanted[3]);

    int assigned[4] = {0, 0, 0, 0};
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;
        const int group = resourceGroup(info, farmer.WorkObjectSN);
        if (group >= 0)
            ++assigned[group];
    }

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER ||
                farmer.NowState != HUMAN_STATE_IDLE)
            continue;

        int group = mostNeededResource(assigned, wanted);
        int targetSN = nearestResource(info, farmer, group);
        if (targetSN == -1) {
            // 地图探索不完整时，按食物、木材、黄金、石头依次回退。
            const int fallback[] = {0, 1, 3, 2};
            for (int candidate : fallback) {
                targetSN = nearestResource(info, farmer, candidate);
                if (targetSN != -1) {
                    group = candidate;
                    break;
                }
            }
        }
        if (targetSN != -1) {
            ai.HumanAction(farmer.SN, targetSN);
            ++assigned[group];
        }
    }
}

const tagArmy *nearestEnemy(const tagInfo &info, double dr, double ur,
                            const tagBuilding *center, bool defendBase)
{
    const tagArmy *best = nullptr;
    double bestScore = numeric_limits<double>::max();

    for (const tagArmy &enemy : info.enemy_armies) {
        if (defendBase && center != nullptr &&
                distanceSquare(enemy.DR, enemy.UR,
                               center->BlockDR, center->BlockUR) > 30.0 * 30.0)
            continue;

        double score = distanceSquare(dr, ur, enemy.DR, enemy.UR);
        if (enemy.Sort == AT_STONE_THROWER)
            score -= 1000.0;
        if (score < bestScore) {
            best = &enemy;
            bestScore = score;
        }
    }
    return best;
}

const tagBuilding *nearestAttackableBuilding(const tagInfo &info,
                                              double dr, double ur)
{
    const tagBuilding *best = nullptr;
    double bestScore = numeric_limits<double>::max();
    for (const tagBuilding &building : info.enemy_buildings) {
        // 攻城武器厂必须留给祭司，普通军队只清理外围建筑。
        if (building.Type == BUILDING_SIEGE)
            continue;
        double score = distanceSquare(dr, ur, building.BlockDR, building.BlockUR);
        if (building.Type == BUILDING_ARROWTOWER)
            score -= 500.0;
        if (score < bestScore) {
            best = &building;
            bestScore = score;
        }
    }
    return best;
}

pair<double, double> oppositeCorner(const tagBuilding &center)
{
    const double dr = center.BlockDR < MAP_L / 2 ? MAP_L - 12.0 : 12.0;
    const double ur = center.BlockUR < MAP_U / 2 ? MAP_U - 12.0 : 12.0;
    return make_pair(dr, ur);
}

void manageScout(UsrAI &ai, const tagInfo &info, const tagBuilding &center,
                 bool underAttack)
{
    if (underAttack || info.GameFrame < 12000 || findEnemySiege(info) != nullptr)
        return;

    const tagArmy *scout = nullptr;
    for (const tagArmy &army : info.armies) {
        if (army.Sort == AT_SCOUT) {
            scout = &army;
            break;
        }
    }
    if (scout == nullptr || scout->NowState != HUMAN_STATE_IDLE)
        return;

    const pair<double, double> corner = oppositeCorner(center);
    const double stepDR = center.BlockDR < MAP_L / 2 ? -15.0 : 15.0;
    const double stepUR = center.BlockUR < MAP_U / 2 ? -15.0 : 15.0;
    const pair<double, double> waypoints[] = {
        corner,
        make_pair(corner.first + stepDR, corner.second),
        make_pair(corner.first, corner.second + stepUR),
        make_pair(corner.first + stepDR, corner.second + stepUR)
    };
    const pair<double, double> target = waypoints[state.scoutWaypoint % 4];
    ai.HumanMove(scout->SN,
                 max(3.0, min(static_cast<double>(MAP_L - 3), target.first)),
                 max(3.0, min(static_cast<double>(MAP_U - 3), target.second)));
    ++state.scoutWaypoint;
}

bool baseUnderAttack(const tagInfo &info, const tagBuilding &center)
{
    for (const tagArmy &enemy : info.enemy_armies) {
        if (distanceSquare(enemy.DR, enemy.UR,
                           center.BlockDR, center.BlockUR) <= 30.0 * 30.0)
            return true;
    }
    return false;
}

bool canGiveArmyOrder(int sn, int frame)
{
    const auto previous = state.lastArmyOrder.find(sn);
    return previous == state.lastArmyOrder.end() || frame - previous->second >= 60;
}

void rememberArmyOrder(int sn, int frame)
{
    state.lastArmyOrder[sn] = frame;
}

void managePriest(UsrAI &ai, const tagInfo &info, const tagBuilding &center,
                  const tagArmy &priest, bool underAttack)
{
    const tagArmy *danger = nearestEnemy(info, priest.DR, priest.UR, &center, false);
    if (danger != nullptr &&
            distanceSquare(priest.DR, priest.UR, danger->DR, danger->UR) < 11.0 * 11.0) {
        if (priest.NowState != HUMAN_STATE_WALKING ||
                distanceSquare(priest.DR0, priest.UR0,
                               center.BlockDR, center.BlockUR) > 4.0) {
            ai.HumanMove(priest.SN, center.BlockDR + 1.5, center.BlockUR + 1.5);
        }
        return;
    }

    const tagBuilding *siege = findEnemySiege(info);
    if (state.finalAttackFrame != -1 && siege != nullptr) {
        const int attackTime = info.GameFrame - state.finalAttackFrame;
        int nearbyEnemies = 0;
        for (const tagArmy &enemy : info.enemy_armies) {
            if (distanceSquare(enemy.DR, enemy.UR,
                               siege->BlockDR, siege->BlockUR) < 13.0 * 13.0)
                ++nearbyEnemies;
        }

        if (attackTime > 2200 && nearbyEnemies <= 2) {
            if (priest.WorkObjectSN != siege->SN)
                ai.HumanAction(priest.SN, siege->SN);
            return;
        }

        const double dx = center.BlockDR - siege->BlockDR;
        const double dy = center.BlockUR - siege->BlockUR;
        const double length = max(1.0, sqrt(dx * dx + dy * dy));
        const double safeDR = siege->BlockDR + dx / length * 9.0;
        const double safeUR = siege->BlockUR + dy / length * 9.0;
        if (priest.NowState == HUMAN_STATE_IDLE ||
                distanceSquare(priest.DR0, priest.UR0, safeDR, safeUR) > 9.0)
            ai.HumanMove(priest.SN, safeDR, safeUR);
        return;
    }

    if (!underAttack && priest.NowState == HUMAN_STATE_IDLE &&
            distanceSquare(priest.DR, priest.UR,
                           center.BlockDR, center.BlockUR) > 7.0 * 7.0) {
        ai.HumanMove(priest.SN, center.BlockDR + 1.5, center.BlockUR + 1.5);
    }
}

void manageArmy(UsrAI &ai, const tagInfo &info)
{
    const tagBuilding *center = findBuilding(info, BUILDING_CENTER, true);
    const tagArmy *priest = findPriest(info);
    if (center == nullptr || priest == nullptr)
        return;

    const bool underAttack = baseUnderAttack(info, *center);
    const int fightingUnits = countArmy(info);
    const bool enoughForAttack = fightingUnits >= 12 ||
            (info.GameFrame > 32000 && fightingUnits >= 6);
    if (state.finalAttackFrame == -1 && info.GameFrame > kThirdWaveFrame + 2500 &&
            !underAttack && info.civilizationStage >= CIVILIZATION_BRONZEAGE &&
            enoughForAttack) {
        state.finalAttackFrame = info.GameFrame;
    }

    managePriest(ai, info, *center, *priest, underAttack);
    manageScout(ai, info, *center, underAttack);

    const tagBuilding *siege = findEnemySiege(info);
    const pair<double, double> searchTarget = oppositeCorner(*center);
    for (const tagArmy &army : info.armies) {
        if (army.Sort == AT_PRIEST || army.Sort == AT_SHIP ||
                !canGiveArmyOrder(army.SN, info.GameFrame))
            continue;

        const tagArmy *enemy = nearestEnemy(info, army.DR, army.UR, center,
                                            state.finalAttackFrame == -1);
        if (enemy != nullptr) {
            if (army.WorkObjectSN != enemy->SN) {
                ai.HumanAction(army.SN, enemy->SN);
                rememberArmyOrder(army.SN, info.GameFrame);
            }
            continue;
        }

        if (state.finalAttackFrame != -1) {
            const tagBuilding *building = nearestAttackableBuilding(info, army.DR, army.UR);
            if (building != nullptr) {
                if (army.WorkObjectSN != building->SN) {
                    ai.HumanAction(army.SN, building->SN);
                    rememberArmyOrder(army.SN, info.GameFrame);
                }
            }
            else if (army.NowState == HUMAN_STATE_IDLE) {
                const double targetDR = siege == nullptr ? searchTarget.first : siege->BlockDR;
                const double targetUR = siege == nullptr ? searchTarget.second : siege->BlockUR;
                ai.HumanMove(army.SN, targetDR, targetUR);
                rememberArmyOrder(army.SN, info.GameFrame);
            }
        }
        else if (army.NowState == HUMAN_STATE_IDLE && army.Sort != AT_SCOUT &&
                 distanceSquare(army.DR, army.UR,
                                center->BlockDR, center->BlockUR) > 10.0 * 10.0) {
            const double offset = static_cast<double>((army.SN % 5) - 2);
            ai.HumanMove(army.SN, center->BlockDR + 4.0 + offset,
                         center->BlockUR + 4.0 - offset);
            rememberArmyOrder(army.SN, info.GameFrame);
        }
    }
}

} // namespace

void UsrAI::processData()
{
    const tagInfo info = getInfo();
    if (info.GameFrame <= 10 || info.GameFrame % kThinkInterval != 0)
        return;

    updatePendingOrders(info);
    clearInsRet();

    if (!state.printedStart) {
        DebugText(string("开始发展经济，准备防守三轮进攻。"));
        state.printedStart = true;
    }
    if (!state.printedBronze && info.civilizationStage >= CIVILIZATION_BRONZEAGE) {
        DebugText(string("已进入铜器时代，开始扩充主力部队。"));
        state.printedBronze = true;
    }

    // 所有生产共用同一份预算，避免同一帧按旧资源重复下单。
    ResourceBudget budget(info);
    manageTechnology(*this, info, budget);
    assignFarmers(*this, info);
    // 建造命令放在采集命令之后，保证被选中的村民最终执行建造。
    tryBuild(*this, info, budget);
    manageCenter(*this, info, budget);
    trainArmy(*this, info, budget);
    manageArmy(*this, info);
}
