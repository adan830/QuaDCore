/*
 * Copyright (C) 2009 QuadCore <http://github.com/QuaDCore/QuaDCore/tree/master>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "PointMovementGenerator.h"
#include "Errors.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DestinationHolderImp.h"

//----- Point Movement Generator
template<class T>
void PointMovementGenerator<T>::Initialize(T &unit)
{
    unit.StopMoving();
    unit.clearUnitState(UNIT_STAT_MOVING);
    Traveller<T> traveller(unit);
    i_destinationHolder.SetDestination(traveller,i_x,i_y,i_z, !unit.hasUnitState(UNIT_STAT_JUMPING));

    if (unit.GetTypeId() == TYPEID_UNIT && ((Creature*)&unit)->canFly())
        unit.AddUnitMovementFlag(MOVEMENTFLAG_FLYING2);
}

template<class T>
bool PointMovementGenerator<T>::Update(T &unit, const uint32 &diff)
{
    if(!&unit)
        return false;

    if(unit.hasUnitState(UNIT_STAT_ROOT | UNIT_STAT_STUNNED))
    {
        if(unit.hasUnitState(UNIT_STAT_CHARGING))
            return false;
        else
            return true;
    }

    Traveller<T> traveller(unit);

    i_destinationHolder.UpdateTraveller(traveller, diff);

    if(i_destinationHolder.HasArrived())
    {
        unit.clearUnitState(UNIT_STAT_MOVE);
        arrived = true;
        return false;
    }

    return true;
}

template<class T>
void PointMovementGenerator<T>:: Finalize(T &unit)
{
    if(unit.hasUnitState(UNIT_STAT_CHARGING))
        unit.clearUnitState(UNIT_STAT_CHARGING | UNIT_STAT_JUMPING);
    else if(arrived)
        MovementInform(unit);
}

template<class T>
void PointMovementGenerator<T>::MovementInform(T &unit)
{
}

template <> void PointMovementGenerator<Creature>::MovementInform(Creature &unit)
{
    unit.AI()->MovementInform(POINT_MOTION_TYPE, id);
}

template void PointMovementGenerator<Player>::Initialize(Player&);
template bool PointMovementGenerator<Player>::Update(Player &, const uint32 &diff);
template void PointMovementGenerator<Player>::MovementInform(Player&);
template void PointMovementGenerator<Player>::Finalize(Player&);

template void PointMovementGenerator<Creature>::Initialize(Creature&);
template bool PointMovementGenerator<Creature>::Update(Creature&, const uint32 &diff);
template void PointMovementGenerator<Creature>::Finalize(Creature&);
