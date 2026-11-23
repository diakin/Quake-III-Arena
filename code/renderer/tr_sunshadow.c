/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_sunshadow.c -- WALKER: sun shadows of the buildings and props

/*

The map generator writes the convex shapes of the buildings and props to
maps/<map>.casters (walker/tools/make_props.py, write_casters); q3map does not
let them block the sun (q3map_nosunshadow), so their sun shadows are drawn
here as stencil shadow volumes, into the same stencil buffer as the robot
shadows, and RB_ShadowFinish darkens all of them at once.

Each volume is the caster swept away from the sun: the near cap is the side
of the caster that faces away from the sun (moved a little further, so the
caster does not shadow itself), the far cap is the same side moved to where
the sweep leaves the world bounds, and the sides are the silhouette edges
swept. The volumes are counted with the depth-fail method, which stays right
when the camera is inside a shadow. They are rebuilt only when the sun
direction or r_sunShadows changes; every frame the volumes outside the view
frustum are skipped.

A surface that faces away from the sun gets no direct sunlight in the baked
lighting already; a volume over it would darken it a second time. After the
volumes are counted, the visible world triangles that face away from the sun
are drawn again (depth equal) and clear the stencil where they are in front.

r_sunShadows: 0 off, 1 simple (fewer, coarser shapes), 2 full.

*/

#include "tr_local.h"

#define	CASTER_BOTH		0
#define	CASTER_SIMPLE	1
#define	CASTER_FULL		2

#define	SELF_SHADOW_GAP	1.0f

#ifndef GL_INCR_WRAP_EXT
#define	GL_INCR_WRAP_EXT	0x8507
#define	GL_DECR_WRAP_EXT	0x8508
#endif		// the near cap is moved this far from the caster

typedef struct {
	int		level;
	int		firstVert, numVerts;
	int		firstTri, numTris;
	// the volume, filled by R_BuildSunVolumes
	int		firstVolumeIndex, numVolumeIndexes;
	vec3_t	volumeBounds[2];
} sunCaster_t;

static int			numCasters;
static sunCaster_t	*casters;
static vec3_t		*casterVerts;
static int			(*casterTris)[3];
static int			(*casterNeighbours)[3];	// triangle across each edge, -1 if none
static vec3_t		*casterNormals;

static int			maxVolumeVerts;
static vec3_t		*volumeVerts;
static glIndex_t	*volumeIndexes;
static glIndex_t	*frameIndexes;

static int			maxAwayVerts;
static vec3_t		*awayVerts;			// world triangles facing away from the sun, this view

static vec3_t		builtSunDirection;
static int			builtLevel = -1;
static qboolean		stencilWrap;

/*
===============
R_FindNeighbours
===============
*/
static void R_FindNeighbours( sunCaster_t *caster ) {
	int		tri, other, edge, otherEdge;
	int		*corners, *otherCorners;

	for ( tri = 0 ; tri < caster->numTris ; tri++ ) {
		corners = casterTris[caster->firstTri + tri];
		for ( edge = 0 ; edge < 3 ; edge++ ) {
			casterNeighbours[caster->firstTri + tri][edge] = -1;
			for ( other = 0 ; other < caster->numTris ; other++ ) {
				if ( other == tri ) {
					continue;
				}
				otherCorners = casterTris[caster->firstTri + other];
				for ( otherEdge = 0 ; otherEdge < 3 ; otherEdge++ ) {
					// a shared edge runs the other way in the neighbour
					if ( otherCorners[otherEdge] == corners[( edge + 1 ) % 3]
						&& otherCorners[( otherEdge + 1 ) % 3] == corners[edge] ) {
						casterNeighbours[caster->firstTri + tri][edge] = other;
					}
				}
			}
		}
	}
}

/*
===============
R_LoadSunCasters

Called after the world is loaded; the data lives on the level hunk.
===============
*/
void R_LoadSunCasters( const char *bspName ) {
	char		name[MAX_QPATH];
	char		*buffer, *cursor, *token;
	int			pass, count, verts, tris, index, corner, axis;
	int			totalVerts, totalTris, totalVolumeVerts;
	sunCaster_t	*caster;
	float		*a, *b, *c;
	vec3_t		edge1, edge2;

	numCasters = 0;
	builtLevel = -1;
	COM_StripExtension( bspName, name );
	Q_strcat( name, sizeof( name ), ".casters" );
	if ( ri.FS_ReadFile( name, (void **)&buffer ) <= 0 ) {
		return;
	}

	stencilWrap = strstr( glConfig.extensions_string, "GL_EXT_stencil_wrap" ) != NULL;

	// first pass counts, second pass fills
	totalVerts = totalTris = count = 0;
	for ( pass = 0 ; pass < 2 ; pass++ ) {
		cursor = buffer;
		count = totalVerts = totalTris = 0;
		while ( 1 ) {
			token = COM_Parse( &cursor );
			if ( !token[0] ) {
				break;
			}
			caster = pass ? &casters[count] : NULL;
			if ( caster ) {
				caster->level = atoi( token );
			}
			verts = atoi( COM_Parse( &cursor ) );
			tris = atoi( COM_Parse( &cursor ) );
			if ( verts < 4 || tris < 4 ) {
				ri.Printf( PRINT_WARNING, "WARNING: bad caster in %s\n", name );
				ri.FS_FreeFile( buffer );
				numCasters = 0;
				return;
			}
			if ( caster ) {
				caster->firstVert = totalVerts;
				caster->numVerts = verts;
				caster->firstTri = totalTris;
				caster->numTris = tris;
			}
			for ( index = 0 ; index < verts ; index++ ) {
				for ( axis = 0 ; axis < 3 ; axis++ ) {
					token = COM_Parse( &cursor );
					if ( caster ) {
						casterVerts[totalVerts + index][axis] = atof( token );
					}
				}
			}
			for ( index = 0 ; index < tris ; index++ ) {
				for ( corner = 0 ; corner < 3 ; corner++ ) {
					token = COM_Parse( &cursor );
					if ( caster ) {
						casterTris[totalTris + index][corner] = atoi( token );
					}
				}
			}
			totalVerts += verts;
			totalTris += tris;
			count++;
		}
		if ( !pass ) {
			casters = ri.Hunk_Alloc( count * sizeof( *casters ), h_low );
			casterVerts = ri.Hunk_Alloc( totalVerts * sizeof( *casterVerts ), h_low );
			casterTris = ri.Hunk_Alloc( totalTris * sizeof( *casterTris ), h_low );
			casterNeighbours = ri.Hunk_Alloc( totalTris * sizeof( *casterNeighbours ), h_low );
			casterNormals = ri.Hunk_Alloc( totalTris * sizeof( *casterNormals ), h_low );
		}
	}
	ri.FS_FreeFile( buffer );
	numCasters = count;

	// triangle normals, neighbours; worst case volume: both caps and every edge
	totalVolumeVerts = 0;
	for ( index = 0 ; index < numCasters ; index++ ) {
		caster = &casters[index];
		for ( tris = 0 ; tris < caster->numTris ; tris++ ) {
			a = casterVerts[caster->firstVert + casterTris[caster->firstTri + tris][0]];
			b = casterVerts[caster->firstVert + casterTris[caster->firstTri + tris][1]];
			c = casterVerts[caster->firstVert + casterTris[caster->firstTri + tris][2]];
			VectorSubtract( b, a, edge1 );
			VectorSubtract( c, a, edge2 );
			CrossProduct( edge1, edge2, casterNormals[caster->firstTri + tris] );
			VectorNormalize( casterNormals[caster->firstTri + tris] );
		}
		R_FindNeighbours( caster );
		totalVolumeVerts += caster->numTris * 3 * 2 + caster->numTris * 3 * 6;
	}
	maxVolumeVerts = totalVolumeVerts;
	volumeVerts = ri.Hunk_Alloc( maxVolumeVerts * sizeof( *volumeVerts ), h_low );
	volumeIndexes = ri.Hunk_Alloc( maxVolumeVerts * sizeof( *volumeIndexes ), h_low );
	frameIndexes = ri.Hunk_Alloc( maxVolumeVerts * sizeof( *frameIndexes ), h_low );

	// room for every world triangle (faces and models)
	maxAwayVerts = 0;
	for ( index = 0 ; index < tr.world->numsurfaces ; index++ ) {
		surfaceType_t	*surface = tr.world->surfaces[index].data;
		if ( *surface == SF_FACE ) {
			maxAwayVerts += ( (srfSurfaceFace_t *)surface )->numIndices;
		} else if ( *surface == SF_TRIANGLES ) {
			maxAwayVerts += ( (srfTriangles_t *)surface )->numIndexes;
		}
	}
	awayVerts = ri.Hunk_Alloc( ( maxAwayVerts + 1 ) * sizeof( *awayVerts ), h_low );

	ri.Printf( PRINT_ALL, "%i sun shadow casters, %i triangles\n", numCasters, totalTris );
}

/*
===============
R_SweepLength

How far the caster can be swept along the direction before a vertex leaves
the world bounds (the far cap must stay inside the far clip plane).
===============
*/
static float R_SweepLength( sunCaster_t *caster, const vec3_t sweep ) {
	vec3_t	*bounds = tr.world->bmodels[0].bounds;
	float	length, limit;
	int		index, axis;
	float	*point;

	length = 1e9f;
	for ( index = 0 ; index < caster->numVerts ; index++ ) {
		point = casterVerts[caster->firstVert + index];
		for ( axis = 0 ; axis < 3 ; axis++ ) {
			if ( sweep[axis] > 0.0001f ) {
				limit = ( bounds[1][axis] - 2 - point[axis] ) / sweep[axis];
			} else if ( sweep[axis] < -0.0001f ) {
				limit = ( bounds[0][axis] + 2 - point[axis] ) / sweep[axis];
			} else {
				continue;
			}
			if ( limit < length ) {
				length = limit;
			}
		}
	}
	return length;
}

/*
===============
R_BuildSunVolumes
===============
*/
static void R_BuildSunVolumes( int level ) {
	int			index, tri, edge, corner, numVerts;
	sunCaster_t	*caster;
	vec3_t		sweep, nearOffset, farOffset;
	float		length, *a, *b;
	int			*corners;
	vec3_t		quad[4];

	VectorCopy( tr.sunDirection, builtSunDirection );
	builtLevel = level;
	VectorScale( tr.sunDirection, -1, sweep );
	VectorScale( sweep, SELF_SHADOW_GAP, nearOffset );

	numVerts = 0;
	for ( index = 0 ; index < numCasters ; index++ ) {
		caster = &casters[index];
		caster->numVolumeIndexes = 0;
		if ( caster->level == ( level == 1 ? CASTER_FULL : CASTER_SIMPLE ) ) {
			continue;
		}
		length = R_SweepLength( caster, sweep );
		if ( length < 4 ) {
			continue;
		}
		VectorScale( sweep, length, farOffset );
		caster->firstVolumeIndex = numVerts;
		ClearBounds( caster->volumeBounds[0], caster->volumeBounds[1] );

#define	EMIT( point ) \
		VectorCopy( point, volumeVerts[numVerts] ); \
		AddPointToBounds( point, caster->volumeBounds[0], caster->volumeBounds[1] ); \
		volumeIndexes[numVerts] = numVerts; \
		numVerts++;

		for ( tri = 0 ; tri < caster->numTris ; tri++ ) {
			if ( DotProduct( casterNormals[caster->firstTri + tri], tr.sunDirection ) > 0 ) {
				continue;		// lit side
			}
			corners = casterTris[caster->firstTri + tri];

			// near cap, facing into the caster: reversed winding
			for ( corner = 2 ; corner >= 0 ; corner-- ) {
				VectorAdd( casterVerts[caster->firstVert + corners[corner]], nearOffset, quad[0] );
				EMIT( quad[0] );
			}
			// far cap
			for ( corner = 0 ; corner < 3 ; corner++ ) {
				VectorAdd( casterVerts[caster->firstVert + corners[corner]], farOffset, quad[0] );
				EMIT( quad[0] );
			}
			// sides on the silhouette edges (the neighbour faces the sun)
			for ( edge = 0 ; edge < 3 ; edge++ ) {
				int neighbour = casterNeighbours[caster->firstTri + tri][edge];
				if ( neighbour >= 0
					&& DotProduct( casterNormals[caster->firstTri + neighbour], tr.sunDirection ) <= 0 ) {
					continue;
				}
				a = casterVerts[caster->firstVert + corners[edge]];
				b = casterVerts[caster->firstVert + corners[( edge + 1 ) % 3]];
				VectorAdd( a, nearOffset, quad[0] );
				VectorAdd( b, nearOffset, quad[1] );
				VectorAdd( b, farOffset, quad[2] );
				VectorAdd( a, farOffset, quad[3] );
				EMIT( quad[0] );
				EMIT( quad[1] );
				EMIT( quad[2] );
				EMIT( quad[0] );
				EMIT( quad[2] );
				EMIT( quad[3] );
			}
		}
#undef EMIT
		caster->numVolumeIndexes = numVerts - caster->firstVolumeIndex;
	}
	if ( numVerts > maxVolumeVerts ) {
		ri.Error( ERR_DROP, "R_BuildSunVolumes: overflow" );
	}
}

/*
===============
R_SunShadowsActive
===============
*/
qboolean R_SunShadowsActive( void ) {
	return numCasters > 0 && r_sunShadows->integer > 0 && glConfig.stencilBits >= 4
		&& VectorLength( tr.sunLight ) > 0;
}

/*
===============
RB_SunShadowVolumes

Draws the visible volumes into the stencil buffer; returns qtrue if any.
===============
*/
/*
===============
RB_CollectAwayTriangles

The opaque world triangles of this view that face away from the sun.
===============
*/
static int RB_CollectAwayTriangles( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	int				index, tri, corner, entityNum, fogNum, dlighted, numVerts;
	shader_t		*shader;
	surfaceType_t	*surface;
	srfSurfaceFace_t	*face;
	srfTriangles_t	*triangles;
	int				*indexes;
	float			*a, *b, *c;
	vec3_t			edge1, edge2, normal;

	numVerts = 0;
	for ( index = 0 ; index < numDrawSurfs ; index++ ) {
		R_DecomposeSort( drawSurfs[index].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( entityNum != ENTITYNUM_WORLD || shader->sort > SS_OPAQUE ) {
			continue;
		}
		surface = drawSurfs[index].surface;
		if ( *surface == SF_FACE ) {
			face = (srfSurfaceFace_t *)surface;
			if ( DotProduct( face->plane.normal, tr.sunDirection ) > 0
				|| numVerts + face->numIndices > maxAwayVerts ) {
				continue;
			}
			indexes = (int *)( (byte *)face + face->ofsIndices );
			for ( corner = 0 ; corner < face->numIndices ; corner++ ) {
				VectorCopy( face->points[indexes[corner]], awayVerts[numVerts] );
				numVerts++;
			}
		} else if ( *surface == SF_TRIANGLES ) {
			triangles = (srfTriangles_t *)surface;
			// the away-facing triangles are found once and kept until the sun moves
			if ( !triangles->awayIndexes || !VectorCompare( triangles->awaySunDirection, tr.sunDirection ) ) {
				if ( !triangles->awayIndexes ) {
					triangles->awayIndexes = ri.Hunk_Alloc( triangles->numIndexes * sizeof( int ), h_low );
				}
				triangles->numAwayIndexes = 0;
				for ( tri = 0 ; tri < triangles->numIndexes ; tri += 3 ) {
					a = triangles->verts[triangles->indexes[tri]].xyz;
					b = triangles->verts[triangles->indexes[tri + 1]].xyz;
					c = triangles->verts[triangles->indexes[tri + 2]].xyz;
					VectorSubtract( b, a, edge1 );
					VectorSubtract( c, a, edge2 );
					CrossProduct( edge1, edge2, normal );
					// Quake winding is clockwise: this normal points into the model
					if ( DotProduct( normal, tr.sunDirection ) < 0 ) {
						continue;
					}
					triangles->awayIndexes[triangles->numAwayIndexes++] = triangles->indexes[tri];
					triangles->awayIndexes[triangles->numAwayIndexes++] = triangles->indexes[tri + 1];
					triangles->awayIndexes[triangles->numAwayIndexes++] = triangles->indexes[tri + 2];
				}
				VectorCopy( tr.sunDirection, triangles->awaySunDirection );
			}
			if ( numVerts + triangles->numAwayIndexes > maxAwayVerts ) {
				continue;
			}
			// models in two detail levels are drawn moved (deformVertexes lodshift): the
			// same move here, or the depth test of this pass misses them
			if ( shader->numDeforms && shader->deforms[0].deformation == DEFORM_LODSHIFT ) {
				for ( tri = 0 ; tri < triangles->numAwayIndexes ; tri++ ) {
					R_LodShiftPoint( &shader->deforms[0], backEnd.viewParms.or.origin,
						triangles->verts[triangles->awayIndexes[tri]].xyz, awayVerts[numVerts] );
					numVerts++;
				}
				continue;
			}
			for ( tri = 0 ; tri < triangles->numAwayIndexes ; tri++ ) {
				VectorCopy( triangles->verts[triangles->awayIndexes[tri]].xyz, awayVerts[numVerts] );
				numVerts++;
			}
		}
	}
	return numVerts;
}

qboolean RB_SunShadowVolumes( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	int			index, level, count, numIndexes, numAway;
	sunCaster_t	*caster;
	int			plane;

	if ( !R_SunShadowsActive() || backEnd.viewParms.isPortal
		|| ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return qfalse;
	}
	level = r_sunShadows->integer >= 2 ? 2 : 1;
	if ( level != builtLevel || !VectorCompare( builtSunDirection, tr.sunDirection ) ) {
		R_BuildSunVolumes( level );
	}

	// volumes inside the view frustum
	numIndexes = 0;
	for ( index = 0 ; index < numCasters ; index++ ) {
		caster = &casters[index];
		if ( !caster->numVolumeIndexes ) {
			continue;
		}
		for ( plane = 0 ; plane < 4 ; plane++ ) {
			if ( BoxOnPlaneSide( caster->volumeBounds[0], caster->volumeBounds[1],
				&backEnd.viewParms.frustum[plane] ) == 2 ) {
				break;		// entirely behind this frustum plane
			}
		}
		if ( plane < 4 ) {
			continue;
		}
		count = caster->numVolumeIndexes;
		Com_Memcpy( frameIndexes + numIndexes, volumeIndexes + caster->firstVolumeIndex, count * sizeof( glIndex_t ) );
		numIndexes += count;
	}
	if ( !numIndexes ) {
		return qfalse;
	}

	qglLoadMatrixf( backEnd.viewParms.world.modelMatrix );
	GL_Bind( tr.whiteImage );
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );		// depth test, no depth write
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	qglDisable( GL_FOG );
	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_ALWAYS, 0, 255 );
	qglEnable( GL_CULL_FACE );

	// only the vertex array may stay on: the arrays of the last shader are much shorter
	// than the volumes, and the driver would read far past their end. The original
	// DrawMultitextured leaves the texture coordinate array of the second unit on.
	if ( qglActiveTextureARB ) {
		GL_SelectTexture( 1 );
		qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
		qglDisable( GL_TEXTURE_2D );
		GL_SelectTexture( 0 );
	}
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisableClientState( GL_COLOR_ARRAY );
	qglVertexPointer( 3, GL_FLOAT, 0, volumeVerts );

	// depth fail: the back faces behind the scene count up, then the front faces
	// behind it count down (all increments first: no underflow without wrapping)
	qglCullFace( GL_FRONT );
	qglStencilOp( GL_KEEP, stencilWrap ? GL_INCR_WRAP_EXT : GL_INCR, GL_KEEP );
	qglDrawElements( GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, frameIndexes );
	qglCullFace( GL_BACK );
	qglStencilOp( GL_KEEP, stencilWrap ? GL_DECR_WRAP_EXT : GL_DECR, GL_KEEP );
	qglDrawElements( GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, frameIndexes );

	// no shadow on what faces away from the sun (it has no direct sunlight anyway)
	numAway = RB_CollectAwayTriangles( drawSurfs, numDrawSurfs );
	if ( numAway ) {
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_EQUAL );
		qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		qglDisable( GL_CULL_FACE );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_ZERO );
		qglVertexPointer( 3, GL_FLOAT, 0, awayVerts );
		qglDrawArrays( GL_TRIANGLES, 0, numAway );
	}

	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglDisable( GL_STENCIL_TEST );
	glState.faceCulling = -1;		// the next GL_Cull sets the culling again
	return qtrue;
}
