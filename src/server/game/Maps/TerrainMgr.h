/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TERRAIN_MGR_H
#define TERRAIN_MGR_H

#include "Define.h"
#include "GridDefines.h"
#include "MapDefines.h"
#include "Optional.h"
#include "Position.h"
#include "Timer.h"
#include <atomic>
#include <bitset>
#include <memory>
#include <mutex>
#include <unordered_map>

class DynamicMapTree;
class GridMap;

class TC_GAME_API TerrainInfo
{
public:
    explicit TerrainInfo(uint32 mapId);
    TerrainInfo(TerrainInfo const&) = delete;
    TerrainInfo(TerrainInfo&&) = delete;
    TerrainInfo& operator=(TerrainInfo const&) = delete;
    TerrainInfo& operator=(TerrainInfo&&) = delete;
    ~TerrainInfo();

    uint32 GetId() const { return _mapId; }
    char const* GetMapName() const;

    void DiscoverGridMapFiles();

    static bool ExistMap(uint32 mapid, int32 gx, int32 gy, bool log = true);
    static bool ExistVMap(uint32 mapid, int32 gx, int32 gy);

    void LoadMapAndVMap(int32 gx, int32 gy);

private:
    void LoadMapAndVMapImpl(int32 gx, int32 gy);
    void LoadMap(int32 gx, int32 gy);
    void LoadVMap(int32 gx, int32 gy);
    void LoadMMap(int32 gx, int32 gy);

public:
    void UnloadMap(int32 gx, int32 gy);

private:
    void UnloadMapImpl(int32 gx, int32 gy);

    GridMap* GetGrid(float x, float y, bool loadIfMissing = true);

public:
    void CleanUpGrids(uint32 diff);

    void GetFullTerrainStatusForPosition(uint32 phaseMask, float x, float y, float z, PositionFullTerrainStatus& data, Optional<map_liquidHeaderTypeFlags> reqLiquidType = {}, float collisionHeight = 2.03128f, DynamicMapTree const* dynamicMapTree = nullptr); // DEFAULT_COLLISION_HEIGHT in Object.h
    ZLiquidStatus GetLiquidStatus(uint32 phaseMask, float x, float y, float z, Optional<map_liquidHeaderTypeFlags> ReqLiquidType, LiquidData* data = nullptr, float collisionHeight = 2.03128f); // DEFAULT_COLLISION_HEIGHT in Object.h

    bool GetAreaInfo(uint32 phaseMask, float x, float y, float z, uint32& mogpflags, int32& adtId, int32& rootId, int32& groupId, DynamicMapTree const* dynamicMapTree = nullptr);
    uint32 GetAreaId(uint32 phaseMask, float x, float y, float z, DynamicMapTree const* dynamicMapTree = nullptr);
    uint32 GetAreaId(uint32 phaseMask, Position const& pos, DynamicMapTree const* dynamicMapTree = nullptr) { return GetAreaId(phaseMask, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), dynamicMapTree); }
    uint32 GetZoneId(uint32 phaseMask, float x, float y, float z, DynamicMapTree const* dynamicMapTree = nullptr);
    uint32 GetZoneId(uint32 phaseMask, Position const& pos, DynamicMapTree const* dynamicMapTree = nullptr) { return GetZoneId(phaseMask, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), dynamicMapTree); }
    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, float x, float y, float z, DynamicMapTree const* dynamicMapTree = nullptr);
    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, Position const& pos, DynamicMapTree const* dynamicMapTree = nullptr) { GetZoneAndAreaId(phaseMask, zoneid, areaid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), dynamicMapTree); }

    float GetMinHeight(float x, float y);
    float GetGridHeight(float x, float y);
    float GetStaticHeight(float x, float y, float z, bool checkVMap = true, float maxSearchDist = DEFAULT_HEIGHT_SEARCH);
    float GetStaticHeight(Position const& pos, bool checkVMap = true, float maxSearchDist = DEFAULT_HEIGHT_SEARCH) { return GetStaticHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), checkVMap, maxSearchDist); }

    float GetWaterLevel(float x, float y);
    bool IsInWater(uint32 phaseMask, float x, float y, float z, LiquidData* data = nullptr);
    bool IsUnderWater(uint32 phaseMask, float x, float y, float z);

    float GetWaterOrGroundLevel(uint32 phaseMask, float x, float y, float z, float* ground = nullptr, bool swim = false, float collisionHeight = 2.03128f, DynamicMapTree const* dynamicMapTree = nullptr); // DEFAULT_COLLISION_HEIGHT in Object.h

private:
    uint32 _mapId;

    std::mutex _loadMutex;
    std::unique_ptr<GridMap> _gridMap[MAX_NUMBER_OF_GRIDS][MAX_NUMBER_OF_GRIDS];
    std::atomic<uint16> _referenceCountFromMap[MAX_NUMBER_OF_GRIDS][MAX_NUMBER_OF_GRIDS];
    std::bitset<MAX_NUMBER_OF_GRIDS* MAX_NUMBER_OF_GRIDS> _gridFileExists; // cache what grids are available for this map

    static constexpr Milliseconds CleanupInterval = 1min;

    // global garbage collection timer
    TimeTracker _cleanupTimer;
};

class TC_GAME_API TerrainMgr
{
    TerrainMgr();
    ~TerrainMgr();
public:
    TerrainMgr(TerrainMgr const&) = delete;
    TerrainMgr(TerrainMgr&&) = delete;
    TerrainMgr& operator=(TerrainMgr const&) = delete;
    TerrainMgr& operator=(TerrainMgr&&) = delete;

    static TerrainMgr& Instance();

    std::shared_ptr<TerrainInfo> LoadTerrain(uint32 mapId);
    void UnloadAll();

    void Update(uint32 diff);

    uint32 GetAreaId(uint32 phaseMask, uint32 mapid, float x, float y, float z);
    uint32 GetAreaId(uint32 phaseMask, uint32 mapid, Position const& pos) { return GetAreaId(phaseMask, mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()); }
    uint32 GetAreaId(uint32 phaseMask, WorldLocation const& loc) { return GetAreaId(phaseMask, loc.GetMapId(), loc); }

    uint32 GetZoneId(uint32 phaseMask, uint32 mapid, float x, float y, float z);
    uint32 GetZoneId(uint32 phaseMask, uint32 mapid, Position const& pos) { return GetZoneId(phaseMask, mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()); }
    uint32 GetZoneId(uint32 phaseMask, WorldLocation const& loc) { return GetZoneId(phaseMask, loc.GetMapId(), loc); }

    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, uint32 mapid, float x, float y, float z);
    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, uint32 mapid, Position const& pos) { GetZoneAndAreaId(phaseMask, zoneid, areaid, mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()); }
    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, WorldLocation const& loc) { GetZoneAndAreaId(phaseMask, zoneid, areaid, loc.GetMapId(), loc); }

    static bool ExistMapAndVMap(uint32 mapid, float x, float y);

private:
    std::unordered_map<uint32, std::weak_ptr<TerrainInfo>> _terrainMaps;
};

#define sTerrainMgr TerrainMgr::Instance()

#endif // TERRAIN_MGR_H
