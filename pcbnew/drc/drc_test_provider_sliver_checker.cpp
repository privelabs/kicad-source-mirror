/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2021-2022 KiCad Developers.
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

#include <atomic>
#include <thread>
#include <board.h>
#include <board_design_settings.h>
#include <zone.h>
#include <footprint.h>
#include <pcb_shape.h>
#include <geometry/shape_poly_set.h>
#include <drc/drc_rule.h>
#include <drc/drc_item.h>
#include <drc/drc_test_provider.h>
#include <advanced_config.h>

/*
    Checks for slivers in copper layers

    Errors generated:
    - DRCE_COPPER_SLIVER
*/

class DRC_TEST_PROVIDER_SLIVER_CHECKER : public DRC_TEST_PROVIDER
{
public:
    DRC_TEST_PROVIDER_SLIVER_CHECKER()
    {
    }

    virtual ~DRC_TEST_PROVIDER_SLIVER_CHECKER()
    {
    }

    virtual bool Run() override;

    virtual const wxString GetName() const override
    {
        return wxT( "sliver checker" );
    };

    virtual const wxString GetDescription() const override
    {
        return wxT( "Checks copper layers for slivers" );
    }

private:
    wxString layerDesc( PCB_LAYER_ID aLayer );
};


wxString DRC_TEST_PROVIDER_SLIVER_CHECKER::layerDesc( PCB_LAYER_ID aLayer )
{
    return wxString::Format( wxT( "(%s)" ), m_drcEngine->GetBoard()->GetLayerName( aLayer ) );
}


bool DRC_TEST_PROVIDER_SLIVER_CHECKER::Run()
{
    if( m_drcEngine->IsErrorLimitExceeded( DRCE_COPPER_SLIVER ) )
        return true;    // Continue with other tests

    if( !reportPhase( _( "Running sliver detection on copper layers..." ) ) )
        return false;   // DRC cancelled

    int    widthTolerance = Millimeter2iu( ADVANCED_CFG::GetCfg().m_SliverWidthTolerance );
    double angleTolerance = ADVANCED_CFG::GetCfg().m_SliverAngleTolerance;
    int    testLength = widthTolerance / ( 2 * sin( DEG2RAD( angleTolerance / 2 ) ) );
    LSET   copperLayerSet = m_drcEngine->GetBoard()->GetEnabledLayers() & LSET::AllCuMask();
    LSEQ   copperLayers = copperLayerSet.Seq();
    int    layerCount = copperLayers.size();

    // Report progress on board zones only.  Everything else is in the noise.
    int    zoneLayerCount = 0;

    for( PCB_LAYER_ID layer : copperLayers )
    {
        for( ZONE* zone : m_drcEngine->GetBoard()->Zones() )
        {
            if( !zone->GetIsRuleArea() && zone->IsOnLayer( layer ) )
                zoneLayerCount++;
        }
    }

    // The first completion may be a long time coming, so this one gets us started.
    zoneLayerCount++;

    if( !m_drcEngine->ReportProgress( 1.0 / (double) zoneLayerCount ) )
        return false;   // DRC cancelled

    std::vector<SHAPE_POLY_SET> layerPolys;
    layerPolys.resize( layerCount );

    std::atomic<size_t> next( 0 );
    std::atomic<size_t> done( 1 );
    std::atomic<size_t> threads_finished( 0 );
    size_t parallelThreadCount = std::max<size_t>( std::thread::hardware_concurrency(), 2 );

    for( size_t ii = 0; ii < parallelThreadCount; ++ii )
    {
        std::thread t = std::thread(
                [&]( )
                {
                    for( int i = next.fetch_add( 1 ); i < layerCount; i = next.fetch_add( 1 ) )
                    {
                        PCB_LAYER_ID    layer = copperLayers[i];
                        SHAPE_POLY_SET& poly = layerPolys[i];
                        SHAPE_POLY_SET  fill;

                        forEachGeometryItem( s_allBasicItems, LSET().set( layer ),
                                [&]( BOARD_ITEM* item ) -> bool
                                {
                                    if( dynamic_cast<ZONE*>( item) )
                                    {
                                        ZONE* zone = static_cast<ZONE*>( item );

                                        if( !zone->GetIsRuleArea() )
                                        {
                                            fill = zone->GetFill( layer )->CloneDropTriangulation();
                                            fill.Unfracture( SHAPE_POLY_SET::PM_FAST );

                                            for( int jj = 0; jj < fill.OutlineCount(); ++jj )
                                                poly.AddOutline( fill.Outline( jj ) );

                                            // Report progress on board zones only.  Everything
                                            // else is in the noise.
                                            done.fetch_add( 1 );
                                        }
                                    }
                                    else
                                    {
                                        item->TransformShapeWithClearanceToPolygon( poly, layer, 0,
                                                                                    ARC_LOW_DEF,
                                                                                    ERROR_OUTSIDE );
                                    }

                                    if( m_drcEngine->IsCancelled() )
                                        return false;

                                    return true;
                                } );

                        poly.Simplify( SHAPE_POLY_SET::PM_FAST );

                        // Sharpen corners
                        poly.Deflate( widthTolerance / 2, ARC_LOW_DEF,
                                      SHAPE_POLY_SET::ALLOW_ACUTE_CORNERS );

                        if( m_drcEngine->IsCancelled() )
                            break;
                    }

                    threads_finished.fetch_add( 1 );
                } );

            t.detach();
    }

    while( threads_finished < parallelThreadCount )
    {
        m_drcEngine->ReportProgress( (double) done / (double) zoneLayerCount );
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    for( int ii = 0; ii < layerCount; ++ii )
    {
        PCB_LAYER_ID    layer = copperLayers[ii];
        SHAPE_POLY_SET& poly = layerPolys[ii];

        if( m_drcEngine->IsErrorLimitExceeded( DRCE_COPPER_SLIVER ) )
            continue;

        // Frequently, in filled areas, some points of the polygons are very near (dist is only
        // a few internal units, like 2 or 3 units.
        // We skip very small vertices: one cannot really compute a valid orientation of
        // such a vertex
        // So skip points near than min_len (in internal units).
        const int min_len = 3;

        for( int jj = 0; jj < poly.OutlineCount(); ++jj )
        {
            const std::vector<VECTOR2I>& pts = poly.Outline( jj ).CPoints();
            int                          ptCount = pts.size();

            for( int kk = 0; kk < ptCount; ++kk )
            {
                VECTOR2I pt = pts[ kk ];
                VECTOR2I ptPrior = pts[ ( ptCount + kk - 1 ) % ptCount ];
                VECTOR2I vPrior = ( ptPrior - pt );

                if( std::abs( vPrior.x ) < min_len && std::abs( vPrior.y ) < min_len && ptCount > 5)
                {
                    ptPrior = pts[ ( ptCount + kk - 2 ) % ptCount ];
                    vPrior = ( ptPrior - pt );
                }

                VECTOR2I ptAfter  = pts[ ( kk + 1 ) % ptCount ];
                VECTOR2I vAfter = ( ptAfter - pt );

                if( std::abs( vAfter.x ) < min_len && std::abs( vAfter.y ) < min_len && ptCount > 5 )
                {
                    ptAfter  = pts[ ( kk + 2 ) % ptCount ];
                    vAfter = ( ptAfter - pt );
                }

                VECTOR2I vIncluded = vPrior.Resize( testLength ) - vAfter.Resize( testLength );

                if( vIncluded.SquaredEuclideanNorm() < SEG::Square( widthTolerance ) )
                {
                    std::shared_ptr<DRC_ITEM> drce = DRC_ITEM::Create( DRCE_COPPER_SLIVER );
                    drce->SetErrorMessage( drce->GetErrorText() + wxS( " " ) + layerDesc( layer ) );
                    reportViolation( drce, pt, layer );
                }
            }
        }
    }

    return true;
}


namespace detail
{
static DRC_REGISTER_TEST_PROVIDER<DRC_TEST_PROVIDER_SLIVER_CHECKER> dummy;
}
