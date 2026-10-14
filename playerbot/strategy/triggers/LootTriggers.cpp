
#include "playerbot/playerbot.h"
#include "LootTriggers.h"
#include "playerbot/LootObjectStack.h"

#include "playerbot/ServerFacade.h"
using namespace ai;

bool LootAvailableTrigger::IsActive()
{
    if (!AI_VALUE(bool, "has available loot") || AI_VALUE2(bool, "combat", "self target") || AI_VALUE2(bool, "mounted", "self target"))
        return false;

    LootObject loot = AI_VALUE(LootObject, "loot target");
    WorldObject* lootObject = loot.IsEmpty() ? nullptr : loot.GetWorldObject(bot);
    return !lootObject || LootObject::IsWithinLootRange(bot, lootObject) || AI_VALUE(std::list<ObjectGuid>, "all targets").empty();
}

bool FarFromCurrentLootTrigger::IsActive()
{
    LootObject loot = AI_VALUE(LootObject, "loot target");

    if (!loot.IsLootPossible(bot))
        return false;

    return !LootObject::IsWithinLootRange(bot, loot.GetWorldObject(bot));
}

bool CanLootTrigger::IsActive()
{
    return AI_VALUE(bool, "can loot");
}
