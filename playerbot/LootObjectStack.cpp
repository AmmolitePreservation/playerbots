#include "LootObjectStack.h"
#include "playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/SharedValueContext.h"

using namespace ai;

#define MAX_LOOT_OBJECT_COUNT 10

LootTarget::LootTarget(ObjectGuid guid) : guid(guid), asOfTime(time(0))
{
}

LootTarget::LootTarget(LootTarget const& other)
{
    guid = other.guid;
    asOfTime = other.asOfTime;
}

LootTarget& LootTarget::operator=(LootTarget const& other)
{
    if((void*)this == (void*)&other)
        return *this;

    guid = other.guid;
    asOfTime = other.asOfTime;

    return *this;
}

bool LootTarget::operator< (const LootTarget& other) const
{
    return guid < other.guid;
}

void LootTargetList::shrink(time_t fromTime)
{
    for (std::set<LootTarget>::iterator i = begin(); i != end(); )
    {
        if (i->asOfTime <= fromTime)
            erase(i++);
		else
			++i;
    }
}

LootObject::LootObject(Player* bot, ObjectGuid guid)
	: guid(), skillId(SKILL_NONE), reqSkillValue(0), reqItem(0)
{
    Refresh(bot, guid);
}

void LootObject::Refresh(Player* bot, ObjectGuid guid, bool debug)
{
    skillId = SKILL_NONE;
    reqSkillValue = 0;
    reqItem = 0;
    this->guid = ObjectGuid();

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Creature* creature = ai->GetCreature(guid);
    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
    {
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature flag lootable.", "debug loot");

            this->guid = guid;
        }

        if (creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
        {
            skillId = creature->GetCreatureInfo()->GetRequiredLootSkill();
            uint32 targetLevel = creature->GetLevel();
            reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
            if (ai->HasSkill((SkillType)skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "Creature flag skinnable and has skill.", "debug loot");
                this->guid = guid;
            }

            if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature flag skinnable not enough skill.", "debug loot");
            return;
        }

        if (debug)
            ai->TellDebug(ai->GetMaster(), "Creature without loot or skin flag.", "debug loot");

        return;
    }

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go) && !go->IsInUse())
    {
        bool isQuestItemOnly = false;

#ifdef MANGOSBOT_TWO
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
        {
            int itemId = go->GetGOInfo()->questItems[i];

            if (ItemUsageValue::IsNeededForQuest(bot, itemId))
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "GO has item needed for quest.", "debug loot");
                this->guid = guid;
                return;
            }
            isQuestItemOnly |= itemId > 0;
        }
#else
        /*if (!guid.IsEmpty())
        {
            for (auto& entry : GAI_VALUE2(std::list<int32>, "item drop list", -1*uint(go->GetEntry())))
            {
                if (IsNeededForQuest(bot, entry))
                {
                    this->guid = guid;
                    return;
                }
                isQuestItemOnly |= entry > 0;
            }
        }*/
#endif

        if (isQuestItemOnly)
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has only quests items we don't need.", "debug loot");
            return;
        }

        uint32 goId = go->GetGOInfo()->id;
        std::set<uint32>& skipGoLootList = ai->GetAiObjectContext()->GetValue<std::set<uint32>&>("skip go loot list")->Get();
        if (skipGoLootList.find(goId) != skipGoLootList.end())
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go in skip go loot list.", "debug loot");
            return;
        }

        uint32 lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has no lockid.", "debug loot");
            return;
        }

        for (int i = 0; i < 8; ++i)
        {
            switch (lockInfo->Type[i])
            {
                case LOCK_KEY_ITEM:
                    if (lockInfo->Index[i] > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go has lock with key requirement.", "debug loot");
                        reqItem = lockInfo->Index[i];
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_SKILL:
                    if (goId == 13891 || goId == 19535) // Serpentbloom
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go is serpentbloom.", "debug loot");
                        this->guid = guid;
                    }
                    else if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go requires skill.", "debug loot");
                        skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_NONE:
                    if (debug)
                        ai->TellDebug(ai->GetMaster(), "Go has open lock.", "debug loot");
                    this->guid = guid;
                    break;
            }
        }
    }

    if (debug && guid && !this->guid)
        ai->TellDebug(ai->GetMaster(), "Go has bad lock.", "debug loot");
}

WorldObject* LootObject::GetWorldObject(Player* bot)
{
    Refresh(bot, guid);

    PlayerbotAI* ai = bot->GetPlayerbotAI();

    Creature *creature = ai->GetCreature(guid);
    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        return creature;

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go))
        return go;

    return NULL;
}

bool LootObject::IsWithinLootRange(Player* bot, WorldObject* target)
{
    return IsWithinLootRangeFrom(bot, target, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

// Corpses are opened with CMSG_LOOT, whose handler rejects anything outside this range as too far. Game objects are opened by spell.
bool LootObject::IsWithinLootRangeFrom(Player* bot, WorldObject* target, float x, float y, float z)
{
    float dx = target->GetPositionX() - x;
    float dy = target->GetPositionY() - y;
    if (!target->isType(TYPEMASK_UNIT))
        return sServerFacade.IsDistanceLessOrEqualThan(sqrt(dx * dx + dy * dy), INTERACTION_DISTANCE);

    float dz = target->GetPositionZ() - z;
    float range = std::max(INTERACTION_DISTANCE, target->GetCombatReach() + (4.0f / 3.0f) + bot->GetCombatReach());
    return dx * dx + dy * dy + dz * dz <= range * range;
}

LootObject::LootObject(const LootObject& other)
{
    guid = other.guid;
    skillId = other.skillId;
    reqSkillValue = other.reqSkillValue;
    reqItem = other.reqItem;
}

bool LootObject::IsLootPossible(Player* bot)
{
    if (IsEmpty())
        return false;

    WorldObject* worldObject = GetWorldObject(bot);
    if (!worldObject)
        return false;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    bool debug = ai->HasStrategy("debug loot", BotState::BOT_STATE_NON_COMBAT);
    std::string objectName = debug ? std::string(worldObject->GetName()) : std::string();

    if (reqItem && !bot->HasItemCount(reqItem, 1))
    {
        if (debug)
            ai->TellDebug(ai->GetMaster(), objectName + ": I lack its key item " + std::to_string(reqItem) + ".", "debug loot");
        return false;
    }

    if (guid.IsCreature())
    {
        Creature* creature = ai->GetCreature(guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            if (creature->m_loot && skillId != SKILL_SKINNING)
                if (!creature->m_loot->CanLoot(bot))
                {
                    if (debug)
                        ai->TellDebug(ai->GetMaster(), objectName + ": the server gave me no loot rights when it died.", "debug loot");
                    return false;
                }
        }
    }

    AiObjectContext* context = ai->GetAiObjectContext();

    if (!AI_VALUE2(bool, "should loot object", std::to_string(guid.GetRawValue())))
    {
        if (debug)
            ai->TellDebug(ai->GetMaster(), objectName + ": nothing on it I want.", "debug loot");
        return false;
    }

    // Check if the game object has quest loot and bot has the quest for it
    if (guid.IsGameObject())
    {
        GameObject* go = ai->GetGameObject(guid);
        if (go)
        {
            // Ignore for mining nodes and herbs
            if (skillId != SKILL_MINING && skillId != SKILL_HERBALISM)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (!go->ActivateToQuest(bot))
                    {
                        return false;
                    }
                }
            }
            
            // herb-like quest objects
            if (skillId == SKILL_HERBALISM && reqSkillValue == 1)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (go->ActivateToQuest(bot))
                    {
                        bool hasQuestItems = false;
                        for (auto& entry : GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int(go->GetEntry())))
                        {
                            if (ItemUsageValue::IsNeededForQuest(bot, entry))
                            {
                                hasQuestItems = true;
                            }
                        }
                        return hasQuestItems || go->GetLootState() != GO_READY;
                    }
                }
            }

            //Ignore objects that are currently in use.
            if (go->IsInUse() || go->GetGoState() == GO_STATE_ACTIVE)
                return false;
        }
    }

    if (skillId == SKILL_NONE)
        return true;

    if (skillId == SKILL_FISHING)
        return false;

    if (!ai->HasSkill((SkillType)skillId))
        return false;

    if (!reqSkillValue)
        return true;

    uint32 skillValue = uint32(bot->GetSkillValue(skillId));
    if (reqSkillValue > skillValue)
        return false;

    if (skillId == SKILL_MINING && !bot->HasItemCount(2901, 1))
        return false;

    if (skillId == SKILL_SKINNING && !bot->HasItemCount(7005, 1))
        return false;

    return true;
}

bool LootObjectStack::Add(ObjectGuid guid)
{
    if (!availableLoot.insert(guid).second)
        return false;

    if (availableLoot.size() < MAX_LOOT_OBJECT_COUNT)
        return true;

    std::vector<LootObject> ordered = OrderByDistance();
    for (size_t i = MAX_LOOT_OBJECT_COUNT; i < ordered.size(); i++)
        Remove(ordered[i].guid);

    return true;
}

void LootObjectStack::Remove(ObjectGuid guid)
{
    LootTargetList::iterator i = availableLoot.find(guid);
    if (i != availableLoot.end())
        availableLoot.erase(i);
}

void LootObjectStack::Clear()
{
    availableLoot.clear();
}

bool LootObjectStack::CanLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return !ordered.empty();
}

LootObject LootObjectStack::GetLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return ordered.empty() ? LootObject() : *ordered.begin();
}

float LootObjectStack::GetPickDistance(Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!bot->GetGroup() || ai->IsGroupLeader())
        return sPlayerbotAIConfig.lootDistance;

    if (ai->HasActivePlayerMaster())
        return sPlayerbotAIConfig.groupMemberLootDistanceWithActiveMaster;

    return sPlayerbotAIConfig.groupMemberLootDistance;
}

std::vector<LootObject> LootObjectStack::OrderByDistance(float maxDistance)
{
    availableLoot.shrink(time(0) - 30);

    Player* master = bot->GetPlayerbotAI()->GetMaster();
    float pickDistance = GetPickDistance(bot);

    std::map<float, LootObject> sortedMap;
    LootTargetList safeCopy(availableLoot);
    for (LootTargetList::iterator i = safeCopy.begin(); i != safeCopy.end(); i++)
    {
        ObjectGuid guid = i->guid;
        LootObject lootObject(bot, guid);
        if (!lootObject.IsLootPossible(bot))
            continue;

        WorldObject* wo = lootObject.GetWorldObject(bot);

        // Kills and attack targets enter the stack without the pick distance check that scanned loot gets.
        if (master && master != bot && (!master->IsInMap(wo) || sServerFacade.IsDistanceGreaterThan(sServerFacade.GetDistance2d(master, wo), pickDistance)))
            continue;

        float distance = bot->GetDistance(wo);
        if (!maxDistance || distance <= maxDistance)
            sortedMap[distance] = lootObject;
    }

    std::vector<LootObject> result;
    for (std::map<float, LootObject>::iterator i = sortedMap.begin(); i != sortedMap.end(); i++)
        result.push_back(i->second);
    return result;
}

