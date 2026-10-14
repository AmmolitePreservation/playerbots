
#include "playerbot/playerbot.h"
#include "FishAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "TellLosAction.h"
#include "EquipAction.h"

#include <algorithm>
#include <cmath>

using namespace ai;

namespace
{
    constexpr float MAX_LOCAL_FISH_SPOT_DISTANCE = 45.0f;
    constexpr float FISHING_SPOT_ARRIVAL_DISTANCE = 2.0f;
    constexpr float FISHING_SPOT_LOCK_DISTANCE = 0.1f;
    constexpr uint32 FISHING_HEADING_SAMPLES = 36;
    constexpr uint32 FISHING_DISTANCE_SAMPLES = 5;
    constexpr uint32 FISHING_ANGLE_SAMPLES = 7;
    constexpr uint32 FISHING_TOTAL_CAST_SAMPLES = FISHING_DISTANCE_SAMPLES * FISHING_ANGLE_SAMPLES;
    constexpr uint32 FISHING_HIGH_QUALITY_PERCENT = 95;
    constexpr uint32 FISHING_HIGH_QUALITY_SAMPLES =
        (FISHING_TOTAL_CAST_SAMPLES * FISHING_HIGH_QUALITY_PERCENT + 99) / 100;

    struct ScoredFishSpot
    {
        WorldPosition position;
        uint32 validSamples = 0;
        float depthScore = 0.0f;
    };

    bool GetFishingRange(PlayerbotAI* ai, float& minRange, float& maxRange)
    {
        uint32 fishingSpellId = ai->GetAiObjectContext()->GetValue<uint32>("spell id", "7731")->Get();
        const SpellEntry* fishingSpell = sServerFacade.LookupSpellInfo(fishingSpellId);
        const SpellRangeEntry* fishingRange = fishingSpell ? sServerFacade.LookupSpellRangeEntry(fishingSpell->rangeIndex) : nullptr;

        if (!fishingRange)
            return false;

        minRange = GetSpellMinRange(fishingRange);
        maxRange = GetSpellMaxRange(fishingRange);
        return maxRange > minRange;
    }

    bool IsFishingSampleInLineOfSight(Player* bot, const WorldPosition& fishSpot, float x, float y, float z)
    {
#if defined(MANGOSBOT_TWO) || MAX_EXPANSION == 2
        return bot->GetMap()->IsInLineOfSight(fishSpot.getX(), fishSpot.getY(), fishSpot.getZ() + bot->GetCollisionHeight(), x, y, z, 0, false);
#else
        return bot->GetMap()->IsInLineOfSight(fishSpot.getX(), fishSpot.getY(), fishSpot.getZ() + bot->GetCollisionHeight(), x, y, z, false);
#endif
    }

    ScoredFishSpot ScoreFishSpot(Player* bot, const WorldPosition& fishSpot, float minRange, float maxRange)
    {
        ScoredFishSpot best;
        best.position = fishSpot;

        const TerrainInfo* terrain = bot->GetTerrain();
        float boundingRadius = bot->GetObjectBoundingRadius();
        float maxAngle = (maxRange - minRange) / (maxRange + boundingRadius);

        for (uint32 headingIndex = 0; headingIndex < FISHING_HEADING_SAMPLES; ++headingIndex)
        {
            float heading = 2.0f * M_PI_F * headingIndex / FISHING_HEADING_SAMPLES;
            uint32 validSamples = 0;
            float depthScore = 0.0f;

            for (uint32 distanceIndex = 0; distanceIndex < FISHING_DISTANCE_SAMPLES; ++distanceIndex)
            {
                float distanceFraction = static_cast<float>(distanceIndex) / (FISHING_DISTANCE_SAMPLES - 1);
                float castDistance = minRange + (maxRange - minRange) * distanceFraction + boundingRadius;

                for (uint32 angleIndex = 0; angleIndex < FISHING_ANGLE_SAMPLES; ++angleIndex)
                {
                    float angleFraction = static_cast<float>(angleIndex) / (FISHING_ANGLE_SAMPLES - 1) - 0.5f;
                    float sampleAngle = heading + maxAngle * angleFraction;
                    float x = fishSpot.getX() + std::cos(sampleAngle) * castDistance;
                    float y = fishSpot.getY() + std::sin(sampleAngle) * castDistance;

                    GridMapLiquidData liquidData;
                    float waterLevel = terrain->GetWaterLevel(x, y, fishSpot.getZ()) - 1.0f;
                    if (!terrain->IsInWater(x, y, waterLevel, &liquidData))
                        continue;

                    if (!IsFishingSampleInLineOfSight(bot, fishSpot, x, y, liquidData.level + 1.0f))
                        continue;

                    ++validSamples;
                    depthScore += std::max(0.0f, liquidData.level - liquidData.depth_level);
                }
            }

            if (validSamples > best.validSamples ||
                (validSamples == best.validSamples && depthScore > best.depthScore))
            {
                best.position.setO(heading);
                best.validSamples = validSamples;
                best.depthScore = depthScore;
            }
        }

        return best;
    }

    bool GetReachableFishSpot(Player* bot, const WorldPosition& requestedPosition,
        float minRange, float maxRange, ScoredFishSpot& fishSpot)
    {
        WorldPosition botPosition(bot);
        WorldPosition reachablePosition = requestedPosition;

        if (requestedPosition.distance(bot) >= FISHING_SPOT_ARRIVAL_DISTANCE)
        {
            std::vector<WorldPosition> candidatePath = botPosition.getPathTo(requestedPosition, bot);
            if (candidatePath.empty())
                return false;

            // Incomplete shoreline paths are useful only at the point the
            // navmesh can actually reach. Score and save that endpoint rather
            // than repeatedly asking movement to reach the original point.
            reachablePosition = candidatePath.back();
        }

        if (!reachablePosition || reachablePosition.getMapId() != botPosition.getMapId() ||
            reachablePosition.sqDistance(botPosition) > MAX_LOCAL_FISH_SPOT_DISTANCE * MAX_LOCAL_FISH_SPOT_DISTANCE ||
            reachablePosition.isInWater() || reachablePosition.isUnderWater())
            return false;

        fishSpot = ScoreFishSpot(bot, reachablePosition, minRange, maxRange);
        return fishSpot.validSamples != 0;
    }

    float OrientationDifference(float first, float second)
    {
        float difference = std::fabs(first - second);
        return std::min(difference, 2.0f * M_PI_F - difference);
    }
}

bool MoveToFishAction::isUseful()
{
    if (qualifier == "travel")
    {
        if (!AI_VALUE(bool, "travel target working"))
            return false;

        TravelTarget* target = AI_VALUE(TravelTarget*, "leader travel target");

        if (target->GetDestination()->GetPurpose() != TravelDestinationPurpose::GatherFishing)
            return false;
    }

    return true;
}

bool MoveToFishAction::Execute(Event& event)
{    
    WorldPosition fishSpot;

    fishSpot = AI_VALUE2(WorldPosition, "custom position", "fish spot");

    WorldPosition botPosition(bot);
    bool fishSpotIsLocal = fishSpot && fishSpot.getMapId() == botPosition.getMapId() &&
        fishSpot.sqDistance(botPosition) <= MAX_LOCAL_FISH_SPOT_DISTANCE * MAX_LOCAL_FISH_SPOT_DISTANCE;

    if (fishSpot && qualifier != "travel" && !fishSpotIsLocal)
        return false;

    if (!fishSpot && qualifier == "travel") //Get travel fish spot if available.
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "leader travel target");
        fishSpot = *target->GetPosition();

        if (AI_VALUE(TravelTarget*, "travel target") != target) //Do not fish ontop of master.
            fishSpot = *sTravelMgr.GetFishSpot(bot, true);
    }
    
    if (!fishSpot) //Get any fish spot.
    {
        float minRange = 0.0f;
        float maxRange = 0.0f;
        if (!GetFishingRange(ai, minRange, maxRange))
            return false;

        std::vector<WorldPosition> nearbySpots = sTravelMgr.GetFishSpotsInRange(bot, MAX_LOCAL_FISH_SPOT_DISTANCE);
        std::vector<ScoredFishSpot> scoredSpots;
        scoredSpots.reserve(nearbySpots.size());

        for (const WorldPosition& nearbySpot : nearbySpots)
        {
            ScoredFishSpot reachableSpot;
            if (GetReachableFishSpot(bot, nearbySpot, minRange, maxRange, reachableSpot))
                scoredSpots.push_back(reachableSpot);
        }

        std::sort(scoredSpots.begin(), scoredSpots.end(), [](const ScoredFishSpot& left, const ScoredFishSpot& right)
        {
            if (left.validSamples != right.validSamples)
                return left.validSamples > right.validSamples;
            return left.depthScore > right.depthScore;
        });

        if (scoredSpots.empty() || !scoredSpots.front().validSamples)
            return false;

        uint32 bestSampleCount = scoredSpots.front().validSamples;
        uint32 preferredSampleCount = bestSampleCount >= FISHING_HIGH_QUALITY_SAMPLES ?
            FISHING_HIGH_QUALITY_SAMPLES : bestSampleCount;

        std::vector<ScoredFishSpot> preferredSpots;
        size_t fallbackIndex = 0;
        while (fallbackIndex < scoredSpots.size() && scoredSpots[fallbackIndex].validSamples >= preferredSampleCount)
        {
            preferredSpots.push_back(scoredSpots[fallbackIndex]);
            ++fallbackIndex;
        }

        uint32 selectedIndex = urand(0, static_cast<uint32>(preferredSpots.size() - 1));
        fishSpot = preferredSpots[selectedIndex].position;

        // Candidate paths are validation-only. Let MoveTo build its own local
        // movement path instead of reusing a partial travel-node route.
        AI_VALUE(LastMovement&, "last movement").setPath(TravelPath());
    }
    
    SET_AI_VALUE2(WorldPosition, "custom position", "fish spot", fishSpot);
   
    if (fishSpot.distance(bot) < FISHING_SPOT_ARRIVAL_DISTANCE)
        return false;

    return MoveTo(fishSpot);
}

bool FishAction::isUseful()
{
    if (qualifier == "travel")
    {
        if (!AI_VALUE(bool, "travel target working"))
            return false;

        TravelTarget* target = AI_VALUE(TravelTarget*, "leader travel target");

        if (target->GetDestination()->GetPurpose() != TravelDestinationPurpose::GatherFishing)
            return false;

        if (!bot->GetGroup() || ai->IsGroupLeader() || target->GetTimeLeft() < 0)
            target->CheckStatus();
    }

    WorldPosition fishSpot = AI_VALUE2(WorldPosition, "custom position", "fish spot");

    if (!fishSpot)
        return false;

    if (!AI_VALUE(bool, "can fish"))
        return false;

    if (fishSpot.distance(bot) >= FISHING_SPOT_ARRIVAL_DISTANCE)
        return false;

    return true;
}

bool FishAction::Execute(Event& event)
{
    if (qualifier == "travel")
    {
        if (!AI_VALUE(bool, "travel target working"))
            return false;
    }

    if (bot->IsMoving())
    {
        ai->StopMoving();
        SetDuration(100);
        return true;
    }

    WorldPosition fishSpot = AI_VALUE2(WorldPosition, "custom position", "fish spot");

    if (qualifier != "travel" && fishSpot.distance(bot) > FISHING_SPOT_LOCK_DISTANCE)
    {
        float minRange = 0.0f;
        float maxRange = 0.0f;
        ScoredFishSpot stationarySpot;

        if (!GetFishingRange(ai, minRange, maxRange))
        {
            ai->GetAiObjectContext()->GetValue<WorldPosition>("custom position", "fish spot")->Reset();
            return false;
        }

        stationarySpot = ScoreFishSpot(bot, WorldPosition(bot), minRange, maxRange);
        if (!stationarySpot.validSamples)
        {
            ai->GetAiObjectContext()->GetValue<WorldPosition>("custom position", "fish spot")->Reset();
            return false;
        }

        // Once the bot has arrived, its exact current coordinates become the
        // permanent fishing spot. It will not chase sub-yard endpoint drift.
        fishSpot = stationarySpot.position;
        SET_AI_VALUE2(WorldPosition, "custom position", "fish spot", fishSpot);
    }

    if (OrientationDifference(fishSpot.getO(), bot->GetOrientation()) > 0.5f)
    {
        bot->SetFacingTo(fishSpot.getO());
        SetDuration(100);
        return true;
    }

    ai->StopMoving();

    std::list<Item*> poles = AI_VALUE2(std::list<Item*>, "inventory items", "fishing pole");

    if (poles.empty())
        return false;

    Item* pole = poles.front();
    uint8 bagIndex = pole->GetBagSlot();
    uint8 slot = pole->GetSlot();

    if (slot != EQUIPMENT_SLOT_MAINHAND)
        EquipAction::EquipItem(ai, GetMaster(), pole);

    Event fishCastEvent = Event("fish", "7731 " + chat->formatWorldobject(bot));
    bool didCast = CastCustomSpellAction::Execute(fishCastEvent);

    SetDuration(sPlayerbotAIConfig.globalCoolDown);

    return didCast;
}

bool UseFishingBobberAction::Execute(Event& event)
{
    std::list<GameObject*> objects = TellLosAction::GoGuidListToObjList(ai, AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los"));

    for (auto& obj : objects)
    {
        if (obj->GetEntry() != 35591)
            continue;

        if (obj->GetOwnerGuid() != bot->GetObjectGuid())
            continue;

        if (obj->GetLootState() != GO_READY)
        {
            time_t bobberActiveTime = obj->GetRespawnTime() - FISHING_BOBBER_READY_TIME;
            if (bobberActiveTime > time(0))
                SetDuration((bobberActiveTime - time(0)) * IN_MILLISECONDS + 500);
            else
                SetDuration(1000);
            return true;
        }

        std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_GAMEOBJ_USE));
        *packet << obj->GetObjectGuid();
        bot->GetSession()->QueuePacket(std::move(packet));

        SetDuration(3000);

        return true;
    }

    return false;
}
