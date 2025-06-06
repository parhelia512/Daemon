/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Daemon GPL Source Code (Daemon Source Code).

Daemon Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Daemon Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following the
terms and conditions of the GNU General Public License which accompanied the Daemon
Source Code.  If not, please request a copy in writing from id Software at the address
below.

If you have questions concerning this license or the applicable additional terms, you
may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville,
Maryland 20850 USA.

===========================================================================
*/

#include "cm_local.h"

#include "cm_patch.h"

// always use bbox vs. bbox collision and never capsule vs. bbox or vice versa
//#define ALWAYS_BBOX_VS_BBOX
// always use capsule vs. capsule collision and never capsule vs. bbox or vice versa
//#define ALWAYS_CAPSULE_VS_CAPSULE

static Cvar::Cvar<bool> cm_noCurves(VM_STRING_PREFIX "cm_noCurves",
	"treat BSP patches as empty space for collision detection", Cvar::CHEAT, false);

/*
===============================================================================

BASIC MATH

===============================================================================
*/

/*
================
RotatePoint
================
*/
void RotatePoint( vec3_t point, const vec3_t matrix[ 3 ] )
{
	vec3_t tvec;

	VectorCopy( point, tvec );
	point[ 0 ] = DotProduct( matrix[ 0 ], tvec );
	point[ 1 ] = DotProduct( matrix[ 1 ], tvec );
	point[ 2 ] = DotProduct( matrix[ 2 ], tvec );
}

/*
================
TransposeMatrix
================
*/
void TransposeMatrix( const vec3_t matrix[ 3 ], vec3_t transpose[ 3 ] )
{
	transpose[ 0 ][ 0 ] = matrix[ 0 ][ 0 ];
	transpose[ 0 ][ 1 ] = matrix[ 1 ][ 0 ];
	transpose[ 0 ][ 2 ] = matrix[ 2 ][ 0 ];

	transpose[ 1 ][ 0 ] = matrix[ 0 ][ 1 ];
	transpose[ 1 ][ 1 ] = matrix[ 1 ][ 1 ];
	transpose[ 1 ][ 2 ] = matrix[ 2 ][ 1 ];

	transpose[ 2 ][ 0 ] = matrix[ 0 ][ 2 ];
	transpose[ 2 ][ 1 ] = matrix[ 1 ][ 2 ];
	transpose[ 2 ][ 2 ] = matrix[ 2 ][ 2 ];
}

/*
================
CreateRotationMatrix
================
*/
void CreateRotationMatrix( const vec3_t angles, vec3_t matrix[ 3 ] )
{
	AngleVectors( angles, matrix[ 0 ], matrix[ 1 ], matrix[ 2 ] );
	VectorInverse( matrix[ 1 ] );
}

/*
================
CM_ProjectPointOntoVector
================
*/
static void CM_ProjectPointOntoVector( const vec3_t point, const vec3_t vStart, const vec3_t vDir, vec3_t vProj )
{
	vec3_t pVec;

	VectorSubtract( point, vStart, pVec );
	// project onto the directional vector for this segment
	VectorMA( vStart, DotProduct( pVec, vDir ), vDir, vProj );
}

/*
================
CM_DistanceFromLineSquared
================
*/
float CM_DistanceFromLineSquared( const vec3_t p, const vec3_t lp1, const vec3_t lp2, vec3_t const dir )
{
	vec3_t proj, t;
	int    j;

	CM_ProjectPointOntoVector( p, lp1, dir, proj );

	for ( j = 0; j < 3; j++ )
	{
		if ( ( proj[ j ] > lp1[ j ] && proj[ j ] > lp2[ j ] ) || ( proj[ j ] < lp1[ j ] && proj[ j ] < lp2[ j ] ) )
		{
			break;
		}
	}

	if ( j < 3 )
	{
		if ( Q_fabs( proj[ j ] - lp1[ j ] ) < Q_fabs( proj[ j ] - lp2[ j ] ) )
		{
			VectorSubtract( p, lp1, t );
		}
		else
		{
			VectorSubtract( p, lp2, t );
		}

		return VectorLengthSquared( t );
	}

	VectorSubtract( p, proj, t );
	return VectorLengthSquared( t );
}

/*
===============================================================================

POSITION TESTING

===============================================================================
*/

/*
================
CM_TestBoxInBrush
================
*/
static void CM_TestBoxInBrush( traceWork_t *tw, const cbrush_t *brush )
{
	float        dist;
	float        d1;
	float        t;
	vec3_t       startp;

	if ( !brush->numsides )
	{
		return;
	}

	// special test for axial
	// the first 6 brush planes are always axial
	if ( tw->bounds[ 0 ][ 0 ] > brush->bounds[ 1 ][ 0 ]
	     || tw->bounds[ 0 ][ 1 ] > brush->bounds[ 1 ][ 1 ]
	     || tw->bounds[ 0 ][ 2 ] > brush->bounds[ 1 ][ 2 ]
	     || tw->bounds[ 1 ][ 0 ] < brush->bounds[ 0 ][ 0 ]
	     || tw->bounds[ 1 ][ 1 ] < brush->bounds[ 0 ][ 1 ] || tw->bounds[ 1 ][ 2 ] < brush->bounds[ 0 ][ 2 ] )
	{
		return;
	}

	const cbrushside_t *firstSide = brush->sides;
	const cbrushside_t *endSide = firstSide + brush->numsides;
	
	// the first six planes are the axial planes, so we only
	// need to test the remainder
	firstSide += 6;

	if ( tw->type == traceType_t::TT_CAPSULE )
	{
		for ( const cbrushside_t *side = firstSide; side < endSide; side++ )
		{
			const cplane_t *plane = side->plane;

			// adjust the plane distance appropriately for radius
			dist = plane->dist + tw->sphere.radius;
			// find the closest point on the capsule to the plane
			t = DotProduct( plane->normal, tw->sphere.offset );

			if ( t > 0 )
			{
				VectorSubtract( tw->start, tw->sphere.offset, startp );
			}
			else
			{
				VectorAdd( tw->start, tw->sphere.offset, startp );
			}

			d1 = DotProduct( startp, plane->normal ) - dist;

			// if completely in front of face, no intersection
			if ( d1 > 0 )
			{
				return;
			}
		}
	}
	else
	{
		for ( const cbrushside_t *side = firstSide; side < endSide; side++ )
		{
			const cplane_t *plane = side->plane;

			// adjust the plane distance appropriately for mins/maxs
			dist = plane->dist - DotProduct( tw->offsets[ plane->signbits ], plane->normal );

			d1 = DotProduct( tw->start, plane->normal ) - dist;

			// if completely in front of face, no intersection
			if ( d1 > 0 )
			{
				return;
			}
		}
	}

	// inside this brush
	tw->trace.startsolid = tw->trace.allsolid = true;
	tw->trace.fraction = 0;
	tw->trace.contents = brush->contents;
}

/*
====================
CM_PositionTestInSurfaceCollide
====================
*/
static bool CM_PositionTestInSurfaceCollide( traceWork_t *tw, const cSurfaceCollide_t *sc )
{
	int      i, j;
	float    offset, t;
	cPlane_t *planes;
	cFacet_t *facet;
	vec3_t   startp;

	if ( tw->isPoint )
	{
		return false;
	}

	//
	facet = sc->facets;

	for ( i = 0; i < sc->numFacets; i++, facet++ )
	{
		planes = &sc->planes[ facet->surfacePlane ];

		plane_t plane = planes->plane;

		if ( tw->type == traceType_t::TT_CAPSULE )
		{
			// adjust the plane distance appropriately for radius
			plane.dist += tw->sphere.radius;

			// find the closest point on the capsule to the plane
			t = DotProduct( plane.normal, tw->sphere.offset );

			if ( t > 0 )
			{
				VectorSubtract( tw->start, tw->sphere.offset, startp );
			}
			else
			{
				VectorAdd( tw->start, tw->sphere.offset, startp );
			}
		}
		else
		{
			offset = DotProduct( tw->offsets[ planes->signbits ], plane.normal );
			plane.dist -= offset;
			VectorCopy( tw->start, startp );
		}

		if ( ( DotProduct( plane.normal, startp ) - plane.dist ) > 0.0f )
		{
			continue;
		}

		for ( j = 0; j < facet->numBorders; j++ )
		{
			planes = &sc->planes[ facet->borderPlanes[ j ] ];

			if ( facet->borderInward[ j ] )
			{
				VectorNegate( planes->plane.normal, plane.normal );
				plane.dist = -planes->plane.dist;
			}
			else
			{
				plane = planes->plane;
			}

			if ( tw->type == traceType_t::TT_CAPSULE )
			{
				// adjust the plane distance appropriately for radius
				plane.dist += tw->sphere.radius;

				// find the closest point on the capsule to the plane
				t = DotProduct( plane.normal, tw->sphere.offset );

				if ( t > 0.0f )
				{
					VectorSubtract( tw->start, tw->sphere.offset, startp );
				}
				else
				{
					VectorAdd( tw->start, tw->sphere.offset, startp );
				}
			}
			else
			{
				// NOTE: this works even though the plane might be flipped because the bbox is centered
				offset = DotProduct( tw->offsets[ planes->signbits ], plane.normal );
				plane.dist += fabsf( offset );
				VectorCopy( tw->start, startp );
			}

			if ( ( DotProduct( plane.normal, startp ) - plane.dist ) > 0.0f )
			{
				break;
			}
		}

		if ( j < facet->numBorders )
		{
			continue;
		}

		// inside this patch facet
		return true;
	}

	return false;
}

/*
================
CM_TestInLeaf
================
*/
void CM_TestInLeaf( traceWork_t *tw, const cLeaf_t *leaf )
{
	// test box position against all brushes in the leaf
	const int *firstBrushNum = leaf->firstLeafBrush;
	const int *endBrushNum = firstBrushNum + leaf->numLeafBrushes;
	for ( const int *brushNum = firstBrushNum; brushNum < endBrushNum; brushNum++ )
	{
		cbrush_t *b = &cm.brushes[ *brushNum ];

		if ( b->checkcount == cm.checkcount )
		{
			continue; // already checked this brush in another leaf
		}

		b->checkcount = cm.checkcount;

		if ( !( b->contents & tw->contents ) )
		{
			continue;
		}

		if ( b->contents & tw->skipContents )
		{
			continue;
		}

		CM_TestBoxInBrush( tw, b );

		if ( tw->trace.allsolid )
		{
			return;
		}
	}

	// test against all surfaces
	const int *firstSurfaceNum = leaf->firstLeafSurface;
	const int *endSurfaceNum = firstSurfaceNum + leaf->numLeafSurfaces;
	for ( const int *surfaceNum = firstSurfaceNum; surfaceNum < endSurfaceNum; surfaceNum++ )
	{
		cSurface_t *surface = cm.surfaces[ *surfaceNum ];

		if ( !surface )
		{
			continue;
		}

		if ( surface->checkcount == cm.checkcount )
		{
			continue; // already checked this surface in another leaf
		}

		surface->checkcount = cm.checkcount;

		if ( !( surface->contents & tw->contents ) )
		{
			continue;
		}

		if ( surface->contents & tw->skipContents )
		{
			continue;
		}

		if ( !cm_noCurves.Get() )
		{
			if ( surface->type == mapSurfaceType_t::MST_PATCH && surface->sc && CM_PositionTestInSurfaceCollide( tw, surface->sc ) )
			{
				tw->trace.startsolid = tw->trace.allsolid = true;
				tw->trace.fraction = 0;
				tw->trace.contents = surface->contents;
				return;
			}
		}

		if ( cm.perPolyCollision || cm_forceTriangles.Get() )
		{
			if ( surface->type == mapSurfaceType_t::MST_TRIANGLE_SOUP && surface->sc && CM_PositionTestInSurfaceCollide( tw, surface->sc ) )
			{
				tw->trace.startsolid = tw->trace.allsolid = true;
				tw->trace.fraction = 0;
				tw->trace.contents = surface->contents;
				return;
			}
		}
	}
}

/*
==================
CM_TestCapsuleInCapsule

Capsule inside capsule check

A capsule is a vertical "pill" shaped volume. It is made of a cylinder with
a tangent sphere at the top and the bottom that has the same radius.
==================
*/
void CM_TestCapsuleInCapsule( traceWork_t *tw, clipHandle_t model )
{
	vec3_t mins, maxs;
	vec3_t top, bottom;
	vec3_t p1, p2, tmp;
	vec3_t offset, symetricSize[ 2 ];
	float  radius, halfwidth, halfheight, offs, r;

	CM_ModelBounds( model, mins, maxs );

	VectorAdd( tw->start, tw->sphere.offset, top );
	VectorSubtract( tw->start, tw->sphere.offset, bottom );

	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		symetricSize[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		symetricSize[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		symetricSize[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		symetricSize[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		symetricSize[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		symetricSize[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];
	}

	halfwidth = symetricSize[ 1 ][ 0 ];
	halfheight = symetricSize[ 1 ][ 2 ];
	radius = ( halfwidth > halfheight ) ? halfheight : halfwidth;
	offs = halfheight - radius;

	r = Square( tw->sphere.radius + radius );
	// check if any of the spheres overlap
	VectorCopy( offset, p1 );
	p1[ 2 ] += offs;
	VectorSubtract( p1, top, tmp );

	if ( VectorLengthSquared( tmp ) < r )
	{
		tw->trace.startsolid = tw->trace.allsolid = true;
		tw->trace.fraction = 0;
	}

	VectorSubtract( p1, bottom, tmp );

	if ( VectorLengthSquared( tmp ) < r )
	{
		tw->trace.startsolid = tw->trace.allsolid = true;
		tw->trace.fraction = 0;
	}

	VectorCopy( offset, p2 );
	p2[ 2 ] -= offs;
	VectorSubtract( p2, top, tmp );

	if ( VectorLengthSquared( tmp ) < r )
	{
		tw->trace.startsolid = tw->trace.allsolid = true;
		tw->trace.fraction = 0;
	}

	VectorSubtract( p2, bottom, tmp );

	if ( VectorLengthSquared( tmp ) < r )
	{
		tw->trace.startsolid = tw->trace.allsolid = true;
		tw->trace.fraction = 0;
	}

	// if between cylinder up and lower bounds
	if ( ( top[ 2 ] >= p1[ 2 ] && top[ 2 ] <= p2[ 2 ] ) || ( bottom[ 2 ] >= p1[ 2 ] && bottom[ 2 ] <= p2[ 2 ] ) )
	{
		// 2d coordinates
		top[ 2 ] = p1[ 2 ] = 0;
		// if the cylinders overlap
		VectorSubtract( top, p1, tmp );

		if ( VectorLengthSquared( tmp ) < r )
		{
			tw->trace.startsolid = tw->trace.allsolid = true;
			tw->trace.fraction = 0;
		}
	}
}

/*
==================
CM_TestBoundingBoxInCapsule

bounding box inside capsule check
==================
*/
void CM_TestBoundingBoxInCapsule( traceWork_t *tw, clipHandle_t model )
{
	vec3_t       mins, maxs, offset, size[ 2 ];
	clipHandle_t h;
	cmodel_t     *cmod;

	// mins maxs of the capsule
	CM_ModelBounds( model, mins, maxs );

	// offset for capsule center
	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		size[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		size[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		size[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		size[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		size[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		size[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];

		tw->start[ 0 ] -= offset[ 0 ];
		tw->start[ 1 ] -= offset[ 1 ];
		tw->start[ 2 ] -= offset[ 2 ];

		tw->end[ 0 ] -= offset[ 0 ];
		tw->end[ 1 ] -= offset[ 1 ];
		tw->end[ 2 ] -= offset[ 2 ];
	}

	// replace the bounding box with the capsule
	tw->type = traceType_t::TT_CAPSULE;
	tw->sphere.radius = ( size[ 1 ][ 0 ] > size[ 1 ][ 2 ] ) ? size[ 1 ][ 2 ] : size[ 1 ][ 0 ];
	tw->sphere.halfheight = size[ 1 ][ 2 ];
	VectorSet( tw->sphere.offset, 0, 0, size[ 1 ][ 2 ] - tw->sphere.radius );

	// replace the capsule with the bounding box
	h = CM_TempBoxModel( tw->size[ 0 ], tw->size[ 1 ], false );
	// calculate collision
	cmod = CM_ClipHandleToModel( h );
	CM_TestInLeaf( tw, &cmod->leaf );
}

/*
==================
CM_PositionTest
==================
*/
static const int MAX_POSITION_LEAFS = 1024;
void CM_PositionTest( traceWork_t *tw )
{
	int        leafs[ MAX_POSITION_LEAFS ];
	int        i;
	leafList_t ll;

	// identify the leafs we are touching
	VectorAdd( tw->start, tw->size[ 0 ], ll.bounds[ 0 ] );
	VectorAdd( tw->start, tw->size[ 1 ], ll.bounds[ 1 ] );

	{
		ll.bounds[ 0 ][ 0 ] -= 1;
		ll.bounds[ 0 ][ 1 ] -= 1;
		ll.bounds[ 0 ][ 2 ] -= 1;

		ll.bounds[ 1 ][ 0 ] += 1;
		ll.bounds[ 1 ][ 1 ] += 1;
		ll.bounds[ 1 ][ 2 ] += 1;
	}

	ll.count = 0;
	ll.maxcount = MAX_POSITION_LEAFS;
	ll.list = leafs;
	ll.storeLeafs = CM_StoreLeafs;
	ll.lastLeaf = 0;
	ll.overflowed = false;

	cm.checkcount++;

	CM_BoxLeafnums_r( &ll, 0 );

	cm.checkcount++;

	// test the contents of the leafs
	for ( i = 0; i < ll.count; i++ )
	{
		CM_TestInLeaf( tw, &cm.leafs[ leafs[ i ] ] );

		if ( tw->trace.allsolid )
		{
			break;
		}
	}
}

/*
===============================================================================

TRACING

===============================================================================
*/

/*
====================
CM_TracePointThroughSurfaceCollide

  special case for point traces because the surface collide "brushes" have no volume
====================
*/
void CM_TracePointThroughSurfaceCollide( traceWork_t *tw, const cSurfaceCollide_t *sc )
{
	static bool frontFacing[ SHADER_MAX_TRIANGLES ];
	static float    intersection[ SHADER_MAX_TRIANGLES ];
	float           intersect;
	const cPlane_t  *planes;
	const cFacet_t  *facet;
	int             i, j, k;

	if ( !tw->isPoint )
	{
		return;
	}

	// determine the trace's relationship to all planes
	planes = sc->planes;

	for ( i = 0; i < sc->numPlanes; i++, planes++ )
	{
		vec_t offset = DotProduct( tw->offsets[ planes->signbits ], planes->plane.normal );
		vec_t d1 = DotProduct( tw->start, planes->plane.normal ) - planes->plane.dist + offset;
		vec_t d2 = DotProduct( tw->end, planes->plane.normal ) - planes->plane.dist + offset;

		if ( d1 <= 0 )
		{
			frontFacing[ i ] = false;
		}
		else
		{
			frontFacing[ i ] = true;
		}

		if ( d1 == d2 )
		{
			intersection[ i ] = 99999;
		}
		else
		{
			intersection[ i ] = d1 / ( d1 - d2 );

			if ( intersection[ i ] <= 0 )
			{
				intersection[ i ] = 99999;
			}
		}
	}

	// see if any of the surface planes are intersected
	for ( i = 0, facet = sc->facets; i < sc->numFacets; i++, facet++ )
	{
		if ( !frontFacing[ facet->surfacePlane ] )
		{
			continue;
		}

		intersect = intersection[ facet->surfacePlane ];

		if ( intersect < 0 )
		{
			continue; // surface is behind the starting point
		}

		if ( intersect > tw->trace.fraction )
		{
			continue; // already hit something closer
		}

		for ( j = 0; j < facet->numBorders; j++ )
		{
			k = facet->borderPlanes[ j ];

			if ( frontFacing[ k ] != facet->borderInward[ j ] )
			{
				if ( intersection[ k ] > intersect )
				{
					break;
				}
			}
			else
			{
				if ( intersection[ k ] < intersect )
				{
					break;
				}
			}
		}

		if ( j == facet->numBorders )
		{
			planes = &sc->planes[ facet->surfacePlane ];

			// calculate intersection with a slight pushoff
			vec_t offset = DotProduct( tw->offsets[ planes->signbits ], planes->plane.normal );
			vec_t d1 = DotProduct( tw->start, planes->plane.normal ) - planes->plane.dist + offset;
			vec_t d2 = DotProduct( tw->end, planes->plane.normal ) - planes->plane.dist + offset;
			tw->trace.fraction = ( d1 - SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

			if ( tw->trace.fraction < 0 )
			{
				tw->trace.fraction = 0;
			}

			VectorCopy( planes->plane.normal, tw->trace.plane.normal );
			tw->trace.plane.dist = planes->plane.dist;
		}
	}
}

/*
====================
CM_CheckFacetPlane
====================
*/
static bool CM_CheckFacetPlane( const plane_t &plane, const vec3_t start, const vec3_t end, float *enterFrac, float *leaveFrac, bool *hit )
{
	float f;

	*hit = false;

	vec_t d1 = DotProduct( start, plane.normal ) - plane.dist;
	vec_t d2 = DotProduct( end, plane.normal ) - plane.dist;

	// if completely in front of face, no intersection with the entire facet
	if ( d1 > 0 && ( d2 >= SURFACE_CLIP_EPSILON || d2 >= d1 ) )
	{
		return false;
	}

	// if it doesn't cross the plane, the plane isn't relevant
	if ( d1 <= 0 && d2 <= 0 )
	{
		return true;
	}

	// crosses face
	if ( d1 > d2 )
	{
		// enter
		f = ( d1 - SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

		if ( f < 0 )
		{
			f = 0;
		}

		//always favor previous plane hits and thus also the surface plane hit
		if ( f > *enterFrac )
		{
			*enterFrac = f;
			*hit = true;
		}
	}
	else
	{
		// leave
		f = ( d1 + SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

		if ( f > 1 )
		{
			f = 1;
		}

		if ( f < *leaveFrac )
		{
			*leaveFrac = f;
		}
	}

	return true;
}

/*
====================
CM_TraceThroughSurfaceCollide
====================
*/
void CM_TraceThroughSurfaceCollide( traceWork_t *tw, const cSurfaceCollide_t *sc )
{
	int           i, j, hitnum;
	float         offset, enterFrac, leaveFrac, t;
	cPlane_t      *planes;
	cFacet_t      *facet;
	vec3_t        startp, endp;

	if ( !CM_BoundsIntersect( tw->bounds[ 0 ], tw->bounds[ 1 ], sc->bounds[ 0 ], sc->bounds[ 1 ] ) )
	{
		return;
	}

	if ( tw->isPoint )
	{
		CM_TracePointThroughSurfaceCollide( tw, sc );
		return;
	}

	plane_t bestplane = {};
	for ( i = 0, facet = sc->facets; i < sc->numFacets; i++, facet++ )
	{
		enterFrac = -1.0f;
		leaveFrac = 1.0f;
		hitnum = -1;

		planes = &sc->planes[ facet->surfacePlane ];

		plane_t plane = planes->plane;

		if ( tw->type == traceType_t::TT_CAPSULE )
		{
			// adjust the plane distance appropriately for radius
			plane.dist += tw->sphere.radius;

			// find the closest point on the capsule to the plane
			t = DotProduct( plane.normal, tw->sphere.offset );

			if ( t > 0.0f )
			{
				VectorSubtract( tw->start, tw->sphere.offset, startp );
				VectorSubtract( tw->end, tw->sphere.offset, endp );
			}
			else
			{
				VectorAdd( tw->start, tw->sphere.offset, startp );
				VectorAdd( tw->end, tw->sphere.offset, endp );
			}
		}
		else
		{
			offset = DotProduct( tw->offsets[ planes->signbits ], plane.normal );
			plane.dist -= offset;
			VectorCopy( tw->start, startp );
			VectorCopy( tw->end, endp );
		}

		bool hit;

		if ( !CM_CheckFacetPlane( plane, startp, endp, &enterFrac, &leaveFrac, &hit ) )
		{
			continue;
		}

		if ( hit )
		{
			bestplane = plane;
		}

		for ( j = 0; j < facet->numBorders; j++ )
		{
			planes = &sc->planes[ facet->borderPlanes[ j ] ];

			if ( facet->borderInward[ j ] )
			{
				VectorNegate( planes->plane.normal, plane.normal );
				plane.dist = -planes->plane.dist;
			}
			else
			{
				VectorCopy( planes->plane.normal, plane.normal );
				plane.dist = planes->plane.dist;
			}

			if ( tw->type == traceType_t::TT_CAPSULE )
			{
				// adjust the plane distance appropriately for radius
				plane.dist += tw->sphere.radius;

				// find the closest point on the capsule to the plane
				t = DotProduct( plane.normal, tw->sphere.offset );

				if ( t > 0.0f )
				{
					VectorSubtract( tw->start, tw->sphere.offset, startp );
					VectorSubtract( tw->end, tw->sphere.offset, endp );
				}
				else
				{
					VectorAdd( tw->start, tw->sphere.offset, startp );
					VectorAdd( tw->end, tw->sphere.offset, endp );
				}
			}
			else
			{
				// NOTE: this works even though the plane might be flipped because the bbox is centered
				offset = DotProduct( tw->offsets[ planes->signbits ], plane.normal );
				plane.dist += fabsf( offset );
				VectorCopy( tw->start, startp );
				VectorCopy( tw->end, endp );
			}

			if ( !CM_CheckFacetPlane( plane, startp, endp, &enterFrac, &leaveFrac, &hit ) )
			{
				break;
			}

			if ( hit )
			{
				hitnum = j;
				bestplane = plane;
			}
		}

		if ( j < facet->numBorders )
		{
			continue;
		}

		//never clip against the back side
		if ( hitnum == facet->numBorders - 1 )
		{
			continue;
		}

		if ( enterFrac < leaveFrac && enterFrac >= 0 )
		{
			if ( enterFrac < tw->trace.fraction )
			{
				if ( enterFrac < 0 )
				{
					enterFrac = 0;
				}

				tw->trace.fraction = enterFrac;
				VectorCopy( bestplane.normal, tw->trace.plane.normal );
				tw->trace.plane.dist = bestplane.dist;
			}
		}
	}
}

/*
================
CM_TraceThroughSurface
================
*/
void CM_TraceThroughSurface( traceWork_t *tw, const cSurface_t *surface )
{
	float oldFrac;

	oldFrac = tw->trace.fraction;

	if ( !cm_noCurves.Get() && surface->type == mapSurfaceType_t::MST_PATCH && surface->sc )
	{
		CM_TraceThroughSurfaceCollide( tw, surface->sc );
		c_patch_traces++;
	}

	if ( ( cm.perPolyCollision || cm_forceTriangles.Get() ) && surface->type == mapSurfaceType_t::MST_TRIANGLE_SOUP && surface->sc )
	{
		CM_TraceThroughSurfaceCollide( tw, surface->sc );
		c_trisoup_traces++;
	}

	if ( tw->trace.fraction < oldFrac )
	{
		tw->trace.surfaceFlags = surface->surfaceFlags;
		tw->trace.contents = surface->contents;
	}
}

/*
================
CM_TraceThroughBrush
================
*/
void CM_TraceThroughBrush( traceWork_t *tw, const cbrush_t *brush )
{
	float        dist;
	float        enterFrac, leaveFrac;
	float        d1, d2;
	bool     getout, startout;
	float        f;
	float        t;
	vec3_t       startp;
	vec3_t       endp;

	enterFrac = -1.0f;
	leaveFrac = 1.0f;

	if ( !brush->numsides )
	{
		return;
	}

	c_brush_traces++;

	getout = false;
	startout = false;

	const cplane_t *clipplane = nullptr;
	const cbrushside_t *leadside = nullptr;

	const cbrushside_t *firstSide = brush->sides;
	const cbrushside_t *endSide = firstSide + brush->numsides;

	if ( tw->type == traceType_t::TT_CAPSULE )
	{
		//
		// compare the trace against all planes of the brush
		// find the latest time the trace crosses a plane towards the interior
		// and the earliest time the trace crosses a plane towards the exterior
		//
		for ( const cbrushside_t *side = firstSide; side < endSide; side++ )
		{
			const cplane_t *plane = side->plane;

			// adjust the plane distance appropriately for radius
			dist = plane->dist + tw->sphere.radius;

			// find the closest point on the capsule to the plane
			t = DotProduct( plane->normal, tw->sphere.offset );

			if ( t > 0 )
			{
				VectorSubtract( tw->start, tw->sphere.offset, startp );
				VectorSubtract( tw->end, tw->sphere.offset, endp );
			}
			else
			{
				VectorAdd( tw->start, tw->sphere.offset, startp );
				VectorAdd( tw->end, tw->sphere.offset, endp );
			}

			d1 = DotProduct( startp, plane->normal ) - dist;
			d2 = DotProduct( endp, plane->normal ) - dist;

			if ( d2 > 0 )
			{
				getout = true; // endpoint is not in solid
			}

			if ( d1 > 0 )
			{
				startout = true;
			}

			// if completely in front of face, no intersection with the entire brush
			if ( d1 > 0 && ( d2 >= SURFACE_CLIP_EPSILON || d2 >= d1 ) )
			{
				return;
			}

			// if it doesn't cross the plane, the plane isn't relevant
			if ( d1 <= 0 && d2 <= 0 )
			{
				continue;
			}

			// crosses face
			if ( d1 > d2 )
			{
				// enter
				f = ( d1 - SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

				if ( f < 0 )
				{
					f = 0;
				}

				if ( f > enterFrac )
				{
					enterFrac = f;
					clipplane = plane;
					leadside = side;
				}
			}
			else
			{
				// leave
				f = ( d1 + SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

				if ( f > 1 )
				{
					f = 1;
				}

				if ( f < leaveFrac )
				{
					leaveFrac = f;
				}
			}
		}
	}
	else
	{
		//
		// compare the trace against all planes of the brush
		// find the latest time the trace crosses a plane towards the interior
		// and the earliest time the trace crosses a plane towards the exterior
		//
		for ( const cbrushside_t *side = firstSide; side < endSide; side++ )
		{
			const cplane_t *plane = side->plane;

			// adjust the plane distance appropriately for mins/maxs
			dist = plane->dist - DotProduct( tw->offsets[ plane->signbits ], plane->normal );

			d1 = DotProduct( tw->start, plane->normal ) - dist;
			d2 = DotProduct( tw->end, plane->normal ) - dist;

			if ( d2 > 0 )
			{
				getout = true; // endpoint is not in solid
			}

			if ( d1 > 0 )
			{
				startout = true;
			}

			// if completely in front of face, no intersection with the entire brush
			if ( d1 > 0 && ( d2 >= SURFACE_CLIP_EPSILON || d2 >= d1 ) )
			{
				return;
			}

			// if it doesn't cross the plane, the plane isn't relevant
			if ( d1 <= 0 && d2 <= 0 )
			{
				continue;
			}

			// crosses face
			if ( d1 > d2 )
			{
				// enter
				f = ( d1 - SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

				if ( f < 0 )
				{
					f = 0;
				}

				if ( f > enterFrac )
				{
					enterFrac = f;
					clipplane = plane;
					leadside = side;
				}
			}
			else
			{
				// leave
				f = ( d1 + SURFACE_CLIP_EPSILON ) / ( d1 - d2 );

				if ( f > 1 )
				{
					f = 1;
				}

				if ( f < leaveFrac )
				{
					leaveFrac = f;
				}
			}
		}
	}

	//
	// all planes have been checked, and the trace was not
	// completely outside the brush
	//
	if ( !startout )
	{
		// original point was inside brush
		tw->trace.startsolid = true;

		if ( !getout )
		{
			tw->trace.allsolid = true;
			tw->trace.fraction = 0;
			tw->trace.contents = brush->contents;
		}

		return;
	}

	if ( enterFrac < leaveFrac )
	{
		if ( enterFrac > -1 && enterFrac < tw->trace.fraction )
		{
			if ( enterFrac < 0 )
			{
				enterFrac = 0;
			}

			tw->trace.fraction = enterFrac;
			VectorCopy(clipplane->normal, tw->trace.plane.normal);
			tw->trace.plane.dist = clipplane->dist;
			tw->trace.surfaceFlags = leadside->surfaceFlags;
			tw->trace.contents = brush->contents;
		}
	}
}

/*
================
CM_TraceThroughLeaf
================
*/
void CM_TraceThroughLeaf( traceWork_t *tw, const cLeaf_t *leaf )
{
	// trace line against all brushes in the leaf
	const int *firstBrushNum = leaf->firstLeafBrush;
	const int *endBrushNum = firstBrushNum + leaf->numLeafBrushes;
	for ( const int *brushNum = firstBrushNum; brushNum < endBrushNum; brushNum++ )
	{
		cbrush_t *b = &cm.brushes[ *brushNum ];

		if ( b->checkcount == cm.checkcount )
		{
			continue; // already checked this brush in another leaf
		}

		b->checkcount = cm.checkcount;

		if ( !( b->contents & tw->contents ) )
		{
			continue;
		}

		if ( b->contents & tw->skipContents )
		{
			continue;
		}

		if ( !CM_BoundsIntersect( tw->bounds[ 0 ], tw->bounds[ 1 ], b->bounds[ 0 ], b->bounds[ 1 ] ) )
		{
			continue;
		}

		CM_TraceThroughBrush( tw, b );

		if ( tw->trace.allsolid )
		{
			return;
		}
	}

	// CM_TraceThroughSurface does not set startsolid/allsolid so 0 fraction is the most we'll know
	if ( !tw->trace.fraction )
	{
		return;
	}

	// trace line against all surfaces in the leaf
	const int *firstSurfaceNum = leaf->firstLeafSurface;
	const int *endSurfaceNum = firstSurfaceNum + leaf->numLeafSurfaces;
	for ( const int *surfaceNum = firstSurfaceNum; surfaceNum < endSurfaceNum; surfaceNum++ )
	{
		cSurface_t *surface = cm.surfaces[ *surfaceNum ];

		if ( !surface )
		{
			continue;
		}

		if ( surface->checkcount == cm.checkcount )
		{
			continue; // already checked this surface in another leaf
		}

		surface->checkcount = cm.checkcount;

		if ( !( surface->contents & tw->contents ) )
		{
			continue;
		}

		if ( surface->contents & tw->skipContents )
		{
			continue;
		}

		if ( !CM_BoundsIntersect( tw->bounds[ 0 ], tw->bounds[ 1 ], surface->sc->bounds[ 0 ], surface->sc->bounds[ 1 ] ) )
		{
			continue;
		}

		CM_TraceThroughSurface( tw, surface );

		if ( !tw->trace.fraction )
		{
			return;
		}
	}
}

static const float RADIUS_EPSILON = 1.0f;

/*
================
CM_TraceThroughSphere

get the first intersection of the ray with the sphere
================
*/
void CM_TraceThroughSphere( traceWork_t *tw, const vec3_t origin, float radius, const vec3_t start, const vec3_t end )
{
	float  l1, l2, length, scale, fraction;
	float  b, c, d, sqrtd;
	vec3_t v1, dir, intersection;

	// if inside the sphere
	VectorSubtract( start, origin, dir );
	l1 = VectorLengthSquared( dir );

	if ( l1 < Square( radius ) )
	{
		tw->trace.fraction = 0;
		tw->trace.startsolid = true;
		// test for allsolid
		VectorSubtract( end, origin, dir );
		l1 = VectorLengthSquared( dir );

		if ( l1 < Square( radius ) )
		{
			tw->trace.allsolid = true;
		}

		return;
	}

	//
	VectorSubtract( end, start, dir );
	length = VectorNormalize( dir );
	//
	l1 = CM_DistanceFromLineSquared( origin, start, end, dir );
	VectorSubtract( end, origin, v1 );
	l2 = VectorLengthSquared( v1 );

	// if no intersection with the sphere and the end point is at least an epsilon away
	if ( l1 >= Square( radius ) && l2 > Square( radius + SURFACE_CLIP_EPSILON ) )
	{
		return;
	}

	//
	//  | origin - (start + t * dir) | = radius
	//  a = dir[0]^2 + dir[1]^2 + dir[2]^2;
	//  b = 2 * (dir[0] * (start[0] - origin[0]) + dir[1] * (start[1] - origin[1]) + dir[2] * (start[2] - origin[2]));
	//  c = (start[0] - origin[0])^2 + (start[1] - origin[1])^2 + (start[2] - origin[2])^2 - radius^2;
	//
	VectorSubtract( start, origin, v1 );
	// dir is normalized so a = 1
	//dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
	b = 2.0f * ( dir[ 0 ] * v1[ 0 ] + dir[ 1 ] * v1[ 1 ] + dir[ 2 ] * v1[ 2 ] );
	c = v1[ 0 ] * v1[ 0 ] + v1[ 1 ] * v1[ 1 ] + v1[ 2 ] * v1[ 2 ] - ( radius + RADIUS_EPSILON ) * ( radius + RADIUS_EPSILON );

	d = b * b - 4.0f * c; // * a;

	if ( d > 0 )
	{
		sqrtd = sqrtf( d );
		// = (- b + sqrtd) * 0.5f; // / (2.0f * a);
		fraction = ( -b - sqrtd ) * 0.5f; // / (2.0f * a);

		//
		if ( fraction < 0 )
		{
			fraction = 0;
		}
		else
		{
			fraction /= length;
		}

		if ( fraction < tw->trace.fraction )
		{
			tw->trace.fraction = fraction;
			VectorSubtract( end, start, dir );
			VectorMA( start, fraction, dir, intersection );
			VectorSubtract( intersection, origin, dir );
			scale = 1 / ( radius + RADIUS_EPSILON );
			VectorScale( dir, scale, dir );
			VectorCopy( dir, tw->trace.plane.normal );
			VectorAdd( tw->modelOrigin, intersection, intersection );
			tw->trace.plane.dist = DotProduct( tw->trace.plane.normal, intersection );
			tw->trace.contents = CONTENTS_BODY;
		}
	}
	else if ( d == 0 )
	{
		//t1 = (- b ) / 2;
		// slide along the sphere
	}

	// no intersection at all
}

/*
================
CM_TraceThroughVerticalCylinder

get the first intersection of the ray with the cylinder
the cylinder extends halfheight above and below the origin
================
*/
void CM_TraceThroughVerticalCylinder( traceWork_t *tw, const vec3_t origin, float radius, float halfheight, const vec3_t start, const vec3_t end )
{
	float  length, scale, fraction, l1, l2;
	float  b, c, d, sqrtd;
	vec3_t v1, dir, start2d, end2d, org2d, intersection;

	// 2d coordinates
	VectorSet( start2d, start[ 0 ], start[ 1 ], 0 );
	VectorSet( end2d, end[ 0 ], end[ 1 ], 0 );
	VectorSet( org2d, origin[ 0 ], origin[ 1 ], 0 );

	// if between lower and upper cylinder bounds
	if ( start[ 2 ] <= origin[ 2 ] + halfheight && start[ 2 ] >= origin[ 2 ] - halfheight )
	{
		// if inside the cylinder
		VectorSubtract( start2d, org2d, dir );
		l1 = VectorLengthSquared( dir );

		if ( l1 < Square( radius ) )
		{
			tw->trace.fraction = 0;
			tw->trace.startsolid = true;
			VectorSubtract( end2d, org2d, dir );
			l1 = VectorLengthSquared( dir );

			if ( l1 < Square( radius ) )
			{
				tw->trace.allsolid = true;
			}

			return;
		}
	}

	//
	VectorSubtract( end2d, start2d, dir );
	length = VectorNormalize( dir );
	//
	l1 = CM_DistanceFromLineSquared( org2d, start2d, end2d, dir );
	VectorSubtract( end2d, org2d, v1 );
	l2 = VectorLengthSquared( v1 );

	// if no intersection with the cylinder and the end point is at least an epsilon away
	if ( l1 >= Square( radius ) && l2 > Square( radius + SURFACE_CLIP_EPSILON ) )
	{
		return;
	}

	//
	//
	// (start[0] - origin[0] - t * dir[0]) ^ 2 + (start[1] - origin[1] - t * dir[1]) ^ 2 = radius ^ 2
	// (v1[0] + t * dir[0]) ^ 2 + (v1[1] + t * dir[1]) ^ 2 = radius ^ 2;
	// v1[0] ^ 2 + 2 * v1[0] * t * dir[0] + (t * dir[0]) ^ 2 +
	//                      v1[1] ^ 2 + 2 * v1[1] * t * dir[1] + (t * dir[1]) ^ 2 = radius ^ 2
	// t ^ 2 * (dir[0] ^ 2 + dir[1] ^ 2) + t * (2 * v1[0] * dir[0] + 2 * v1[1] * dir[1]) +
	//                      v1[0] ^ 2 + v1[1] ^ 2 - radius ^ 2 = 0
	//
	VectorSubtract( start, origin, v1 );
	// dir is normalized so we can use a = 1
	// * (dir[0] * dir[0] + dir[1] * dir[1]);
	b = 2.0f * ( v1[ 0 ] * dir[ 0 ] + v1[ 1 ] * dir[ 1 ] );
	c = v1[ 0 ] * v1[ 0 ] + v1[ 1 ] * v1[ 1 ] - ( radius + RADIUS_EPSILON ) * ( radius + RADIUS_EPSILON );

	d = b * b - 4.0f * c; // * a;

	if ( d > 0 )
	{
		sqrtd = sqrtf( d );
		// = (- b + sqrtd) * 0.5f;// / (2.0f * a);
		fraction = ( -b - sqrtd ) * 0.5f; // / (2.0f * a);

		//
		if ( fraction < 0 )
		{
			fraction = 0;
		}
		else
		{
			fraction /= length;
		}

		if ( fraction < tw->trace.fraction )
		{
			VectorSubtract( end, start, dir );
			VectorMA( start, fraction, dir, intersection );

			// if the intersection is between the cylinder lower and upper bound
			if ( intersection[ 2 ] <= origin[ 2 ] + halfheight && intersection[ 2 ] >= origin[ 2 ] - halfheight )
			{
				//
				tw->trace.fraction = fraction;
				VectorSubtract( intersection, origin, dir );
				dir[ 2 ] = 0;
				scale = 1 / ( radius + RADIUS_EPSILON );
				VectorScale( dir, scale, dir );
				VectorCopy( dir, tw->trace.plane.normal );
				VectorAdd( tw->modelOrigin, intersection, intersection );
				tw->trace.plane.dist = DotProduct( tw->trace.plane.normal, intersection );
				tw->trace.contents = CONTENTS_BODY;
			}
		}
	}
	else if ( d == 0 )
	{
		//t[0] = (- b ) / 2 * a;
		// slide along the cylinder
	}

	// no intersection at all
}

/*
================
CM_TraceCapsuleThroughCapsule

capsule vs. capsule collision (not rotated)
================
*/
void CM_TraceCapsuleThroughCapsule( traceWork_t *tw, clipHandle_t model )
{
	vec3_t mins, maxs;
	vec3_t top, bottom, starttop, startbottom, endtop, endbottom;
	vec3_t offset, symetricSize[ 2 ];
	float  radius, halfwidth, halfheight, offs, h;

	CM_ModelBounds( model, mins, maxs );

	// test trace bounds vs. capsule bounds
	if ( tw->bounds[ 0 ][ 0 ] > maxs[ 0 ] + RADIUS_EPSILON
	     || tw->bounds[ 0 ][ 1 ] > maxs[ 1 ] + RADIUS_EPSILON
	     || tw->bounds[ 0 ][ 2 ] > maxs[ 2 ] + RADIUS_EPSILON
	     || tw->bounds[ 1 ][ 0 ] < mins[ 0 ] - RADIUS_EPSILON
	     || tw->bounds[ 1 ][ 1 ] < mins[ 1 ] - RADIUS_EPSILON
	     || tw->bounds[ 1 ][ 2 ] < mins[ 2 ] - RADIUS_EPSILON )
	{
		return;
	}

	// top origin and bottom origin of each sphere at start and end of trace
	VectorAdd( tw->start, tw->sphere.offset, starttop );
	VectorSubtract( tw->start, tw->sphere.offset, startbottom );
	VectorAdd( tw->end, tw->sphere.offset, endtop );
	VectorSubtract( tw->end, tw->sphere.offset, endbottom );

	// calculate top and bottom of the capsule spheres to collide with
	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		symetricSize[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		symetricSize[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		symetricSize[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		symetricSize[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		symetricSize[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		symetricSize[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];
	}

	halfwidth = symetricSize[ 1 ][ 0 ];
	halfheight = symetricSize[ 1 ][ 2 ];
	radius = ( halfwidth > halfheight ) ? halfheight : halfwidth;
	offs = halfheight - radius;
	VectorCopy( offset, top );
	top[ 2 ] += offs;
	VectorCopy( offset, bottom );
	bottom[ 2 ] -= offs;
	// expand radius of spheres
	radius += tw->sphere.radius;

	// if there is horizontal movement
	if ( tw->start[ 0 ] != tw->end[ 0 ] || tw->start[ 1 ] != tw->end[ 1 ] )
	{
		// height of the expanded cylinder is the height of both cylinders minus the radius of both spheres
		h = halfheight + tw->sphere.halfheight - radius;

		// if the cylinder has a height
		if ( h > 0 )
		{
			// test for collisions between the cylinders
			CM_TraceThroughVerticalCylinder( tw, offset, radius, h, tw->start, tw->end );
		}
	}

	// test for collision between the spheres
	CM_TraceThroughSphere( tw, top, radius, startbottom, endbottom );
	CM_TraceThroughSphere( tw, bottom, radius, starttop, endtop );
}

/*
================
CM_TraceBoundingBoxThroughCapsule

bounding box vs. capsule collision
================
*/
void CM_TraceBoundingBoxThroughCapsule( traceWork_t *tw, clipHandle_t model )
{
	vec3_t       mins, maxs, offset, size[ 2 ];
	clipHandle_t h;
	cmodel_t     *cmod;

	// mins maxs of the capsule
	CM_ModelBounds( model, mins, maxs );

	// offset for capsule center
	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		size[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		size[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		size[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		size[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		size[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		size[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];

		tw->start[ 0 ] -= offset[ 0 ];
		tw->start[ 1 ] -= offset[ 1 ];
		tw->start[ 2 ] -= offset[ 2 ];

		tw->end[ 0 ] -= offset[ 0 ];
		tw->end[ 1 ] -= offset[ 1 ];
		tw->end[ 2 ] -= offset[ 2 ];
	}

	// replace the bounding box with the capsule
	tw->type = traceType_t::TT_CAPSULE;
	tw->sphere.radius = ( size[ 1 ][ 0 ] > size[ 1 ][ 2 ] ) ? size[ 1 ][ 2 ] : size[ 1 ][ 0 ];
	tw->sphere.halfheight = size[ 1 ][ 2 ];
	VectorSet( tw->sphere.offset, 0, 0, size[ 1 ][ 2 ] - tw->sphere.radius );

	// replace the capsule with the bounding box
	h = CM_TempBoxModel( tw->size[ 0 ], tw->size[ 1 ], false );

	// calculate collision
	cmod = CM_ClipHandleToModel( h );
	CM_TraceThroughLeaf( tw, &cmod->leaf );
}

//=========================================================================================

/*
==================
CM_TraceThroughTree

Traverse all the contacted leafs from the start to the end position.
If the trace is a point, they will be exactly in order, but for larger
trace volumes it is possible to hit something in a later leaf with
a smaller intercept fraction.
==================
*/
static void CM_TraceThroughTree( traceWork_t *tw, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2 )
{
	cNode_t  *node;
	cplane_t *plane;
	float    t1, t2, offset;
	float    frac, frac2;
	float    idist;
	vec3_t   mid;
	int      side;
	float    midf;

	if ( tw->trace.fraction < p1f )
	{
		return; // already hit something nearer
	}

	// if < 0, we are in a leaf node
	if ( num < 0 )
	{
		CM_TraceThroughLeaf( tw, &cm.leafs[ -1 - num ] );
		return;
	}

	//
	// find the point distances to the separating plane
	// and the offset for the size of the box
	//
	node = cm.nodes + num;
	plane = node->plane;

	// adjust the plane distance appropriately for mins/maxs
	if ( plane->type < 3 )
	{
		t1 = p1[ plane->type ] - plane->dist;
		t2 = p2[ plane->type ] - plane->dist;
		offset = tw->extents[ plane->type ];
	}
	else
	{
		t1 = DotProduct( plane->normal, p1 ) - plane->dist;
		t2 = DotProduct( plane->normal, p2 ) - plane->dist;
		offset = tw->maxOffset;
	}

	// see which sides we need to consider
	if ( t1 >= offset + 1 && t2 >= offset + 1 )
	{
		CM_TraceThroughTree( tw, node->children[ 0 ], p1f, p2f, p1, p2 );
		return;
	}

	if ( t1 < -offset - 1 && t2 < -offset - 1 )
	{
		CM_TraceThroughTree( tw, node->children[ 1 ], p1f, p2f, p1, p2 );
		return;
	}

	// put the crosspoint SURFACE_CLIP_EPSILON pixels on the near side
	if ( t1 < t2 )
	{
		idist = 1.0f / ( t1 - t2 );
		side = 1;
		frac2 = ( t1 + offset + SURFACE_CLIP_EPSILON ) * idist;
		frac = ( t1 - offset + SURFACE_CLIP_EPSILON ) * idist;
	}
	else if ( t1 > t2 )
	{
		idist = 1.0f / ( t1 - t2 );
		side = 0;
		frac2 = ( t1 - offset - SURFACE_CLIP_EPSILON ) * idist;
		frac = ( t1 + offset + SURFACE_CLIP_EPSILON ) * idist;
	}
	else
	{
		side = 0;
		frac = 1;
		frac2 = 0;
	}

	// move up to the node
	if ( frac < 0 )
	{
		frac = 0;
	}

	if ( frac > 1 )
	{
		frac = 1;
	}

	midf = p1f + ( p2f - p1f ) * frac;

	mid[ 0 ] = p1[ 0 ] + frac * ( p2[ 0 ] - p1[ 0 ] );
	mid[ 1 ] = p1[ 1 ] + frac * ( p2[ 1 ] - p1[ 1 ] );
	mid[ 2 ] = p1[ 2 ] + frac * ( p2[ 2 ] - p1[ 2 ] );

	CM_TraceThroughTree( tw, node->children[ side ], p1f, midf, p1, mid );

	// go past the node
	if ( frac2 < 0 )
	{
		frac2 = 0;
	}

	if ( frac2 > 1 )
	{
		frac2 = 1;
	}

	midf = p1f + ( p2f - p1f ) * frac2;

	mid[ 0 ] = p1[ 0 ] + frac2 * ( p2[ 0 ] - p1[ 0 ] );
	mid[ 1 ] = p1[ 1 ] + frac2 * ( p2[ 1 ] - p1[ 1 ] );
	mid[ 2 ] = p1[ 2 ] + frac2 * ( p2[ 2 ] - p1[ 2 ] );

	CM_TraceThroughTree( tw, node->children[ side ^ 1 ], midf, p2f, mid, p2 );
}

//======================================================================

/*
==================
CM_Trace
==================
*/
static void CM_Trace( trace_t *results, const vec3_t start, const vec3_t end, const vec3_t mins,
                      const vec3_t maxs, clipHandle_t model, const vec3_t origin, int brushmask,
                      int skipmask, traceType_t type, const sphere_t *sphere )
{
	int         i;
	vec3_t      offset;
	cmodel_t    *cmod;

	cmod = CM_ClipHandleToModel( model );

	cm.checkcount++; // for multi-check avoidance

	c_traces++; // for statistics, may be zeroed

	// fill in a default trace
	traceWork_t tw{};
	tw.trace.fraction = 1; // assume it goes the entire distance until shown otherwise
	VectorCopy( origin, tw.modelOrigin );
	tw.type = type;

	if ( !cm.numNodes )
	{
		*results = tw.trace;

		return; // map not loaded, shouldn't happen
	}

	// allow nullptr to be passed in for 0,0,0
	if ( !mins )
	{
		mins = vec3_origin;
	}

	if ( !maxs )
	{
		maxs = vec3_origin;
	}

	// set basic parms
	tw.contents = brushmask;
	tw.skipContents = skipmask;

	// adjust so that mins and maxs are always symmetric, which
	// avoids some complications with plane expanding of rotated
	// bmodels
	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		tw.size[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		tw.size[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		tw.size[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		tw.size[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		tw.size[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		tw.size[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];

		tw.start[ 0 ] = start[ 0 ] + offset[ 0 ];
		tw.start[ 1 ] = start[ 1 ] + offset[ 1 ];
		tw.start[ 2 ] = start[ 2 ] + offset[ 2 ];

		tw.end[ 0 ] = end[ 0 ] + offset[ 0 ];
		tw.end[ 1 ] = end[ 1 ] + offset[ 1 ];
		tw.end[ 2 ] = end[ 2 ] + offset[ 2 ];
	}

	// if a sphere is already specified
	if ( sphere )
	{
		tw.sphere = *sphere;
	}
	else
	{
		tw.sphere.radius = ( tw.size[ 1 ][ 0 ] > tw.size[ 1 ][ 2 ] ) ? tw.size[ 1 ][ 2 ] : tw.size[ 1 ][ 0 ];
		tw.sphere.halfheight = tw.size[ 1 ][ 2 ];
		VectorSet( tw.sphere.offset, 0, 0, tw.size[ 1 ][ 2 ] - tw.sphere.radius );
	}

	tw.maxOffset = VectorLength( tw.size[ 1 ] );

	// tw.offsets[signbits] = vector to appropriate corner from origin
	tw.offsets[ 0 ][ 0 ] = tw.size[ 0 ][ 0 ];
	tw.offsets[ 0 ][ 1 ] = tw.size[ 0 ][ 1 ];
	tw.offsets[ 0 ][ 2 ] = tw.size[ 0 ][ 2 ];

	tw.offsets[ 1 ][ 0 ] = tw.size[ 1 ][ 0 ];
	tw.offsets[ 1 ][ 1 ] = tw.size[ 0 ][ 1 ];
	tw.offsets[ 1 ][ 2 ] = tw.size[ 0 ][ 2 ];

	tw.offsets[ 2 ][ 0 ] = tw.size[ 0 ][ 0 ];
	tw.offsets[ 2 ][ 1 ] = tw.size[ 1 ][ 1 ];
	tw.offsets[ 2 ][ 2 ] = tw.size[ 0 ][ 2 ];

	tw.offsets[ 3 ][ 0 ] = tw.size[ 1 ][ 0 ];
	tw.offsets[ 3 ][ 1 ] = tw.size[ 1 ][ 1 ];
	tw.offsets[ 3 ][ 2 ] = tw.size[ 0 ][ 2 ];

	tw.offsets[ 4 ][ 0 ] = tw.size[ 0 ][ 0 ];
	tw.offsets[ 4 ][ 1 ] = tw.size[ 0 ][ 1 ];
	tw.offsets[ 4 ][ 2 ] = tw.size[ 1 ][ 2 ];

	tw.offsets[ 5 ][ 0 ] = tw.size[ 1 ][ 0 ];
	tw.offsets[ 5 ][ 1 ] = tw.size[ 0 ][ 1 ];
	tw.offsets[ 5 ][ 2 ] = tw.size[ 1 ][ 2 ];

	tw.offsets[ 6 ][ 0 ] = tw.size[ 0 ][ 0 ];
	tw.offsets[ 6 ][ 1 ] = tw.size[ 1 ][ 1 ];
	tw.offsets[ 6 ][ 2 ] = tw.size[ 1 ][ 2 ];

	tw.offsets[ 7 ][ 0 ] = tw.size[ 1 ][ 0 ];
	tw.offsets[ 7 ][ 1 ] = tw.size[ 1 ][ 1 ];
	tw.offsets[ 7 ][ 2 ] = tw.size[ 1 ][ 2 ];

	//
	// calculate bounds
	//
	if ( tw.type == traceType_t::TT_CAPSULE )
	{
		for ( i = 0; i < 3; i++ )
		{
			if ( tw.start[ i ] < tw.end[ i ] )
			{
				tw.bounds[ 0 ][ i ] = tw.start[ i ] - fabsf( tw.sphere.offset[ i ] ) - tw.sphere.radius;
				tw.bounds[ 1 ][ i ] = tw.end[ i ] + fabsf( tw.sphere.offset[ i ] ) + tw.sphere.radius;
			}
			else
			{
				tw.bounds[ 0 ][ i ] = tw.end[ i ] - fabsf( tw.sphere.offset[ i ] ) - tw.sphere.radius;
				tw.bounds[ 1 ][ i ] = tw.start[ i ] + fabsf( tw.sphere.offset[ i ] ) + tw.sphere.radius;
			}
		}
	}
	else
	{
		for ( i = 0; i < 3; i++ )
		{
			if ( tw.start[ i ] < tw.end[ i ] )
			{
				tw.bounds[ 0 ][ i ] = tw.start[ i ] + tw.size[ 0 ][ i ];
				tw.bounds[ 1 ][ i ] = tw.end[ i ] + tw.size[ 1 ][ i ];
			}
			else
			{
				tw.bounds[ 0 ][ i ] = tw.end[ i ] + tw.size[ 0 ][ i ];
				tw.bounds[ 1 ][ i ] = tw.start[ i ] + tw.size[ 1 ][ i ];
			}
		}
	}

	//
	// check for position test special case
	//
	if ( start[ 0 ] == end[ 0 ] && start[ 1 ] == end[ 1 ] && start[ 2 ] == end[ 2 ] )
	{
		if ( model )
		{
#ifdef ALWAYS_BBOX_VS_BBOX // FIXME - compile time flag?

			if ( model == BOX_MODEL_HANDLE || model == CAPSULE_MODEL_HANDLE )
			{
				tw.type = TT_AABB;
				CM_TestInLeaf( &tw, &cmod->leaf );
			}
			else
#elif defined( ALWAYS_CAPSULE_VS_CAPSULE )
			if ( model == BOX_MODEL_HANDLE || model == CAPSULE_MODEL_HANDLE )
			{
				CM_TestCapsuleInCapsule( &tw, model );
			}
			else
#endif
				if ( model == CAPSULE_MODEL_HANDLE )
				{
					if ( tw.type == traceType_t::TT_CAPSULE )
					{
						CM_TestCapsuleInCapsule( &tw, model );
					}
					else
					{
						CM_TestBoundingBoxInCapsule( &tw, model );
					}
				}
				else
				{
					CM_TestInLeaf( &tw, &cmod->leaf );
				}
		}
		else
		{
			CM_PositionTest( &tw );
		}
	}
	else
	{
		//
		// check for point special case
		//
		if ( tw.size[ 0 ][ 0 ] == 0 && tw.size[ 0 ][ 1 ] == 0 && tw.size[ 0 ][ 2 ] == 0 )
		{
			tw.isPoint = true;
			VectorClear( tw.extents );
		}
		else
		{
			tw.isPoint = false;
			tw.extents[ 0 ] = tw.size[ 1 ][ 0 ];
			tw.extents[ 1 ] = tw.size[ 1 ][ 1 ];
			tw.extents[ 2 ] = tw.size[ 1 ][ 2 ];
		}

		//
		// general sweeping through world
		//
		if ( model )
		{
#ifdef ALWAYS_BBOX_VS_BBOX

			if ( model == BOX_MODEL_HANDLE || model == CAPSULE_MODEL_HANDLE )
			{
				tw.type = TT_AABB;
				CM_TraceThroughLeaf( &tw, &cmod->leaf );
			}
			else
#elif defined( ALWAYS_CAPSULE_VS_CAPSULE )
			if ( model == BOX_MODEL_HANDLE || model == CAPSULE_MODEL_HANDLE )
			{
				CM_TraceCapsuleThroughCapsule( &tw, model );
			}
			else
#endif
				if ( model == CAPSULE_MODEL_HANDLE )
				{
					if ( tw.type == traceType_t::TT_CAPSULE )
					{
						CM_TraceCapsuleThroughCapsule( &tw, model );
					}
					else
					{
						CM_TraceBoundingBoxThroughCapsule( &tw, model );
					}
				}
				else
				{
					CM_TraceThroughLeaf( &tw, &cmod->leaf );
				}
		}
		else
		{
			CM_TraceThroughTree( &tw, 0, 0, 1, tw.start, tw.end );
		}
	}

	// generate endpos from the original, unmodified start/end
	if ( tw.trace.fraction == 1 )
	{
		VectorCopy( end, tw.trace.endpos );
	}
	else
	{
		VectorLerp( start, end, tw.trace.fraction, tw.trace.endpos );
	}

	*results = tw.trace;
}

/*
==================
CM_BoxTrace
==================
*/
void CM_BoxTrace( trace_t *results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs,
                  clipHandle_t model, int brushmask, int skipmask, traceType_t type )
{
	CM_Trace( results, start, end, mins, maxs, model, vec3_origin, brushmask, skipmask, type, nullptr );
}

/*
==================
CM_TransformedBoxTrace

Handles offseting and rotation of the end points for moving and
rotating entities
==================
*/
void CM_TransformedBoxTrace( trace_t *results, const vec3_t start, const vec3_t end,
                             const vec3_t mins, const vec3_t maxs, clipHandle_t model,
                             int brushmask, int skipmask, const vec3_t origin, const vec3_t angles,
                             traceType_t type )
{
	trace_t  trace;
	vec3_t   start_l, end_l;
	bool rotated;
	vec3_t   offset;
	vec3_t   symetricSize[ 2 ];
	vec3_t   matrix[ 3 ], transpose[ 3 ];
	float    halfwidth;
	float    halfheight;
	float    t;
	sphere_t sphere;

	if ( !mins )
	{
		mins = vec3_origin;
	}

	if ( !maxs )
	{
		maxs = vec3_origin;
	}

	// adjust so that mins and maxs are always symmetric, which
	// avoids some complications with plane expanding of rotated
	// bmodels
	{
		offset[ 0 ] = ( mins[ 0 ] + maxs[ 0 ] ) * 0.5;
		offset[ 1 ] = ( mins[ 1 ] + maxs[ 1 ] ) * 0.5;
		offset[ 2 ] = ( mins[ 2 ] + maxs[ 2 ] ) * 0.5;

		symetricSize[ 0 ][ 0 ] = mins[ 0 ] - offset[ 0 ];
		symetricSize[ 0 ][ 1 ] = mins[ 1 ] - offset[ 1 ];
		symetricSize[ 0 ][ 2 ] = mins[ 2 ] - offset[ 2 ];

		symetricSize[ 1 ][ 0 ] = maxs[ 0 ] - offset[ 0 ];
		symetricSize[ 1 ][ 1 ] = maxs[ 1 ] - offset[ 1 ];
		symetricSize[ 1 ][ 2 ] = maxs[ 2 ] - offset[ 2 ];

		start_l[ 0 ] = start[ 0 ] + offset[ 0 ];
		start_l[ 1 ] = start[ 1 ] + offset[ 1 ];
		start_l[ 2 ] = start[ 2 ] + offset[ 2 ];

		end_l[ 0 ] = end[ 0 ] + offset[ 0 ];
		end_l[ 1 ] = end[ 1 ] + offset[ 1 ];
		end_l[ 2 ] = end[ 2 ] + offset[ 2 ];
	}

	// subtract origin offset
	VectorSubtract( start_l, origin, start_l );
	VectorSubtract( end_l, origin, end_l );

	// rotate start and end into the models frame of reference
	if ( model != BOX_MODEL_HANDLE && ( angles[ 0 ] || angles[ 1 ] || angles[ 2 ] ) )
	{
		rotated = true;
	}
	else
	{
		rotated = false;
	}

	halfwidth = symetricSize[ 1 ][ 0 ];
	halfheight = symetricSize[ 1 ][ 2 ];

	sphere.radius = ( halfwidth > halfheight ) ? halfheight : halfwidth;
	sphere.halfheight = halfheight;
	t = halfheight - sphere.radius;

	if ( rotated )
	{
		// rotation on trace line (start-end) instead of rotating the bmodel
		// NOTE: This is still incorrect for bounding boxes because the actual bounding
		//       box that is swept through the model is not rotated. We cannot rotate
		//       the bounding box or the bmodel because that would make all the brush
		//       bevels invalid.
		//       However this is correct for capsules since a capsule itself is rotated too.
		CreateRotationMatrix( angles, matrix );
		RotatePoint( start_l, matrix );
		RotatePoint( end_l, matrix );
		// rotated sphere offset for capsule
		sphere.offset[ 0 ] = matrix[ 0 ][ 2 ] * t;
		sphere.offset[ 1 ] = -matrix[ 1 ][ 2 ] * t;
		sphere.offset[ 2 ] = matrix[ 2 ][ 2 ] * t;
	}
	else
	{
		VectorSet( sphere.offset, 0, 0, t );
	}

	// sweep the box through the model
	CM_Trace( &trace, start_l, end_l, symetricSize[ 0 ], symetricSize[ 1 ], model, origin,
			  brushmask, skipmask, type, &sphere );

	// if the bmodel was rotated and there was a collision
	if ( rotated && trace.fraction != 1.0f )
	{
		// rotation of bmodel collision plane
		TransposeMatrix( matrix, transpose );
		RotatePoint( trace.plane.normal, transpose );
	}

	// re-calculate the end position of the trace because the trace.endpos
	// calculated by CM_Trace could be rotated and have an offset
	trace.endpos[ 0 ] = start[ 0 ] + trace.fraction * ( end[ 0 ] - start[ 0 ] );
	trace.endpos[ 1 ] = start[ 1 ] + trace.fraction * ( end[ 1 ] - start[ 1 ] );
	trace.endpos[ 2 ] = start[ 2 ] + trace.fraction * ( end[ 2 ] - start[ 2 ] );

	*results = trace;
}

// Checks the invariants of a trace - that the trace_t result is
// consistent with itself and the arguments.
// Returns a string describing a problem if there is one, or the empty string if not.
std::string CM_CheckTraceConsistency( const vec3_t start, const vec3_t end, int contentmask, int skipmask, const trace_t &tr )
{
	if ( !( tr.fraction >= 0.0f && tr.fraction <= 1.0f ) )
	{
		return "fraction out of range";
	}

	if ( tr.allsolid )
	{
		if ( !tr.startsolid )
		{
			return "allsolid without startsolid";
		}
		if ( tr.fraction != 0.0f )
		{
			return "with allsolid fraction should be 0";
		}
	}

	// check contents
	if ( tr.fraction == 1.0f )
	{
		if ( tr.contents != 0 )
		{
			return "should not have content flags with fraction==1";
		}
	}
	else
	{
		if ( !( tr.contents & contentmask) )
		{
			return "trace has collision but no matching content flags";
		}
		if ( tr.contents & skipmask )
		{
			return "skipmask not respected";
		}
	}

	// check endpos. Special cases for exact equality
	if ( tr.allsolid )
	{
		if ( !VectorCompare( tr.endpos, start ) )
		{
			return "endpos not exactly equal to start with allsolid=true";
		}
	}
	else if ( tr.fraction == 1.0f )
	{
		if ( !VectorCompare( tr.endpos, end ) )
		{
			return "endpos not exactly equal to end with fraction=1";
		}
	}
	else
	{
		vec3_t expectedEndpos;
		VectorScale( end, tr.fraction, expectedEndpos );
		VectorMA( expectedEndpos, 1.0f - tr.fraction, start, expectedEndpos );
		if ( DistanceSquared( tr.endpos, expectedEndpos ) > Square( 0.001f ) )
		{
			return "endpos significantly different from expected";
		}
	}

	// If the trace "hit" something (excluding allsolid), then plane and surfaceFlags are valid
	// (but there is no way to verify surfaceFlags)
	if ( !tr.allsolid && ( tr.fraction != 1.0f ) )
	{
		float normalLength = VectorLength( tr.plane.normal );
		if ( normalLength < 0.999999f || normalLength > 1.000001f )
		{
			return "plane normal has wrong length";
		}
	}

	return "";
}

static float CM_DistanceToBrush( const vec3_t loc, const cbrush_t *brush )
{
	float        dist = -999999.0f;
	float        d1;

	if ( !brush->numsides )
	{
		return 999999.0f;
	}

	const cbrushside_t *firstSide = brush->sides;
	const cbrushside_t *endSide = firstSide + brush->numsides;
	for ( const cbrushside_t *side = firstSide; side < endSide; side++ )
	{
		const cplane_t *plane = side->plane;

		d1 = DotProduct( loc, plane->normal ) - plane->dist;

		// get maximum plane distance
		if ( d1 > dist )
		{
			dist = d1;
		}
	}

	// FIXME: if outside brush, check distance to corners and edges

	return dist;
}

float CM_DistanceToModel( const vec3_t loc, clipHandle_t model ) {
	cmodel_t    *cmod;
	float      dist = 999999.0f;
	float      d1;

	cmod = CM_ClipHandleToModel( model );

	// test box position against all brushes in the leaf
	const cLeaf_t *leaf = &cmod->leaf;
	const int *firstBrushNum = leaf->firstLeafBrush;
	const int *endBrushNum = firstBrushNum + leaf->numLeafBrushes;
	for ( const int *brushNum = firstBrushNum; brushNum < endBrushNum; brushNum++ )
	{
		const cbrush_t *b = &cm.brushes[ *brushNum ];

		d1 = CM_DistanceToBrush( loc, b );
		if( d1 < dist )
			dist = d1;
	}

	return dist;
}
