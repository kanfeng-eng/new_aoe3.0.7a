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

// 按给定权重选择当前最缺人的任务。正常权重是3:2:1，资源不足时
// 调用处会临时提高食物或木材权重。
GatherKind chooseGatherKind(int foodWorkers,
                            int woodWorkers,
                            int stoneWorkers,
                            int foodWeight,
                            int woodWeight,
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
    if (hasStone && (result == GATHER_NONE || stoneWorkers < bestLoad))
        result = GATHER_STONE;

    return result;
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
    static int lastHouseAttemptFrame = -50;
    static int nextHousePosition = 0;

    // 在同一个程序中重新开始游戏时，帧数会从0重新计算。
    // 同时重置这些静态变量，避免沿用上一局的冷却时间和建房位置。
    if (info.GameFrame < lastDecisionFrame) {
        lastDecisionFrame = -10;
        lastHouseAttemptFrame = -50;
        nextHousePosition = 0;
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
    bool houseUnderConstruction = false;

    for (const tagBuilding &building : info.buildings) {
        if (building.Type == BUILDING_HOME && building.Percent < 100) {
            houseUnderConstruction = true;
            break;
        }
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

    // 4. 人口只剩一个空位时提前建房。正在建造的房屋也算作“已经安排”，
    // 否则AI会在房屋完成前连续放下多个地基，浪费木材和村民时间。
    int builderSN = -1;
    const bool needHouse = info.Human_MaxNum - info.Human_Num <= 1.0;
    if (needHouse &&
        !houseUnderConstruction &&
        info.Wood >= BUILD_HOUSE_WOOD &&
        info.GameFrame - lastHouseAttemptFrame >= 50) {
        // 房屋按顺序尝试市镇中心周围的位置。某个位置被树木或地形挡住时，
        // 50帧后会自动尝试下一个位置，不需要把地图占用规则复制进AI。
        static const int houseOffsets[][2] = {
            {4, 0}, {-3, 0}, {0, 4}, {0, -3},
            {4, 4}, {-3, 4}, {4, -3}, {-3, -3},
            {7, 0}, {-6, 0}, {0, 7}, {0, -6}
        };
        const int positionCount =
            static_cast<int>(sizeof(houseOffsets) / sizeof(houseOffsets[0]));
        const int positionIndex = nextHousePosition % positionCount;
        const int houseDR = center->BlockDR + houseOffsets[positionIndex][0];
        const int houseUR = center->BlockUR + houseOffsets[positionIndex][1];

        // 选择离市镇中心最近的普通村民建房。建房是有意改变原任务，
        // 因此这里允许从采集岗位临时抽调一人。
        double nearestDistance = 0.0;
        for (const tagFarmer &farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER)
                continue;

            const double dr = farmer.BlockDR - center->BlockDR;
            const double ur = farmer.BlockUR - center->BlockUR;
            const double distance = dr * dr + ur * ur;
            if (builderSN == -1 || distance < nearestDistance) {
                builderSN = farmer.SN;
                nearestDistance = distance;
            }
        }

        if (builderSN != -1) {
            HumanBuild(builderSN, BUILDING_HOME, houseDR, houseUR);
            lastHouseAttemptFrame = info.GameFrame;
            ++nextHousePosition;
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

    // 6. 正常比例仍是3:2:1；食物或木材低于安全线时临时增加对应权重。
    // 这只影响新出现的空闲村民，不会强行打断正在采集的村民。
    const int foodWeight = info.Meat < 150 ? 4 : 3;
    const int woodWeight = info.Wood < 100 ? 3 : 2;

    // 7. 只给空闲的陆地村民安排采集任务。正在移动或工作的村民会继续执行
    // 原来的命令，不需要也不应该每帧重新发送HumanAction。
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER ||
            farmer.NowState != HUMAN_STATE_IDLE ||
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
