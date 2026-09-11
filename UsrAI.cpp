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
// 采集任务的三种分类。NONE表示当前没有可分配的资源。
enum GatherKind
{
    GATHER_NONE,
    GATHER_FOOD,
    GATHER_WOOD,
    GATHER_STONE
};

// 把游戏中的具体资源类型归入食物、木材或石头三类。
// 第一阶段只选择浆果和瞪羚作为食物，暂时不让村民招惹狮子和大象。
GatherKind getGatherKind(int resourceType)
{
    if (resourceType == RESOURCE_BUSH || resourceType == RESOURCE_GAZELLE)
        return GATHER_FOOD;
    if (resourceType == RESOURCE_TREE)
        return GATHER_WOOD;
    if (resourceType == RESOURCE_STONE)
        return GATHER_STONE;
    return GATHER_NONE;
}

// 为一个村民寻找最近的指定资源，返回资源的SN。
// 使用SN而不是数组下标，因为游戏每帧都会打乱资源和单位列表的顺序。
int findNearestResource(const tagInfo &info,
                        const tagFarmer &farmer,
                        GatherKind wantedKind)
{
    int nearestSN = -1;
    double nearestDistance = 0.0;

    for (const tagResource &resource : info.resources) {
        if (getGatherKind(resource.Type) != wantedKind)
            continue;

        // 活着的动物用Blood表示是否有效，普通资源用Cnt表示剩余数量。
        if (resource.Blood <= 0 && resource.Cnt <= 0)
            continue;

        const double dr = farmer.DR - resource.DR;
        const double ur = farmer.UR - resource.UR;
        const double distance = dr * dr + ur * ur;

        if (nearestSN == -1 || distance < nearestDistance) {
            nearestSN = resource.SN;
            nearestDistance = distance;
        }
    }

    return nearestSN;
}

// 判断村民是否正在修建尚未完工的建筑。建造过程中有些帧会短暂显示为空闲，
// 不能只依赖NowState，否则采集命令会把正在施工的村民调走。
bool isConstructingBuilding(const tagInfo &info, const tagFarmer &farmer)
{
    for (const tagBuilding &building : info.buildings) {
        if (building.Percent < 100 && farmer.WorkObjectSN == building.SN)
            return true;
    }
    return false;
}

// 按给定权重选择当前最缺人的任务。正常权重是3:2:1，资源不足时
// 调用处会临时提高食物或木材权重。
GatherKind chooseGatherKind(int foodWorkers,
                            int woodWorkers,
                            int stoneWorkers,
                            int foodWeight,
                            int woodWeight,
                            int stoneWeight,
                            bool hasFood,
                            bool hasWood,
                            bool hasStone)
{
    GatherKind result = GATHER_NONE;
    double bestLoad = 0.0;

    if (hasFood) {
        result = GATHER_FOOD;
        bestLoad = static_cast<double>(foodWorkers) / foodWeight;
    }
    if (hasWood &&
        (result == GATHER_NONE ||
         static_cast<double>(woodWorkers) / woodWeight < bestLoad)) {
        result = GATHER_WOOD;
        bestLoad = static_cast<double>(woodWorkers) / woodWeight;
    }
    if (hasStone &&
        (result == GATHER_NONE ||
         static_cast<double>(stoneWorkers) / stoneWeight < bestLoad))
        result = GATHER_STONE;

    return result;
}

// 计算两个单位（或建筑）之间的块坐标距离平方。
// 防守判断只比较远近，不开平方可以让代码更简单，也避免不必要的浮点计算。
int blockDistanceSquared(const tagObj &first, const tagObj &second)
{
    const int dr = first.BlockDR - second.BlockDR;
    const int ur = first.BlockUR - second.BlockUR;
    return dr * dr + ur * ur;
}
}

/* ============================== 主入口 ============================== */
void UsrAI::processData()
{
    // 1. 每次决策首先取得当前帧的完整游戏快照。
    // info只在本次调用中有效，下一次调用时必须重新获取。
    const tagInfo info = getInfo();

    // processData可能在同一帧被多次调用。每隔10帧决策一次，既能及时响应，
    // 又能避免主线程还没执行旧命令时，AI重复给同一名村民发送命令。
    static int lastDecisionFrame = -10;
    static int lastBuildAttemptFrame = -50;
    static int nextBuildPosition = 0;
    static int towerResearchRequestFrame = -1;
    static bool towerResearchRequested = false;
    static bool towerResearchWasRunning = false;
    static bool towerResearchCompleted = false;

    // 在同一个程序中重新开始游戏时，帧数会从0重新计算。
    // 同时重置这些静态变量，避免沿用上一局的冷却时间和建房位置。
    if (info.GameFrame < lastDecisionFrame) {
        lastDecisionFrame = -10;
        lastBuildAttemptFrame = -50;
        nextBuildPosition = 0;
        towerResearchRequestFrame = -1;
        towerResearchRequested = false;
        towerResearchWasRunning = false;
        towerResearchCompleted = false;
    }
    if (info.GameFrame - lastDecisionFrame < 10)
        return;
    lastDecisionFrame = info.GameFrame;

    // 2. 找到我方市镇中心。第一阶段的村民生产和人口判断都依赖它，
    // 如果中心已经被摧毁，就暂时停止经济调度。
    const tagBuilding *center = nullptr;
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == BUILDING_CENTER) {
            center = &building;
            break;
        }
    }
    if (center == nullptr)
        return;

    // 3. 统计正在采集各类资源的陆地村民。这样新出现的空闲村民会被分配到
    // 当前最缺人的一类，而不是所有人都去采集列表中的第一个资源。
    int foodWorkers = 0;
    int woodWorkers = 0;
    int stoneWorkers = 0;
    int farmerCount = 0;
    int combatArmyCount = 0;
    int slingerCount = 0;
    int towerCount = 0;
    const tagArmy *priest = nullptr;
    bool houseUnderConstruction = false;
    bool otherBuildingUnderConstruction = false;
    const tagBuilding *granary = nullptr;
    const tagBuilding *armyCamp = nullptr;

    for (const tagBuilding &building : info.buildings) {
        if (building.Percent < 100) {
            if (building.Type == BUILDING_HOME)
                houseUnderConstruction = true;
            else
                otherBuildingUnderConstruction = true;
        }
        if (building.Type == BUILDING_GRANARY)
            granary = &building;
        else if (building.Type == BUILDING_ARMYCAMP)
            armyCamp = &building;
        else if (building.Type == BUILDING_ARROWTOWER)
            ++towerCount;
    }

    for (const tagArmy &army : info.armies) {
        // 祭司是开局英雄，不计入“基础守军”；战船也不参与陆地防守。
        if (army.Sort == AT_PRIEST) {
            priest = &army;
            continue;
        }
        if (army.Sort == AT_SHIP)
            continue;
        ++combatArmyCount;
        if (army.Sort == AT_SLINGER)
            ++slingerCount;
    }

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;

        ++farmerCount;

        for (const tagResource &resource : info.resources) {
            if (farmer.WorkObjectSN != resource.SN)
                continue;

            const GatherKind kind = getGatherKind(resource.Type);
            if (kind == GATHER_FOOD)
                ++foodWorkers;
            else if (kind == GATHER_WOOD)
                ++woodWorkers;
            else if (kind == GATHER_STONE)
                ++stoneWorkers;
            break;
        }
    }

    // 4. 每次只安排一座建筑：人口紧张时房屋优先，其次依次补兵营、
    // 谷仓和箭塔。串行建造可以避免同一名村民在一帧收到多个建造命令。
    int builderSN = -1;
    const bool needHouse = info.Human_MaxNum - info.Human_Num <= 1.0;
    int buildingToBuild = -1;
    if (needHouse &&
        !houseUnderConstruction &&
        info.Wood >= BUILD_HOUSE_WOOD) {
        buildingToBuild = BUILDING_HOME;
    } else if (!otherBuildingUnderConstruction) {
        if (armyCamp == nullptr && info.Wood >= BUILD_ARMYCAMP_WOOD)
            buildingToBuild = BUILDING_ARMYCAMP;
        else if (armyCamp != nullptr && armyCamp->Percent >= 100 &&
                 granary == nullptr && info.Wood >= BUILD_GRANARY_WOOD)
            buildingToBuild = BUILDING_GRANARY;
        else if (towerResearchCompleted && towerCount == 0 &&
                 info.Stone >= BUILD_ARROWTOWER_STONE)
            buildingToBuild = BUILDING_ARROWTOWER;
    }

    if (buildingToBuild != -1 &&
        info.GameFrame - lastBuildAttemptFrame >= 50) {
        // 建筑按顺序尝试市镇中心周围的位置。某个位置被树木或地形挡住时，
        // 50帧后会自动尝试下一个位置，不需要把地图占用规则复制进AI。
        static const int buildingOffsets[][2] = {
            {4, 0}, {-4, 0}, {0, 4}, {0, -4},
            {4, 4}, {-4, 4}, {4, -4}, {-4, -4},
            {8, 0}, {-8, 0}, {0, 8}, {0, -8},
            {8, 4}, {-8, 4}, {8, -4}, {-8, -4}
        };
        const int positionCount =
            static_cast<int>(sizeof(buildingOffsets) /
                             sizeof(buildingOffsets[0]));
        const int positionIndex = nextBuildPosition % positionCount;
        const int buildDR = center->BlockDR + buildingOffsets[positionIndex][0];
        const int buildUR = center->BlockUR + buildingOffsets[positionIndex][1];

        // 先避开与别人挤在同一格的村民，再选择离施工点最近的人。
        // 固定地图中存在重叠出生的村民，直接选中容易因碰撞无法走出。
        int leastCrowding = INT_MAX;
        double nearestDistance = 0.0;
        for (const tagFarmer &farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER)
                continue;

            if (isConstructingBuilding(info, farmer))
                continue;

            int crowding = 0;
            for (const tagFarmer &other : info.farmers) {
                if (other.BlockDR == farmer.BlockDR &&
                    other.BlockUR == farmer.BlockUR)
                    ++crowding;
            }

            const double dr = farmer.BlockDR - buildDR;
            const double ur = farmer.BlockUR - buildUR;
            const double distance = dr * dr + ur * ur;
            if (builderSN == -1 || crowding < leastCrowding ||
                (crowding == leastCrowding && distance < nearestDistance)) {
                builderSN = farmer.SN;
                leastCrowding = crowding;
                nearestDistance = distance;
            }
        }

        if (builderSN != -1) {
            HumanBuild(builderSN, buildingToBuild, buildDR, buildUR);
            lastBuildAttemptFrame = info.GameFrame;
            ++nextBuildPosition;
        }
    }

    // 5. 第一阶段把普通村民补到12人。市镇中心空闲、食物够用且人口
    // 没有达到上限时才下达命令，避免每10帧重复请求或收到人口上限错误。
    const int firstStageFarmerTarget = 12;
    if (farmerCount < firstStageFarmerTarget &&
        center->Percent >= 100 &&
        center->Project == ACT_NULL &&
        info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD &&
        info.Human_Num < info.Human_MaxNum) {
        BuildingAction(center->SN, BUILDING_CENTER_CREATEFARMER);
    }

    // 6. 谷仓建好后研究箭塔。先观察到项目进入运行状态，再等待它回到
    // 空闲状态，才能确认研究真正完成并开始建造箭塔。
    if (towerCount > 0)
        towerResearchCompleted = true;

    if (granary != nullptr && granary->Percent >= 100) {
        if (granary->Project == BUILDING_GRANARY_ARROWTOWER) {
            towerResearchRequested = true;
            towerResearchWasRunning = true;
        } else if (towerResearchWasRunning && granary->Project == ACT_NULL) {
            towerResearchCompleted = true;
        }

        // 正常情况下命令会在下一轮进入运行状态。如果20帧后仍未开始，
        // 说明命令被拒绝，清除标记后允许再次尝试。
        if (towerResearchRequested && !towerResearchWasRunning &&
            granary->Project == ACT_NULL &&
            info.GameFrame - towerResearchRequestFrame >= 20) {
            towerResearchRequested = false;
        }

        if (!towerResearchCompleted &&
            !towerResearchRequested &&
            granary->Project == ACT_NULL &&
            info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
            BuildingAction(granary->SN, BUILDING_GRANARY_ARROWTOWER);
            towerResearchRequested = true;
            towerResearchRequestFrame = info.GameFrame;
        }
    }

    // 7. 村民经济成型后训练6名基础守军。先补1名投石兵克制第一波的
    // 弓箭手，其余训练棍棒兵；兵营忙碌或人口已满时不会重复下令。
    const int firstDefenseArmyTarget = 6;
    if (armyCamp != nullptr &&
        armyCamp->Percent >= 100 &&
        armyCamp->Project == ACT_NULL &&
        farmerCount >= firstStageFarmerTarget &&
        combatArmyCount < firstDefenseArmyTarget &&
        info.Human_Num < info.Human_MaxNum) {
        if (slingerCount == 0 &&
            info.Meat >= BUILDING_ARMYCAMP_CREATE_SLINGER_FOOD &&
            info.Stone >= BUILDING_ARMYCAMP_CREATE_SLINGER_STONE) {
            BuildingAction(armyCamp->SN, BUILDING_ARMYCAMP_CREATE_SLINGER);
        } else if (info.Meat >= BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD) {
            BuildingAction(armyCamp->SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
        }
    }

    // 8. 把基地周围20格作为防守区。只有进入这个范围的敌军
    // 才会触发迎击，避免守军因为看到远处单位而离开基地。
    const int defenseRadius = 20;
    vector<const tagArmy *> nearbyEnemies;
    for (const tagArmy &enemy : info.enemy_armies) {
        if (enemy.Blood > 0 &&
            blockDistanceSquared(*center, enemy) <=
                defenseRadius * defenseRadius) {
            nearbyEnemies.push_back(&enemy);
        }
    }

    if (!nearbyEnemies.empty()) {
        // 祭司优先转化离自己最近的敌军。ConvertCooldown为0
        // 才表示技能可用；正在转化同一目标时不重复下令。
        if (priest != nullptr && priest->ConvertCooldown == 0) {
            const tagArmy *nearestEnemy = nullptr;
            int nearestDistance = INT_MAX;
            for (const tagArmy *enemy : nearbyEnemies) {
                const int distance = blockDistanceSquared(*priest, *enemy);
                if (distance < nearestDistance) {
                    nearestEnemy = enemy;
                    nearestDistance = distance;
                }
            }

            if (nearestEnemy != nullptr &&
                priest->WorkObjectSN != nearestEnemy->SN) {
                HumanAction(priest->SN, nearestEnemy->SN);
            }
        } else if (priest != nullptr) {
            // 转化后的20秒冷却期无法再出手。敌人靠近祭司时，
            // 让他退到市镇中心旁，利用守军和箭塔保护自己。
            const int priestDangerRadius = 7;
            bool priestInDanger = false;
            for (const tagArmy *enemy : nearbyEnemies) {
                if (blockDistanceSquared(*priest, *enemy) <=
                    priestDangerRadius * priestDangerRadius) {
                    priestInDanger = true;
                    break;
                }
            }

            if (priestInDanger && priest->NowState != HUMAN_STATE_WALKING) {
                const double blockSize = static_cast<double>(BLOCKSIDELENGTH);
                HumanMove(priest->SN,
                          (center->BlockDR + 0.5) * blockSize,
                          (center->BlockUR + 0.5) * blockSize);
            }
        }

        // 基础守军各自攻击离自己最近的入侵者。如果已经在攻击
        // 防区内的敌人，保留原目标，避免频繁切换目标导致只走不打。
        for (const tagArmy &army : info.armies) {
            if (army.Sort == AT_PRIEST || army.Sort == AT_SHIP)
                continue;

            bool alreadyAttackingThreat = false;
            for (const tagArmy *enemy : nearbyEnemies) {
                if (army.WorkObjectSN == enemy->SN) {
                    alreadyAttackingThreat = true;
                    break;
                }
            }
            if (alreadyAttackingThreat)
                continue;

            const tagArmy *nearestEnemy = nullptr;
            int nearestDistance = INT_MAX;
            for (const tagArmy *enemy : nearbyEnemies) {
                const int distance = blockDistanceSquared(army, *enemy);
                if (distance < nearestDistance) {
                    nearestEnemy = enemy;
                    nearestDistance = distance;
                }
            }
            if (nearestEnemy != nullptr)
                HumanAction(army.SN, nearestEnemy->SN);
        }
    } else {
        // 没有入侵者时，把守军分散集结在市镇中心周围。
        // 若上一个攻击目标已经退出防区，也会立即停止追击并归队。
        static const int rallyOffsets[][2] = {
            {6, 0}, {0, 6}, {-6, 0}, {0, -6}
        };
        const int rallyDistance = 3;
        const double blockSize = static_cast<double>(BLOCKSIDELENGTH);

        for (const tagArmy &army : info.armies) {
            if (army.Sort == AT_PRIEST || army.Sort == AT_SHIP)
                continue;

            const int slot = army.SN % 4;
            const int rallyDR = center->BlockDR + rallyOffsets[slot][0];
            const int rallyUR = center->BlockUR + rallyOffsets[slot][1];
            const int dr = army.BlockDR - rallyDR;
            const int ur = army.BlockUR - rallyUR;
            const bool farFromRally =
                dr * dr + ur * ur > rallyDistance * rallyDistance;

            if (army.WorkObjectSN != -1 ||
                (army.NowState == HUMAN_STATE_IDLE && farFromRally)) {
                HumanMove(army.SN,
                          (rallyDR + 0.5) * blockSize,
                          (rallyUR + 0.5) * blockSize);
            }
        }

        // 祭司不参与普通编队，平时单独留在市镇中心旁。
        // 这样第一波来临时既能及时转化，又不会成为最前排目标。
        if (priest != nullptr) {
            const int priestRallyDR = center->BlockDR - 3;
            const int priestRallyUR = center->BlockUR - 3;
            const int dr = priest->BlockDR - priestRallyDR;
            const int ur = priest->BlockUR - priestRallyUR;
            const bool farFromRally = dr * dr + ur * ur > 9;

            if (priest->WorkObjectSN != -1 ||
                (priest->NowState == HUMAN_STATE_IDLE && farFromRally)) {
                HumanMove(priest->SN,
                          (priestRallyDR + 0.5) * blockSize,
                          (priestRallyUR + 0.5) * blockSize);
            }
        }
    }

    // 9. 正常比例仍是3:2:1；食物、木材不足或箭塔尚未建成时，
    // 临时提高相应权重。这只影响新出现的空闲村民，不强行中断采集。
    const int foodWeight = info.Meat < 150 ? 4 : 3;
    const int woodWeight = info.Wood < 100 ? 3 : 2;
    const int stoneWeight = towerCount == 0 &&
                            info.Stone < BUILD_ARROWTOWER_STONE ? 2 : 1;

    // 10. 只给空闲的陆地村民安排采集任务。正在移动或工作的村民会继续执行
    // 原来的命令，不需要也不应该每帧重新发送HumanAction。
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER ||
            farmer.NowState != HUMAN_STATE_IDLE ||
            isConstructingBuilding(info, farmer) ||
            farmer.SN == builderSN)
            continue;

        // 分别寻找离当前村民最近的食物、木材和石头，再按当前权重分配。
        // 如果某一类资源当前不存在，选择函数会自动在其余资源中分配。
        const int foodSN = findNearestResource(info, farmer, GATHER_FOOD);
        const int woodSN = findNearestResource(info, farmer, GATHER_WOOD);
        const int stoneSN = findNearestResource(info, farmer, GATHER_STONE);
        const GatherKind kind = chooseGatherKind(foodWorkers,
                                                  woodWorkers,
                                                  stoneWorkers,
                                                  foodWeight,
                                                  woodWeight,
                                                  stoneWeight,
                                                  foodSN != -1,
                                                  woodSN != -1,
                                                  stoneSN != -1);

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
        }

        if (targetSN != -1)
            HumanAction(farmer.SN, targetSN);
    }

}
