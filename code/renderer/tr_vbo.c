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
static void	( APIENTRY *qglMultiDrawElementsEXT )( GLenum mode, const GLsizei *count, GLenum type, const void **indices, GLsizei primcount );

cvar_t			*r_vbo;

static qboolean	vboFunctions;		// the OpenGL functions were found
static GLuint	worldVertexBuffer;
static GLuint	worldIndexBuffer;
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
	worldVertexBuffer = worldIndexBuffer = 0;
	worldVBOVertexes = worldVBOIndexes = 0;

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
	worldVertexBuffer = worldIndexBuffer = 0;
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
		} else if ( *surf->data == SF_TRIANGLES ) {
			( (srfTriangles_t *)surf->data )->vboNumIndexes = 0;
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
