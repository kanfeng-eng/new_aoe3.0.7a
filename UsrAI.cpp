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

// 按3:2:1的目标比例选择当前最缺人的任务。
// 用“已有工人数 / 目标权重”比较，可以让新增的空闲村民逐渐补齐比例。
GatherKind chooseGatherKind(int foodWorkers,
                            int woodWorkers,
                            int stoneWorkers,
                            bool hasFood,
                            bool hasWood,
                            bool hasStone)
{
    GatherKind result = GATHER_NONE;
    double bestLoad = 0.0;

    if (hasFood) {
        result = GATHER_FOOD;
        bestLoad = foodWorkers / 3.0;
    }
    if (hasWood && (result == GATHER_NONE || woodWorkers / 2.0 < bestLoad)) {
        result = GATHER_WOOD;
        bestLoad = woodWorkers / 2.0;
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
    if (info.GameFrame - lastDecisionFrame < 10)
        return;
    lastDecisionFrame = info.GameFrame;

    // 2. 找到我方市镇中心。当前阶段虽然还不生产村民，但后续所有经济逻辑
    // 都以市镇中心为基础；中心不存在时继续调度已经没有意义。
    int centerSN = -1;
    for (const tagBuilding &building : info.buildings) {
        if (building.Type == BUILDING_CENTER) {
            centerSN = building.SN;
            break;
        }
    }
    if (centerSN == -1)
        return;

    // 先统计正在采集各类资源的陆地村民。这样新出现的空闲村民会被分配到
    // 当前最缺人的一类，而不是所有人都去采集列表中的第一个资源。
    int foodWorkers = 0;
    int woodWorkers = 0;
    int stoneWorkers = 0;

    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER)
            continue;

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

    // 3. 只给空闲的陆地村民安排新任务。正在移动或工作的村民会继续执行
    // 原来的命令，不需要也不应该每帧重新发送HumanAction。
    for (const tagFarmer &farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER ||
            farmer.NowState != HUMAN_STATE_IDLE)
            continue;

        // 4. 分别寻找离当前村民最近的食物、木材和石头，再按3:2:1分配。
        // 如果某一类资源当前不存在，选择函数会自动在其余资源中分配。
        const int foodSN = findNearestResource(info, farmer, GATHER_FOOD);
        const int woodSN = findNearestResource(info, farmer, GATHER_WOOD);
        const int stoneSN = findNearestResource(info, farmer, GATHER_STONE);
        const GatherKind kind = chooseGatherKind(foodWorkers,
                                                  woodWorkers,
                                                  stoneWorkers,
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
