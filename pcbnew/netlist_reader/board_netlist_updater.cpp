/**
 * @file board_netlist_updater.h
 * @brief BOARD_NETLIST_UPDATER class definition
 */

/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2015 CERN
 * Copyright (C) 2012 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2011 Wayne Stambaugh <stambaughw@verizon.net>
 *
 * Copyright (C) 1992-2019 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


#include <common.h>                         // for PAGE_INFO

#include <board.h>
#include <netinfo.h>
#include <footprint.h>
#include <pad.h>
#include <track.h>
#include <zone.h>
#include <kicad_string.h>
#include <pcbnew_settings.h>
#include <pcb_edit_frame.h>
#include <netlist_reader/pcb_netlist.h>
#include <connectivity/connectivity_data.h>
#include <reporter.h>

#include "board_netlist_updater.h"


BOARD_NETLIST_UPDATER::BOARD_NETLIST_UPDATER( PCB_EDIT_FRAME* aFrame, BOARD* aBoard ) :
    m_frame( aFrame ),
    m_commit( aFrame ),
    m_board( aBoard )
{
    m_reporter = &NULL_REPORTER::GetInstance();

    m_deleteSinglePadNets = true;
    m_deleteUnusedComponents = false;
    m_isDryRun = false;
    m_replaceFootprints = true;
    m_lookupByTimestamp = false;
    m_warnForNoNetPads = false;

    m_warningCount = 0;
    m_errorCount = 0;
    m_newFootprintsCount = 0;
}


BOARD_NETLIST_UPDATER::~BOARD_NETLIST_UPDATER()
{
}


// These functions allow inspection of pad nets during dry runs by keeping a cache of
// current pad netnames indexed by pad.

void BOARD_NETLIST_UPDATER::cacheNetname( PAD* aPad, const wxString& aNetname )
{
    m_padNets[ aPad ] = aNetname;
}


wxString BOARD_NETLIST_UPDATER::getNetname( PAD* aPad )
{
    if( m_isDryRun && m_padNets.count( aPad ) )
        return m_padNets[ aPad ];
    else
        return aPad->GetNetname();
}


void BOARD_NETLIST_UPDATER::cachePinFunction( PAD* aPad, const wxString& aPinFunction )
{
    m_padPinFunctions[ aPad ] = aPinFunction;
}


wxString BOARD_NETLIST_UPDATER::getPinFunction( PAD* aPad )
{
    if( m_isDryRun && m_padPinFunctions.count( aPad ) )
        return m_padPinFunctions[ aPad ];
    else
        return aPad->GetPinFunction();
}


wxPoint BOARD_NETLIST_UPDATER::estimateComponentInsertionPosition()
{
    wxPoint bestPosition;

    if( !m_board->IsEmpty() )
    {
        // Position new components below any existing board features.
        EDA_RECT bbox = m_board->GetBoardEdgesBoundingBox();

        if( bbox.GetWidth() || bbox.GetHeight() )
        {
            bestPosition.x = bbox.Centre().x;
            bestPosition.y = bbox.GetBottom() + Millimeter2iu( 10 );
        }
    }
    else
    {
        // Position new components in the center of the page when the board is empty.
        wxSize pageSize = m_board->GetPageSettings().GetSizeIU();

        bestPosition.x = pageSize.GetWidth() / 2;
        bestPosition.y = pageSize.GetHeight() / 2;
    }

    return bestPosition;
}


FOOTPRINT* BOARD_NETLIST_UPDATER::addNewComponent( COMPONENT* aComponent )
{
    wxString msg;

    if( aComponent->GetFPID().empty() )
    {
        msg.Printf( _( "Cannot add %s (no footprint assigned)." ),
                    aComponent->GetReference(),
                    aComponent->GetFPID().Format().wx_str() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    FOOTPRINT* footprint = m_frame->LoadFootprint( aComponent->GetFPID() );

    if( footprint == nullptr )
    {
        msg.Printf( _( "Cannot add %s (footprint \"%s\" not found)." ),
                    aComponent->GetReference(),
                    aComponent->GetFPID().Format().wx_str() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    msg.Printf( _( "Add %s (footprint \"%s\")." ),
                aComponent->GetReference(),
                aComponent->GetFPID().Format().wx_str() );
    m_reporter->Report( msg, RPT_SEVERITY_ACTION );

    for( PAD* pad : footprint->Pads() )
    {
        // Set the pads ratsnest settings to the global settings
        pad->SetLocalRatsnestVisible( m_frame->GetDisplayOptions().m_ShowGlobalRatsnest );
        pad->SetLocked( !m_frame->Settings().m_AddUnlockedPads );

        // Pads in the library all have orphaned nets.  Replace with Default.
        pad->SetNetCode( 0 );
    }

    m_newFootprintsCount++;

    if( !m_isDryRun )
    {
        footprint->SetParent( m_board );
        footprint->SetPosition( estimateComponentInsertionPosition( ) );

        m_addedComponents.push_back( footprint );
        m_commit.Add( footprint );

        return footprint;
    }
    else
    {
        delete footprint;
    }

    return NULL;
}


FOOTPRINT* BOARD_NETLIST_UPDATER::replaceComponent( NETLIST& aNetlist, FOOTPRINT* aPcbComponent,
                                                    COMPONENT* aNewComponent )
{
    wxString msg;

    if( aNewComponent->GetFPID().empty() )
    {
        msg.Printf( _( "Cannot update %s (no footprint assigned)." ),
                    aNewComponent->GetReference(),
                    aNewComponent->GetFPID().Format().wx_str() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    FOOTPRINT* newFootprint = m_frame->LoadFootprint( aNewComponent->GetFPID() );

    if( newFootprint == nullptr )
    {
        msg.Printf( _( "Cannot update %s (footprint \"%s\" not found)." ),
                    aNewComponent->GetReference(),
                    aNewComponent->GetFPID().Format().wx_str() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    msg.Printf( _( "Change %s footprint from \"%s\" to \"%s\"."),
                aPcbComponent->GetReference(),
                aPcbComponent->GetFPID().Format().wx_str(),
                aNewComponent->GetFPID().Format().wx_str() );
    m_reporter->Report( msg, RPT_SEVERITY_ACTION );

    m_newFootprintsCount++;

    if( !m_isDryRun )
    {
        m_frame->ExchangeFootprint( aPcbComponent, newFootprint, m_commit );
        return newFootprint;
    }
    else
    {
        delete newFootprint;
    }

    return nullptr;
}


bool BOARD_NETLIST_UPDATER::updateFootprintParameters( FOOTPRINT* aPcbFootprint,
                                                       COMPONENT* aNetlistComponent )
{
    wxString msg;

    // Create a copy only if the footprint has not been added during this update
    FOOTPRINT* copy = m_commit.GetStatus( aPcbFootprint ) ? nullptr
                                                          : (FOOTPRINT*) aPcbFootprint->Clone();
    bool       changed = false;

    // Test for reference designator field change.
    if( aPcbFootprint->GetReference() != aNetlistComponent->GetReference() )
    {
        msg.Printf( _( "Change %s reference designator to %s." ),
                    aPcbFootprint->GetReference(),
                    aNetlistComponent->GetReference() );
        m_reporter->Report( msg, RPT_SEVERITY_ACTION );

        if ( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetReference( aNetlistComponent->GetReference() );
        }
    }

    // Test for value field change.
    if( aPcbFootprint->GetValue() != aNetlistComponent->GetValue() )
    {
        msg.Printf( _( "Change %s value from %s to %s." ),
                    aPcbFootprint->GetReference(),
                    aPcbFootprint->GetValue(),
                    aNetlistComponent->GetValue() );
        m_reporter->Report( msg, RPT_SEVERITY_ACTION );

        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetValue( aNetlistComponent->GetValue() );
        }
    }

    // Test for time stamp change.
    if( aPcbFootprint->GetPath() != aNetlistComponent->GetPath() )
    {
        msg.Printf( _( "Update %s symbol association from %s to %s." ),
                    aPcbFootprint->GetReference(),
                    aPcbFootprint->GetPath().AsString(),
                    aNetlistComponent->GetPath().AsString() );
        m_reporter->Report( msg, RPT_SEVERITY_ACTION );

        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetPath( aNetlistComponent->GetPath() );
        }
    }

    if( aPcbFootprint->GetProperties() != aNetlistComponent->GetProperties() )
    {
        msg.Printf( _( "Update %s properties." ),
                    aPcbFootprint->GetReference() );
        m_reporter->Report( msg, RPT_SEVERITY_ACTION );

        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetProperties( aNetlistComponent->GetProperties() );
        }
    }

    if( ( aNetlistComponent->GetProperties().count( "exclude_from_bom" ) > 0 )
            != ( ( aPcbFootprint->GetAttributes() & FP_EXCLUDE_FROM_BOM ) > 0 ) )
    {
        int attributes = aPcbFootprint->GetAttributes();

        if( aNetlistComponent->GetProperties().count( "exclude_from_bom" ) )
        {
            attributes |= FP_EXCLUDE_FROM_BOM;
            msg.Printf( _( "Setting %s 'exclude from BOM' fabrication attribute." ),
                        aPcbFootprint->GetReference() );
        }
        else
        {
            attributes &= ~FP_EXCLUDE_FROM_BOM;
            msg.Printf( _( "Removing %s 'exclude from BOM' fabrication attribute." ),
                        aPcbFootprint->GetReference() );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );

        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetAttributes( attributes );
        }
    }

    if( changed && copy )
        m_commit.Modified( aPcbFootprint, copy );
    else
        delete copy;

    return true;
}


bool BOARD_NETLIST_UPDATER::updateComponentPadConnections( FOOTPRINT* aFootprint,
                                                           COMPONENT* aNewComponent )
{
    wxString msg;

    // Create a copy only if the footprint has not been added during this update
    FOOTPRINT* copy = m_commit.GetStatus( aFootprint ) ? nullptr : (FOOTPRINT*) aFootprint->Clone();
    bool       changed = false;

    // At this point, the component footprint is updated.  Now update the nets.
    for( PAD* pad : aFootprint->Pads() )
    {
        const COMPONENT_NET& net = aNewComponent->GetNet( pad->GetName() );

        wxString pinFunction;
        wxString pinType;

        if( net.IsValid() )     // i.e. the pad has a name
        {
            pinFunction = net.GetPinFunction();
            pinType = net.GetPinType();
        }

        if( !m_isDryRun )
        {
            if( pad->GetPinFunction() != pinFunction )
            {
                changed = true;
                pad->SetPinFunction( pinFunction );
            }

            if( pad->GetPinType() != pinType )
            {
                changed = true;
                pad->SetPinType( pinType );
            }
        }
        else
            cachePinFunction( pad, pinFunction );

        // Test if new footprint pad has no net (pads not on copper layers have no net).
        if( !net.IsValid() || !pad->IsOnCopperLayer() )
        {
            if( !pad->GetNetname().IsEmpty() )
            {
                msg.Printf( _( "Disconnect %s pin %s." ),
                            aFootprint->GetReference(),
                            pad->GetName() );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }
            else if( m_warnForNoNetPads && pad->IsOnCopperLayer() && !pad->GetName().IsEmpty() )
            {
                // pad is connectable but has no net found in netlist
                msg.Printf( _( "No net for symbol %s pin %s." ),
                            aFootprint->GetReference(),
                            pad->GetName() );
                m_reporter->Report( msg, RPT_SEVERITY_WARNING);
            }

            if( !m_isDryRun )
            {
                changed = true;
                pad->SetNetCode( NETINFO_LIST::UNCONNECTED );

                // If the pad has no net from netlist (i.e. not in netlist
                // it cannot have a pin function
                if( pad->GetNetname().IsEmpty() )
                    pad->SetPinFunction( wxEmptyString );

            }
            else
                cacheNetname( pad, wxEmptyString );
        }
        else                                 // New footprint pad has a net.
        {
            const wxString& netName = net.GetNetName();
            NETINFO_ITEM* netinfo = m_board->FindNet( netName );

            if( netinfo && !m_isDryRun )
                netinfo->SetIsCurrent( true );

            if( pad->GetNetname() != netName )
            {

                if( netinfo == nullptr )
                {
                    // It might be a new net that has not been added to the board yet
                    if( m_addedNets.count( netName ) )
                        netinfo = m_addedNets[ netName ];
                }

                if( netinfo == nullptr )
                {
                    netinfo = new NETINFO_ITEM( m_board, netName );

                    // It is a new net, we have to add it
                    if( !m_isDryRun )
                    {
                        changed = true;
                        m_commit.Add( netinfo );
                    }

                    m_addedNets[netName] = netinfo;
                    msg.Printf( _( "Add net %s." ), UnescapeString( netName ) );
                    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                }

                if( !pad->GetNetname().IsEmpty() )
                {
                    m_oldToNewNets[ pad->GetNetname() ] = netName;

                    msg.Printf( _( "Reconnect %s pin %s from %s to %s."),
                                aFootprint->GetReference(),
                                pad->GetName(),
                                UnescapeString( pad->GetNetname() ),
                                UnescapeString( netName ) );
                }
                else
                {
                    msg.Printf( _( "Connect %s pin %s to %s."),
                                aFootprint->GetReference(),
                                pad->GetName(),
                                UnescapeString( netName ) );
                }
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                if( !m_isDryRun )
                {
                    changed = true;
                    pad->SetNet( netinfo );
                }
                else
                    cacheNetname( pad, netName );
            }
        }
    }

    if( changed && copy )
        m_commit.Modified( aFootprint, copy );
    else
        delete copy;

    return true;
}


void BOARD_NETLIST_UPDATER::cacheCopperZoneConnections()
{
    for( ZONE* zone : m_board->Zones() )
    {
        if( !zone->IsOnCopperLayer() || zone->GetIsRuleArea() )
            continue;

        m_zoneConnectionsCache[ zone ] = m_board->GetConnectivity()->GetConnectedPads( zone );
    }
}


bool BOARD_NETLIST_UPDATER::updateCopperZoneNets( NETLIST& aNetlist )
{
    wxString msg;
    std::set<wxString> netlistNetnames;

    for( int ii = 0; ii < (int) aNetlist.GetCount(); ii++ )
    {
        const COMPONENT* component = aNetlist.GetComponent( ii );
        for( unsigned jj = 0; jj < component->GetNetCount(); jj++ )
        {
            const COMPONENT_NET& net = component->GetNet( jj );
            netlistNetnames.insert( net.GetNetName() );
        }
    }

    for( TRACK* via : m_board->Tracks() )
    {
        if( via->Type() != PCB_VIA_T )
            continue;

        if( netlistNetnames.count( via->GetNetname() ) == 0 )
        {
            wxString updatedNetname = wxEmptyString;

            // Take via name from name change map if it didn't match to a new pad
            // (this is useful for stitching vias that don't connect to tracks)
            if( m_oldToNewNets.count( via->GetNetname() ) )
            {
                updatedNetname = m_oldToNewNets[via->GetNetname()];
            }

            if( !updatedNetname.IsEmpty() )
            {
                msg.Printf( _( "Reconnect via from %s to %s." ),
                            UnescapeString( via->GetNetname() ),
                            UnescapeString( updatedNetname ) );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                if( !m_isDryRun )
                {
                    NETINFO_ITEM* netinfo = m_board->FindNet( updatedNetname );

                    if( !netinfo )
                        netinfo = m_addedNets[updatedNetname];

                    if( netinfo )
                    {
                        m_commit.Modify( via );
                        via->SetNet( netinfo );
                    }
                }
            }
            else
            {
                msg.Printf( _( "Via connected to unknown net (%s)." ),
                            UnescapeString( via->GetNetname() ) );
                m_reporter->Report( msg, RPT_SEVERITY_WARNING );
                ++m_warningCount;
            }
        }
    }

    // Test copper zones to detect "dead" nets (nets without any pad):
    for( ZONE* zone : m_board->Zones() )
    {
        if( !zone->IsOnCopperLayer() || zone->GetIsRuleArea() )
            continue;

        if( netlistNetnames.count( zone->GetNetname() ) == 0 )
        {
            // Look for a pad in the zone's connected-pad-cache which has been updated to
            // a new net and use that. While this won't always be the right net, the dead
            // net is guaranteed to be wrong.
            wxString updatedNetname = wxEmptyString;

            for( PAD* pad : m_zoneConnectionsCache[ zone ] )
            {
                if( getNetname( pad ) != zone->GetNetname() )
                {
                    updatedNetname = getNetname( pad );
                    break;
                }
            }

            // Take zone name from name change map if it didn't match to a new pad
            // (this is useful for zones on internal layers)
            if( updatedNetname.IsEmpty() && m_oldToNewNets.count( zone->GetNetname() ) )
            {
                updatedNetname = m_oldToNewNets[ zone->GetNetname() ];
            }

            if( !updatedNetname.IsEmpty() )
            {
                msg.Printf( _( "Reconnect copper zone from %s to %s." ),
                            UnescapeString( zone->GetNetname() ),
                            UnescapeString( updatedNetname ) );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                if( !m_isDryRun )
                {
                    NETINFO_ITEM* netinfo = m_board->FindNet( updatedNetname );

                    if( !netinfo )
                        netinfo = m_addedNets[ updatedNetname ];

                    if( netinfo )
                    {
                        m_commit.Modify( zone );
                        zone->SetNet( netinfo );
                    }
                }
            }
            else
            {
                msg.Printf( _( "Copper zone (%s) has no pads connected." ),
                            UnescapeString( zone->GetNetname() ) );
                m_reporter->Report( msg, RPT_SEVERITY_WARNING );
                ++m_warningCount;
            }
        }
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::deleteSinglePadNets()
{
    int       count = 0;
    wxString  netname;
    wxString  msg;
    PAD*      previouspad = NULL;

    // We need the pad list for next tests.

    m_board->BuildListOfNets();

    std::vector<PAD*> padlist = m_board->GetPads();

    // Sort pads by netlist name
    std::sort( padlist.begin(), padlist.end(), [ this ]( PAD* a, PAD* b ) -> bool
                                               {
                                                   return getNetname( a ) < getNetname( b );
                                               } );

    for( PAD* pad : padlist )
    {
        if( getNetname( pad ).IsEmpty() )
            continue;

        if( netname != getNetname( pad ) )  // End of net
        {
            if( previouspad && count == 1 )
            {
                // First, see if we have a copper zone attached to this pad.
                // If so, this is not really a single pad net

                for( ZONE* zone : m_board->Zones() )
                {
                    if( !zone->IsOnCopperLayer() )
                        continue;

                    if( zone->GetIsRuleArea() )
                        continue;

                    if( zone->GetNetname() == getNetname( previouspad ) )
                    {
                        count++;
                        break;
                    }
                }

                if( count == 1 )    // Really one pad, and nothing else
                {
                    msg.Printf( _( "Remove single pad net %s." ),
                                UnescapeString( getNetname( previouspad ) ) );
                    m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                    if( !m_isDryRun )
                        previouspad->SetNetCode( NETINFO_LIST::UNCONNECTED );
                    else
                        cacheNetname( previouspad, wxEmptyString );
                }
            }

            netname = getNetname( pad );
            count = 1;
        }
        else
        {
            count++;
        }

        previouspad = pad;
    }

    // Examine last pad
    if( count == 1 )
    {
        if( !m_isDryRun )
            previouspad->SetNetCode( NETINFO_LIST::UNCONNECTED );
        else
            cacheNetname( previouspad, wxEmptyString );
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::testConnectivity( NETLIST& aNetlist,
                                              std::map<COMPONENT*, FOOTPRINT*>& aFootprintMap )
{
    // Verify that board contains all pads in netlist: if it doesn't then footprints are
    // wrong or missing.

    wxString msg;
    wxString padname;

    for( int i = 0; i < (int) aNetlist.GetCount(); i++ )
    {
        COMPONENT* component = aNetlist.GetComponent( i );
        FOOTPRINT* footprint = aFootprintMap[component];

        if( !footprint )    // It can be missing in partial designs
            continue;

        // Explore all pins/pads in component
        for( unsigned jj = 0; jj < component->GetNetCount(); jj++ )
        {
            const COMPONENT_NET& net = component->GetNet( jj );
            padname = net.GetPinName();

            if( footprint->FindPadByName( padname ) )
                continue;   // OK, pad found

            // not found: bad footprint, report error
            msg.Printf( _( "%s pad %s not found in %s." ),
                        component->GetReference(),
                        padname,
                        footprint->GetFPID().Format().wx_str() );
            m_reporter->Report( msg, RPT_SEVERITY_ERROR );
            ++m_errorCount;
        }
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::UpdateNetlist( NETLIST& aNetlist )
{
    FOOTPRINT* lastPreexistingFootprint = nullptr;
    COMPONENT* component = nullptr;
    wxString   msg;

    m_errorCount = 0;
    m_warningCount = 0;
    m_newFootprintsCount = 0;

    std::map<COMPONENT*, FOOTPRINT*> footprintMap;

    if( !m_board->Footprints().empty() )
        lastPreexistingFootprint = m_board->Footprints().back();

    cacheCopperZoneConnections();

    // First mark all nets (except <no net>) as stale; we'll update those which are current
    // in the following two loops.
    //
    if( !m_isDryRun )
    {
        m_board->SetStatus( 0 );

        for( NETINFO_ITEM* net : m_board->GetNetInfo() )
            net->SetIsCurrent( net->GetNetCode() == 0 );
    }

    // Next go through the netlist updating all board footprints which have matching component
    // entries and adding new footprints for those that don't.
    //
    for( unsigned i = 0; i < aNetlist.GetCount(); i++ )
    {
        component = aNetlist.GetComponent( i );

        if( component->GetProperties().count( "exclude_from_board" ) )
            continue;

        msg.Printf( _( "Processing symbol '%s:%s'." ),
                    component->GetReference(),
                    component->GetFPID().Format().wx_str() );
        m_reporter->Report( msg, RPT_SEVERITY_INFO );

        int matchCount = 0;

        for( FOOTPRINT* footprint : m_board->Footprints() )
        {
            bool match = false;

            if( m_lookupByTimestamp )
                match = footprint->GetPath() == component->GetPath();
            else
                match = footprint->GetReference().CmpNoCase( component->GetReference() ) == 0;

            if( match )
            {
                FOOTPRINT* tmp = footprint;

                if( m_replaceFootprints && component->GetFPID() != footprint->GetFPID() )
                    tmp = replaceComponent( aNetlist, footprint, component );

                if( tmp )
                {
                    footprintMap[ component ] = tmp;

                    updateFootprintParameters( tmp, component );
                    updateComponentPadConnections( tmp, component );
                }

                matchCount++;
            }

            if( footprint == lastPreexistingFootprint )
            {
                // No sense going through the newly-created footprints: end of loop
                break;
            }
        }

        if( matchCount == 0 )
        {
            FOOTPRINT* footprint = addNewComponent( component );

            if( footprint )
            {
                footprintMap[ component ] = footprint;

                updateFootprintParameters( footprint, component );
                updateComponentPadConnections( footprint, component );
            }
        }
        else if( matchCount > 1 )
        {
            msg.Printf( _( "Multiple footprints found for \"%s\"." ),
                        component->GetReference() );
            m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        }
    }

    updateCopperZoneNets( aNetlist );

    // Finally go through the board footprints and update all those that *don't* have matching
    // component entries.
    //
    for( FOOTPRINT* footprint : m_board->Footprints() )
    {
        bool doDelete = m_deleteUnusedComponents;

        if( ( footprint->GetAttributes() & FP_BOARD_ONLY ) > 0 )
            doDelete = false;

        if( doDelete )
        {
            if( m_lookupByTimestamp )
                component = aNetlist.GetComponentByPath( footprint->GetPath() );
            else
                component = aNetlist.GetComponentByReference( footprint->GetReference() );

            if( component && component->GetProperties().count( "exclude_from_board" ) == 0 )
                doDelete = false;
        }

        if( doDelete && footprint->IsLocked() )
        {
            msg.Printf( _( "Cannot remove unused footprint %s (locked)." ),
                        footprint->GetReference() );
            m_reporter->Report( msg, RPT_SEVERITY_WARNING );
            doDelete = false;
        }

        if( doDelete )
        {
            msg.Printf( _( "Remove unused footprint %s." ), footprint->GetReference() );
            m_reporter->Report( msg, RPT_SEVERITY_ACTION );

            if( !m_isDryRun )
                m_commit.Remove( footprint );
        }
        else if( !m_isDryRun )
        {
            for( PAD* pad : footprint->Pads() )
            {
                if( pad->GetNet() )
                    pad->GetNet()->SetIsCurrent( true );
            }
        }
    }

    if( !m_isDryRun )
    {
        m_board->GetConnectivity()->Build( m_board );
        testConnectivity( aNetlist, footprintMap );

        // Now the connectivity data is rebuilt, we can delete single pads nets
        if( m_deleteSinglePadNets )
            deleteSinglePadNets();

        for( NETINFO_ITEM* net : m_board->GetNetInfo() )
        {
            if( !net->IsCurrent() )
            {
                msg.Printf( _( "Remove unused net \"%s\"." ), net->GetNetname() );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                m_commit.Removed( net );
            }
        }

        m_board->GetNetInfo().RemoveUnusedNets();
        m_commit.Push( _( "Update netlist" ) );
    }
    else if( m_deleteSinglePadNets && !m_newFootprintsCount )
    {
        // We can delete single net pads in dry run mode only if no new footprints
        // are added, because these new footprints are not actually added to the board
        // and the current pad list is wrong in this case.
        deleteSinglePadNets();
    }

    if( m_isDryRun )
    {
        for( const std::pair<const wxString, NETINFO_ITEM*>& addedNet : m_addedNets )
            delete addedNet.second;

        m_addedNets.clear();
    }

    // Update the ratsnest
    m_reporter->ReportTail( wxT( "" ), RPT_SEVERITY_ACTION );
    m_reporter->ReportTail( wxT( "" ), RPT_SEVERITY_ACTION );

    msg.Printf( _( "Total warnings: %d, errors: %d." ), m_warningCount, m_errorCount );
    m_reporter->ReportTail( msg, RPT_SEVERITY_INFO );

    return true;
}