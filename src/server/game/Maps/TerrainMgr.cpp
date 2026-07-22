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

#include "TerrainMgr.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "DynamicTree.h"
#include "GridMap.h"
#include "Log.h"
#include "MMapFactory.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Util.h"
#include "VMapFactory.h"
#include "VMapManager2.h"
#include "World.h"
#include <G3D/g3dmath.h>
#include <string_view>

TerrainInfo::TerrainInfo(uint32 mapId) : _mapId(mapId), _cleanupTimer(randtime(CleanupInterval / 2, CleanupInterval))
{
}

TerrainInfo::~TerrainInfo() = default;

char const* TerrainInfo::GetMapName() const
{
    return sMapStore.AssertEntry(GetId())->MapName[sWorld->GetDefaultDbcLocale()];
}

void TerrainInfo::DiscoverGridMapFiles()
{
    for (uint32 gx = 0; gx < MAX_NUMBER_OF_GRIDS; ++gx)
        for (uint32 gy = 0; gy < MAX_NUMBER_OF_GRIDS; ++gy)
            _gridFileExists[gx * MAX_NUMBER_OF_GRIDS + gy] = ExistMap(GetId(), gx, gy, false);
}

bool TerrainInfo::ExistMap(uint32 mapid, int32 gx, int32 gy, bool log /*= true*/)
{
    std::string fileName = Trinity::StringFormat("{}maps/{:03}{:02}{:02}.map", sWorld->GetDataPath(), mapid, gx, gy);

    bool ret = false;
    FILE* file = fopen(fileName.c_str(), "rb");
    if (!file)
    {
        if (log)
        {
            TC_LOG_ERROR("maps", "Map file '{}' does not exist!", fileName);
            TC_LOG_ERROR("maps", "Please place MAP-files (*.map) in the appropriate directory ({}), or correct the DataDir setting in your worldserver.conf file.", (sWorld->GetDataPath() + "maps/"));
        }
    }
    else
    {
        map_fileheader header;
        if (fread(&header, sizeof(header), 1, file) == 1)
        {
            if (header.mapMagic != MapMagic || header.versionMagic != MapVersionMagic)
            {
                if (log)
                    TC_LOG_ERROR("maps", "Map file '{}' is from an incompatible map version ({} v{}), {} v{} is expected. Please pull your source, recompile tools and recreate maps using the updated mapextractor, then replace your old map files with new files. If you still have problems search on forum for error TCE00018.",
                        fileName, std::string_view(header.mapMagic.data(), 4), header.versionMagic, std::string_view(MapMagic.data(), 4), MapVersionMagic);
            }
            else
                ret = true;
        }
        fclose(file);
    }

    return ret;
}

bool TerrainInfo::ExistVMap(uint32 mapid, int32 gx, int32 gy)
{
    if (VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager())
    {
        if (vmgr->isMapLoadingEnabled())
        {
            VMAP::LoadResult result = vmgr->existsMap((sWorld->GetDataPath() + "vmaps").c_str(), mapid, gx, gy);
            std::string name = vmgr->getDirFileName(mapid, gx, gy);
            switch (result)
            {
                case VMAP::LoadResult::Success:
                    break;
                case VMAP::LoadResult::FileNotFound:
                    TC_LOG_ERROR("maps", "VMap file '{}' does not exist", (sWorld->GetDataPath() + "vmaps/" + name));
                    TC_LOG_ERROR("maps", "Please place VMAP files (*.vmtree and *.vmtile) in the vmap directory ({}), or correct the DataDir setting in your worldserver.conf file.", (sWorld->GetDataPath() + "vmaps/"));
                    return false;
                case VMAP::LoadResult::VersionMismatch:
                    TC_LOG_ERROR("maps", "VMap file '{}' couldn't be loaded", (sWorld->GetDataPath() + "vmaps/" + name));
                    TC_LOG_ERROR("maps", "This is because the version of the VMap file and the version of this module are different, please re-extract the maps with the tools compiled with this module.");
                    return false;
                case VMAP::LoadResult::ReadFromFileFailed:
                    TC_LOG_ERROR("maps", "VMap file '{}' couldn't be loaded", (sWorld->GetDataPath() + "vmaps/" + name));
                    TC_LOG_ERROR("maps", "This is because VMAP files are corrupted, please re-extract the maps with the tools compiled with this module.");
                    return false;
                case VMAP::LoadResult::DisabledInConfig:
                    TC_LOG_ERROR("maps", "VMap file '{}' couldn't be loaded", (sWorld->GetDataPath() + "vmaps/" + name));
                    TC_LOG_ERROR("maps", "This is because VMAP is disabled in config file.");
                    return false;
            }
        }
    }

    return true;
}

void TerrainInfo::LoadMapAndVMap(int32 gx, int32 gy)
{
    if (++_referenceCountFromMap[gx][gy] != 1)    // check if already loaded
        return;

    std::lock_guard<std::mutex> lock(_loadMutex);
    LoadMapAndVMapImpl(gx, gy);
}

void TerrainInfo::LoadMapAndVMapImpl(int32 gx, int32 gy)
{
    LoadMap(gx, gy);
    LoadVMap(gx, gy);
    LoadMMap(gx, gy);
}

void TerrainInfo::LoadMap(int32 gx, int32 gy)
{
    if (_gridMap[gx][gy])
        return;

    if (!_gridFileExists[gx * MAX_NUMBER_OF_GRIDS + gy])
        return;

    // map file name
    std::string fileName = Trinity::StringFormat("{}maps/{:03}{:02}{:02}.map", sWorld->GetDataPath(), GetId(), gx, gy);
    TC_LOG_DEBUG("maps", "Loading map {}", fileName);
    // loading data
    std::unique_ptr<GridMap> gridMap = std::make_unique<GridMap>();
    GridMap::LoadResult gridMapLoadResult = gridMap->loadData(fileName.c_str());
    if (gridMapLoadResult == GridMap::LoadResult::Ok)
        _gridMap[gx][gy] = std::move(gridMap);
    else
        _gridFileExists[gx * MAX_NUMBER_OF_GRIDS + gy] = false;

    if (gridMapLoadResult == GridMap::LoadResult::InvalidFile)
        TC_LOG_ERROR("maps", "Error loading map file: {}", fileName);
}

void TerrainInfo::LoadVMap(int32 gx, int32 gy)
{
    if (!VMAP::VMapFactory::createOrGetVMapManager()->isMapLoadingEnabled())
        return;
                                                            // x and y are swapped !!
    VMAP::LoadResult vmapLoadResult = VMAP::VMapFactory::createOrGetVMapManager()->loadMap((sWorld->GetDataPath() + "vmaps").c_str(), GetId(), gx, gy);
    switch (vmapLoadResult)
    {
        case VMAP::LoadResult::Success:
            TC_LOG_DEBUG("maps", "VMAP loaded name:{}, id:{}, x:{}, y:{} (vmap rep.: x:{}, y:{})", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
        case VMAP::LoadResult::DisabledInConfig:
            TC_LOG_DEBUG("maps", "Ignored VMAP name:{}, id:{}, x:{}, y:{} (vmap rep.: x:{}, y:{})", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
        default:
            TC_LOG_ERROR("maps", "Could not load VMAP name:{}, id:{}, x:{}, y:{} (vmap rep.: x:{}, y:{})", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
    }
}

void TerrainInfo::LoadMMap(int32 gx, int32 gy)
{
    if (!DisableMgr::IsPathfindingEnabled(GetId()))
        return;

    bool mmapLoadResult = MMAP::MMapFactory::createOrGetMMapManager()->loadMap(sWorld->GetDataPath(), GetId(), gx, gy);

    if (mmapLoadResult)
        TC_LOG_DEBUG("mmaps.tiles", "MMAP loaded name:{}, id:{}, x:{}, y:{} (mmap rep.: x:{}, y:{})", GetMapName(), GetId(), gx, gy, gx, gy);
    else
        TC_LOG_WARN("mmaps.tiles", "Could not load MMAP name:{}, id:{}, x:{}, y:{} (mmap rep.: x:{}, y:{})", GetMapName(), GetId(), gx, gy, gx, gy);
}

void TerrainInfo::UnloadMap(int32 gx, int32 gy)
{
    --_referenceCountFromMap[gx][gy];
    // unload later
}

void TerrainInfo::UnloadMapImpl(int32 gx, int32 gy)
{
    _gridMap[gx][gy] = nullptr;
    VMAP::VMapFactory::createOrGetVMapManager()->unloadMap(GetId(), gx, gy);
    MMAP::MMapFactory::createOrGetMMapManager()->unloadMap(GetId(), gx, gy);
}

GridMap* TerrainInfo::GetGrid(float x, float y, bool loadIfMissing /*= true*/)
{
    // half opt method
    int32 gx = (int)(CENTER_GRID_ID - x / SIZE_OF_GRIDS);                   //grid x
    int32 gy = (int)(CENTER_GRID_ID - y / SIZE_OF_GRIDS);                   //grid y

    // ensure GridMap is loaded
    if (!_gridMap[gx][gy] && loadIfMissing)
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        LoadMapAndVMapImpl(gx, gy);
    }

    return _gridMap[gx][gy].get();
}

void TerrainInfo::CleanUpGrids(uint32 diff)
{
    _cleanupTimer.Update(diff);
    if (!_cleanupTimer.Passed())
        return;

    // delete those GridMap objects which have refcount = 0
    for (int32 x = 0; x < MAX_NUMBER_OF_GRIDS; ++x)
        for (int32 y = 0; y < MAX_NUMBER_OF_GRIDS; ++y)
            if (_gridMap[x][y] && !_referenceCountFromMap[x][y])
                UnloadMapImpl(x, y);

    _cleanupTimer.Reset(CleanupInterval);
}

static inline bool IsInWMOInterior(uint32 mogpFlags)
{
    return (mogpFlags & 0x2000) != 0;
}

void TerrainInfo::GetFullTerrainStatusForPosition(uint32 phaseMask, float x, float y, float z, PositionFullTerrainStatus& data, Optional<map_liquidHeaderTypeFlags> reqLiquidType /*= {}*/, float collisionHeight /*= DEFAULT_COLLISION_HEIGHT*/, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    VMAP::AreaAndLiquidData vmapData;
    VMAP::AreaAndLiquidData dynData;
    VMAP::AreaAndLiquidData* wmoData = nullptr;
    GridMap* gmap = GetGrid(x, y);
    vmgr->getAreaAndLiquidData(GetId(), x, y, z, reqLiquidType ? AsUnderlyingType(*reqLiquidType) : Optional<uint8>(), vmapData);
    if (dynamicMapTree)
        dynamicMapTree->getAreaAndLiquidData(x, y, z, phaseMask, reqLiquidType ? AsUnderlyingType(*reqLiquidType) : Optional<uint8>(), dynData);

    uint32 gridAreaId = 0;
    float gridMapHeight = INVALID_HEIGHT;
    if (gmap)
    {
        gridAreaId = gmap->getArea(x, y);
        gridMapHeight = gmap->getHeight(x, y);
    }

    bool useGridLiquid = true;

    // floor is the height we are closer to (but only if above)
    data.floorZ = VMAP_INVALID_HEIGHT;
    if (gridMapHeight > INVALID_HEIGHT && G3D::fuzzyGe(z, gridMapHeight - GROUND_HEIGHT_TOLERANCE))
        data.floorZ = gridMapHeight;
    if (vmapData.floorZ > VMAP_INVALID_HEIGHT &&
        G3D::fuzzyGe(z, vmapData.floorZ - GROUND_HEIGHT_TOLERANCE) &&
        (G3D::fuzzyLt(z, gridMapHeight - GROUND_HEIGHT_TOLERANCE) || vmapData.floorZ > gridMapHeight))
    {
        data.floorZ = vmapData.floorZ;
        wmoData = &vmapData;
    }
    // NOTE: Objects will not detect a case when a wmo providing area/liquid despawns from under them
    // but this is fine as these kind of objects are not meant to be spawned and despawned a lot
    // example: Lich King platform
    if (dynData.floorZ > VMAP_INVALID_HEIGHT &&
        G3D::fuzzyGe(z, dynData.floorZ - GROUND_HEIGHT_TOLERANCE) &&
        (G3D::fuzzyLt(z, gridMapHeight - GROUND_HEIGHT_TOLERANCE) || dynData.floorZ > gridMapHeight) &&
        (G3D::fuzzyLt(z, vmapData.floorZ - GROUND_HEIGHT_TOLERANCE) || dynData.floorZ > vmapData.floorZ))
    {
        data.floorZ = dynData.floorZ;
        wmoData = &dynData;
    }

    if (wmoData)
    {
        if (wmoData->areaInfo)
        {
            data.wmoLocation.emplace(wmoData->areaInfo->groupId, wmoData->areaInfo->adtId, wmoData->areaInfo->rootId, wmoData->areaInfo->uniqueId);
            // wmo found
            WMOAreaTableEntry const* wmoEntry = GetWMOAreaTableEntryByTripple(wmoData->areaInfo->rootId, wmoData->areaInfo->adtId, wmoData->areaInfo->groupId);
            data.outdoors = (wmoData->areaInfo->mogpFlags & 0x8) != 0;
            if (wmoEntry)
            {
                data.areaId = wmoEntry->AreaTableID;
                if (wmoEntry->Flags & 4)
                    data.outdoors = true;
                else if (wmoEntry->Flags & 2)
                    data.outdoors = false;
            }

            if (!data.areaId)
                data.areaId = gridAreaId;

            useGridLiquid = !IsInWMOInterior(wmoData->areaInfo->mogpFlags);
        }
    }
    else
    {
        data.outdoors = true;
        data.areaId = gridAreaId;
        if (AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(data.areaId))
            data.outdoors = (areaEntry->Flags & (AREA_FLAG_INSIDE | AREA_FLAG_OUTSIDE)) != AREA_FLAG_INSIDE;
    }

    if (!data.areaId)
        data.areaId = sMapStore.AssertEntry(GetId())->AreaTableID;

    AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(data.areaId);

    // liquid processing
    data.liquidStatus = LIQUID_MAP_NO_WATER;
    if (wmoData && wmoData->liquidInfo && wmoData->liquidInfo->level > wmoData->floorZ)
    {
        uint32 liquidType = wmoData->liquidInfo->type;
        if (GetId() == 530 && liquidType == 2) // gotta love blizzard hacks
            liquidType = 15;

        uint32 liquidFlagType = 0;
        if (LiquidTypeEntry const* liquidData = sLiquidTypeStore.LookupEntry(liquidType))
            liquidFlagType = liquidData->SoundBank;

        if (liquidType && liquidType < 21 && areaEntry)
        {
            uint32 overrideLiquid = areaEntry->LiquidTypeID[liquidFlagType];
            if (!overrideLiquid && areaEntry->ParentAreaID)
            {
                AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(areaEntry->ParentAreaID);
                if (zoneEntry)
                    overrideLiquid = zoneEntry->LiquidTypeID[liquidFlagType];
            }

            if (LiquidTypeEntry const* overrideData = sLiquidTypeStore.LookupEntry(overrideLiquid))
            {
                liquidType = overrideLiquid;
                liquidFlagType = overrideData->SoundBank;
            }
        }

        data.liquidInfo.emplace();
        data.liquidInfo->level = wmoData->liquidInfo->level;
        data.liquidInfo->depth_level = wmoData->floorZ;
        data.liquidInfo->entry = liquidType;
        data.liquidInfo->type_flags = map_liquidHeaderTypeFlags(1 << liquidFlagType);

        float delta = wmoData->liquidInfo->level - z;
        if (delta > collisionHeight)
            data.liquidStatus = LIQUID_MAP_UNDER_WATER;
        else if (delta > 0.0f)
            data.liquidStatus = LIQUID_MAP_IN_WATER;
        else if (delta > -0.1f)
            data.liquidStatus = LIQUID_MAP_WATER_WALK;
        else
            data.liquidStatus = LIQUID_MAP_ABOVE_WATER;
    }
    // look up liquid data from grid map
    if (gmap && useGridLiquid)
    {
        LiquidData gridMapLiquid;
        ZLiquidStatus gridMapStatus = gmap->GetLiquidStatus(x, y, z, reqLiquidType, &gridMapLiquid, collisionHeight);
        if (gridMapStatus != LIQUID_MAP_NO_WATER && (!wmoData || gridMapLiquid.level > wmoData->floorZ))
        {
            if (GetId() == 530 && gridMapLiquid.entry == 2)
                gridMapLiquid.entry = 15;
            data.liquidInfo = gridMapLiquid;
            data.liquidStatus = gridMapStatus;
        }
    }
}

ZLiquidStatus TerrainInfo::GetLiquidStatus(uint32 phaseMask, float x, float y, float z, Optional<map_liquidHeaderTypeFlags> ReqLiquidType, LiquidData* data /*= nullptr*/, float collisionHeight /*= DEFAULT_COLLISION_HEIGHT*/)
{
    ZLiquidStatus result = LIQUID_MAP_NO_WATER;
    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    VMAP::AreaAndLiquidData vmapData;
    bool useGridLiquid = true;
    if (vmgr->getAreaAndLiquidData(GetId(), x, y, z, ReqLiquidType ? AsUnderlyingType(*ReqLiquidType) : Optional<uint8>(), vmapData) && vmapData.liquidInfo)
    {
        useGridLiquid = !vmapData.areaInfo || !IsInWMOInterior(vmapData.areaInfo->mogpFlags);
        TC_LOG_DEBUG("maps", "GetLiquidStatus(): vmap liquid level: {} ground: {} type: {}", vmapData.liquidInfo->level, vmapData.floorZ, vmapData.liquidInfo->type);
        // Check water level and ground level
        if (vmapData.liquidInfo->level > vmapData.floorZ && G3D::fuzzyGe(z, vmapData.floorZ - GROUND_HEIGHT_TOLERANCE))
        {
            // All ok in water -> store data
            if (data)
            {
                // hardcoded in client like this
                if (GetId() == 530 && vmapData.liquidInfo->type == 2)
                    vmapData.liquidInfo->type = 15;

                uint32 liquidFlagType = 0;
                if (LiquidTypeEntry const* liq = sLiquidTypeStore.LookupEntry(vmapData.liquidInfo->type))
                    liquidFlagType = liq->SoundBank;

                if (vmapData.liquidInfo->type && vmapData.liquidInfo->type < 21)
                {
                    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(GetAreaId(phaseMask, x, y, z)))
                    {
                        uint32 overrideLiquid = area->LiquidTypeID[liquidFlagType];
                        if (!overrideLiquid && area->ParentAreaID)
                        {
                            area = sAreaTableStore.LookupEntry(area->ParentAreaID);
                            if (area)
                                overrideLiquid = area->LiquidTypeID[liquidFlagType];
                        }

                        if (LiquidTypeEntry const* liq = sLiquidTypeStore.LookupEntry(overrideLiquid))
                        {
                            vmapData.liquidInfo->type = overrideLiquid;
                            liquidFlagType = liq->SoundBank;
                        }
                    }
                }

                data->level = vmapData.liquidInfo->level;
                data->depth_level = vmapData.floorZ;

                data->entry = vmapData.liquidInfo->type;
                data->type_flags = map_liquidHeaderTypeFlags(1 << liquidFlagType);
            }

            float delta = vmapData.liquidInfo->level - z;

            // Get position delta
            if (delta > collisionHeight)                   // Under water
                return LIQUID_MAP_UNDER_WATER;
            if (delta > 0.0f)                   // In water
                return LIQUID_MAP_IN_WATER;
            if (delta > -0.1f)                   // Walk on water
                return LIQUID_MAP_WATER_WALK;
            result = LIQUID_MAP_ABOVE_WATER;
        }
    }

    if (useGridLiquid)
    {
        if (GridMap* gmap = GetGrid(x, y))
        {
            LiquidData map_data;
            ZLiquidStatus map_result = gmap->GetLiquidStatus(x, y, z, ReqLiquidType, &map_data, collisionHeight);
            // Not override LIQUID_MAP_ABOVE_WATER with LIQUID_MAP_NO_WATER:
            if (map_result != LIQUID_MAP_NO_WATER && (map_data.level > vmapData.floorZ))
            {
                if (data)
                {
                    // hardcoded in client like this
                    if (GetId() == 530 && map_data.entry == 2)
                        map_data.entry = 15;

                    *data = map_data;
                }
                return map_result;
            }
        }
    }
    return result;
}

bool TerrainInfo::GetAreaInfo(uint32 phaseMask, float x, float y, float z, uint32& flags, int32& adtId, int32& rootId, int32& groupId, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    float check_z = z;
    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    VMAP::AreaAndLiquidData vdata;
    VMAP::AreaAndLiquidData ddata;

    bool hasVmapAreaInfo = vmgr->getAreaAndLiquidData(GetId(), x, y, z, {}, vdata) && vdata.areaInfo.has_value();
    bool hasDynamicAreaInfo = dynamicMapTree ? dynamicMapTree->getAreaAndLiquidData(x, y, z, phaseMask, {}, ddata) && ddata.areaInfo.has_value() : false;
    auto useVmap = [&] { check_z = vdata.floorZ; groupId = vdata.areaInfo->groupId; adtId = vdata.areaInfo->adtId; rootId = vdata.areaInfo->rootId; flags = vdata.areaInfo->mogpFlags; };
    auto useDyn = [&] { check_z = ddata.floorZ; groupId = ddata.areaInfo->groupId; adtId = ddata.areaInfo->adtId; rootId = ddata.areaInfo->rootId; flags = ddata.areaInfo->mogpFlags; };
    if (hasVmapAreaInfo)
    {
        if (hasDynamicAreaInfo && ddata.floorZ > vdata.floorZ)
            useDyn();
        else
            useVmap();
    }
    else if (hasDynamicAreaInfo)
    {
        useDyn();
    }

    if (hasVmapAreaInfo || hasDynamicAreaInfo)
    {
        // check if there's terrain between player height and object height
        if (GridMap* gmap = GetGrid(x, y))
        {
            float mapHeight = gmap->getHeight(x, y);
            // z + 2.0f condition taken from GetHeight(), not sure if it's such a great choice...
            if (z + 2.0f > mapHeight && mapHeight > check_z)
                return false;
        }
        return true;
    }
    return false;
}

uint32 TerrainInfo::GetAreaId(uint32 phaseMask, float x, float y, float z, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    uint32 mogpFlags;
    int32 adtId, rootId, groupId;
    float vmapZ = z;
    bool hasVmapArea = GetAreaInfo(phaseMask, x, y, vmapZ, mogpFlags, adtId, rootId, groupId, dynamicMapTree);

    uint32 gridAreaId = 0;
    float gridMapHeight = INVALID_HEIGHT;
    if (GridMap* gmap = GetGrid(x, y))
    {
        gridAreaId = gmap->getArea(x, y);
        gridMapHeight = gmap->getHeight(x, y);
    }

    uint32 areaId = 0;

    // floor is the height we are closer to (but only if above)
    if (hasVmapArea && G3D::fuzzyGe(z, vmapZ - GROUND_HEIGHT_TOLERANCE) && (G3D::fuzzyLt(z, gridMapHeight - GROUND_HEIGHT_TOLERANCE) || vmapZ > gridMapHeight))
    {
        // wmo found
        if (WMOAreaTableEntry const* wmoEntry = GetWMOAreaTableEntryByTripple(rootId, adtId, groupId))
            areaId = wmoEntry->AreaTableID;

        if (!areaId)
            areaId = gridAreaId;
    }
    else
        areaId = gridAreaId;

    if (!areaId)
        areaId = sMapStore.AssertEntry(GetId())->AreaTableID;

    return areaId;
}

uint32 TerrainInfo::GetZoneId(uint32 phaseMask, float x, float y, float z, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    uint32 areaId = GetAreaId(phaseMask, x, y, z, dynamicMapTree);
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
        if (area->ParentAreaID)
            return area->ParentAreaID;

    return areaId;
}

void TerrainInfo::GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, float x, float y, float z, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    areaid = zoneid = GetAreaId(phaseMask, x, y, z, dynamicMapTree);
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaid))
        if (area->ParentAreaID)
            zoneid = area->ParentAreaID;
}

float TerrainInfo::GetMinHeight(float x, float y)
{
    if (GridMap const* grid = GetGrid(x, y))
        return grid->getMinHeight(x, y);

    return -500.0f;
}

float TerrainInfo::GetGridHeight(float x, float y)
{
    if (GridMap* gmap = GetGrid(x, y))
        return gmap->getHeight(x, y);

    return VMAP_INVALID_HEIGHT_VALUE;
}

float TerrainInfo::GetStaticHeight(float x, float y, float z, bool checkVMap /*= true*/, float maxSearchDist /*= DEFAULT_HEIGHT_SEARCH*/)
{
    // find raw .map surface under Z coordinates
    float mapHeight = VMAP_INVALID_HEIGHT_VALUE;
    float gridHeight = GetGridHeight(x, y);
    if (G3D::fuzzyGe(z, gridHeight - GROUND_HEIGHT_TOLERANCE))
        mapHeight = gridHeight;

    float vmapHeight = VMAP_INVALID_HEIGHT_VALUE;
    if (checkVMap)
    {
        VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
        if (vmgr->isHeightCalcEnabled())
            vmapHeight = vmgr->getHeight(GetId(), x, y, z, maxSearchDist);
    }

    // mapHeight set for any above raw ground Z or <= INVALID_HEIGHT
    // vmapheight set for any under Z value or <= INVALID_HEIGHT
    if (vmapHeight > INVALID_HEIGHT)
    {
        if (mapHeight > INVALID_HEIGHT)
        {
            // we have mapheight and vmapheight and must select more appropriate

            // vmap height above map height
            // or if the distance of the vmap height is less the land height distance
            if (vmapHeight > mapHeight || std::fabs(mapHeight - z) > std::fabs(vmapHeight - z))
                return vmapHeight;

            return mapHeight;                           // better use .map surface height
        }

        return vmapHeight;                              // we have only vmapHeight (if have)
    }

    return mapHeight;                               // explicitly use map data
}

float TerrainInfo::GetWaterLevel(float x, float y)
{
    if (GridMap* gmap = GetGrid(x, y))
        return gmap->getLiquidLevel(x, y);
    else
        return 0;
}

bool TerrainInfo::IsInWater(uint32 phaseMask, float x, float y, float pZ, LiquidData* data /*= nullptr*/)
{
    LiquidData liquid_status;
    LiquidData* liquid_ptr = data ? data : &liquid_status;
    return (GetLiquidStatus(phaseMask, x, y, pZ, {}, liquid_ptr) & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER)) != 0;
}

bool TerrainInfo::IsUnderWater(uint32 phaseMask, float x, float y, float z)
{
    return (GetLiquidStatus(phaseMask, x, y, z, map_liquidHeaderTypeFlags::Water | map_liquidHeaderTypeFlags::Ocean) & LIQUID_MAP_UNDER_WATER) != 0;
}

float TerrainInfo::GetWaterOrGroundLevel(uint32 phaseMask, float x, float y, float z, float* ground /*= nullptr*/, bool /*swim = false*/, float collisionHeight /*= DEFAULT_COLLISION_HEIGHT*/, DynamicMapTree const* dynamicMapTree /*= nullptr*/)
{
    if (GetGrid(x, y))
    {
        // we need ground level (including grid height version) for proper return water level in point
        float ground_z = GetStaticHeight(x, y, z + Z_OFFSET_FIND_HEIGHT, true, 50.0f);
        if (dynamicMapTree)
            ground_z = std::max<float>(ground_z, dynamicMapTree->getHeight(x, y, z + Z_OFFSET_FIND_HEIGHT, 50.0f, phaseMask));

        if (ground)
            *ground = ground_z;

        LiquidData liquid_status;

        ZLiquidStatus res = GetLiquidStatus(phaseMask, x, y, ground_z, {}, &liquid_status, collisionHeight);
        switch (res)
        {
            case LIQUID_MAP_ABOVE_WATER:
                return std::max<float>(liquid_status.level, ground_z);
            case LIQUID_MAP_NO_WATER:
                return ground_z;
            default:
                return liquid_status.level;
        }
    }

    return VMAP_INVALID_HEIGHT_VALUE;
}

TerrainMgr::TerrainMgr() = default;

TerrainMgr::~TerrainMgr() = default;

TerrainMgr& TerrainMgr::Instance()
{
    static TerrainMgr instance;
    return instance;
}

std::shared_ptr<TerrainInfo> TerrainMgr::LoadTerrain(uint32 mapId)
{
    if (!sMapStore.LookupEntry(mapId))
        return nullptr;

    auto itr = _terrainMaps.find(mapId);
    if (itr != _terrainMaps.end())
        if (std::shared_ptr<TerrainInfo> terrain = itr->second.lock())
            return terrain;

    std::shared_ptr<TerrainInfo> terrainInfo(new TerrainInfo(mapId)); // intentionally not using make_shared, don't want control block allocated together, will be relying on weak_ptr

    terrainInfo->DiscoverGridMapFiles();

    _terrainMaps[mapId] = terrainInfo;
    return terrainInfo;
}

void TerrainMgr::UnloadAll()
{
    _terrainMaps.clear();
}

void TerrainMgr::Update(uint32 diff)
{
    // global garbage collection
    for (auto& [mapId, terrainRef] : _terrainMaps)
        if (std::shared_ptr<TerrainInfo> terrain = terrainRef.lock())
            terrain->CleanUpGrids(diff);
}

uint32 TerrainMgr::GetAreaId(uint32 phaseMask, uint32 mapid, float x, float y, float z)
{
    if (std::shared_ptr<TerrainInfo> t = LoadTerrain(mapid))
        return t->GetAreaId(phaseMask, x, y, z);
    return 0;
}

uint32 TerrainMgr::GetZoneId(uint32 phaseMask, uint32 mapid, float x, float y, float z)
{
    if (std::shared_ptr<TerrainInfo> t = LoadTerrain(mapid))
        return t->GetZoneId(phaseMask, x, y, z);
    return 0;
}

void TerrainMgr::GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, uint32 mapid, float x, float y, float z)
{
    if (std::shared_ptr<TerrainInfo> t = LoadTerrain(mapid))
        t->GetZoneAndAreaId(phaseMask, zoneid, areaid, x, y, z);
}

bool TerrainMgr::ExistMapAndVMap(uint32 mapid, float x, float y)
{
    GridCoord p = Trinity::ComputeGridCoord(x, y);

    int32 gx = (MAX_NUMBER_OF_GRIDS - 1) - p.x_coord;
    int32 gy = (MAX_NUMBER_OF_GRIDS - 1) - p.y_coord;

    return TerrainInfo::ExistMap(mapid, gx, gy) && TerrainInfo::ExistVMap(mapid, gx, gy);
}
