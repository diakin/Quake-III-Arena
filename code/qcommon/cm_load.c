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
// cmodel.c -- model loading

#include "cm_local.h"

#ifdef BSPC

#include "../bspc/l_qfiles.h"

void SetPlaneSignbits (cplane_t *out) {
	int	bits, j;

	// for fast box on planeside test
	bits = 0;
	for (j=0 ; j<3 ; j++) {
		if (out->normal[j] < 0) {
			bits |= 1<<j;
		}
	}
	out->signbits = bits;
}
#endif //BSPC

// to allow boxes to be treated as brush models, we allocate
// some extra indexes along with those needed by the map
#define	BOX_BRUSHES		1
#define	BOX_SIDES		6
#define	BOX_LEAFS		2
#define	BOX_PLANES		12

#define	LL(x) x=LittleLong(x)


clipMap_t	cm;
int			c_pointcontents;
int			c_traces, c_brush_traces, c_patch_traces;


byte		*cmod_base;

#ifndef BSPC
cvar_t		*cm_noAreas;
cvar_t		*cm_noCurves;
cvar_t		*cm_playerCurveClip;
#endif

cmodel_t	box_model;
cplane_t	*box_planes;
cbrush_t	*box_brush;



void	CM_InitBoxHull (void);
void	CM_SplitLeafs( void );
void	CM_FloodAreaConnections (void);


/*
===============================================================================

					MAP LOADING

===============================================================================
*/

/*
=================
CMod_LoadShaders
=================
*/
void CMod_LoadShaders( lump_t *l ) {
	dshader_t	*in, *out;
	int			i, count;

	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in)) {
		Com_Error (ERR_DROP, "CMod_LoadShaders: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	if (count < 1) {
		Com_Error (ERR_DROP, "Map with no shaders");
	}
	cm.shaders = Hunk_Alloc( count * sizeof( *cm.shaders ), h_high );
	cm.numShaders = count;

	Com_Memcpy( cm.shaders, in, count * sizeof( *cm.shaders ) );

	out = cm.shaders;
	for ( i=0 ; i<count ; i++, in++, out++ ) {
		out->contentFlags = LittleLong( out->contentFlags );
		out->surfaceFlags = LittleLong( out->surfaceFlags );
	}
}


/*
=================
CMod_LoadSubmodels
=================
*/
void CMod_LoadSubmodels( lump_t *l ) {
	dmodel_t	*in;
	cmodel_t	*out;
	int			i, j, count;
	int			*indexes;

	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadSubmodels: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no models");
	cm.cmodels = Hunk_Alloc( count * sizeof( *cm.cmodels ), h_high );
	cm.numSubModels = count;

	if ( count > MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "MAX_SUBMODELS exceeded" );
	}

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out = &cm.cmodels[i];

		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
		}

		if ( i == 0 ) {
			continue;	// world model doesn't need other info
		}

		// make a "leaf" just to hold the model's brushes and surfaces
		out->leaf.numLeafBrushes = LittleLong( in->numBrushes );
		indexes = Hunk_Alloc( out->leaf.numLeafBrushes * 4, h_high );
		out->leaf.firstLeafBrush = indexes - cm.leafbrushes;
		for ( j = 0 ; j < out->leaf.numLeafBrushes ; j++ ) {
			indexes[j] = LittleLong( in->firstBrush ) + j;
		}

		out->leaf.numLeafSurfaces = LittleLong( in->numSurfaces );
		indexes = Hunk_Alloc( out->leaf.numLeafSurfaces * 4, h_high );
		out->leaf.firstLeafSurface = indexes - cm.leafsurfaces;
		for ( j = 0 ; j < out->leaf.numLeafSurfaces ; j++ ) {
			indexes[j] = LittleLong( in->firstSurface ) + j;
		}
	}
}


/*
=================
CMod_LoadNodes

=================
*/
void CMod_LoadNodes( lump_t *l ) {
	dnode_t		*in;
	int			child;
	cNode_t		*out;
	int			i, j, count;
	
	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map has no nodes");
	cm.nodes = Hunk_Alloc( count * sizeof( *cm.nodes ), h_high );
	cm.numNodes = count;

	out = cm.nodes;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->plane = cm.planes + LittleLong( in->planeNum );
		for (j=0 ; j<2 ; j++)
		{
			child = LittleLong (in->children[j]);
			out->children[j] = child;
		}
	}

}

/*
=================
CM_BoundBrush

=================
*/
void CM_BoundBrush( cbrush_t *b ) {
	b->bounds[0][0] = -b->sides[0].plane->dist;
	b->bounds[1][0] = b->sides[1].plane->dist;

	b->bounds[0][1] = -b->sides[2].plane->dist;
	b->bounds[1][1] = b->sides[3].plane->dist;

	b->bounds[0][2] = -b->sides[4].plane->dist;
	b->bounds[1][2] = b->sides[5].plane->dist;
}


/*
=================
CMod_LoadBrushes

=================
*/
void CMod_LoadBrushes( lump_t *l ) {
	dbrush_t	*in;
	cbrush_t	*out;
	int			i, count;

	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in)) {
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	cm.brushes = Hunk_Alloc( ( BOX_BRUSHES + count ) * sizeof( *cm.brushes ), h_high );
	cm.numBrushes = count;

	out = cm.brushes;

	for ( i=0 ; i<count ; i++, out++, in++ ) {
		out->sides = cm.brushsides + LittleLong(in->firstSide);
		out->numsides = LittleLong(in->numSides);

		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushes: bad shaderNum: %i", out->shaderNum );
		}
		out->contents = cm.shaders[out->shaderNum].contentFlags;

		CM_BoundBrush( out );
	}

}

/*
=================
CMod_LoadLeafs
=================
*/
void CMod_LoadLeafs (lump_t *l)
{
	int			i;
	cLeaf_t		*out;
	dleaf_t 	*in;
	int			count;
	
	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no leafs");

	cm.leafs = Hunk_Alloc( ( BOX_LEAFS + count ) * sizeof( *cm.leafs ), h_high );
	cm.numLeafs = count;

	out = cm.leafs;	
	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->cluster = LittleLong (in->cluster);
		out->area = LittleLong (in->area);
		out->firstLeafBrush = LittleLong (in->firstLeafBrush);
		out->numLeafBrushes = LittleLong (in->numLeafBrushes);
		out->firstLeafSurface = LittleLong (in->firstLeafSurface);
		out->numLeafSurfaces = LittleLong (in->numLeafSurfaces);

		if (out->cluster >= cm.numClusters)
			cm.numClusters = out->cluster + 1;
		if (out->area >= cm.numAreas)
			cm.numAreas = out->area + 1;
	}

	cm.areas = Hunk_Alloc( cm.numAreas * sizeof( *cm.areas ), h_high );
	cm.areaPortals = Hunk_Alloc( cm.numAreas * cm.numAreas * sizeof( *cm.areaPortals ), h_high );
}

/*
=================
CMod_LoadPlanes
=================
*/
void CMod_LoadPlanes (lump_t *l)
{
	int			i, j;
	cplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;
	
	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no planes");
	cm.planes = Hunk_Alloc( ( BOX_PLANES + count ) * sizeof( *cm.planes ), h_high );
	cm.numPlanes = count;

	out = cm.planes;	

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
void CMod_LoadLeafBrushes (lump_t *l)
{
	int			i;
	int			*out;
	int		 	*in;
	int			count;
	
	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	count = l->filelen / sizeof(*in);

	cm.leafbrushes = Hunk_Alloc( (count + BOX_BRUSHES) * sizeof( *cm.leafbrushes ), h_high );
	cm.numLeafBrushes = count;

	out = cm.leafbrushes;

	for ( i=0 ; i<count ; i++, in++, out++) {
		*out = LittleLong (*in);
	}
}

/*
=================
CMod_LoadLeafSurfaces
=================
*/
void CMod_LoadLeafSurfaces( lump_t *l )
{
	int			i;
	int			*out;
	int		 	*in;
	int			count;
	
	in = (void *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	count = l->filelen / sizeof(*in);

	cm.leafsurfaces = Hunk_Alloc( count * sizeof( *cm.leafsurfaces ), h_high );
	cm.numLeafSurfaces = count;

	out = cm.leafsurfaces;

	for ( i=0 ; i<count ; i++, in++, out++) {
		*out = LittleLong (*in);
	}
}

/*
=================
CMod_LoadBrushSides
=================
*/
void CMod_LoadBrushSides (lump_t *l)
{
	int				i;
	cbrushside_t	*out;
	dbrushside_t 	*in;
	int				count;
	int				num;

	in = (void *)(cmod_base + l->fileofs);
	if ( l->filelen % sizeof(*in) ) {
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	cm.brushsides = Hunk_Alloc( ( BOX_SIDES + count ) * sizeof( *cm.brushsides ), h_high );
	cm.numBrushSides = count;

	out = cm.brushsides;	

	for ( i=0 ; i<count ; i++, in++, out++) {
		num = LittleLong( in->planeNum );
		out->plane = &cm.planes[num];
		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushSides: bad shaderNum: %i", out->shaderNum );
		}
		out->surfaceFlags = cm.shaders[out->shaderNum].surfaceFlags;
	}
}


/*
=================
CMod_LoadEntityString
=================
*/
void CMod_LoadEntityString( lump_t *l ) {
	cm.entityString = Hunk_Alloc( l->filelen, h_high );
	cm.numEntityChars = l->filelen;
	Com_Memcpy (cm.entityString, cmod_base + l->fileofs, l->filelen);
}

/*
=================
CMod_LoadVisibility
=================
*/
#define	VIS_HEADER	8
void CMod_LoadVisibility( lump_t *l ) {
	int		len;
	byte	*buf;

    len = l->filelen;
	if ( !len ) {
		cm.clusterBytes = ( cm.numClusters + 31 ) & ~31;
		cm.visibility = Hunk_Alloc( cm.clusterBytes, h_high );
		Com_Memset( cm.visibility, 255, cm.clusterBytes );
		return;
	}
	buf = cmod_base + l->fileofs;

	cm.vised = qtrue;
	cm.visibility = Hunk_Alloc( len, h_high );
	cm.numClusters = LittleLong( ((int *)buf)[0] );
	cm.clusterBytes = LittleLong( ((int *)buf)[1] );
	Com_Memcpy (cm.visibility, buf + VIS_HEADER, len - VIS_HEADER );
}

//==================================================================


/*
=================
CMod_LoadPatches
=================
*/
#define	MAX_PATCH_VERTS		1024
void CMod_LoadPatches( lump_t *surfs, lump_t *verts ) {
	drawVert_t	*dv, *dv_p;
	dsurface_t	*in;
	int			count;
	int			i, j;
	int			c;
	cPatch_t	*patch;
	vec3_t		points[MAX_PATCH_VERTS];
	int			width, height;
	int			shaderNum;

	in = (void *)(cmod_base + surfs->fileofs);
	if (surfs->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	cm.numSurfaces = count = surfs->filelen / sizeof(*in);
	cm.surfaces = Hunk_Alloc( cm.numSurfaces * sizeof( cm.surfaces[0] ), h_high );

	dv = (void *)(cmod_base + verts->fileofs);
	if (verts->filelen % sizeof(*dv))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");

	// scan through all the surfaces, but only load patches,
	// not planar faces
	for ( i = 0 ; i < count ; i++, in++ ) {
		if ( LittleLong( in->surfaceType ) != MST_PATCH ) {
			continue;		// ignore other surfaces
		}
		// FIXME: check for non-colliding patches

		cm.surfaces[ i ] = patch = Hunk_Alloc( sizeof( *patch ), h_high );

		// load the full drawverts onto the stack
		width = LittleLong( in->patchWidth );
		height = LittleLong( in->patchHeight );
		c = width * height;
		if ( c > MAX_PATCH_VERTS ) {
			Com_Error( ERR_DROP, "ParseMesh: MAX_PATCH_VERTS" );
		}

		dv_p = dv + LittleLong( in->firstVert );
		for ( j = 0 ; j < c ; j++, dv_p++ ) {
			points[j][0] = LittleFloat( dv_p->xyz[0] );
			points[j][1] = LittleFloat( dv_p->xyz[1] );
			points[j][2] = LittleFloat( dv_p->xyz[2] );
		}

		shaderNum = LittleLong( in->shaderNum );
		patch->contents = cm.shaders[shaderNum].contentFlags;
		patch->surfaceFlags = cm.shaders[shaderNum].surfaceFlags;

		// create the internal facet structure
		patch->pc = CM_GeneratePatchCollide( width, height, points );
	}
}

//==================================================================

unsigned CM_LumpChecksum(lump_t *lump) {
	return LittleLong (Com_BlockChecksum (cmod_base + lump->fileofs, lump->filelen));
}

unsigned CM_Checksum(dheader_t *header) {
	unsigned checksums[16];
	checksums[0] = CM_LumpChecksum(&header->lumps[LUMP_SHADERS]);
	checksums[1] = CM_LumpChecksum(&header->lumps[LUMP_LEAFS]);
	checksums[2] = CM_LumpChecksum(&header->lumps[LUMP_LEAFBRUSHES]);
	checksums[3] = CM_LumpChecksum(&header->lumps[LUMP_LEAFSURFACES]);
	checksums[4] = CM_LumpChecksum(&header->lumps[LUMP_PLANES]);
	checksums[5] = CM_LumpChecksum(&header->lumps[LUMP_BRUSHSIDES]);
	checksums[6] = CM_LumpChecksum(&header->lumps[LUMP_BRUSHES]);
	checksums[7] = CM_LumpChecksum(&header->lumps[LUMP_MODELS]);
	checksums[8] = CM_LumpChecksum(&header->lumps[LUMP_NODES]);
	checksums[9] = CM_LumpChecksum(&header->lumps[LUMP_SURFACES]);
	checksums[10] = CM_LumpChecksum(&header->lumps[LUMP_DRAWVERTS]);

	return LittleLong(Com_BlockChecksum(checksums, 11 * 4));
}

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
void CM_LoadMap( const char *name, qboolean clientload, int *checksum ) {
	int				*buf;
	int				i;
	dheader_t		header;
	int				length;
	static unsigned	last_checksum;

	if ( !name || !name[0] ) {
		Com_Error( ERR_DROP, "CM_LoadMap: NULL name" );
	}

#ifndef BSPC
	cm_noAreas = Cvar_Get ("cm_noAreas", "0", CVAR_CHEAT);
	cm_noCurves = Cvar_Get ("cm_noCurves", "0", CVAR_CHEAT);
	cm_playerCurveClip = Cvar_Get ("cm_playerCurveClip", "1", CVAR_ARCHIVE|CVAR_CHEAT );
#endif
	Com_DPrintf( "CM_LoadMap( %s, %i )\n", name, clientload );

	if ( !strcmp( cm.name, name ) && clientload ) {
		*checksum = last_checksum;
		return;
	}

	// free old stuff
	Com_Memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();

	if ( !name[0] ) {
		cm.numLeafs = 1;
		cm.numClusters = 1;
		cm.numAreas = 1;
		cm.cmodels = Hunk_Alloc( sizeof( *cm.cmodels ), h_high );
		*checksum = 0;
		return;
	}

	//
	// load the file
	//
#ifndef BSPC
	length = FS_ReadFile( name, (void **)&buf );
#else
	length = LoadQuakeFile((quakefile_t *) name, (void **)&buf);
#endif

	if ( !buf ) {
		Com_Error (ERR_DROP, "Couldn't load %s", name);
	}

	last_checksum = LittleLong (Com_BlockChecksum (buf, length));
	*checksum = last_checksum;

	header = *(dheader_t *)buf;
	for (i=0 ; i<sizeof(dheader_t)/4 ; i++) {
		((int *)&header)[i] = LittleLong ( ((int *)&header)[i]);
	}

	if ( header.version != BSP_VERSION ) {
		Com_Error (ERR_DROP, "CM_LoadMap: %s has wrong version number (%i should be %i)"
		, name, header.version, BSP_VERSION );
	}

	cmod_base = (byte *)buf;

	// load into heap
	CMod_LoadShaders( &header.lumps[LUMP_SHADERS] );
	CMod_LoadLeafs (&header.lumps[LUMP_LEAFS]);
	CMod_LoadLeafBrushes (&header.lumps[LUMP_LEAFBRUSHES]);
	CMod_LoadLeafSurfaces (&header.lumps[LUMP_LEAFSURFACES]);
	CMod_LoadPlanes (&header.lumps[LUMP_PLANES]);
	CMod_LoadBrushSides (&header.lumps[LUMP_BRUSHSIDES]);
	CMod_LoadBrushes (&header.lumps[LUMP_BRUSHES]);
	CMod_LoadSubmodels (&header.lumps[LUMP_MODELS]);
	CMod_LoadNodes (&header.lumps[LUMP_NODES]);
	CMod_LoadEntityString (&header.lumps[LUMP_ENTITIES]);
	CMod_LoadVisibility( &header.lumps[LUMP_VISIBILITY] );
	CMod_LoadPatches( &header.lumps[LUMP_SURFACES], &header.lumps[LUMP_DRAWVERTS] );

	// we are NOT freeing the file, because it is cached for the ref
	FS_FreeFile (buf);

	CM_SplitLeafs ();		// WALKER: smaller collision leafs (before the box hull: it uses the array ends)

	CM_InitBoxHull ();

	CM_FloodAreaConnections ();

	// allow this to be cached if it is loaded by the server
	if ( !clientload ) {
		Q_strncpyz( cm.name, name, sizeof( cm.name ) );
	}
}

/*
==================
CM_ClearMap
==================
*/
void CM_ClearMap( void ) {
	Com_Memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();
}

/*
==================
CM_ClipHandleToModel
==================
*/
cmodel_t	*CM_ClipHandleToModel( clipHandle_t handle ) {
	if ( handle < 0 ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle );
	}
	if ( handle < cm.numSubModels ) {
		return &cm.cmodels[handle];
	}
	if ( handle == BOX_MODEL_HANDLE ) {
		return &box_model;
	}
	if ( handle < MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i < %i < %i", 
			cm.numSubModels, handle, MAX_SUBMODELS );
	}
	Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle + MAX_SUBMODELS );

	return NULL;

}

/*
==================
CM_InlineModel
==================
*/
clipHandle_t	CM_InlineModel( int index ) {
	if ( index < 0 || index >= cm.numSubModels ) {
		Com_Error (ERR_DROP, "CM_InlineModel: bad number");
	}
	return index;
}

int		CM_NumClusters( void ) {
	return cm.numClusters;
}

int		CM_NumInlineModels( void ) {
	return cm.numSubModels;
}

char	*CM_EntityString( void ) {
	return cm.entityString;
}

int		CM_LeafCluster( int leafnum ) {
	if (leafnum < 0 || leafnum >= cm.numLeafs) {
		Com_Error (ERR_DROP, "CM_LeafCluster: bad number");
	}
	return cm.leafs[leafnum].cluster;
}

int		CM_LeafArea( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cm.numLeafs ) {
		Com_Error (ERR_DROP, "CM_LeafArea: bad number");
	}
	return cm.leafs[leafnum].area;
}

//=======================================================================



/*
=================================================================================

WALKER: the leafs of the collision tree split into smaller cells (27.11.2026)

The maps are mostly detail brushes (terrain, rocks, props): they do not split the
BSP, and q3map stops at its 1024-unit blocks. wk_islands had 65 leafs for 17608
brushes, up to 1862 in one leaf, and every trace or point test through a leaf
tested all of them: a bot visibility trace cost 0.1 ms there, three times more than
on wk_bigbeacons. Here a leaf with more than CM_SPLIT_BRUSHES brushes is cut in
half across its longer horizontal side, again and again, down to CM_SPLIT_MIN units.
The new leafs keep the cluster and the area of the old one: visibility, areas and
sounds do not change; only the brushes a trace looks at.

=================================================================================
*/
#define CM_SPLIT_BRUSHES	32
#define CM_SPLIT_MIN		128.0f
#define CM_SPLIT_DEPTH		12

typedef struct {
	int			numNodes, maxNodes;
	cNode_t		*nodes;
	int			numLeafs, maxLeafs;
	cLeaf_t		*leafs;
	int			numLeafBrushes, maxLeafBrushes;
	int			*leafbrushes;
	int			numPlanes, maxPlanes;
	cplane_t	**planes;		// allocated one by one: the nodes point at them
} cmSplit_t;

static cmSplit_t	cmSplit;

// room for "needed" elements; "used" of them are kept
static void *CM_SplitGrow( void *array, int *maximum, int used, int needed, int size ) {
	void	*bigger;

	if ( needed <= *maximum ) {
		return array;
	}
	*maximum = needed * 2 > 64 ? needed * 2 : 64;
	bigger = Z_Malloc( *maximum * size );
	if ( array ) {
		if ( used > 0 ) {
			Com_Memcpy( bigger, array, used * size );
		}
		Z_Free( array );
	}
	return bigger;
}

// a node of the new tree, returned as its index; a leaf as -1 - index
static int CM_SplitRegion( const cLeaf_t *parent, const int *brushes, int count, vec3_t mins, vec3_t maxs, int depth ) {
	int		axis = ( maxs[0] - mins[0] ) >= ( maxs[1] - mins[1] ) ? 0 : 1;
	float	size = maxs[axis] - mins[axis];
	int		index, side, nodeIndex;
	float	middle;
	cplane_t	*plane;

	if ( count <= CM_SPLIT_BRUSHES || size < CM_SPLIT_MIN * 2 || depth >= CM_SPLIT_DEPTH ) {
		cLeaf_t	*leaf;
		int		leafIndex = cmSplit.numLeafs++;

		cmSplit.leafs = CM_SplitGrow( cmSplit.leafs, &cmSplit.maxLeafs, leafIndex, cmSplit.numLeafs, sizeof( cLeaf_t ) );
		cmSplit.leafbrushes = CM_SplitGrow( cmSplit.leafbrushes, &cmSplit.maxLeafBrushes, cmSplit.numLeafBrushes,
			cmSplit.numLeafBrushes + count, sizeof( int ) );
		leaf = &cmSplit.leafs[leafIndex];
		*leaf = *parent;
		leaf->firstLeafBrush = cmSplit.numLeafBrushes;		// into the new array, offset later
		leaf->numLeafBrushes = count;
		Com_Memcpy( cmSplit.leafbrushes + cmSplit.numLeafBrushes, brushes, count * sizeof( int ) );
		cmSplit.numLeafBrushes += count;
		return -1 - leafIndex;
	}

	// the cut on a 64-unit line near the middle
	middle = floor( ( mins[axis] + maxs[axis] ) * 0.5f / 64.0f + 0.5f ) * 64.0f;
	if ( middle <= mins[axis] + 1 || middle >= maxs[axis] - 1 ) {
		middle = ( mins[axis] + maxs[axis] ) * 0.5f;
	}
	plane = Hunk_Alloc( sizeof( *plane ), h_high );
	VectorClear( plane->normal );
	plane->normal[axis] = 1;
	plane->dist = middle;
	plane->type = axis;
	plane->signbits = 0;

	nodeIndex = cmSplit.numNodes++;
	cmSplit.nodes = CM_SplitGrow( cmSplit.nodes, &cmSplit.maxNodes, nodeIndex, cmSplit.numNodes, sizeof( cNode_t ) );
	cmSplit.nodes[nodeIndex].plane = plane;

	for ( side = 0; side < 2; side++ ) {
		int		*list = Z_Malloc( ( count > 0 ? count : 1 ) * sizeof( int ) );
		int		listCount = 0, child;
		vec3_t	childMins, childMaxs;

		VectorCopy( mins, childMins );
		VectorCopy( maxs, childMaxs );
		if ( side == 0 ) {
			childMins[axis] = middle;		// front: above the plane
		} else {
			childMaxs[axis] = middle;
		}
		for ( index = 0; index < count; index++ ) {
			cbrush_t	*brush = &cm.brushes[brushes[index]];
			// touching the cut counts for both sides
			if ( brush->bounds[0][axis] <= childMaxs[axis] + 1 && brush->bounds[1][axis] >= childMins[axis] - 1 ) {
				list[listCount++] = brushes[index];
			}
		}
		child = CM_SplitRegion( parent, list, listCount, childMins, childMaxs, depth + 1 );
		cmSplit.nodes[nodeIndex].children[side] = child;	// the array may have moved: index again
		Z_Free( list );
	}
	return nodeIndex;
}

// the bounds of every leaf of the original tree (axial planes narrow them)
static void CM_LeafBounds( int num, vec3_t mins, vec3_t maxs, vec3_t *leafMins, vec3_t *leafMaxs ) {
	while ( num >= 0 ) {
		cNode_t		*node = &cm.nodes[num];
		cplane_t	*plane = node->plane;

		if ( plane->type < 3 ) {
			vec3_t	frontMins, backMaxs;
			VectorCopy( mins, frontMins );
			VectorCopy( maxs, backMaxs );
			if ( plane->dist > frontMins[plane->type] ) {
				frontMins[plane->type] = plane->dist;
			}
			if ( plane->dist < backMaxs[plane->type] ) {
				backMaxs[plane->type] = plane->dist;
			}
			CM_LeafBounds( node->children[0], frontMins, maxs, leafMins, leafMaxs );
			CM_LeafBounds( node->children[1], mins, backMaxs, leafMins, leafMaxs );
			return;
		}
		CM_LeafBounds( node->children[0], mins, maxs, leafMins, leafMaxs );
		num = node->children[1];
	}
	VectorCopy( mins, leafMins[-1 - num] );
	VectorCopy( maxs, leafMaxs[-1 - num] );
}

/*
cm_splitCheck 1: random traces (points, boxes of a robot) and point tests over the
world, answered by the old tree and by the split one, the differences printed
*/
#define CM_CHECK_COUNT	4000
static trace_t	cmCheckTraces[CM_CHECK_COUNT];
static int		cmCheckContents[CM_CHECK_COUNT];

static void CM_SplitCheck( qboolean compare ) {
	vec3_t	mins, maxs, boxMins = { -30, -30, 0 }, boxMaxs = { 30, 30, 60 }, zero = { 0, 0, 0 };
	int		index, differ = 0, seed = 12345, hits = 0, solid = 0;

	VectorCopy( cm.cmodels[0].mins, mins );
	VectorCopy( cm.cmodels[0].maxs, maxs );
	for ( index = 0; index < CM_CHECK_COUNT; index++ ) {
		vec3_t	start, end;
		trace_t	trace;
		int		contents, axis;
		for ( axis = 0; axis < 3; axis++ ) {
			seed = seed * 1103515245 + 12345;
			start[axis] = mins[axis] + ( maxs[axis] - mins[axis] ) * ( ( seed >> 8 ) & 0xffff ) / 65535.0f;
			seed = seed * 1103515245 + 12345;
			end[axis] = start[axis] + ( ( ( seed >> 8 ) & 0xffff ) / 65535.0f - 0.5f ) * ( axis == 2 ? 800 : 3000 );
		}
		CM_BoxTrace( &trace, start, end, ( index & 1 ) ? boxMins : zero, ( index & 1 ) ? boxMaxs : zero, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
		contents = CM_PointContents( start, 0 );
		hits += trace.fraction < 1;
		solid += ( contents & CONTENTS_SOLID ) != 0;
		if ( !compare ) {
			cmCheckTraces[index] = trace;
			cmCheckContents[index] = contents;
			continue;
		}
		if ( fabs( trace.fraction - cmCheckTraces[index].fraction ) > 0.0001f || trace.startsolid != cmCheckTraces[index].startsolid
			|| trace.allsolid != cmCheckTraces[index].allsolid || contents != cmCheckContents[index] ) {
			if ( differ < 5 ) {
				Com_Printf( "collision check: differs at (%.0f %.0f %.0f): fraction %f / %f, contents %i / %i\n",
					start[0], start[1], start[2], cmCheckTraces[index].fraction, trace.fraction, cmCheckContents[index], contents );
			}
			differ++;
		}
	}
	if ( compare ) {
		Com_Printf( "collision check: %i of %i traces and point tests differ (%i traces hit, %i points in solid)\n",
			differ, CM_CHECK_COUNT, hits, solid );
	}
}

void CM_SplitLeafs( void ) {
	vec3_t		*leafMins, *leafMaxs;
	int			*newRoot;
	int			leafNum, nodeNum, side, index;
	int			oldLeafs = cm.numLeafs, oldNodes = cm.numNodes, oldLeafBrushes = cm.numLeafBrushes;
	int			split = 0, before = 0, after = 0, maxBefore = 0, maxAfter = 0;
	int			*oldBase = cm.leafbrushes;
	cNode_t		*nodes;
	cLeaf_t		*leafs;
	int			*leafbrushes;

	if ( !Cvar_Get( "cm_splitLeafs", "1", 0 )->integer || cm.numNodes < 1 ) {
		return;
	}
	Com_Memset( &cmSplit, 0, sizeof( cmSplit ) );
	leafMins = Z_Malloc( oldLeafs * sizeof( vec3_t ) );
	leafMaxs = Z_Malloc( oldLeafs * sizeof( vec3_t ) );
	newRoot = Z_Malloc( oldLeafs * sizeof( int ) );
	CM_LeafBounds( 0, cm.cmodels[0].mins, cm.cmodels[0].maxs, leafMins, leafMaxs );

	for ( leafNum = 0; leafNum < oldLeafs; leafNum++ ) {
		cLeaf_t	*leaf = &cm.leafs[leafNum];
		newRoot[leafNum] = 0;
		before += leaf->numLeafBrushes;
		if ( leaf->numLeafBrushes > maxBefore ) {
			maxBefore = leaf->numLeafBrushes;
		}
		if ( leaf->numLeafBrushes <= CM_SPLIT_BRUSHES ) {
			after += leaf->numLeafBrushes;
			if ( leaf->numLeafBrushes > maxAfter ) {
				maxAfter = leaf->numLeafBrushes;
			}
			continue;
		}
		{
			int		firstNewLeaf = cmSplit.numLeafs;
			int		root = CM_SplitRegion( leaf, cm.leafbrushes + leaf->firstLeafBrush, leaf->numLeafBrushes,
						leafMins[leafNum], leafMaxs[leafNum], 0 );
			for ( index = firstNewLeaf; index < cmSplit.numLeafs; index++ ) {
				after += cmSplit.leafs[index].numLeafBrushes;
				if ( cmSplit.leafs[index].numLeafBrushes > maxAfter ) {
					maxAfter = cmSplit.leafs[index].numLeafBrushes;
				}
			}
			newRoot[leafNum] = root >= 0 ? root + 1 : root - 1;	// 0 = kept; +1 / -1 so that node 0 counts
			split++;
		}
	}

	if ( split && Cvar_Get( "cm_splitCheck", "0", 0 )->integer ) {
		CM_SplitCheck( qfalse );		// the answers of the old tree, compared below
	}
	if ( split ) {
		// the arrays again, old entries first; the box hull keeps its room at the end
		nodes = Hunk_Alloc( ( oldNodes + cmSplit.numNodes ) * sizeof( *nodes ), h_high );
		Com_Memcpy( nodes, cm.nodes, oldNodes * sizeof( *nodes ) );
		leafs = Hunk_Alloc( ( BOX_LEAFS + oldLeafs + cmSplit.numLeafs ) * sizeof( *leafs ), h_high );
		Com_Memcpy( leafs, cm.leafs, oldLeafs * sizeof( *leafs ) );
		leafbrushes = Hunk_Alloc( ( oldLeafBrushes + cmSplit.numLeafBrushes + BOX_BRUSHES ) * sizeof( *leafbrushes ), h_high );
		Com_Memcpy( leafbrushes, cm.leafbrushes, oldLeafBrushes * sizeof( *leafbrushes ) );
		Com_Memcpy( leafbrushes + oldLeafBrushes, cmSplit.leafbrushes, cmSplit.numLeafBrushes * sizeof( *leafbrushes ) );

		for ( index = 0; index < cmSplit.numNodes; index++ ) {
			cNode_t	*node = &nodes[oldNodes + index];
			*node = cmSplit.nodes[index];
			for ( side = 0; side < 2; side++ ) {
				node->children[side] = node->children[side] >= 0 ? node->children[side] + oldNodes
					: -1 - ( oldLeafs + ( -1 - node->children[side] ) );
			}
		}
		for ( index = 0; index < cmSplit.numLeafs; index++ ) {
			leafs[oldLeafs + index] = cmSplit.leafs[index];
			leafs[oldLeafs + index].firstLeafBrush += oldLeafBrushes;
		}
		// the parents of the split leafs point at the new subtrees
		for ( nodeNum = 0; nodeNum < oldNodes; nodeNum++ ) {
			for ( side = 0; side < 2; side++ ) {
				int		child = nodes[nodeNum].children[side];
				if ( child < 0 && newRoot[-1 - child] ) {
					int	root = newRoot[-1 - child];
					nodes[nodeNum].children[side] = root > 0 ? root - 1 + oldNodes : -1 - ( oldLeafs + ( -1 - ( root + 1 ) ) );
				}
			}
		}
		// the submodels hold their brushes past the old array: the same places, the new base
		for ( index = 1; index < cm.numSubModels; index++ ) {
			cm.cmodels[index].leaf.firstLeafBrush = ( oldBase + cm.cmodels[index].leaf.firstLeafBrush ) - leafbrushes;
		}
		cm.nodes = nodes;
		cm.numNodes = oldNodes + cmSplit.numNodes;
		cm.leafs = leafs;
		cm.numLeafs = oldLeafs + cmSplit.numLeafs;
		cm.leafbrushes = leafbrushes;
		cm.numLeafBrushes = oldLeafBrushes + cmSplit.numLeafBrushes;
		Com_Printf( "collision: %i of %i leafs split into %i (brushes in a leaf: up to %i, was %i; tested %i, was %i)\n",
			split, oldLeafs, cmSplit.numLeafs, maxAfter, maxBefore, after, before );
		if ( Cvar_Get( "cm_splitCheck", "0", 0 )->integer ) {
			CM_SplitCheck( qtrue );
		}
	}
	if ( cmSplit.nodes ) Z_Free( cmSplit.nodes );
	if ( cmSplit.leafs ) Z_Free( cmSplit.leafs );
	if ( cmSplit.leafbrushes ) Z_Free( cmSplit.leafbrushes );
	Z_Free( leafMins );
	Z_Free( leafMaxs );
	Z_Free( newRoot );
}

/*
===================
CM_InitBoxHull

Set up the planes and nodes so that the six floats of a bounding box
can just be stored out and get a proper clipping hull structure.
===================
*/
void CM_InitBoxHull (void)
{
	int			i;
	int			side;
	cplane_t	*p;
	cbrushside_t	*s;

	box_planes = &cm.planes[cm.numPlanes];

	box_brush = &cm.brushes[cm.numBrushes];
	box_brush->numsides = 6;
	box_brush->sides = cm.brushsides + cm.numBrushSides;
	box_brush->contents = CONTENTS_BODY;

	box_model.leaf.numLeafBrushes = 1;
//	box_model.leaf.firstLeafBrush = cm.numBrushes;
	box_model.leaf.firstLeafBrush = cm.numLeafBrushes;
	cm.leafbrushes[cm.numLeafBrushes] = cm.numBrushes;

	for (i=0 ; i<6 ; i++)
	{
		side = i&1;

		// brush sides
		s = &cm.brushsides[cm.numBrushSides+i];
		s->plane = 	cm.planes + (cm.numPlanes+i*2+side);
		s->surfaceFlags = 0;

		// planes
		p = &box_planes[i*2];
		p->type = i>>1;
		p->signbits = 0;
		VectorClear (p->normal);
		p->normal[i>>1] = 1;

		p = &box_planes[i*2+1];
		p->type = 3 + (i>>1);
		p->signbits = 0;
		VectorClear (p->normal);
		p->normal[i>>1] = -1;

		SetPlaneSignbits( p );
	}	
}

/*
===================
CM_TempBoxModel

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
Capsules are handled differently though.
===================
*/
clipHandle_t CM_TempBoxModel( const vec3_t mins, const vec3_t maxs, int capsule ) {

	VectorCopy( mins, box_model.mins );
	VectorCopy( maxs, box_model.maxs );

	if ( capsule ) {
		return CAPSULE_MODEL_HANDLE;
	}

	box_planes[0].dist = maxs[0];
	box_planes[1].dist = -maxs[0];
	box_planes[2].dist = mins[0];
	box_planes[3].dist = -mins[0];
	box_planes[4].dist = maxs[1];
	box_planes[5].dist = -maxs[1];
	box_planes[6].dist = mins[1];
	box_planes[7].dist = -mins[1];
	box_planes[8].dist = maxs[2];
	box_planes[9].dist = -maxs[2];
	box_planes[10].dist = mins[2];
	box_planes[11].dist = -mins[2];

	VectorCopy( mins, box_brush->bounds[0] );
	VectorCopy( maxs, box_brush->bounds[1] );

	return BOX_MODEL_HANDLE;
}

/*
===================
CM_ModelBounds
===================
*/
void CM_ModelBounds( clipHandle_t model, vec3_t mins, vec3_t maxs ) {
	cmodel_t	*cmod;

	cmod = CM_ClipHandleToModel( model );
	VectorCopy( cmod->mins, mins );
	VectorCopy( cmod->maxs, maxs );
}


