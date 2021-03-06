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

#include "Common.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "Object.h"
#include "Creature.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "Util.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Transports.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "VMapFactory.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"

#include "TemporarySummon.h"
#include "Totem.h"

uint32 GuidHigh2TypeId(uint32 guid_hi)
{
    switch(guid_hi)
    {
        case HIGHGUID_ITEM:         return TYPEID_ITEM;
        //case HIGHGUID_CONTAINER:    return TYPEID_CONTAINER; HIGHGUID_CONTAINER==HIGHGUID_ITEM currently
        case HIGHGUID_UNIT:         return TYPEID_UNIT;
        case HIGHGUID_PET:          return TYPEID_UNIT;
        case HIGHGUID_PLAYER:       return TYPEID_PLAYER;
        case HIGHGUID_GAMEOBJECT:   return TYPEID_GAMEOBJECT;
        case HIGHGUID_DYNAMICOBJECT:return TYPEID_DYNAMICOBJECT;
        case HIGHGUID_CORPSE:       return TYPEID_CORPSE;
        case HIGHGUID_MO_TRANSPORT: return TYPEID_GAMEOBJECT;
        case HIGHGUID_VEHICLE:      return TYPEID_UNIT;
    }
    return MAX_TYPEID;                                      // unknown
}

Object::Object( )
{
    m_objectTypeId      = TYPEID_OBJECT;
    m_objectType        = TYPEMASK_OBJECT;

    m_uint32Values      = 0;
    m_uint32Values_mirror = 0;
    m_valuesCount       = 0;

    m_inWorld           = false;
    m_objectUpdated     = false;

    m_PackGUID.clear();
    m_PackGUID.appendPackGUID(0);
}

Object::~Object( )
{
    //if(m_objectUpdated)
    //    ObjectAccessor::Instance().RemoveUpdateObject(this);

    if(IsInWorld())
    {
        sLog.outCrash("Object::~Object - guid="I64FMTD", typeid=%d deleted but still in world!!", GetGUID(), GetTypeId());
        assert(false);
    }

    assert(!m_objectUpdated);

    if(m_uint32Values)
    {
        //DEBUG_LOG("Object desctr 1 check (%p)",(void*)this);
        delete [] m_uint32Values;
        delete [] m_uint32Values_mirror;
        m_uint32Values = NULL;
        m_uint32Values_mirror = NULL;
        //DEBUG_LOG("Object desctr 2 check (%p)",(void*)this);
    }
}

void Object::_InitValues()
{
    m_uint32Values = new uint32[ m_valuesCount ];
    memset(m_uint32Values, 0, m_valuesCount*sizeof(uint32));

    m_uint32Values_mirror = new uint32[ m_valuesCount ];
    memset(m_uint32Values_mirror, 0, m_valuesCount*sizeof(uint32));

    m_objectUpdated = false;
}

void Object::_Create( uint32 guidlow, uint32 entry, HighGuid guidhigh )
{
    if(!m_uint32Values) _InitValues();

    uint64 guid = MAKE_NEW_GUID(guidlow, entry, guidhigh);  // required more changes to make it working
    SetUInt64Value( OBJECT_FIELD_GUID, guid );
    SetUInt32Value( OBJECT_FIELD_TYPE, m_objectType );
    m_PackGUID.clear();
    m_PackGUID.appendPackGUID(GetGUID());
}

void Object::BuildMovementUpdateBlock(UpdateData * data, uint32 flags ) const
{
    ByteBuffer buf(500);

    buf << uint8( UPDATETYPE_MOVEMENT );
    buf << GetGUID();

    _BuildMovementUpdate(&buf, flags);

    data->AddUpdateBlock(buf);
}

void Object::BuildCreateUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    if(!target)
    {
        return;
    }

    uint8  updatetype = UPDATETYPE_CREATE_OBJECT;
    uint8  flags      = m_updateFlag;

    /** lower flag1 **/
    if(target == this)                                      // building packet for oneself
        flags |= UPDATEFLAG_SELF;

    if(flags & UPDATEFLAG_HAS_POSITION)
    {
        // UPDATETYPE_CREATE_OBJECT2 dynamic objects, corpses...
        if(isType(TYPEMASK_DYNAMICOBJECT) || isType(TYPEMASK_CORPSE) || isType(TYPEMASK_PLAYER))
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for pets...
        if(target->GetPetGUID() == GetGUID())
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for some gameobject types...
        if(isType(TYPEMASK_GAMEOBJECT))
        {
            switch(((GameObject*)this)->GetGoType())
            {
                case GAMEOBJECT_TYPE_TRAP:
                case GAMEOBJECT_TYPE_DUEL_ARBITER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                case GAMEOBJECT_TYPE_FLAGDROP:
                    updatetype = UPDATETYPE_CREATE_OBJECT2;
                    break;
                case GAMEOBJECT_TYPE_TRANSPORT:
                    flags |= UPDATEFLAG_TRANSPORT;
                    break;
                default:
                    break;
            }
        }

        if(isType(TYPEMASK_UNIT))
        {
            if(((Unit*)this)->getVictim())
                flags |= UPDATEFLAG_HAS_TARGET;
        }
    }

    //sLog.outDebug("BuildCreateUpdate: update-type: %u, object-type: %u got flags: %X, flags2: %X", updatetype, m_objectTypeId, flags, flags2);

    ByteBuffer buf(500);
    buf << (uint8)updatetype;
    //buf.append(GetPackGUID());    //client crashes when using this
    buf << (uint8)0xFF << GetGUID();
    buf << (uint8)m_objectTypeId;

    _BuildMovementUpdate(&buf, flags);

    UpdateMask updateMask;
    updateMask.SetCount( m_valuesCount );
    _SetCreateBits( &updateMask, target );
    _BuildValuesUpdate(updatetype, &buf, &updateMask, target );
    data->AddUpdateBlock(buf);
}

void Object::BuildUpdate(UpdateDataMapType &update_players)
{
    ObjectAccessor::_buildUpdateObject(this,update_players);
    ClearUpdateMask(true);
}

void Object::SendUpdateToPlayer(Player* player)
{
    // send update to another players
    SendUpdateObjectToAllExcept(player);

    // send create update to player
    UpdateData upd;
    WorldPacket packet;

    BuildCreateUpdateBlockForPlayer(&upd, player);
    upd.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);

    // now object updated/(create updated)
}

void Object::BuildValuesUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    ByteBuffer buf(500);

    buf << (uint8) UPDATETYPE_VALUES;
    //buf.append(GetPackGUID());    //client crashes when using this. but not have crash in debug mode
    buf << (uint8)0xFF;
    buf << GetGUID();

    UpdateMask updateMask;
    updateMask.SetCount( m_valuesCount );

    _SetUpdateBits( &updateMask, target );
    _BuildValuesUpdate(UPDATETYPE_VALUES, &buf, &updateMask, target );

    data->AddUpdateBlock(buf);
}

void Object::BuildOutOfRangeUpdateBlock(UpdateData * data) const
{
    data->AddOutOfRangeGUID(GetGUID());
}

void Object::DestroyForPlayer(Player *target) const
{
    ASSERT(target);

    WorldPacket data(SMSG_DESTROY_OBJECT, 8);
    data << GetGUID();
    data << uint8(0);                                       // WotLK (bool)
    target->GetSession()->SendPacket( &data );
}

void Object::_BuildMovementUpdate(ByteBuffer * data, uint8 flags) const
{
    *data << (uint8)flags;                                  // update flags

    // 0x20
    if (flags & UPDATEFLAG_LIVING)
    {
        uint32 flags2 = ((Unit*)this)->GetUnitMovementFlags();
        switch(GetTypeId())
        {
            case TYPEID_UNIT:
            {
                flags2 &= ~MOVEMENTFLAG_SPLINE2;
                if(((Creature*)this)->isVehicle())
                    ((Unit*)this)->m_movementInfo.unk1 |= 0x20;     // always allow pitch
            }
            break;
            case TYPEID_PLAYER:
            {
                // remove unknown, unused etc flags for now
                flags2 &= ~MOVEMENTFLAG_SPLINE2;            // will be set manually

                if(((Player*)this)->isInFlight())
                {
                    WPAssert(((Player*)this)->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE);
                    flags2 = (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_SPLINE2);
                }
            }
            break;
        }

        *data << uint32(flags2);                            // movement flags
        *data << uint16(((Unit*)this)->m_movementInfo.unk1);// unknown 2.3.0
        *data << uint32(getMSTime());                       // time (in milliseconds)
    }

    // 0x40
    if (flags & UPDATEFLAG_HAS_POSITION)
    {
        // 0x02
        if(flags & UPDATEFLAG_TRANSPORT && ((GameObject*)this)->GetGoType() == GAMEOBJECT_TYPE_MO_TRANSPORT)
        {
            *data << (float)0;
            *data << (float)0;
            *data << (float)0;
            *data << ((WorldObject *)this)->GetOrientation();
        }
        else
        {
            *data << ((WorldObject *)this)->GetPositionX();
            *data << ((WorldObject *)this)->GetPositionY();
            *data << ((WorldObject *)this)->GetPositionZ();
            *data << ((WorldObject *)this)->GetOrientation();
        }
    }

    // 0x20
    if(flags & UPDATEFLAG_LIVING)
    {
        ((Unit*)this)->BuildMovementPacket(data);

        *data << ((Unit*)this)->GetSpeed( MOVE_WALK );
        *data << ((Unit*)this)->GetSpeed( MOVE_RUN );
        *data << ((Unit*)this)->GetSpeed( MOVE_SWIM_BACK );
        *data << ((Unit*)this)->GetSpeed( MOVE_SWIM );
        *data << ((Unit*)this)->GetSpeed( MOVE_RUN_BACK );
        *data << ((Unit*)this)->GetSpeed( MOVE_FLIGHT );
        *data << ((Unit*)this)->GetSpeed( MOVE_FLIGHT_BACK );
        *data << ((Unit*)this)->GetSpeed( MOVE_TURN_RATE );
        *data << ((Unit*)this)->GetSpeed( MOVE_PITCH_RATE );

        // 0x08000000
        if(GetTypeId() == TYPEID_PLAYER && ((Player*)this)->isInFlight())
        {
            WPAssert(((Player*)this)->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE);

            FlightPathMovementGenerator *fmg = (FlightPathMovementGenerator*)(((Player*)this)->GetMotionMaster()->top());

            uint32 flags3 = 0x00000300;

            *data << uint32(flags3);                        // splines flag?

            if(flags3 & 0x10000)                            // probably x,y,z coords there
            {
                *data << (float)0;
                *data << (float)0;
                *data << (float)0;
            }

            if(flags3 & 0x20000)                            // probably guid there
            {
                *data << uint64(0);
            }

            if(flags3 & 0x40000)                            // may be orientation
            {
                *data << (float)0;
            }

            Path &path = fmg->GetPath();

            float x, y, z;
            ((Player*)this)->GetPosition(x, y, z);

            uint32 inflighttime = uint32(path.GetPassedLength(fmg->GetCurrentNode(), x, y, z) * 32);
            uint32 traveltime = uint32(path.GetTotalLength() * 32);

            *data << uint32(inflighttime);                  // passed move time?
            *data << uint32(traveltime);                    // full move time?
            *data << uint32(0);                             // ticks count?

            uint32 poscount = uint32(path.Size());

            *data << uint32(poscount);                      // points count

            for(uint32 i = 0; i < poscount; ++i)
            {
                *data << path.GetNodes()[i].x;
                *data << path.GetNodes()[i].y;
                *data << path.GetNodes()[i].z;
            }

            *data << uint8(0);                              // added in 3.0.8

            /*for(uint32 i = 0; i < poscount; i++)
            {
                // path points
                *data << (float)0;
                *data << (float)0;
                *data << (float)0;
            }*/

            *data << path.GetNodes()[poscount-1].x;
            *data << path.GetNodes()[poscount-1].y;
            *data << path.GetNodes()[poscount-1].z;

            // target position (path end)
            /**data << ((Unit*)this)->GetPositionX();
             *data << ((Unit*)this)->GetPositionY();
             *data << ((Unit*)this)->GetPositionZ();*/
        }
    }

    // 0x8
    if(flags & UPDATEFLAG_LOWGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetGUIDLow());              // GetGUIDLow()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if(flags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x10
    if(flags & UPDATEFLAG_HIGHGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetGUIDHigh());             // GetGUIDHigh()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if(flags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x4
    if(flags & UPDATEFLAG_HAS_TARGET)                       // packed guid (current target guid)
    {
        if(Unit *victim = ((Unit*)this)->getVictim())
            data->append(victim->GetPackGUID());
        else
            *data << uint8(0);
    }

    // 0x2
    if(flags & UPDATEFLAG_TRANSPORT)
    {
        *data << uint32(getMSTime());                       // ms time
    }

    // 0x80
    if(flags & UPDATEFLAG_VEHICLE)                          // unused for now
    {
        *data << uint32(((Vehicle*)this)->GetVehicleInfo()->m_ID);  // vehicle id
        *data << float(0);                                  // facing adjustment
    }
}

void Object::_BuildValuesUpdate(uint8 updatetype, ByteBuffer * data, UpdateMask *updateMask, Player *target) const
{
    if(!target)
        return;

    bool IsActivateToQuest = false;
    if (updatetype == UPDATETYPE_CREATE_OBJECT || updatetype == UPDATETYPE_CREATE_OBJECT2)
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsTransport())
        {
            if ( ((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
            {
                IsActivateToQuest = true;
                updateMask->SetBit(GAMEOBJECT_DYNAMIC);
            }
            if (((GameObject*)this)->GetGoArtKit())
                updateMask->SetBit(GAMEOBJECT_BYTES_1);
        }
    }
    else                                                    //case UPDATETYPE_VALUES
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsTransport())
        {
            if ( ((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
            {
                IsActivateToQuest = true;
            }
            updateMask->SetBit(GAMEOBJECT_DYNAMIC);
            updateMask->SetBit(GAMEOBJECT_BYTES_1);
        }
    }

    WPAssert(updateMask && updateMask->GetCount() == m_valuesCount);

    *data << (uint8)updateMask->GetBlockCount();
    data->append( updateMask->GetMask(), updateMask->GetLength() );

    // 2 specialized loops for speed optimization in non-unit case
    if(isType(TYPEMASK_UNIT))                               // unit (creature/player) case
    {
        for( uint16 index = 0; index < m_valuesCount; index ++ )
        {
            if( updateMask->GetBit( index ) )
            {
                if( index == UNIT_NPC_FLAGS )
                {
                    // remove custom flag before sending
                    uint32 appendValue = m_uint32Values[ index ] & ~(UNIT_NPC_FLAG_GUARD + UNIT_NPC_FLAG_OUTDOORPVP);

                    if (GetTypeId() == TYPEID_UNIT && !target->canSeeSpellClickOn((Creature*)this))
                        appendValue &= ~UNIT_NPC_FLAG_SPELLCLICK;

                    *data << uint32(appendValue);
                }
                // FIXME: Some values at server stored in float format but must be sent to client in uint32 format
                else if(index >= UNIT_FIELD_BASEATTACKTIME && index <= UNIT_FIELD_RANGEDATTACKTIME)
                {
                    // convert from float to uint32 and send
                    *data << uint32(m_floatValues[ index ] < 0 ? 0 : m_floatValues[ index ]);
                }
                // there are some float values which may be negative or can't get negative due to other checks
                else if ((index >= UNIT_FIELD_NEGSTAT0   && index <= UNIT_FIELD_NEGSTAT4) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + 6)) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + 6)) ||
                    (index >= UNIT_FIELD_POSSTAT0   && index <= UNIT_FIELD_POSSTAT4))
                {
                    *data << uint32(m_floatValues[ index ]);
                }
                // Gamemasters should be always able to select units - remove not selectable flag
                else if(index == UNIT_FIELD_FLAGS && target->isGameMaster())
                {
                    *data << (m_uint32Values[ index ] & ~UNIT_FLAG_NOT_SELECTABLE);
                }
                // use modelid_a if not gm, _h if gm for CREATURE_FLAG_EXTRA_TRIGGER creatures
                else if(index == UNIT_FIELD_DISPLAYID && GetTypeId() == TYPEID_UNIT)
                {
                    const CreatureInfo* cinfo = ((Creature*)this)->GetCreatureInfo();
                    if(cinfo->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER)
                    {
                        if(target->isGameMaster())
                        {
                            if(cinfo->Modelid_A2)
                                *data << cinfo->Modelid_A1;
                            else
                                *data << 17519; // world invisible trigger's model
                        }
                        else
                        {
                            if(cinfo->Modelid_A2)
                                *data << cinfo->Modelid_A2;
                            else
                                *data << 11686; // world invisible trigger's model
                        }
                    }
                    else
                        *data << m_uint32Values[ index ];
                }
                // hide lootable animation for unallowed players
                else if(index == UNIT_DYNAMIC_FLAGS && GetTypeId() == TYPEID_UNIT)
                {
                    if(!target->isAllowedToLoot((Creature*)this))
                        *data << (m_uint32Values[ index ] & ~UNIT_DYNFLAG_LOOTABLE);
                    else
                        *data << (m_uint32Values[ index ] & ~UNIT_DYNFLAG_OTHER_TAGGER);
                }
                // FG: pretend that OTHER players in own group are friendly ("blue")
                else if(index == UNIT_FIELD_BYTES_2 || index == UNIT_FIELD_FACTIONTEMPLATE)
                {
                    bool ch = false;
                    if(GetTypeId() == TYPEID_PLAYER && target != this
                        && ((Player*)this)->IsInSameRaidWith(target))
                    {
                        // Allow targetting opposite faction in party when enabled in config
                        if(sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) && index == UNIT_FIELD_BYTES_2)
                        {
                            DEBUG_LOG("-- VALUES_UPDATE: Sending '%s' the blue-group-fix from '%s' (flag)", target->GetName(), ((Player*)this)->GetName());
                            *data << ( m_uint32Values[ index ] & ((UNIT_BYTE2_FLAG_SANCTUARY /*| UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5*/) << 8) ); // this flag is at uint8 offset 1 !!

                            ch = true;
                        }
                        else
                        {
                            FactionTemplateEntry const *ft1, *ft2;
                            ft1 = ((Player*)this)->getFactionTemplateEntry();
                            ft2 = target->getFactionTemplateEntry();
                            if(ft1 && ft2 && !ft1->IsFriendlyTo(*ft2))
                            {
                                uint32 faction = target->getFaction(); // pretend that all other HOSTILE players have own faction, to allow follow, heal, rezz (trade wont work)
                                DEBUG_LOG("-- VALUES_UPDATE: Sending '%s' the blue-group-fix from '%s' (faction %u)", target->GetName(), ((Player*)this)->GetName(), faction);
                                *data << uint32(faction);
                                ch = true;
                            }
                        }
                    }
                    if(!ch)
                        *data << m_uint32Values[ index ];
                }
                else
                {
                    // send in current format (float as float, uint32 as uint32)
                    *data << m_uint32Values[ index ];
                }
            }
        }
    }
    else if(isType(TYPEMASK_GAMEOBJECT))                    // gameobject case
    {
        for( uint16 index = 0; index < m_valuesCount; index ++ )
        {
            if( updateMask->GetBit( index ) )
            {
                // send in current format (float as float, uint32 as uint32)
                if ( index == GAMEOBJECT_DYNAMIC )
                {
                    if(IsActivateToQuest )
                    {
                        switch(((GameObject*)this)->GetGoType())
                        {
                            case GAMEOBJECT_TYPE_CHEST:
                                *data << uint32(9);         // enable quest object. Represent 9, but 1 for client before 2.3.0
                                break;
                            case GAMEOBJECT_TYPE_GOOBER:
                                *data << uint32(1);
                                break;
                            default:
                                *data << uint32(0);         // unknown. not happen.
                                break;
                        }
                    }
                    else
                        *data << uint32(0);                 // disable quest object
                }
                else
                    *data << m_uint32Values[ index ];       // other cases
            }
        }
    }
    else                                                    // other objects case (no special index checks)
    {
        for( uint16 index = 0; index < m_valuesCount; index ++ )
        {
            if( updateMask->GetBit( index ) )
            {
                // send in current format (float as float, uint32 as uint32)
                *data << m_uint32Values[ index ];
            }
        }
    }
}

void Object::ClearUpdateMask(bool remove)
{
    uint32 *temp = m_uint32Values;

    memcpy(m_uint32Values_mirror, m_uint32Values, m_valuesCount*sizeof(uint32));

    if(m_objectUpdated)
    {
        if(remove)
            ObjectAccessor::Instance().RemoveUpdateObject(this);
        m_objectUpdated = false;
    }
}

// Send current value fields changes to all viewers
void Object::SendUpdateObjectToAllExcept(Player* exceptPlayer)
{
    // changes will be send in create packet
    if(!IsInWorld())
        return;

    // nothing do
    if(!m_objectUpdated)
        return;

    ObjectAccessor::UpdateObject(this,exceptPlayer);
}

bool Object::LoadValues(const char* data)
{
    if(!m_uint32Values) _InitValues();

    Tokens tokens = StrSplit(data, " ");

    if(tokens.size() != m_valuesCount)
        return false;

    Tokens::iterator iter;
    int index;
    for (iter = tokens.begin(), index = 0; index < m_valuesCount; ++iter, ++index)
    {
        m_uint32Values[index] = atol((*iter).c_str());
    }

    return true;
}

void Object::_SetUpdateBits(UpdateMask *updateMask, Player* /*target*/) const
{
    uint32 *value = m_uint32Values;
    uint32 *mirror = m_uint32Values_mirror;

    for(uint16 index = 0; index < m_valuesCount; ++index, ++value, ++mirror)
    {
        if(*mirror != *value)
            updateMask->SetBit(index);
    }
}

void Object::_SetCreateBits(UpdateMask *updateMask, Player* /*target*/) const
{
    uint32 *value = m_uint32Values;

    for(uint16 index = 0; index < m_valuesCount; ++index, ++value)
    {
        if(*value)
            updateMask->SetBit(index);
    }
}

void Object::SetInt32Value( uint16 index, int32 value )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(m_int32Values[ index ] != value)
    {
        m_int32Values[ index ] = value;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetUInt32Value( uint16 index, uint32 value )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(m_uint32Values[ index ] != value)
    {
        m_uint32Values[ index ] = value;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetUInt64Value( uint16 index, const uint64 &value )
{
    ASSERT( index + 1 < m_valuesCount || PrintIndexError( index , true ) );
    if(*((uint64*)&(m_uint32Values[ index ])) != value)
    {
        m_uint32Values[ index ] = *((uint32*)&value);
        m_uint32Values[ index + 1 ] = *(((uint32*)&value) + 1);

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

bool Object::AddUInt64Value(uint16 index, const uint64 &value)
{
    ASSERT( index + 1 < m_valuesCount || PrintIndexError( index , true ) );
    if(value && !*((uint64*)&(m_uint32Values[index])))
    {
        m_uint32Values[ index ] = *((uint32*)&value);
        m_uint32Values[ index + 1 ] = *(((uint32*)&value) + 1);

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
        return true;
    }
    return false;
}

bool Object::RemoveUInt64Value(uint16 index, const uint64 &value)
{
    ASSERT( index + 1 < m_valuesCount || PrintIndexError( index , true ) );
    if(value && *((uint64*)&(m_uint32Values[index])) == value)
    {
        m_uint32Values[ index ] = 0;
        m_uint32Values[ index + 1 ] = 0;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
        return true;
    }
    return false;
}

void Object::SetFloatValue( uint16 index, float value )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(m_floatValues[ index ] != value)
    {
        m_floatValues[ index ] = value;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetByteValue( uint16 index, uint8 offset, uint8 value )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(offset > 4)
    {
        sLog.outError("Object::SetByteValue: wrong offset %u", offset);
        return;
    }

    if(uint8(m_uint32Values[ index ] >> (offset * 8)) != value)
    {
        m_uint32Values[ index ] &= ~uint32(uint32(0xFF) << (offset * 8));
        m_uint32Values[ index ] |= uint32(uint32(value) << (offset * 8));

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetUInt16Value( uint16 index, uint8 offset, uint16 value )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(offset > 2)
    {
        sLog.outError("Object::SetUInt16Value: wrong offset %u", offset);
        return;
    }

    if(uint8(m_uint32Values[ index ] >> (offset * 16)) != value)
    {
        m_uint32Values[ index ] &= ~uint32(uint32(0xFFFF) << (offset * 16));
        m_uint32Values[ index ] |= uint32(uint32(value) << (offset * 16));

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetStatFloatValue( uint16 index, float value)
{
    if(value < 0)
        value = 0.0f;

    SetFloatValue(index, value);
}

void Object::SetStatInt32Value( uint16 index, int32 value)
{
    if(value < 0)
        value = 0;

    SetUInt32Value(index, uint32(value));
}

void Object::ApplyModUInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetUInt32Value(index);
    cur += (apply ? val : -val);
    if(cur < 0)
        cur = 0;
    SetUInt32Value(index,cur);
}

void Object::ApplyModInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetInt32Value(index);
    cur += (apply ? val : -val);
    SetInt32Value(index,cur);
}

void Object::ApplyModSignedFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    SetFloatValue(index,cur);
}

void Object::ApplyModPositiveFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    if(cur < 0)
        cur = 0;
    SetFloatValue(index,cur);
}

void Object::SetFlag( uint16 index, uint32 newFlag )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );
    uint32 oldval = m_uint32Values[ index ];
    uint32 newval = oldval | newFlag;

    if(oldval != newval)
    {
        m_uint32Values[ index ] = newval;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::RemoveFlag( uint16 index, uint32 oldFlag )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );
    uint32 oldval = m_uint32Values[ index ];
    uint32 newval = oldval & ~oldFlag;

    if(oldval != newval)
    {
        m_uint32Values[ index ] = newval;

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::SetByteFlag( uint16 index, uint8 offset, uint8 newFlag )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(offset > 4)
    {
        sLog.outError("Object::SetByteFlag: wrong offset %u", offset);
        return;
    }

    if(!(uint8(m_uint32Values[ index ] >> (offset * 8)) & newFlag))
    {
        m_uint32Values[ index ] |= uint32(uint32(newFlag) << (offset * 8));

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

void Object::RemoveByteFlag( uint16 index, uint8 offset, uint8 oldFlag )
{
    ASSERT( index < m_valuesCount || PrintIndexError( index , true ) );

    if(offset > 4)
    {
        sLog.outError("Object::RemoveByteFlag: wrong offset %u", offset);
        return;
    }

    if(uint8(m_uint32Values[ index ] >> (offset * 8)) & oldFlag)
    {
        m_uint32Values[ index ] &= ~uint32(uint32(oldFlag) << (offset * 8));

        if(m_inWorld)
        {
            if(!m_objectUpdated)
            {
                ObjectAccessor::Instance().AddUpdateObject(this);
                m_objectUpdated = true;
            }
        }
    }
}

bool Object::PrintIndexError(uint32 index, bool set) const
{
    sLog.outError("Attempt %s non-existed value field: %u (count: %u) for object typeid: %u type mask: %u",(set ? "set value to" : "get value from"),index,m_valuesCount,GetTypeId(),m_objectType);

    // assert must fail after function call
    return false;
}

WorldObject::WorldObject()
    : m_mapId(0), m_InstanceId(0), m_phaseMask(PHASEMASK_NORMAL),
    m_positionX(0.0f), m_positionY(0.0f), m_positionZ(0.0f), m_orientation(0.0f)
{
    m_positionX         = 0.0f;
    m_positionY         = 0.0f;
    m_positionZ         = 0.0f;
    m_orientation       = 0.0f;

    m_mapId             = 0;
    m_InstanceId        = 0;
    m_map               = NULL;

    m_name = "";

    m_isActive          = false;
    IsTempWorldObject   = false;
}

void WorldObject::SetWorldObject(bool on)
{
    if(!IsInWorld())
        return;

    GetMap()->AddObjectToSwitchList(this, on);
}

void WorldObject::setActive( bool on )
{
    if(m_isActive == on)
        return;

    if(GetTypeId() == TYPEID_PLAYER)
        return;

    m_isActive = on;

    if(!IsInWorld())
        return;

    Map *map = FindMap();
    if(!map)
        return;

    if(on)
    {
        if(GetTypeId() == TYPEID_UNIT)
            map->AddToActive((Creature*)this);
        else if(GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->AddToActive((DynamicObject*)this);
    }
    else
    {
        if(GetTypeId() == TYPEID_UNIT)
            map->RemoveFromActive((Creature*)this);
        else if(GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->RemoveFromActive((DynamicObject*)this);
    }
}

void WorldObject::_Create( uint32 guidlow, HighGuid guidhigh, uint32 mapid, uint32 phaseMask )
{
    Object::_Create(guidlow, 0, guidhigh);

    m_mapId = mapid;
    m_phaseMask = phaseMask;
}

uint32 WorldObject::GetZoneId() const
{
    return MapManager::Instance().GetBaseMap(m_mapId)->GetZoneId(m_positionX,m_positionY,m_positionZ);
}

uint32 WorldObject::GetAreaId() const
{
    return MapManager::Instance().GetBaseMap(m_mapId)->GetAreaId(m_positionX,m_positionY,m_positionZ);
}

void WorldObject::GetZoneAndAreaId(uint32& zoneid, uint32& areaid) const
{
    MapManager::Instance().GetBaseMap(m_mapId)->GetZoneAndAreaId(zoneid,areaid,m_positionX,m_positionY,m_positionZ);
}

InstanceData* WorldObject::GetInstanceData()
{
    Map *map = GetMap();
    return map->IsDungeon() ? ((InstanceMap*)map)->GetInstanceData() : NULL;
}

                                                            //slow
float WorldObject::GetDistance(const WorldObject* obj) const
{
    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float dz = GetPositionZ() - obj->GetPositionZ();
    float sizefactor = GetObjectSize() + obj->GetObjectSize();
    float dist = sqrt((dx*dx) + (dy*dy) + (dz*dz)) - sizefactor;
    return ( dist > 0 ? dist : 0);
}

float WorldObject::GetDistance2d(float x, float y) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float sizefactor = GetObjectSize();
    
    SET_SIGN_FLOAT(dx,0);
    SET_SIGN_FLOAT(dy,0);

    float dist;

    if ( dx < dy )
        dist = 0.961f*dy+0.398f*dx - sizefactor;
    else
        dist = 0.961f*dx+0.398f*dy - sizefactor;

    return ( dist > 0 ? dist : 0);
}

float WorldObject::GetExactDistance2d(const float x, const float y) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    return sqrt((dx*dx) + (dy*dy));
}

float WorldObject::GetDistance(const float x, const float y, const float z) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float dz = GetPositionZ() - z;
    float sizefactor = GetObjectSize();
    float dist = sqrt((dx*dx) + (dy*dy) + (dz*dz)) - sizefactor;
    return ( dist > 0 ? dist : 0);
}

float WorldObject::GetDistanceSq(const float &x, const float &y, const float &z) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float dz = GetPositionZ() - z;
    return dx*dx + dy*dy + dz*dz;
}

float WorldObject::GetDistance2d(const WorldObject* obj) const
{
    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float sizefactor = GetObjectSize() + obj->GetObjectSize();
    
    SET_SIGN_FLOAT(dx,0);
    SET_SIGN_FLOAT(dy,0);

    float dist;

    if ( dx < dy )  
        dist = 0.961f*dy+0.398f*dx - sizefactor;
    else
        dist = 0.961f*dx+0.398f*dy - sizefactor;

    return ( dist > 0 ? dist : 0);
}

float WorldObject::GetDistanceZ(const WorldObject* obj) const
{
    float dz = fabs(GetPositionZ() - obj->GetPositionZ());
    float sizefactor = GetObjectSize() + obj->GetObjectSize();
    float dist = dz - sizefactor;
    return ( dist > 0 ? dist : 0);
}

bool WorldObject::IsWithinDistInMap(const WorldObject* obj, const float dist2compare, const bool is3D) const
{
    if (!obj || !IsInMap(obj)) return false;

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float distsq = dx*dx + dy*dy;
    if(is3D)
    {
        float dz = GetPositionZ() - obj->GetPositionZ();
        distsq += dz*dz;
    }
    float sizefactor = GetObjectSize() + obj->GetObjectSize();
    float maxdist = dist2compare + sizefactor;

    return distsq < maxdist * maxdist;
}

bool WorldObject::IsWithinLOSInMap(const WorldObject* obj) const
{
    if (!IsInMap(obj)) return false;
    float ox,oy,oz;
    obj->GetPosition(ox,oy,oz);
    return(IsWithinLOS(ox, oy, oz ));
}

bool WorldObject::IsWithinLOS(const float ox, const float oy, const float oz ) const
{
    float x,y,z;
    GetPosition(x,y,z);
    VMAP::IVMapManager *vMapManager = VMAP::VMapFactory::createOrGetVMapManager();
    return vMapManager->isInLineOfSight(GetMapId(), x, y, z+2.0f, ox, oy, oz+2.0f);
}

float WorldObject::GetAngle(const WorldObject* obj) const
{
    if(!obj) return 0;
    return GetAngle( obj->GetPositionX(), obj->GetPositionY() );
}

// Return angle in range 0..2*pi
float WorldObject::GetAngle( const float x, const float y ) const
{
    float dx = x - GetPositionX();
    float dy = y - GetPositionY();

    float ang = atan2(dy, dx);
    ang = (ang >= 0) ? ang : 2 * M_PI + ang;
    return ang;
}

void WorldObject::GetSinCos(const float x, const float y, float &vsin, float &vcos)
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;

    if(dx < 0.001f && dy < 0.001f)
    {
        float angle = rand_norm()*2*M_PI;
        vcos = cos(angle);
        vsin = sin(angle);
    }
    else
    {
        float dist = sqrt((dx*dx) + (dy*dy));
        vcos = dx / dist;
        vsin = dy / dist;
    }
}

bool WorldObject::HasInArc(const float arcangle, const WorldObject* obj) const
{
    // always have self in arc
    if(obj == this)
        return true;

    float arc = arcangle;

    // move arc to range 0.. 2*pi
    while( arc >= 2.0f * M_PI )
        arc -=  2.0f * M_PI;
    while( arc < 0 )
        arc +=  2.0f * M_PI;

    float angle = GetAngle( obj );
    angle -= m_orientation;

    // move angle to range -pi ... +pi
    while( angle > M_PI)
        angle -= 2.0f * M_PI;
    while(angle < -M_PI)
        angle += 2.0f * M_PI;

    float lborder =  -1 * (arc/2.0f);                       // in range -pi..0
    float rborder = (arc/2.0f);                             // in range 0..pi
    return (( angle >= lborder ) && ( angle <= rborder ));
}

bool WorldObject::IsInBetween(const WorldObject *obj1, const WorldObject *obj2, float size) const
{
    if(GetPositionX() > std::max(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionX() < std::min(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionY() > std::max(obj1->GetPositionY(), obj2->GetPositionY())
        || GetPositionY() < std::min(obj1->GetPositionY(), obj2->GetPositionY()))
        return false;

    if(!size)
        size = GetObjectSize() / 2;

    float angle = obj1->GetAngle(this) - obj1->GetAngle(obj2);
    return abs(sin(angle)) * GetExactDistance2d(obj1->GetPositionX(), obj1->GetPositionY()) < size;
}

void WorldObject::GetRandomPoint( float x, float y, float z, float distance, float &rand_x, float &rand_y, float &rand_z) const
{
    if(distance==0)
    {
        rand_x = x;
        rand_y = y;
        rand_z = z;
        return;
    }

    // angle to face `obj` to `this`
    float angle = rand_norm()*2*M_PI;
    float new_dist = rand_norm()*distance;

    rand_x = x + new_dist * cos(angle);
    rand_y = y + new_dist * sin(angle);
    rand_z = z;

    Quad::NormalizeMapCoord(rand_x);
    Quad::NormalizeMapCoord(rand_y);
    UpdateGroundPositionZ(rand_x,rand_y,rand_z);            // update to LOS height if available
}

void WorldObject::UpdateGroundPositionZ(float x, float y, float &z) const
{
    float new_z = MapManager::Instance().GetBaseMap(GetMapId())->GetHeight(x,y,z,true);
    if(new_z > INVALID_HEIGHT)
        z = new_z+ 0.05f;                                   // just to be sure that we are not a few pixel under the surface
}

bool WorldObject::IsPositionValid() const
{
    return Quad::IsValidMapCoord(m_positionX,m_positionY,m_positionZ,m_orientation);
}

void WorldObject::MonsterSay(const char* text, uint32 language, uint64 TargetGuid)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data,CHAT_MSG_MONSTER_SAY,text,language,GetName(),TargetGuid);
    SendMessageToSetInRange(&data,sWorld.getConfig(CONFIG_LISTEN_RANGE_SAY),true);
}

void WorldObject::MonsterYell(const char* text, uint32 language, uint64 TargetGuid)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data,CHAT_MSG_MONSTER_YELL,text,language,GetName(),TargetGuid);
    SendMessageToSetInRange(&data,sWorld.getConfig(CONFIG_LISTEN_RANGE_YELL),true);
}

void WorldObject::MonsterTextEmote(const char* text, uint64 TargetGuid, bool IsBossEmote)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data,IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE,text,LANG_UNIVERSAL,GetName(),TargetGuid);
    SendMessageToSetInRange(&data,sWorld.getConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE),true);
}

void WorldObject::MonsterWhisper(const char* text, uint64 receiver, bool IsBossWhisper)
{
    Player *player = objmgr.GetPlayer(receiver);
    if(!player || !player->GetSession())
        return;

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data,IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER,text,LANG_UNIVERSAL,GetName(),receiver);

    player->GetSession()->SendPacket(&data);
}

void WorldObject::SendPlaySound(uint32 Sound, bool OnlySelf)
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << Sound;
    if (OnlySelf && GetTypeId() == TYPEID_PLAYER )
        ((Player*)this)->GetSession()->SendPacket( &data );
    else
        SendMessageToSet( &data, true ); // ToSelf ignored in this case
}

void Object::ForceValuesUpdateAtIndex(uint32 i)
{
    m_uint32Values_mirror[i] = GetUInt32Value(i) + 1; // makes server think the field changed
    if(m_inWorld)
    {
        if(!m_objectUpdated)
        {
            ObjectAccessor::Instance().AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

namespace Quad
{
    class MonsterChatBuilder
    {
        public:
            MonsterChatBuilder(WorldObject const& obj, ChatMsg msgtype, int32 textId, uint32 language, uint64 targetGUID)
                : i_object(obj), i_msgtype(msgtype), i_textId(textId), i_language(language), i_targetGUID(targetGUID) {}
            void operator()(WorldPacket& data, int32 loc_idx)
            {
                char const* text = objmgr.GetQuadCoreString(i_textId,loc_idx);

                // TODO: i_object.GetName() also must be localized?
                i_object.BuildMonsterChat(&data,i_msgtype,text,i_language,i_object.GetNameForLocaleIdx(loc_idx),i_targetGUID);
            }

        private:
            WorldObject const& i_object;
            ChatMsg i_msgtype;
            int32 i_textId;
            uint32 i_language;
            uint64 i_targetGUID;
    };
}                                                           // namespace Quad

void WorldObject::MonsterSay(int32 textId, uint32 language, uint64 TargetGuid)
{
    CellPair p = Quad::ComputeCellPair(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    QuadCore::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_SAY, textId,language,TargetGuid);
    QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> say_do(say_build);
    QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> > say_worker(this,sWorld.getConfig(CONFIG_LISTEN_RANGE_SAY),say_do);
    TypeContainerVisitor<QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> >, WorldTypeMapContainer > message(say_worker);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, message, *GetMap());
}

void WorldObject::MonsterYell(int32 textId, uint32 language, uint64 TargetGuid)
{
    CellPair p = Quad::ComputeCellPair(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    QuadCore::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId,language,TargetGuid);
    QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> say_do(say_build);
    QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> > say_worker(this,sWorld.getConfig(CONFIG_LISTEN_RANGE_YELL),say_do);
    TypeContainerVisitor<QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> >, WorldTypeMapContainer > message(say_worker);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, message, *GetMap());
}

void WorldObject::MonsterYellToZone(int32 textId, uint32 language, uint64 TargetGuid)
{
    QuadCore::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId,language,TargetGuid);
    QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> say_do(say_build);

    uint32 zoneid = GetZoneId();

    Map::PlayerList const& pList = GetMap()->GetPlayers();
    for(Map::PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
        if(itr->getSource()->GetZoneId()==zoneid)
            say_do(itr->getSource());
}

void WorldObject::MonsterTextEmote(int32 textId, uint64 TargetGuid, bool IsBossEmote)
{
    CellPair p = Quad::ComputeCellPair(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    QuadCore::MonsterChatBuilder say_build(*this, IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, textId,LANG_UNIVERSAL,TargetGuid);
    QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> say_do(say_build);
    QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> > say_worker(this,sWorld.getConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE),say_do);
    TypeContainerVisitor<QuadCore::PlayerDistWorker<QuadCore::LocalizedPacketDo<QuadCore::MonsterChatBuilder> >, WorldTypeMapContainer > message(say_worker);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, message, *GetMap());
}

void WorldObject::MonsterWhisper(int32 textId, uint64 receiver, bool IsBossWhisper)
{
    Player *player = objmgr.GetPlayer(receiver);
    if(!player || !player->GetSession())
        return;

    uint32 loc_idx = player->GetSession()->GetSessionDbLocaleIndex();
    char const* text = objmgr.GetQuadString(textId,loc_idx);

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data,IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER,text,LANG_UNIVERSAL,GetNameForLocaleIdx(loc_idx),receiver);

    player->GetSession()->SendPacket(&data);
}

void WorldObject::BuildMonsterChat(WorldPacket *data, uint8 msgtype, char const* text, uint32 language, char const* name, uint64 targetGuid) const
{
    bool pre = (msgtype==CHAT_MSG_MONSTER_EMOTE || msgtype==CHAT_MSG_RAID_BOSS_EMOTE);

    *data << (uint8)msgtype;
    *data << (uint32)language;
    *data << (uint64)GetGUID();
    *data << (uint32)0;                                     //2.1.0
    *data << (uint32)(strlen(name)+1);
    *data << name;
    *data << (uint64)targetGuid;                            //Unit Target
    if( targetGuid && !IS_PLAYER_GUID(targetGuid) )
    {
        *data << (uint32)1;                                 // target name length
        *data << (uint8)0;                                  // target name
    }
    *data << (uint32)(strlen(text)+1+(pre?3:0));
    if(pre)
        data->append("%s ",3);
    *data << text;
    *data << (uint8)0;                                      // ChatTag
}

void WorldObject::BuildHeartBeatMsg(WorldPacket *data) const
{
    //Heartbeat message cannot be used for non-units
    if (!isType(TYPEMASK_UNIT))
        return;

    data->Initialize(MSG_MOVE_HEARTBEAT, 32);
    data->append(GetPackGUID());
    *data << uint32(((Unit*)this)->GetUnitMovementFlags()); // movement flags
    *data << uint16(0);                                     // 2.3.0
    *data << getMSTime();                                   // time
    *data << m_positionX;
    *data << m_positionY;
    *data << m_positionZ;
    *data << m_orientation;

    ((Unit*)this)->BuildMovementPacket(data);
}

void WorldObject::BuildTeleportAckMsg(WorldPacket *data, float x, float y, float z, float ang) const
{
    //TeleportAck message cannot be used for non-units
    if (!isType(TYPEMASK_UNIT))
        return;

    data->Initialize(MSG_MOVE_TELEPORT_ACK, 41);
    data->append(GetPackGUID());
    *data << uint32(0);                                     // this value increments every time
    *data << uint32(((Unit*)this)->GetUnitMovementFlags()); // movement flags
    *data << uint16(0);                                     // 2.3.0
    *data << getMSTime();                                   // time
    *data << x;
    *data << y;
    *data << z;
    *data << ang;

    ((Unit*)this)->BuildMovementPacket(data);
}

void WorldObject::SendMessageToSet(WorldPacket *data, bool /*fake*/, bool bToPossessor)
{
    MapManager::Instance().GetMap(m_mapId, this)->MessageBroadcast(this, data, bToPossessor);
}

void WorldObject::SendMessageToSetInRange(WorldPacket *data, float dist, bool /*bToSelf*/, bool bToPossessor)
{
    MapManager::Instance().GetMap(m_mapId, this)->MessageDistBroadcast(this, data, dist, bToPossessor);
}

void WorldObject::SendObjectDeSpawnAnim(uint64 guid)
{
    WorldPacket data(SMSG_GAMEOBJECT_DESPAWN_ANIM, 8);
    data << guid;
    SendMessageToSet(&data, true);
}

Map* WorldObject::_getMap()
{
    return m_map = MapManager::Instance().GetMap(GetMapId(), this);
}

Map* WorldObject::_findMap()
{
    return m_map = MapManager::Instance().FindMap(GetMapId(), GetInstanceId());
}

Map const* WorldObject::GetBaseMap() const
{
    return MapManager::Instance().GetBaseMap(GetMapId());
}

void WorldObject::AddObjectToRemoveList()
{
    assert(m_uint32Values);

    Map* map = FindMap();
    if(!map)
    {
        sLog.outError("Object (TypeId: %u Entry: %u GUID: %u) at attempt add to move list not have valid map (Id: %u).",GetTypeId(),GetEntry(),GetGUIDLow(),GetMapId());
        return;
    }

    map->AddObjectToRemoveList(this);
}

TempSummon *Map::SummonCreature(uint32 entry, float x, float y, float z, float angle, SummonPropertiesEntry const *properties, uint32 duration, Unit *summoner)
{
    uint32 mask = SUMMON_MASK_SUMMON;
    if(properties)
    {
        if(properties->Category == SUMMON_CATEGORY_PET
            || properties->Type == SUMMON_TYPE_GUARDIAN
            || properties->Type == SUMMON_TYPE_MINION)
            mask = SUMMON_MASK_GUARDIAN;
        else if(properties->Type == SUMMON_TYPE_TOTEM)
            mask = SUMMON_MASK_TOTEM;
        else if(properties->Category == SUMMON_CATEGORY_VEHICLE
            || properties->Type == SUMMON_TYPE_VEHICLE
            || properties->Type == SUMMON_TYPE_VEHICLE2)
            mask = SUMMON_MASK_VEHICLE;
        else if(properties->Type == SUMMON_TYPE_MINIPET)
            mask = SUMMON_MASK_MINION;
    }

    uint32 phase = PHASEMASK_NORMAL, team = 0;
    if(summoner)
    {
        phase = summoner->GetPhaseMask();
        if(summoner->GetTypeId() == TYPEID_PLAYER)
            team = ((Player*)summoner)->GetTeam();
    }

    TempSummon *summon = NULL;
    switch(mask)
    {
        case SUMMON_MASK_SUMMON:    summon = new TempSummon (properties, summoner);  break;
        case SUMMON_MASK_GUARDIAN:  summon = new Guardian   (properties, summoner);  break;
        case SUMMON_MASK_TOTEM:     summon = new Totem      (properties, summoner);  break;
        case SUMMON_MASK_MINION:    summon = new Minion     (properties, summoner);  break;
        default:    return NULL;
    }
    if(!summon->Create(objmgr.GenerateLowGuid(HIGHGUID_UNIT), this, phase, entry, team))
    {
        delete summon;
        return NULL;
    }

    summon->Relocate(x, y, z, angle);
    if(!summon->IsPositionValid())
    {
        sLog.outError("Creature (guidlow %d, entry %d) not summoned. Suggested coordinates isn't valid (X: %f Y: %f)",summon->GetGUIDLow(),summon->GetEntry(),summon->GetPositionX(),summon->GetPositionY());
        delete summon;
        return NULL;
    }
	
    summon->SetHomePosition(x, y, z, angle);

    summon->InitStats(duration);
    Add((Creature*)summon);
    summon->InitSummon();

    //ObjectAccessor::UpdateObjectVisibility(summon);

    return summon;
}

TempSummon* WorldObject::SummonCreature(uint32 entry, float x, float y, float z, float ang, TempSummonType spwtype, uint32 duration)
{
    Map *map = FindMap();
    if(!map)
        return NULL;

    if (x == 0.0f && y == 0.0f && z == 0.0f)
        GetClosePoint(x, y, z, GetObjectSize());

    TempSummon *pCreature = map->SummonCreature(entry, x, y, z, ang, NULL, duration, isType(TYPEMASK_UNIT) ? (Unit*)this : NULL);
    if(!pCreature)
        return NULL;

    pCreature->SetTempSummonType(spwtype);

    return pCreature;
}

Vehicle* WorldObject::SummonVehicle(uint32 entry, float x, float y, float z, float ang)
{
    CreatureInfo const *ci = objmgr.GetCreatureTemplate(entry);
    if(!ci)
        return NULL;

    uint32 id = ci->VehicleId; //temp store id here
    if(!id) id = 156;
    VehicleEntry const *ve = sVehicleStore.LookupEntry(id);
    if(!ve)
        return NULL;

    Vehicle *v = new Vehicle;
    Map *map = GetMap();
    uint32 team = 0;
    if (GetTypeId()==TYPEID_PLAYER)
        team = ((Player*)this)->GetTeam();
    if(!v->Create(objmgr.GenerateLowGuid(HIGHGUID_VEHICLE), map, GetPhaseMask(), entry, id, team))
    {
        delete v;
        return NULL;
    }

    v->Relocate(x, y, z, ang);

    if(!v->IsPositionValid())
    {
        sLog.outError("ERROR: Vehicle (guidlow %d, entry %d) not created. Suggested coordinates isn't valid (X: %f Y: %f)",
            v->GetGUIDLow(), v->GetEntry(), v->GetPositionX(), v->GetPositionY());
        delete v;
        return NULL;
    }

    map->Add((Creature*)v);

    //ObjectAccessor::UpdateObjectVisibility(v);

    return v;
}

Pet* Player::SummonPet(uint32 entry, float x, float y, float z, float ang, PetType petType, uint32 duration)
{
    Pet* pet = new Pet(this, petType);

    if(petType == SUMMON_PET && pet->LoadPetFromDB(this, entry))
    {
        // Remove Demonic Sacrifice auras (known pet)
        Unit::AuraEffectList const& auraClassScripts = GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
        for(Unit::AuraEffectList::const_iterator itr = auraClassScripts.begin();itr!=auraClassScripts.end();)
        {
            if((*itr)->GetMiscValue()==2228)
            {
                RemoveAurasDueToSpell((*itr)->GetId());
                itr = auraClassScripts.begin();
            }
            else
                ++itr;
        }

        if(duration > 0)
            pet->SetDuration(duration);

        return NULL;
    }

    // petentry==0 for hunter "call pet" (current pet summoned if any)
    if(!entry)
    {
        delete pet;
        return NULL;
    }

    Map *map = GetMap();
    uint32 pet_number = objmgr.GeneratePetNumber();
    if(!pet->Create(objmgr.GenerateLowGuid(HIGHGUID_PET), map, GetPhaseMask(), entry, pet_number))
    {
        sLog.outError("no such creature entry %u", entry);
        delete pet;
        return NULL;
    }

    pet->Relocate(x, y, z, ang);

    if(!pet->IsPositionValid())
    {
        sLog.outError("ERROR: Pet (guidlow %d, entry %d) not summoned. Suggested coordinates isn't valid (X: %f Y: %f)",pet->GetGUIDLow(),pet->GetEntry(),pet->GetPositionX(),pet->GetPositionY());
        delete pet;
        return NULL;
    }

    pet->SetCreatorGUID(GetGUID());
    pet->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, getFaction());

    // this enables pet details window (Shift+P)
    pet->GetCharmInfo()->SetPetNumber(pet_number, false);

    pet->setPowerType(POWER_MANA);
    pet->SetUInt32Value(UNIT_NPC_FLAGS , 0);
    pet->SetUInt32Value(UNIT_FIELD_BYTES_1,0);
    pet->InitStatsForLevel(getLevel());

    SetMinion(pet, true);

    switch(petType)
    {
        case POSSESSED_PET:
            pet->SetUInt32Value(UNIT_FIELD_FLAGS,0);
            break;
        case SUMMON_PET:
            pet->SetUInt32Value(UNIT_FIELD_BYTES_0, 2048);
            pet->SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
            pet->SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, 1000);
            pet->SetHealth(pet->GetMaxHealth());
            pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
            pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, time(NULL));
            break;
    }

    map->Add((Creature*)pet);

    switch(petType)
    {
        case POSSESSED_PET:
            pet->SetCharmedOrPossessedBy(this, true);
            break;
        case SUMMON_PET:
            pet->InitPetCreateSpells();
            pet->InitTalentForLevel();
            pet->SavePetToDB(PET_SAVE_AS_CURRENT);
            PetSpellInitialize();
            break;
    }

    if(petType == SUMMON_PET)
    {
        // Remove Demonic Sacrifice auras (known pet)
        Unit::AuraEffectList const& auraClassScripts = GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
        for(Unit::AuraEffectList::const_iterator itr = auraClassScripts.begin();itr!=auraClassScripts.end();)
        {
            if((*itr)->GetMiscValue()==2228)
            {
                RemoveAurasDueToSpell((*itr)->GetId());
                itr = auraClassScripts.begin();
            }
            else
                ++itr;
        }
    }

    if(duration > 0)
        pet->SetDuration(duration);

    //ObjectAccessor::UpdateObjectVisibility(pet);

    return pet;
}

GameObject* WorldObject::SummonGameObject(uint32 entry, float x, float y, float z, float ang, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime)
{
    if(!IsInWorld())
        return NULL;

    GameObjectInfo const* goinfo = objmgr.GetGameObjectInfo(entry);
    if(!goinfo)
    {
        sLog.outErrorDb("Gameobject template %u not found in database!", entry);
        return NULL;
    }
    Map *map = GetMap();
    GameObject *go = new GameObject();
    if(!go->Create(objmgr.GenerateLowGuid(HIGHGUID_GAMEOBJECT), entry, map, GetPhaseMask(), x,y,z,ang,rotation0,rotation1,rotation2,rotation3,100,GO_STATE_READY))
    {
        delete go;
        return NULL;
    }
    go->SetRespawnTime(respawnTime);
    if(GetTypeId()==TYPEID_PLAYER || GetTypeId()==TYPEID_UNIT) //not sure how to handle this
        ((Unit*)this)->AddGameObject(go);
    else
        go->SetSpawnedByDefault(false);
    map->Add(go);

    return go;
}

Creature* WorldObject::SummonTrigger(float x, float y, float z, float ang, uint32 duration, CreatureAI* (*GetAI)(Creature*))
{
    TempSummonType summonType = (duration == 0) ? TEMPSUMMON_DEAD_DESPAWN : TEMPSUMMON_TIMED_DESPAWN;
    Creature* summon = SummonCreature(WORLD_TRIGGER, x, y, z, ang, summonType, duration);
    if(!summon)
        return NULL;

    //summon->SetName(GetName());
    if(GetTypeId()==TYPEID_PLAYER || GetTypeId()==TYPEID_UNIT)
    {
        summon->setFaction(((Unit*)this)->getFaction());
        summon->SetLevel(((Unit*)this)->getLevel());
    }

    if(GetAI)
        summon->AIM_Initialize(GetAI(summon));
    return summon;
}

void WorldObject::GetNearPoint2D(float &x, float &y, float distance2d, float absAngle ) const
{
    x = GetPositionX() + (GetObjectSize() + distance2d) * cos(absAngle);
    y = GetPositionY() + (GetObjectSize() + distance2d) * sin(absAngle);

    Quad::NormalizeMapCoord(x);
    Quad::NormalizeMapCoord(y);
}

void WorldObject::GetNearPoint(WorldObject const* searcher, float &x, float &y, float &z, float searcher_size, float distance2d, float absAngle ) const
{
    GetNearPoint2D(x,y,distance2d+searcher_size,absAngle);

    z = GetPositionZ();

    UpdateGroundPositionZ(x,y,z);
}

void WorldObject::GetGroundPoint(float &x, float &y, float &z, float dist, float angle)
{
    angle += GetOrientation();
    x += dist * cos(angle);
    y += dist * sin(angle);
    Quad::NormalizeMapCoord(x);
    Quad::NormalizeMapCoord(y);
    UpdateGroundPositionZ(x, y, z);
}

void WorldObject::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    m_phaseMask = newPhaseMask;

    if(update && IsInWorld())
        ObjectAccessor::UpdateObjectVisibility(this);
}

void WorldObject::PlayDistanceSound( uint32 sound_id, Player* target /*= NULL*/ )
{
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND,4+8);
    data << uint32(sound_id);
    data << GetGUID();
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}

void WorldObject::PlayDirectSound( uint32 sound_id, Player* target /*= NULL*/ )
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(sound_id);
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}
