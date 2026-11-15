/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
* @file cs_mmaps.cpp
* @brief .mmap related commands
*
* This file contains the CommandScripts for all
* mmap sub-commands
*/

#include "CellImpl.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DetourNavMesh.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MMapMgr.h"
#include "Map.h"
#include "MapDefines.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TargetedMovementGenerator.h"
#include "WorldScript.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace Acore::ChatCommands;

namespace
{
    // Rebuilds a single mmap tile by running mmaps_generator for it on a worker thread, then swaps the
    // fresh tile into the live navmesh on the main thread (between map updates, so no map thread is
    // querying the mesh while it changes).
    class NavMeshTileRebuilder
    {
    public:
        struct Job
        {
            ObjectGuid requester;
            uint32 mapId;
            uint32 instanceId;
            uint32 gridX;       // tile file name is <map><gridX><gridY>.mmtile, same as the server loads it
            uint32 gridY;
            std::string dataDir;
            std::string generatorPath;
            std::string configPath; // optional
        };

        struct Result
        {
            Job job;
            bool success{false};
            std::string message;
            uint32 seconds{0};
        };

        // false when a rebuild is already running
        static bool TryStart(Job job)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_running)
                return false;

            if (_worker.joinable())
                _worker.join();

            _running = true;
            _worker = std::thread([job = std::move(job)]() mutable { Run(std::move(job)); });
            return true;
        }

        // main thread, called every world tick
        static void ProcessCompleted()
        {
            if (!_hasResults.load(std::memory_order_acquire))
                return;

            std::vector<Result> results;
            {
                std::lock_guard<std::mutex> guard(_mutex);
                results.swap(_results);
                _hasResults = false;
            }

            for (Result const& result : results)
                Apply(result);
        }

        static void Shutdown()
        {
            std::thread worker;
            {
                std::lock_guard<std::mutex> guard(_mutex);
                worker = std::move(_worker);
            }
            if (worker.joinable())
                worker.join();
        }

    private:
        static std::string TilePath(Job const& job)
        {
            return Acore::StringFormat(MMAP::TILE_FILE_NAME_FORMAT, job.dataDir, job.mapId, job.gridX, job.gridY);
        }

        static int ExitCodeOf(int rc)
        {
#ifdef _WIN32
            return rc;
#else
            return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
        }

        // worker thread: touches files and the generator process only, never server state
        static void Run(Job job)
        {
            namespace fs = std::filesystem;

            Result result;
            result.job = job;
            auto const start = std::chrono::steady_clock::now();

            std::string const tile = TilePath(job);
            std::string const backup = tile + ".bak";
            std::error_code ec;

            // mmaps_generator skips a tile that already exists with the same settings, so move the old one away
            bool const hadOld = fs::exists(tile, ec);
            if (hadOld)
            {
                fs::remove(backup, ec);
                fs::rename(tile, backup, ec);
                if (ec)
                {
                    result.message = Acore::StringFormat("could not move the old tile away ({}): {}", tile, ec.message());
                    Finish(std::move(result), start);
                    return;
                }
            }

            // generator args: <map> --tile <tileX>,<tileY>; the generator names the file <map><tileY><tileX>,
            // so its X is the server's gridY and its Y is the server's gridX
            std::string command = Acore::StringFormat("\"{}\" {} --tile {},{} --silent --threads 1",
                job.generatorPath, job.mapId, job.gridY, job.gridX);
            if (!job.configPath.empty())
                command += Acore::StringFormat(" --config \"{}\"", job.configPath);
#ifdef _WIN32
            command = "\"" + command + "\"";   // cmd.exe strips the outer quotes
#endif

            LOG_INFO("maps", "NavMesh rebuild: running {}", command);
            int const exitCode = ExitCodeOf(std::system(command.c_str()));

            bool const produced = fs::exists(tile, ec);
            if (exitCode == 0 && produced)
            {
                result.success = true;
                fs::remove(backup, ec);
            }
            else
            {
                // keep the previous tile on any failure
                if (produced)
                    fs::remove(tile, ec);
                if (hadOld)
                    fs::rename(backup, tile, ec);

                if (exitCode != 0)
                    result.message = Acore::StringFormat("mmaps_generator failed (exit code {}). Check MMapRebuild.GeneratorPath / MMapRebuild.ConfigPath and the console output", exitCode);
                else
                    result.message = Acore::StringFormat("mmaps_generator produced no tile (no geometry here, or the dataDir in mmaps-config.yaml differs from DataDir: expected {})", tile);
            }

            Finish(std::move(result), start);
        }

        static void Finish(Result result, std::chrono::steady_clock::time_point start)
        {
            result.seconds = uint32(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());

            std::lock_guard<std::mutex> guard(_mutex);
            _results.push_back(std::move(result));
            _hasResults = true;
            _running = false;
        }

        // main thread
        static void Apply(Result const& result)
        {
            Job const& job = result.job;
            Player* player = ObjectAccessor::FindConnectedPlayer(job.requester);

            auto say = [&](std::string const& text)
            {
                LOG_INFO("maps", "NavMesh rebuild [map {} tile {},{}]: {}", job.mapId, job.gridX, job.gridY, text);
                if (player && player->GetSession())
                    ChatHandler(player->GetSession()).SendSysMessage(text);
            };

            if (!result.success)
            {
                say("Rebuild failed, old tile kept: " + result.message);
                return;
            }

            Map* map = sMapMgr->FindMap(job.mapId, job.instanceId);
            dtNavMesh* navMesh = map ? map->GetMapCollisionData().GetMMapNavMeshSharedPtr().get() : nullptr;
            if (!navMesh)
            {
                say(Acore::StringFormat("Tile {:03}{:02}{:02} rebuilt in {}s. The map has no live navmesh, so it will be used the next time the map loads.", job.mapId, job.gridX, job.gridY, result.seconds));
                return;
            }

            // read the new file's detour header to find which live tile it replaces
            dtMeshHeader meshHeader;
            {
                FILE* file = fopen(TilePath(job).c_str(), "rb");
                MmapTileHeader fileHeader;
                bool ok = file && fread(&fileHeader, sizeof(fileHeader), 1, file) == 1 && fileHeader.mmapMagic == MMAP_MAGIC &&
                          fread(&meshHeader, sizeof(meshHeader), 1, file) == 1;
                if (file)
                    fclose(file);
                if (!ok)
                {
                    say("Tile rebuilt but its header could not be read, restart to apply it.");
                    return;
                }
            }

            if (dtTileRef oldRef = navMesh->getTileRefAt(meshHeader.x, meshHeader.y, meshHeader.layer))
                navMesh->removeTile(oldRef, nullptr, nullptr);

            if (MMAP::MMapMgr::LoadTile(navMesh, job.mapId, job.gridX, job.gridY))
                say(Acore::StringFormat("Navmesh tile {:03}[{:02},{:02}] rebuilt in {}s and reloaded.", job.mapId, job.gridX, job.gridY, result.seconds));
            else
                say("Tile rebuilt but reloading it into the live navmesh failed, restart to apply it.");
        }

        static inline std::mutex _mutex;
        static inline std::thread _worker;
        static inline std::vector<Result> _results;
        static inline std::atomic<bool> _hasResults{false};
        static inline bool _running{false};
    };
}

class mmaps_commandscript : public CommandScript
{
public:
    mmaps_commandscript() : CommandScript("mmaps_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable mmapCommandTable =
        {
            { "loadedtiles", HandleMmapLoadedTilesCommand, rbac::RBAC_PERM_COMMAND_MMAP_LOADEDTILES, Console::No },
            { "loc",         HandleMmapLocCommand,         rbac::RBAC_PERM_COMMAND_MMAP_LOC,         Console::No },
            { "path",        HandleMmapPathCommand,        rbac::RBAC_PERM_COMMAND_MMAP_PATH,        Console::No },
            { "stats",       HandleMmapStatsCommand,       rbac::RBAC_PERM_COMMAND_MMAP_STATS,       Console::No },
            { "testarea",    HandleMmapTestArea,           rbac::RBAC_PERM_COMMAND_MMAP_TESTAREA,    Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "mmap",           mmapCommandTable },
            { "rebuildnavmesh", HandleRebuildNavMeshCommand, SEC_GAMEMASTER, Console::No }
        };
        return commandTable;
    }

    // .rebuildnavmesh: rebuilds the mmap tile the player stands on, from the current maps/vmaps data
    static bool HandleRebuildNavMeshCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        Map* map = player->GetMap();
        if (!map->GetMapCollisionData().GetMMapData().GetNavMesh())
        {
            handler->SendSysMessage("This map has no navmesh loaded (no mmaps/<map>.mmap). Generate it once with mmaps_generator first.");
            return true;
        }

        std::string const generatorPath = sConfigMgr->GetOption<std::string>("MMapRebuild.GeneratorPath", "mmaps_generator");
        std::string const configPath = sConfigMgr->GetOption<std::string>("MMapRebuild.ConfigPath", "");
        if (generatorPath.find('"') != std::string::npos || configPath.find('"') != std::string::npos)
        {
            handler->SendSysMessage("MMapRebuild.GeneratorPath / MMapRebuild.ConfigPath must not contain double quotes.");
            return true;
        }

        // the same grid the server uses to pick the tile file of this position
        Cell cell(Acore::ComputeCellCoord(player->GetPositionX(), player->GetPositionY()));

        NavMeshTileRebuilder::Job job;
        job.requester = player->GetGUID();
        job.mapId = map->GetId();
        job.instanceId = map->GetInstanceId();
        job.gridX = cell.GridX();
        job.gridY = cell.GridY();
        job.dataDir = sConfigMgr->GetOption<std::string>("DataDir", ".");
        job.generatorPath = generatorPath;
        job.configPath = configPath;

        if (!NavMeshTileRebuilder::TryStart(std::move(job)))
        {
            handler->SendSysMessage("A navmesh rebuild is already running, wait for it to finish.");
            return true;
        }

        handler->PSendSysMessage("Rebuilding navmesh tile {:03}[{:02},{:02}] in the background, you will be told when it is done.", map->GetId(), cell.GridX(), cell.GridY());
        return true;
    }

    static bool HandleMmapPathCommand(ChatHandler* handler, Optional<std::string> param)
    {
        Map* map = handler->GetSession()->GetPlayer()->GetMap();
        if (!map->GetMapCollisionData().GetMMapData().GetNavMesh())
        {
            handler->PSendSysMessage("NavMesh not loaded for current map.");
            return true;
        }

        handler->PSendSysMessage("mmap path:");

        // units
        Player* player = handler->GetSession()->GetPlayer();
        Unit* target = handler->getSelectedUnit();
        if (!player || !target)
        {
            handler->PSendSysMessage("Invalid target/source selection.");
            return true;
        }

        bool useStraightPath = false;
        bool useRaycast = false;
        if (param)
        {
            auto paramValue = param.value();
            if (paramValue.starts_with("true"))
            {
                useStraightPath = true;
            }

            if (paramValue.starts_with("line") || paramValue.starts_with("ray") || paramValue.starts_with("raycast"))
            {
                useRaycast = true;
            }
        }

        // unit locations
        float x, y, z;
        player->GetPosition(x, y, z);

        // path
        PathGenerator path(target);
        path.SetUseStraightPath(useStraightPath);
        path.SetUseRaycast(useRaycast);
        bool result = path.CalculatePath(x, y, z, false);

        Movement::PointsArray const& pointPath = path.GetPath();
        handler->PSendSysMessage("{}'s path to {}:", target->GetName(), player->GetName());
        handler->PSendSysMessage("Building: {}", useStraightPath ? "StraightPath" : useRaycast ? "Raycast" : "SmoothPath");
        handler->PSendSysMessage("Result: {} - Length: {} - Type: {}", (result ? "true" : "false"), pointPath.size(), path.GetPathType());

        G3D::Vector3 const& start = path.GetStartPosition();
        G3D::Vector3 const& end = path.GetEndPosition();
        G3D::Vector3 const& actualEnd = path.GetActualEndPosition();

        handler->PSendSysMessage("StartPosition     ({}, {}, {})", start.x, start.y, start.z);
        handler->PSendSysMessage("EndPosition       ({}, {}, {})", end.x, end.y, end.z);
        handler->PSendSysMessage("ActualEndPosition ({}, {}, {})", actualEnd.x, actualEnd.y, actualEnd.z);

        if (!player->IsGameMaster())
            handler->PSendSysMessage("Enable GM mode to see the path points.");

        for (auto i : pointPath)
            player->SummonCreature(VISUAL_WAYPOINT, i.x, i.y, i.z, 0, TEMPSUMMON_TIMED_DESPAWN, 9000);

        return true;
    }

    static bool HandleMmapLocCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("mmap tileloc:");

        // grid tile location
        Player* player = handler->GetSession()->GetPlayer();

        GridCoord const gridCoord = Acore::ComputeGridCoord(player->GetPositionX(), player->GetPositionY());

        handler->PSendSysMessage("{}{}{}.mmtile", player->GetMapId(), gridCoord.x_coord, gridCoord.y_coord);

        std::string fileName = Acore::StringFormat(MMAP::TILE_FILE_NAME_FORMAT, sConfigMgr->GetOption<std::string>("DataDir", "."), player->GetMapId(), gridCoord.x_coord, gridCoord.y_coord);
        FILE* file = fopen(fileName.c_str(), "rb");
        if (!file)
        {
            LOG_DEBUG("maps", "MMAP:loadMap: Could not open mmtile file '{}'", fileName);
            return false;
        }

        // read header
        MmapTileHeader fileHeader;
        if (fread(&fileHeader, sizeof(MmapTileHeader), 1, file) != 1 || fileHeader.mmapMagic != MMAP_MAGIC)
        {
            LOG_ERROR("maps", "MMAP:loadMap: Bad header in mmap {:03}{:02}{:02}.mmtile", player->GetMapId(), gridCoord.x_coord, gridCoord.y_coord);
            fclose(file);
            return false;
        }
        fclose(file);
        handler->PSendSysMessage("Recast config used:");
        handler->PSendSysMessage("- walkableSlopeAngle: {}", fileHeader.recastConfig.walkableSlopeAngle);

        const float cellHeight = fileHeader.recastConfig.cellSizeVertical;
        handler->PSendSysMessage("- walkableHeight: {} ({} units)", fileHeader.recastConfig.walkableHeight * cellHeight, fileHeader.recastConfig.walkableHeight);
        handler->PSendSysMessage("- walkableClimb: {} ({} units)", fileHeader.recastConfig.walkableClimb * cellHeight, fileHeader.recastConfig.walkableClimb);
        handler->PSendSysMessage("- walkableRadius: {} ({} units)", fileHeader.recastConfig.walkableRadius * cellHeight, fileHeader.recastConfig.walkableRadius);

        handler->PSendSysMessage("- maxSimplificationError: {}", fileHeader.recastConfig.maxSimplificationError);
        handler->PSendSysMessage("- vertexPerMapEdge: {}", fileHeader.recastConfig.vertexPerMapEdge);
        handler->PSendSysMessage("- vertexPerTileEdge: {}", fileHeader.recastConfig.vertexPerTileEdge);
        handler->PSendSysMessage("- tilesPerMapEdge: {}", fileHeader.recastConfig.tilesPerMapEdge);
        handler->PSendSysMessage("- baseUnitDim: {}", fileHeader.recastConfig.baseUnitDim);
        handler->PSendSysMessage("- cellSizeHorizontal: {}", fileHeader.recastConfig.cellSizeHorizontal);
        handler->PSendSysMessage("- cellSizeVertical: {}", fileHeader.recastConfig.cellSizeVertical);

        handler->PSendSysMessage("gridloc [{}, {}]", gridCoord.x_coord, gridCoord.y_coord);

        Map* map = handler->GetSession()->GetPlayer()->GetMap();

        // calculate navmesh tile location
        dtNavMesh const* navmesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        dtNavMeshQuery const* navmeshquery = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!navmesh || !navmeshquery)
        {
            handler->PSendSysMessage("NavMesh not loaded for current map.");
            return true;
        }

        float const* min = navmesh->getParams()->orig;
        float x, y, z;
        player->GetPosition(x, y, z);
        float location[VERTEX_SIZE] = {y, z, x};
        float extents[VERTEX_SIZE] = {3.0f, 5.0f, 3.0f};

        int32 tilex = int32((y - min[0]) / SIZE_OF_GRIDS);
        int32 tiley = int32((x - min[2]) / SIZE_OF_GRIDS);

        handler->PSendSysMessage("Calc   [{}, {}]", tilex, tiley);

        // navmesh poly -> navmesh tile location
        dtQueryFilterExt filter = dtQueryFilterExt();
        dtPolyRef polyRef = INVALID_POLYREF;
        if (dtStatusFailed(navmeshquery->findNearestPoly(location, extents, &filter, &polyRef, nullptr)))
        {
            handler->PSendSysMessage("Dt     [??,??] (invalid poly, probably no tile loaded)");
            return true;
        }

        if (polyRef == INVALID_POLYREF)
            handler->PSendSysMessage("Dt     [??, ??] (invalid poly, probably no tile loaded)");
        else
        {
            dtMeshTile const* tile;
            dtPoly const* poly;
            if (dtStatusSucceed(navmesh->getTileAndPolyByRef(polyRef, &tile, &poly)))
            {
                if (tile)
                {
                    handler->PSendSysMessage("Dt     [{},{}]", tile->header->x, tile->header->y);
                    return false;
                }
            }

            handler->PSendSysMessage("Dt     [??,??] (no tile loaded)");
        }

        return true;
    }

    static bool HandleMmapLoadedTilesCommand(ChatHandler* handler)
    {
        Map* map = handler->GetSession()->GetPlayer()->GetMap();
        dtNavMesh const* navmesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        dtNavMeshQuery const* navmeshquery = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!navmesh || !navmeshquery)
        {
            handler->PSendSysMessage("NavMesh not loaded for current map.");
            return true;
        }

        handler->PSendSysMessage("mmap loadedtiles:");

        for (int32 i = 0; i < navmesh->getMaxTiles(); ++i)
        {
            dtMeshTile const* tile = navmesh->getTile(i);
            if (!tile || !tile->header)
                continue;

            handler->PSendSysMessage("[{}, {}]", tile->header->x, tile->header->y);
        }

        return true;
    }

    static bool HandleMmapStatsCommand(ChatHandler* handler)
    {
        Map* map = handler->GetSession()->GetPlayer()->GetMap();

        //handler->PSendSysMessage("mmap stats:");
        //handler->PSendSysMessage("  global mmap pathfinding is {}abled", sDisableMgr->IsPathfindingEnabled(mapId) ? "en" : "dis");
        //MMAP::MMapMgr* manager = MMAP::MMapFactory::createOrGetMMapMgr();
        //handler->PSendSysMessage(" {} maps loaded with {} tiles overall", manager->getLoadedMapsCount(), manager->getLoadedTilesCount());

        dtNavMesh const* navmesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!navmesh)
        {
            handler->PSendSysMessage("NavMesh not loaded for current map.");
            return true;
        }

        uint32 tileCount = 0;
        uint32 nodeCount = 0;
        uint32 polyCount = 0;
        uint32 vertCount = 0;
        uint32 triCount = 0;
        uint32 triVertCount = 0;
        uint32 dataSize = 0;
        for (int32 i = 0; i < navmesh->getMaxTiles(); ++i)
        {
            dtMeshTile const* tile = navmesh->getTile(i);
            if (!tile || !tile->header)
                continue;

            tileCount++;
            nodeCount += tile->header->bvNodeCount;
            polyCount += tile->header->polyCount;
            vertCount += tile->header->vertCount;
            triCount += tile->header->detailTriCount;
            triVertCount += tile->header->detailVertCount;
            dataSize += tile->dataSize;
        }

        handler->PSendSysMessage("Navmesh stats:");
        handler->PSendSysMessage(" {} tiles loaded", tileCount);
        handler->PSendSysMessage(" {} BVTree nodes", nodeCount);
        handler->PSendSysMessage(" {} polygons ({} vertices)", polyCount, vertCount);
        handler->PSendSysMessage(" {} triangles ({} vertices)", triCount, triVertCount);
        handler->PSendSysMessage(" {} MB of data (not including pointers)", ((float)dataSize / sizeof(unsigned char)) / 1048576);

        return true;
    }

    static bool HandleMmapTestArea(ChatHandler* handler)
    {
        float radius = 40.0f;
        WorldObject* object = handler->GetSession()->GetPlayer();

        // Get Creatures
        std::list<Creature*> creatureList;
        Acore::AnyUnitInObjectRangeCheck go_check(object, radius);
        Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> go_search(object, creatureList, go_check);
        Cell::VisitObjects(object, go_search, radius);

        if (!creatureList.empty())
        {
            handler->PSendSysMessage("Found {} Creatures.", creatureList.size());

            uint32 paths = 0;
            uint32 uStartTime = getMSTime();

            float gx, gy, gz;
            object->GetPosition(gx, gy, gz);
            for (std::list<Creature*>::iterator itr = creatureList.begin(); itr != creatureList.end(); ++itr)
            {
                PathGenerator path(*itr);
                path.CalculatePath(gx, gy, gz);
                ++paths;
            }

            uint32 uPathLoadTime = getMSTimeDiff(uStartTime, getMSTime());
            handler->PSendSysMessage("Generated {} paths in {} ms", paths, uPathLoadTime);
        }
        else
            handler->PSendSysMessage("No creatures in {} yard range.", radius);

        return true;
    }
};

// applies finished rebuilds on the main thread, right after the maps were updated
class mmaps_rebuild_worldscript : public WorldScript
{
public:
    mmaps_rebuild_worldscript() : WorldScript("mmaps_rebuild_worldscript", { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

    void OnUpdate(uint32 /*diff*/) override { NavMeshTileRebuilder::ProcessCompleted(); }
    void OnShutdown() override { NavMeshTileRebuilder::Shutdown(); }
};

void AddSC_mmaps_commandscript()
{
    new mmaps_commandscript();
    new mmaps_rebuild_worldscript();
}
