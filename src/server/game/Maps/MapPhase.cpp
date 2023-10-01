/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Map.h"
#include "Battleground.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DisableMgr.h"
#include "DynamicTree.h"
#include "GameTime.h"
#include "Geometry.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridStates.h"
#include "Group.h"
#include "InstanceScript.h"
#include "LFGMgr.h"
#include "MapInstanced.h"
#include "Metric.h"
#include "MiscPackets.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectGridLoader.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "VMapFactory.h"
#include "Vehicle.h"
#include "Weather.h"

MapPhase::MapPhase(uint32 phaseMask, Map* owningMap) : _creatureToMoveLock(false), _gameObjectsToMoveLock(false), _dynamicObjectsToMoveLock(false),
m_activeNonPlayersIter(m_activeNonPlayers.end()), _transportsUpdateIter(_transports.end())
{
    _phaseMask = phaseMask;
    _map = owningMap;
}


void MapPhase::Update(const uint32 t_diff, const uint32 s_diff, bool  /*thread*/)
{
    if (t_diff)
        _dynamicTree.update(t_diff);

    /// update worldsessions for existing players
    for (m_mapRefIter = m_mapRefMgr.begin(); m_mapRefIter != m_mapRefMgr.end(); ++m_mapRefIter)
    {
        Player* player = m_mapRefIter->GetSource();
        if (player && player->IsInWorld())
        {
            //player->Update(t_diff);
            WorldSession* session = player->GetSession();
            MapSessionFilter updater(session);
            session->Update(s_diff, updater);
        }
    }


    Acore::ObjectUpdater updater(t_diff);

    // for creature
    TypeContainerVisitor<Acore::ObjectUpdater, GridTypeMapContainer  > grid_object_update(updater);
    // for pets
    TypeContainerVisitor<Acore::ObjectUpdater, WorldTypeMapContainer > world_object_update(updater);

    // pussywizard: container for far creatures in combat with players
    std::vector<Creature*> updateList;
    updateList.reserve(10);

    // non-player active objects, increasing iterator in the loop in case of object removal
    for (m_activeNonPlayersIter = m_activeNonPlayers.begin(); m_activeNonPlayersIter != m_activeNonPlayers.end();)
    {
        WorldObject* obj = *m_activeNonPlayersIter;
        ++m_activeNonPlayersIter;

        if (!obj || !obj->IsInWorld())
            continue;

        _map->VisitNearbyCellsOf(obj, grid_object_update, world_object_update);
    }

    // the player iterator is stored in the map object
    // to make sure calls to Map::Remove don't invalidate it
    for (m_mapRefIter = m_mapRefMgr.begin(); m_mapRefIter != m_mapRefMgr.end(); ++m_mapRefIter)
    {
        Player* player = m_mapRefIter->GetSource();

        if (!player || !player->IsInWorld())
            continue;

        // update players at tick
        player->Update(s_diff);

        _map->VisitNearbyCellsOf(player, grid_object_update, world_object_update);

        // If player is using far sight, visit that object too
        if (WorldObject* viewPoint = player->GetViewpoint())
            _map->VisitNearbyCellsOf(viewPoint, grid_object_update, world_object_update);

        // handle updates for creatures in combat with player and are more than X yards away
        if (player->IsInCombat())
        {
            updateList.clear();
            float rangeSq = player->GetGridActivationRange() - 1.0f;
            rangeSq = rangeSq * rangeSq;
            HostileReference* ref = player->getHostileRefMgr().getFirst();
            while (ref)
            {
                if (Unit* unit = ref->GetSource()->GetOwner())
                    if (Creature* cre = unit->ToCreature())
                        if (cre->FindMap() == player->FindMap() && cre->GetExactDist2dSq(player) > rangeSq)
                            updateList.push_back(cre);
                ref = ref->next();
            }
            for (std::vector<Creature*>::const_iterator itr = updateList.begin(); itr != updateList.end(); ++itr)
                _map->VisitNearbyCellsOf(*itr, grid_object_update, world_object_update);
        }
    }

    for (_transportsUpdateIter = _transports.begin(); _transportsUpdateIter != _transports.end();) // pussywizard: transports updated after VisitNearbyCellsOf, grids around are loaded, everything ok
    {
        MotionTransport* transport = *_transportsUpdateIter;
        ++_transportsUpdateIter;

        if (!transport->IsInWorld())
            continue;

        transport->Update(t_diff);
    }

    MoveAllCreaturesInMoveList();
    MoveAllGameObjectsInMoveList();
    MoveAllDynamicObjectsInMoveList();
}

void MapPhase::MoveAllCreaturesInMoveList()
{
    _creatureToMoveLock = true;
    for (std::vector<Creature*>::iterator itr = _creaturesToMove.begin(); itr != _creaturesToMove.end(); ++itr)
    {
        Creature* c = *itr;
        if (c->FindMap() != _map) //pet is teleported to another map
            continue;

        if (c->_moveState != MAP_OBJECT_CELL_MOVE_ACTIVE)
        {
            c->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
            continue;
        }

        c->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
        if (!c->IsInWorld())
            continue;

        // do move or do move to respawn or remove creature if previous all fail
        if (_map->CreatureCellRelocation(c, Cell(c->_newPosition.m_positionX, c->_newPosition.m_positionY)))
        {
            // update pos
            c->Relocate(c->_newPosition);
            if (c->IsVehicle())
                c->GetVehicleKit()->RelocatePassengers();
            //CreatureRelocationNotify(c, new_cell, new_cell.cellCoord());
            c->UpdatePositionData();
            c->UpdateObjectVisibility(false);
        }
        else
        {
            // if creature can't be move in new cell/grid (not loaded) move it to repawn cell/grid
            // creature coordinates will be updated and notifiers send
            if (!_map->CreatureRespawnRelocation(c, false))
            {
                // ... or unload (if respawn grid also not loaded)
#ifdef ACORE_DEBUG
                LOG_DEBUG("maps", "Creature {} cannot be move to unloaded respawn grid.", c->GetGUID().ToString().c_str());
#endif
                //AddObjectToRemoveList(Pet*) should only be called in Pet::Remove
                //This may happen when a player just logs in and a pet moves to a nearby unloaded cell
                //To avoid this, we can load nearby cells when player log in
                //But this check is always needed to ensure safety
                /// @todo pets will disappear if this is outside CreatureRespawnRelocation
                //need to check why pet is frequently relocated to an unloaded cell
                if (c->IsPet())
                    ((Pet*)c)->Remove(PET_SAVE_NOT_IN_SLOT, true);
                else
                    _map->AddObjectToRemoveList(c);
            }
        }
    }
    _creaturesToMove.clear();
    _creatureToMoveLock = false;
}

void MapPhase::MoveAllGameObjectsInMoveList()
{
    _gameObjectsToMoveLock = true;
    for (std::vector<GameObject*>::iterator itr = _gameObjectsToMove.begin(); itr != _gameObjectsToMove.end(); ++itr)
    {
        GameObject* go = *itr;
        if (go->FindMap() != _map) //transport is teleported to another map
            continue;

        if (go->_moveState != MAP_OBJECT_CELL_MOVE_ACTIVE)
        {
            go->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
            continue;
        }

        go->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
        if (!go->IsInWorld())
            continue;

        // do move or do move to respawn or remove creature if previous all fail
        if (_map->GameObjectCellRelocation(go, Cell(go->_newPosition.m_positionX, go->_newPosition.m_positionY)))
        {
            // update pos
            go->Relocate(go->_newPosition);
            go->UpdateModelPosition();
            go->UpdatePositionData();
            go->UpdateObjectVisibility(false);
        }
        else
        {
            // if GameObject can't be move in new cell/grid (not loaded) move it to repawn cell/grid
            // GameObject coordinates will be updated and notifiers send
            if (!_map->GameObjectRespawnRelocation(go, false))
            {
                // ... or unload (if respawn grid also not loaded)
#ifdef ACORE_DEBUG
                LOG_DEBUG("maps", "GameObject {} cannot be move to unloaded respawn grid.", go->GetGUID().ToString().c_str());
#endif
                _map->AddObjectToRemoveList(go);
            }
        }
    }
    _gameObjectsToMove.clear();
    _gameObjectsToMoveLock = false;
}

void MapPhase::MoveAllDynamicObjectsInMoveList()
{
    _dynamicObjectsToMoveLock = true;
    for (std::vector<DynamicObject*>::iterator itr = _dynamicObjectsToMove.begin(); itr != _dynamicObjectsToMove.end(); ++itr)
    {
        DynamicObject* dynObj = *itr;
        if (dynObj->FindMap() != _map) //transport is teleported to another map
            continue;

        if (dynObj->_moveState != MAP_OBJECT_CELL_MOVE_ACTIVE)
        {
            dynObj->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
            continue;
        }

        dynObj->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
        if (!dynObj->IsInWorld())
            continue;

        // do move or do move to respawn or remove creature if previous all fail
        if (_map->DynamicObjectCellRelocation(dynObj, Cell(dynObj->_newPosition.m_positionX, dynObj->_newPosition.m_positionY)))
        {
            // update pos
            dynObj->Relocate(dynObj->_newPosition);
            dynObj->UpdatePositionData();
            dynObj->UpdateObjectVisibility(false);
        }
        else
        {
#ifdef ACORE_DEBUG
            LOG_DEBUG("maps", "DynamicObject {} cannot be moved to unloaded grid.", dynObj->GetGUID().ToString().c_str());
#endif
        }
    }

    _dynamicObjectsToMove.clear();
    _dynamicObjectsToMoveLock = false;
}
