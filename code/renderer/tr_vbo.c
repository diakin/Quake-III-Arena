/*
===========================================================================
WALKER: the map geometry in video memory (vertex buffer objects)

Q3 copies the vertices of every visible surface into the tess arrays each
frame, computes texture coordinates and colours for every vertex of every
stage on the processor, and the driver copies them to the video card again
on every draw. On wk_islands that is about 250 thousand triangles a frame.

Here the faces and triangle surfaces of the map (SF_FACE, SF_TRIANGLES, the
merged faces too) go once, at map load, into one vertex buffer and one index
buffer. A surface whose shader needs nothing computed per vertex on the
processor (tr_shade.c, RB_ShaderVBOCapable) adds only its range of indexes to
the batch; the batch is drawn with one glMultiDrawElements per stage. Texture
coordinates: tcGen texture / lightmap from the buffer, tcGen vector by OpenGL
texgen, tcMod scale / scroll / transform / rotate by the texture matrix.

Surfaces with dynamic lights on them, grids (their level of detail changes),
shaders with deforms, per-vertex colours or texture coordinates computed on
the processor, the sky, portals and fog passes go the old way.

r_vbo 0 (vid_restart) turns it off.
===========================================================================
*/

#include <stddef.h>
#include "tr_local.h"

#define GL_ARRAY_BUFFER_ARB				0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ARB		0x8893
#define GL_STATIC_DRAW_ARB				0x88E4

typedef ptrdiff_t	vboSize_t;

static void	( APIENTRY *qglGenBuffersARB )( GLsizei count, GLuint *buffers );
static void	( APIENTRY *qglDeleteBuffersARB )( GLsizei count, const GLuint *buffers );
static void	( APIENTRY *qglBindBufferARB )( GLenum target, GLuint buffer );
static void	( APIENTRY *qglBufferDataARB )( GLenum target, vboSize_t size, const void *data, GLenum usage );
static void	( APIENTRY *qglMultiDrawArraysEXT )( GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount );
static void	( APIENTRY *qglMultiDrawElementsEXT )( GLenum mode, const GLsizei *count, GLenum type, const void **indices, GLsizei primcount );

cvar_t			*r_vbo;

static void		R_VBOResetMeshTables( void );

static qboolean	vboFunctions;		// the OpenGL functions were found
qboolean		R_VBOFunctionsAvailable( void ) { return vboFunctions; }
static GLuint	worldVertexBuffer;
static GLuint	worldIndexBuffer;
static GLuint	worldAwayBuffer;		// the triangles facing away from the sun (tr_sunshadow.c)
static vec3_t	worldAwaySun;
static int		worldVBOVertexes, worldVBOIndexes;


/*
=================
R_VBOProc
=================
*/
static void *R_VBOProc( const char *name, const char *other, qboolean *ok ) {
	void	*proc = NULL;

#ifdef _WIN32
	proc = (void *)qwglGetProcAddress( name );
	if ( !proc && other ) {
		proc = (void *)qwglGetProcAddress( other );
	}
#endif
	if ( !proc ) {
		*ok = qfalse;
	}
	return proc;
}


/*
=================
R_VBOInit

after every start of OpenGL
=================
*/
void R_VBOInit( void ) {
	qboolean	ok = qtrue;

	r_vbo = ri.Cvar_Get( "r_vbo", "1", CVAR_ARCHIVE | CVAR_LATCH );
	vboFunctions = qfalse;
	worldVertexBuffer = worldIndexBuffer = worldAwayBuffer = 0;
	worldVBOVertexes = worldVBOIndexes = 0;
	R_VBOResetMeshTables();

	qglGenBuffersARB = R_VBOProc( "glGenBuffers", "glGenBuffersARB", &ok );
	qglDeleteBuffersARB = R_VBOProc( "glDeleteBuffers", "glDeleteBuffersARB", &ok );
	qglBindBufferARB = R_VBOProc( "glBindBuffer", "glBindBufferARB", &ok );
	qglBufferDataARB = R_VBOProc( "glBufferData", "glBufferDataARB", &ok );
	if ( !ok ) {
		ri.Printf( PRINT_ALL, "VBO: OpenGL 1.5 buffer functions not found, the map is drawn the old way\n" );
		return;
	}
	ok = qtrue;
	qglMultiDrawElementsEXT = R_VBOProc( "glMultiDrawElements", "glMultiDrawElementsEXT", &ok );
	if ( !ok ) {
		qglMultiDrawElementsEXT = NULL;		// a glDrawElements for every range instead
	}
	ok = qtrue;
	qglMultiDrawArraysEXT = R_VBOProc( "glMultiDrawArrays", "glMultiDrawArraysEXT", &ok );
	if ( !ok ) {
		qglMultiDrawArraysEXT = NULL;
	}
	vboFunctions = qtrue;
}


/*
=================
R_VBOShutdown

before OpenGL goes, and before another map is loaded
=================
*/
void R_VBOShutdown( void ) {
	if ( !vboFunctions ) {
		return;
	}
	if ( worldVertexBuffer ) {
		qglDeleteBuffersARB( 1, &worldVertexBuffer );
	}
	if ( worldIndexBuffer ) {
		qglDeleteBuffersARB( 1, &worldIndexBuffer );
	}
	if ( worldAwayBuffer ) {
		qglDeleteBuffersARB( 1, &worldAwayBuffer );
	}
	worldVertexBuffer = worldIndexBuffer = worldAwayBuffer = 0;
	worldVBOVertexes = worldVBOIndexes = 0;
}


/*
=================
R_VBOWorldActive
=================
*/
qboolean R_VBOWorldActive( void ) {
	return worldVertexBuffer && worldIndexBuffer && r_vbo->integer;
}


/*
=================
R_VBOBindWorld / R_VBOUnbind
=================
*/
void R_VBOBindWorld( void ) {
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, worldVertexBuffer );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, worldIndexBuffer );
}

void R_VBOUnbind( void ) {
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, 0 );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, 0 );
}


/*
=================
R_VBOCreateBuffer / R_VBODeleteBuffer / R_VBOBindBuffers / R_VBODrawArrayRanges

small helpers for other users of buffers (tr_sunshadow.c)
=================
*/
unsigned int R_VBOCreateBuffer( qboolean elements, int size, const void *data ) {
	GLuint	buffer = 0;
	GLenum	target = elements ? GL_ELEMENT_ARRAY_BUFFER_ARB : GL_ARRAY_BUFFER_ARB;

	if ( !vboFunctions || !r_vbo->integer || size <= 0 ) {
		return 0;
	}
	while ( qglGetError() != GL_NO_ERROR ) {
	}
	qglGenBuffersARB( 1, &buffer );
	qglBindBufferARB( target, buffer );
	qglBufferDataARB( target, size, data, GL_STATIC_DRAW_ARB );
	qglBindBufferARB( target, 0 );
	if ( qglGetError() != GL_NO_ERROR ) {
		qglDeleteBuffersARB( 1, &buffer );
		return 0;
	}
	return buffer;
}

void R_VBODeleteBuffer( unsigned int *buffer ) {
	if ( vboFunctions && *buffer ) {
		qglDeleteBuffersARB( 1, buffer );
	}
	*buffer = 0;
}

void R_VBOBindBuffers( unsigned int vertexBuffer, unsigned int indexBuffer ) {
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, vertexBuffer );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, indexBuffer );
}

void R_VBODrawArrayRanges( int count, const GLint *firsts, const GLsizei *counts ) {
	int		index;

	if ( qglMultiDrawArraysEXT ) {
		qglMultiDrawArraysEXT( GL_TRIANGLES, firsts, counts, count );
		backEnd.pc.c_drawCalls++;
		return;
	}
	for ( index = 0; index < count; index++ ) {
		qglDrawArrays( GL_TRIANGLES, firsts[index], counts[index] );
	}
	backEnd.pc.c_drawCalls += count;
}

qboolean R_VBOWorldAwayActive( void ) {
	return worldAwayBuffer && R_VBOWorldActive() && VectorCompare( worldAwaySun, tr.sunDirection );
}

void R_VBOBindWorldAway( void ) {
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, worldVertexBuffer );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, worldAwayBuffer );
}


/*
=================
R_VBODrawRanges

all index ranges of the batch with one call when the driver has glMultiDrawElements
=================
*/
void R_VBODrawRanges( int count, const GLsizei *counts, const void **offsets ) {
	int		index;

	if ( qglMultiDrawElementsEXT ) {
		qglMultiDrawElementsEXT( GL_TRIANGLES, counts, GL_INDEX_TYPE, offsets, count );
		backEnd.pc.c_drawCalls++;
		return;
	}
	for ( index = 0; index < count; index++ ) {
		qglDrawElements( GL_TRIANGLES, counts[index], GL_INDEX_TYPE, offsets[index] );
	}
	backEnd.pc.c_drawCalls += count;
}


/*
=================
R_VBOBuildWorld

after the map is loaded (and its faces merged)
=================
*/
void R_VBOBuildWorld( void ) {
	vboVertex_t	*vertexes, *vertex;
	glIndex_t	*indexes;
	byte		*used;
	int			numVertexes, numIndexes, surfaceNum, index;
	int			vertexBase, indexBase;
	msurface_t	*surf;

	R_VBOShutdown();
	if ( !vboFunctions || !r_vbo->integer || !tr.world ) {
		return;
	}
	ri.Printf( PRINT_ALL, "VBO: building the map buffers\n" );

	// the surfaces drawn: those the leaves list (the faces R_MergeWorldFaces joined
	// stay in the array, unused) and those of the brush models after the world's
	used = ri.Hunk_AllocateTempMemory( tr.world->numsurfaces );
	Com_Memset( used, 0, tr.world->numsurfaces );
	for ( index = 0; index < tr.world->nummarksurfaces; index++ ) {
		surfaceNum = tr.world->marksurfaces[index] - tr.world->surfaces;
		if ( surfaceNum >= 0 && surfaceNum < tr.world->numsurfaces ) {
			used[surfaceNum] = 1;
		}
	}
	for ( surfaceNum = tr.world->bmodels[0].numSurfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++ ) {
		used[surfaceNum] = 1;
	}

	// count
	numVertexes = numIndexes = 0;
	for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
		if ( *surf->data == SF_FACE ) {
			( (srfSurfaceFace_t *)surf->data )->vboNumIndexes = 0;
			( (srfSurfaceFace_t *)surf->data )->vboAwayFirst = 0;
			( (srfSurfaceFace_t *)surf->data )->vboAwayCount = 0;
		} else if ( *surf->data == SF_TRIANGLES ) {
			( (srfTriangles_t *)surf->data )->vboNumIndexes = 0;
			( (srfTriangles_t *)surf->data )->vboAwayFirst = 0;
			( (srfTriangles_t *)surf->data )->vboAwayCount = 0;
		}
	}
	for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
		if ( !used[surfaceNum] ) {
			continue;
		}
		if ( *surf->data == SF_FACE ) {
			srfSurfaceFace_t	*face = (srfSurfaceFace_t *)surf->data;
			numVertexes += face->numPoints;
			numIndexes += face->numIndices;
		} else if ( *surf->data == SF_TRIANGLES ) {
			srfTriangles_t		*tri = (srfTriangles_t *)surf->data;
			numVertexes += tri->numVerts;
			numIndexes += tri->numIndexes;
		}
	}
	if ( !numVertexes || !numIndexes ) {
		ri.Hunk_FreeTempMemory( used );
		return;
	}

	vertexes = ri.Hunk_AllocateTempMemory( numVertexes * sizeof( vboVertex_t ) );
	indexes = ri.Hunk_AllocateTempMemory( numIndexes * sizeof( glIndex_t ) );

	// fill (a surface met twice is taken once)
	vertexBase = indexBase = 0;
	for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
		if ( !used[surfaceNum] ) {
			continue;
		}
		if ( *surf->data == SF_FACE ) {
			srfSurfaceFace_t	*face = (srfSurfaceFace_t *)surf->data;
			int					*faceIndexes = (int *)( (byte *)face + face->ofsIndices );
			float				*point;

			if ( face->vboNumIndexes || face->numIndices <= 0 ) {
				continue;
			}
			vertex = vertexes + vertexBase;
			for ( index = 0, point = face->points[0]; index < face->numPoints; index++, point += VERTEXSIZE, vertex++ ) {
				VectorCopy( point, vertex->xyz );
				vertex->st[0] = point[3];
				vertex->st[1] = point[4];
				vertex->lightmap[0] = point[5];
				vertex->lightmap[1] = point[6];
				*(unsigned int *)vertex->color = *(unsigned int *)&point[7];
			}
			for ( index = 0; index < face->numIndices; index++ ) {
				indexes[indexBase + index] = vertexBase + faceIndexes[index];
			}
			face->vboFirstIndex = indexBase;
			face->vboNumIndexes = face->numIndices;
			vertexBase += face->numPoints;
			indexBase += face->numIndices;
		} else if ( *surf->data == SF_TRIANGLES ) {
			srfTriangles_t		*tri = (srfTriangles_t *)surf->data;
			drawVert_t			*drawVert;

			if ( tri->vboNumIndexes || tri->numIndexes <= 0 ) {
				continue;
			}
			vertex = vertexes + vertexBase;
			for ( index = 0, drawVert = tri->verts; index < tri->numVerts; index++, drawVert++, vertex++ ) {
				VectorCopy( drawVert->xyz, vertex->xyz );
				vertex->st[0] = drawVert->st[0];
				vertex->st[1] = drawVert->st[1];
				vertex->lightmap[0] = drawVert->lightmap[0];
				vertex->lightmap[1] = drawVert->lightmap[1];
				*(unsigned int *)vertex->color = *(unsigned int *)drawVert->color;
			}
			for ( index = 0; index < tri->numIndexes; index++ ) {
				indexes[indexBase + index] = vertexBase + tri->indexes[index];
			}
			tri->vboFirstIndex = indexBase;
			tri->vboNumIndexes = tri->numIndexes;
			vertexBase += tri->numVerts;
			indexBase += tri->numIndexes;
		}
	}

	// the triangles facing away from the sun, for the sun shadows (tr_sunshadow.c,
	// RB_CollectAwayTriangles drew them from the processor's memory every frame)
	{
		glIndex_t	*away = ri.Hunk_AllocateTempMemory( indexBase * sizeof( glIndex_t ) );
		int			numAway = 0;

		for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
			if ( *surf->data == SF_FACE ) {
				srfSurfaceFace_t	*face = (srfSurfaceFace_t *)surf->data;
				if ( !face->vboNumIndexes || face->vboAwayFirst < 0 || face->vboAwayCount ) {
					continue;
				}
				face->vboAwayFirst = numAway;
				face->vboAwayCount = 0;
				if ( DotProduct( face->plane.normal, tr.sunDirection ) > 0 ) {
					face->vboAwayFirst = -1;	// done (a surface met twice)
					continue;
				}
				Com_Memcpy( away + numAway, indexes + face->vboFirstIndex, face->vboNumIndexes * sizeof( glIndex_t ) );
				numAway += face->vboNumIndexes;
				face->vboAwayCount = face->vboNumIndexes;
			} else if ( *surf->data == SF_TRIANGLES ) {
				srfTriangles_t		*tri = (srfTriangles_t *)surf->data;
				int					corner;
				if ( !tri->vboNumIndexes || tri->vboAwayFirst < 0 || tri->vboAwayCount ) {
					continue;
				}
				tri->vboAwayFirst = numAway;
				for ( corner = 0; corner < tri->vboNumIndexes; corner += 3 ) {
					glIndex_t	*triangle = indexes + tri->vboFirstIndex + corner;
					vec3_t		edge1, edge2, normal;
					VectorSubtract( vertexes[triangle[1]].xyz, vertexes[triangle[0]].xyz, edge1 );
					VectorSubtract( vertexes[triangle[2]].xyz, vertexes[triangle[0]].xyz, edge2 );
					CrossProduct( edge1, edge2, normal );
					// Quake winding is clockwise: this normal points into the model
					if ( DotProduct( normal, tr.sunDirection ) < 0 ) {
						continue;
					}
					away[numAway++] = triangle[0];
					away[numAway++] = triangle[1];
					away[numAway++] = triangle[2];
				}
				tri->vboAwayCount = numAway - tri->vboAwayFirst;
				if ( !tri->vboAwayCount ) {
					tri->vboAwayFirst = -1;
				}
			}
		}
		// faces left at -1 had nothing facing away: mark them empty again
		for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
			if ( *surf->data == SF_FACE && ( (srfSurfaceFace_t *)surf->data )->vboAwayFirst < 0 ) {
				( (srfSurfaceFace_t *)surf->data )->vboAwayFirst = 0;
				( (srfSurfaceFace_t *)surf->data )->vboAwayCount = 0;
			} else if ( *surf->data == SF_TRIANGLES && ( (srfTriangles_t *)surf->data )->vboAwayFirst < 0 ) {
				( (srfTriangles_t *)surf->data )->vboAwayFirst = 0;
				( (srfTriangles_t *)surf->data )->vboAwayCount = 0;
			}
		}
		if ( numAway ) {
			qglGenBuffersARB( 1, &worldAwayBuffer );
			qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, worldAwayBuffer );
			qglBufferDataARB( GL_ELEMENT_ARRAY_BUFFER_ARB, numAway * sizeof( glIndex_t ), away, GL_STATIC_DRAW_ARB );
			qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, 0 );
			VectorCopy( tr.sunDirection, worldAwaySun );
		}
		ri.Hunk_FreeTempMemory( away );
	}

	qglGenBuffersARB( 1, &worldVertexBuffer );
	qglGenBuffersARB( 1, &worldIndexBuffer );
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, worldVertexBuffer );
	qglBufferDataARB( GL_ARRAY_BUFFER_ARB, vertexBase * sizeof( vboVertex_t ), vertexes, GL_STATIC_DRAW_ARB );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, worldIndexBuffer );
	qglBufferDataARB( GL_ELEMENT_ARRAY_BUFFER_ARB, indexBase * sizeof( glIndex_t ), indexes, GL_STATIC_DRAW_ARB );
	R_VBOUnbind();

	ri.Hunk_FreeTempMemory( indexes );
	ri.Hunk_FreeTempMemory( vertexes );
	ri.Hunk_FreeTempMemory( used );

	if ( qglGetError() != GL_NO_ERROR ) {
		ri.Printf( PRINT_WARNING, "VBO: the map buffers could not be made, the map is drawn the old way\n" );
		R_VBOShutdown();
		for ( surfaceNum = 0, surf = tr.world->surfaces; surfaceNum < tr.world->numsurfaces; surfaceNum++, surf++ ) {
			if ( *surf->data == SF_FACE ) {
				( (srfSurfaceFace_t *)surf->data )->vboNumIndexes = 0;
			} else if ( *surf->data == SF_TRIANGLES ) {
				( (srfTriangles_t *)surf->data )->vboNumIndexes = 0;
			}
		}
		return;
	}
	worldVBOVertexes = vertexBase;
	worldVBOIndexes = indexBase;
	ri.Printf( PRINT_ALL, "VBO: map in video memory, %i vertexes, %i triangles (%i KB)%s\n",
		vertexBase, indexBase / 3, ( vertexBase * (int)sizeof( vboVertex_t ) + indexBase * (int)sizeof( glIndex_t ) ) / 1024,
		qglMultiDrawElementsEXT ? "" : ", no glMultiDrawElements" );
}



/*
===========================================================================
WALKER: models in video memory

Every frame of an MD3 model (and of a GLB one, turned into MD3 at load) goes
into one buffer per model and level of detail: the texture coordinates once,
then frame after frame the vertexes of all its surfaces, as MD3 keeps them
(three shorts and the packed normal, 8 bytes). The indexes of all surfaces
point into that, so the surfaces of one entity with one shader are drawn with
one glMultiDrawElements a stage. The blend of two frames, the light, the
environment map and the specular alpha are computed by the GLSL program of
tr_glsl.c (GLSL_Mesh*) instead of the processor (LerpMeshVertexes,
RB_CalcDiffuseColor, ...). A surface is found by its address in a table.
===========================================================================
*/

#define	MAX_MESH_MODELS		1024
#define	MESH_TABLE_SIZE		8192		// power of two

static meshModelVBO_t	meshModels[MAX_MESH_MODELS];
static int				numMeshModels;

typedef struct {
	const md3Surface_t	*surface;
	meshSurfaceVBO_t	mesh;
} meshTableEntry_t;

static meshTableEntry_t	meshTable[MESH_TABLE_SIZE];
static int				meshBytes;

static unsigned R_MeshHash( const void *pointer ) {
	size_t	value = (size_t)pointer;
	return (unsigned)( ( value >> 4 ) ^ ( value >> 13 ) ) & ( MESH_TABLE_SIZE - 1 );
}

const meshSurfaceVBO_t *R_VBOMeshSurface( const md3Surface_t *surface ) {
	unsigned	slot = R_MeshHash( surface );
	int			probe;

	for ( probe = 0; probe < MESH_TABLE_SIZE; probe++, slot = ( slot + 1 ) & ( MESH_TABLE_SIZE - 1 ) ) {
		if ( meshTable[slot].surface == surface ) {
			return &meshTable[slot].mesh;
		}
		if ( !meshTable[slot].surface ) {
			return NULL;
		}
	}
	return NULL;
}

static qboolean R_MeshTableAdd( const md3Surface_t *surface, meshModelVBO_t *model, int firstIndex, int numIndexes ) {
	unsigned	slot = R_MeshHash( surface );
	int			probe;

	for ( probe = 0; probe < MESH_TABLE_SIZE / 2; probe++, slot = ( slot + 1 ) & ( MESH_TABLE_SIZE - 1 ) ) {
		if ( !meshTable[slot].surface || meshTable[slot].surface == surface ) {
			meshTable[slot].surface = surface;
			meshTable[slot].mesh.model = model;
			meshTable[slot].mesh.firstIndex = firstIndex;
			meshTable[slot].mesh.numIndexes = numIndexes;
			return qtrue;
		}
	}
	return qfalse;		// full enough: the rest the old way
}

/*
=================
R_VBOBuildMesh

after an MD3 level of detail is loaded
=================
*/
void R_VBOBuildMesh( md3Header_t *header ) {
	md3Surface_t	*surface;
	meshModelVBO_t	*model;
	byte			*data;
	glIndex_t		*indexes;
	int				surfaceNum, frame, numVerts = 0, numIndexes = 0, vertexBase, indexBase, size, offset;

	if ( !vboFunctions || !r_vbo->integer || !GLSL_MeshAvailable() || !header ) {
		return;
	}
	if ( header->numSurfaces <= 0 || header->numFrames <= 0 || numMeshModels >= MAX_MESH_MODELS ) {
		return;
	}
	surface = (md3Surface_t *)( (byte *)header + header->ofsSurfaces );
	if ( R_VBOMeshSurface( surface ) ) {
		return;		// the same level loaded twice
	}
	for ( surfaceNum = 0; surfaceNum < header->numSurfaces; surfaceNum++ ) {
		if ( surface->numFrames != header->numFrames ) {
			return;
		}
		numVerts += surface->numVerts;
		numIndexes += surface->numTriangles * 3;
		surface = (md3Surface_t *)( (byte *)surface + surface->ofsEnd );
	}
	if ( !numVerts || !numIndexes ) {
		return;
	}

	model = &meshModels[numMeshModels];
	model->numVerts = numVerts;
	model->numFrames = header->numFrames;
	model->stOffset = 0;
	model->framesOffset = numVerts * 2 * sizeof( float );
	size = model->framesOffset + header->numFrames * numVerts * sizeof( md3XyzNormal_t );

	data = ri.Hunk_AllocateTempMemory( size );
	indexes = ri.Hunk_AllocateTempMemory( numIndexes * sizeof( glIndex_t ) );

	vertexBase = indexBase = 0;
	surface = (md3Surface_t *)( (byte *)header + header->ofsSurfaces );
	for ( surfaceNum = 0; surfaceNum < header->numSurfaces; surfaceNum++ ) {
		int		*triangles = (int *)( (byte *)surface + surface->ofsTriangles );
		int		index;

		Com_Memcpy( data + model->stOffset + vertexBase * 2 * sizeof( float ),
			(byte *)surface + surface->ofsSt, surface->numVerts * 2 * sizeof( float ) );
		for ( frame = 0; frame < header->numFrames; frame++ ) {
			offset = model->framesOffset + ( frame * numVerts + vertexBase ) * sizeof( md3XyzNormal_t );
			Com_Memcpy( data + offset, (byte *)surface + surface->ofsXyzNormals + frame * surface->numVerts * sizeof( md3XyzNormal_t ),
				surface->numVerts * sizeof( md3XyzNormal_t ) );
		}
		for ( index = 0; index < surface->numTriangles * 3; index++ ) {
			indexes[indexBase + index] = vertexBase + triangles[index];
		}
		if ( !R_MeshTableAdd( surface, model, indexBase, surface->numTriangles * 3 ) ) {
			ri.Printf( PRINT_WARNING, "VBO: the model table is full\n" );
		}
		vertexBase += surface->numVerts;
		indexBase += surface->numTriangles * 3;
		surface = (md3Surface_t *)( (byte *)surface + surface->ofsEnd );
	}

	qglGenBuffersARB( 1, &model->vertexBuffer );
	qglGenBuffersARB( 1, &model->indexBuffer );
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, model->vertexBuffer );
	qglBufferDataARB( GL_ARRAY_BUFFER_ARB, size, data, GL_STATIC_DRAW_ARB );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, model->indexBuffer );
	qglBufferDataARB( GL_ELEMENT_ARRAY_BUFFER_ARB, numIndexes * sizeof( glIndex_t ), indexes, GL_STATIC_DRAW_ARB );
	R_VBOUnbind();

	ri.Hunk_FreeTempMemory( indexes );
	ri.Hunk_FreeTempMemory( data );

	if ( qglGetError() != GL_NO_ERROR ) {
		// out of video memory, most likely: this model the old way
		ri.Printf( PRINT_WARNING, "VBO: a model buffer could not be made (%i KB), it is drawn the old way\n", size / 1024 );
		qglDeleteBuffersARB( 1, &model->vertexBuffer );
		qglDeleteBuffersARB( 1, &model->indexBuffer );
		model->vertexBuffer = model->indexBuffer = 0;
		model->numVerts = 0;		// R_VBOMeshSurface finds it, RB_SurfaceMesh checks this
		numMeshModels++;
		return;
	}
	numMeshModels++;
	meshBytes += size + numIndexes * sizeof( glIndex_t );
	ri.Printf( PRINT_DEVELOPER, "VBO: model %i surfaces, %i vertexes, %i frames, %i KB (models %i KB)\n",
		header->numSurfaces, numVerts, header->numFrames, ( size + numIndexes * (int)sizeof( glIndex_t ) ) / 1024, meshBytes / 1024 );
}

void R_VBOBindMesh( const meshModelVBO_t *model ) {
	qglBindBufferARB( GL_ARRAY_BUFFER_ARB, model->vertexBuffer );
	qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, model->indexBuffer );
}

/*
=================
R_VBOShutdownMeshes

before OpenGL goes
=================
*/
void R_VBOShutdownMeshes( void ) {
	int		index;

	if ( vboFunctions ) {
		for ( index = 0; index < numMeshModels; index++ ) {
			if ( meshModels[index].vertexBuffer ) {
				qglDeleteBuffersARB( 1, &meshModels[index].vertexBuffer );
			}
			if ( meshModels[index].indexBuffer ) {
				qglDeleteBuffersARB( 1, &meshModels[index].indexBuffer );
			}
		}
	}
	R_VBOResetMeshTables();
}

static void R_VBOResetMeshTables( void ) {
	Com_Memset( meshModels, 0, sizeof( meshModels ) );
	Com_Memset( meshTable, 0, sizeof( meshTable ) );
	numMeshModels = 0;
	meshBytes = 0;
}

int R_VBOMeshMemory( void ) {
	return meshBytes;
}
