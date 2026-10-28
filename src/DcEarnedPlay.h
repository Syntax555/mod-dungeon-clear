/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license,
 * version 3 or (at your option) any later version.
 */
#ifndef DC_EARNED_PLAY_H
#define DC_EARNED_PLAY_H
#include "Map.h"
#include "PlayerbotAIConfig.h"
#include "PathGenerator.h"
#include "Player.h"
#include <algorithm>

namespace DcEarnedPlay
{
    inline bool Active() { return sPlayerbotAIConfig.naturalProgression; }
    inline bool UsesRealPath(PathGenerator const& path)
    {
        auto const type = path.GetPathType();
        return (type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORT)) &&
               !(type & (PATHFIND_SHORTCUT | PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH |
                         PATHFIND_FARFROMPOLY)) && path.GetPath().size() >= 2;
    }
    inline bool ReachableLeg(Player* bot, float x, float y, float z)
    {
        PathGenerator path(bot);
        return path.CalculatePath(x, y, z, false) && UsesRealPath(path);
    }
    inline bool RealSpline(Player* bot, Movement::PointsArray const& points)
    {
        if (!bot || points.size() < 2)
            return false;
        auto const& first = points.front();
        float const dx = first.x - bot->GetPositionX();
        float const dy = first.y - bot->GetPositionY();
        float const dz = first.z - bot->GetPositionZ();
        if (dx * dx + dy * dy + dz * dz > 0.0025f)
            return false;
        for (size_t i = 1; i < points.size(); ++i)
        {
            auto const& a = points[i - 1];
            auto const& b = points[i];
            PathGenerator leg(bot);
            // A detour route does not authorize a raw straight spline through
            // a wall. Raycast every actual segment, including the first one.
            leg.SetUseRaycast(true);
            if (!leg.CalculatePath(a.x, a.y, a.z, b.x, b.y, b.z, false) ||
                !UsesRealPath(leg) || !(leg.GetPathType() & PATHFIND_NORMAL) ||
                (leg.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORT)) ||
                (leg.GetPath().back() - b).magnitude() > 0.05f)
                return false;
        }
        return true;
    }
    // A raw spline moves at the speed of the current movement flags. A
    // player swims through water on the way, so a window touching water uses
    // the swim speed: earned movement is never faster than a client's.
    inline void MatchSwimming(Player* bot, Movement::PointsArray const& points)
    {
        Map const* map = bot->GetMap();
        bool const water = std::any_of(points.begin(), points.end(), [&](G3D::Vector3 const& p)
        { return map->IsInWater(bot->GetPhaseMask(), p.x, p.y, p.z, bot->GetCollisionHeight()); });
        if (bot->SetSwim(water))
            bot->SendMovementFlagUpdate();
    }
}
#endif
