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
#include "Cell.h"
#include "DBCStructure.h"
#include "DataMap.h"
#include "Define.h"
#include "DynamicTree.h"
#include "GameObjectModel.h"
#include "GridDefines.h"
#include "GridRefMgr.h"
#include "MapRefMgr.h"
#include "Transport.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Position.h"
#include "SharedDefines.h"
#include "Timer.h"
#include <bitset>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>

class MapPhase
{
    friend class MapReference;
    friend class Map;
    friend class InstanceMap;
    friend class BattlegroundMap;
public:
    MapPhase(uint32 phaseMask, Map* owningMap);

    /// <summary>
    ///     The phase mask of this phase
    /// </summary>
    /// <returns>Phase</returns>
    const uint32& PhaseMask() const { return _phaseMask; }

    /// <summary>
    ///     The owner map of this phase
    /// </summary>
    /// <returns>Map Object</returns>
    const Map* OwningMap() const { return _map; }

    /// <summary>
    ///     Regular update loop
    /// </summary>
    /// <param name="threadDiff">Offset time from waiting on a thread to be available for the current update tick</param>
    /// <param name="updateDiff">Update tick offset</param>
    /// <param name="thread">Is in a thread</param>
    virtual void Update(const uint32 threadDiff, const uint32 updateDiff, bool thread = true);

    void MoveAllCreaturesInMoveList();
    void MoveAllGameObjectsInMoveList();
    void MoveAllDynamicObjectsInMoveList();

protected:
    Map* _map;
    uint32 _phaseMask;
    std::mutex Lock;

    typedef std::set<WorldObject*> ActiveNonPlayers;
    typedef std::set<MotionTransport*> TransportsContainer;
    ActiveNonPlayers m_activeNonPlayers;
    ActiveNonPlayers::iterator m_activeNonPlayersIter;

    // Objects that must update even in inactive grids without activating them
    TransportsContainer _transports;
    TransportsContainer::iterator _transportsUpdateIter;

    bool _creatureToMoveLock;
    std::vector<Creature*> _creaturesToMove;

    bool _gameObjectsToMoveLock;
    std::vector<GameObject*> _gameObjectsToMove;

    bool _dynamicObjectsToMoveLock;
    std::vector<DynamicObject*> _dynamicObjectsToMove;

    DynamicMapTree _dynamicTree;
    MapRefMgr m_mapRefMgr;
    MapRefMgr::iterator m_mapRefIter;
};
