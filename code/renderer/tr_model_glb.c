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
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_model_glb.c -- glTF 2.0 binary (.glb) loader
//
// The .glb file is converted in memory into an ordinary MD3 image, which is
// then handed to R_LoadMD3, so the rest of the renderer and the game code
// see a normal MD3 model.
//
// Conversion rules:
//  - axes: glTF +Y up, +Z forward, +X left  ->  Quake +Z up, +X forward, +Y left
//  - units: glTF metres are multiplied by asset.extras.q3_scale or, if that is
//    absent, by the cvar r_glbScale (default 39.37: metres to inches)
//  - all mesh primitives with the same material are merged into one surface;
//    a surface is split so that it never exceeds the tess buffer limits
//  - the surface name is the material name (use it in .skin files); the shader
//    name is material.extras.q3_shader, or "<model dir>/<image uri without
//    extension>" for an external base colour image, or "<model dir>/<material>"
//  - every glTF animation is sampled at animation.extras.fps (default 15) and
//    appended to the frame list in the order of the "animations" array;
//    a file without animations gives one frame (the rest pose)
//  - nodes whose name starts with "tag_" become MD3 tags
//  - rigid node animation and skinning (JOINTS_0 / WEIGHTS_0) are supported;
//    morph targets, sparse accessors and compressed extensions are not
//  - triangle winding is reversed (glTF front faces are counter-clockwise,
//    Quake front faces are clockwise)

#include "tr_local.h"

#define GLB_MAGIC				0x46546C67		// "glTF"
#define GLB_CHUNK_JSON			0x4E4F534A		// "JSON"
#define GLB_CHUNK_BIN			0x004E4942		// "BIN\0"

// WALKER: half of the tess buffer: stencil shadows (RB_ShadowTessEnd) need room for
// a second, extruded copy of every vertex and skip larger surfaces, so the robot
// with its baked texture atlas (more vertices at UV seams) lost its shadow
#define GLB_MAX_SURFACE_VERTS	( SHADER_MAX_VERTEXES / 2 - 1 )
#define GLB_MAX_SURFACE_TRIS	( SHADER_MAX_VERTEXES / 2 - 1 )
#define GLB_MAX_SURFACES		256
#define GLB_DEFAULT_FPS			15.0f
#define GLB_DEFAULT_SCALE		"39.37"
#define GLB_MAX_JSON_DEPTH		64

#define GLB_COMP_BYTE			5120
#define GLB_COMP_UBYTE			5121
#define GLB_COMP_SHORT			5122
#define GLB_COMP_USHORT			5123
#define GLB_COMP_UINT			5125
#define GLB_COMP_FLOAT			5126

/*
==============================================================================

MEMORY

All working memory of one conversion comes from the temporary hunk (so a
large model cannot exhaust the zone), is chained newest first and is
released in reverse order of allocation, as the temporary hunk requires.

==============================================================================
*/

typedef struct glbAlloc_s {
	struct glbAlloc_s	*next;
	double				align;		// keeps the user block 8-byte aligned
} glbAlloc_t;

typedef struct {
	glbAlloc_t		*allocs;
	const char		*modelName;
} glbMemory_t;

static void *GLB_Alloc( glbMemory_t *memory, int size ) {
	glbAlloc_t	*block;

	if ( size < 1 ) {
		size = 1;
	}
	block = ri.Hunk_AllocateTempMemory( sizeof( glbAlloc_t ) + size );
	Com_Memset( block, 0, sizeof( glbAlloc_t ) + size );
	block->next = memory->allocs;
	memory->allocs = block;
	return (void *)( block + 1 );
}

static void GLB_FreeAll( glbMemory_t *memory ) {
	glbAlloc_t	*block, *next;

	for ( block = memory->allocs ; block ; block = next ) {
		next = block->next;
		ri.Hunk_FreeTempMemory( block );
	}
	memory->allocs = NULL;
}

/*
==============================================================================

JSON

A small DOM parser.  Strings are decoded in place inside a private copy of
the JSON text.  The parser runs twice: the first pass only counts nodes.

==============================================================================
*/

typedef enum {
	JSON_NULL,
	JSON_FALSE,
	JSON_TRUE,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} jsonType_t;

typedef struct jsonNode_s {
	jsonType_t			type;
	const char			*key;		// member name inside an object
	const char			*string;
	double				number;
	int					count;		// number of children
	struct jsonNode_s	*child;
	struct jsonNode_s	*next;
} jsonNode_t;

typedef struct {
	char		*cursor;
	char		*end;
	jsonNode_t	*nodes;			// NULL during the counting pass
	int			numNodes;
	int			maxNodes;
	jsonNode_t	scratch;
	const char	*error;
} jsonParser_t;

static jsonNode_t *JSON_NewNode( jsonParser_t *parser, jsonType_t type ) {
	jsonNode_t	*node;

	if ( !parser->nodes ) {
		parser->numNodes++;
		Com_Memset( &parser->scratch, 0, sizeof( parser->scratch ) );
		parser->scratch.type = type;
		return &parser->scratch;
	}
	if ( parser->numNodes >= parser->maxNodes ) {
		parser->error = "node pool overflow";
		return NULL;
	}
	node = &parser->nodes[ parser->numNodes++ ];
	Com_Memset( node, 0, sizeof( *node ) );
	node->type = type;
	return node;
}

static void JSON_SkipSpace( jsonParser_t *parser ) {
	while ( parser->cursor < parser->end ) {
		char	ch = *parser->cursor;
		if ( ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' ) {
			break;
		}
		parser->cursor++;
	}
}

static int JSON_HexDigit( char ch ) {
	if ( ch >= '0' && ch <= '9' ) return ch - '0';
	if ( ch >= 'a' && ch <= 'f' ) return ch - 'a' + 10;
	if ( ch >= 'A' && ch <= 'F' ) return ch - 'A' + 10;
	return -1;
}

// cursor is on the opening quote; returns the decoded string (fill pass only)
static const char *JSON_ParseString( jsonParser_t *parser ) {
	char		*read, *write, *start;
	qboolean	decode = ( parser->nodes != NULL );

	read = parser->cursor + 1;
	start = write = read;
	while ( 1 ) {
		char	ch;

		if ( read >= parser->end ) {
			parser->error = "unterminated string";
			return NULL;
		}
		ch = *read++;
		if ( ch == '"' ) {
			break;
		}
		if ( ch == '\\' ) {
			if ( read >= parser->end ) {
				parser->error = "bad escape";
				return NULL;
			}
			ch = *read++;
			switch ( ch ) {
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'n': ch = '\n'; break;
			case 'r': ch = '\r'; break;
			case 't': ch = '\t'; break;
			case 'u': {
				int		code = 0, digit, i;
				for ( i = 0 ; i < 4 ; i++ ) {
					if ( read >= parser->end || ( digit = JSON_HexDigit( *read ) ) < 0 ) {
						parser->error = "bad \\u escape";
						return NULL;
					}
					code = code * 16 + digit;
					read++;
				}
				// names are expected to be ASCII; anything else becomes '_'
				ch = ( code > 0 && code < 128 ) ? (char)code : '_';
				break;
			}
			default:
				break;		// \" \\ \/
			}
		}
		if ( decode ) {
			*write = ch;
		}
		write++;
	}
	parser->cursor = read;
	if ( decode ) {
		*write = 0;		// write never passes the closing quote
		return start;
	}
	return "";
}

static jsonNode_t *JSON_ParseValue( jsonParser_t *parser, int depth );

static jsonNode_t *JSON_ParseContainer( jsonParser_t *parser, int depth, qboolean isObject ) {
	jsonNode_t	*container, *last = NULL, *item;
	char		closing = isObject ? '}' : ']';

	container = JSON_NewNode( parser, isObject ? JSON_OBJECT : JSON_ARRAY );
	if ( !container ) {
		return NULL;
	}
	// the counting pass reuses one scratch node, so keep our own copy
	if ( !parser->nodes ) {
		container = NULL;
	}
	parser->cursor++;		// skip the bracket
	JSON_SkipSpace( parser );
	if ( parser->cursor < parser->end && *parser->cursor == closing ) {
		parser->cursor++;
		return container ? container : &parser->scratch;
	}
	while ( 1 ) {
		const char	*key = NULL;

		JSON_SkipSpace( parser );
		if ( isObject ) {
			if ( parser->cursor >= parser->end || *parser->cursor != '"' ) {
				parser->error = "expected member name";
				return NULL;
			}
			key = JSON_ParseString( parser );
			if ( !key ) {
				return NULL;
			}
			JSON_SkipSpace( parser );
			if ( parser->cursor >= parser->end || *parser->cursor != ':' ) {
				parser->error = "expected ':'";
				return NULL;
			}
			parser->cursor++;
		}
		item = JSON_ParseValue( parser, depth + 1 );
		if ( !item ) {
			return NULL;
		}
		if ( container ) {
			item->key = key;
			if ( last ) {
				last->next = item;
			} else {
				container->child = item;
			}
			last = item;
			container->count++;
		}
		JSON_SkipSpace( parser );
		if ( parser->cursor >= parser->end ) {
			parser->error = "unexpected end";
			return NULL;
		}
		if ( *parser->cursor == ',' ) {
			parser->cursor++;
			continue;
		}
		if ( *parser->cursor == closing ) {
			parser->cursor++;
			break;
		}
		parser->error = "expected ',' or closing bracket";
		return NULL;
	}
	return container ? container : &parser->scratch;
}

static jsonNode_t *JSON_ParseValue( jsonParser_t *parser, int depth ) {
	jsonNode_t	*node;
	char		ch;

	if ( depth > GLB_MAX_JSON_DEPTH ) {
		parser->error = "nesting too deep";
		return NULL;
	}
	JSON_SkipSpace( parser );
	if ( parser->cursor >= parser->end ) {
		parser->error = "unexpected end";
		return NULL;
	}
	ch = *parser->cursor;
	if ( ch == '{' || ch == '[' ) {
		return JSON_ParseContainer( parser, depth, ch == '{' );
	}
	if ( ch == '"' ) {
		const char	*text = JSON_ParseString( parser );
		if ( !text ) {
			return NULL;
		}
		node = JSON_NewNode( parser, JSON_STRING );
		if ( node ) {
			node->string = text;
		}
		return node;
	}
	if ( ch == 't' && parser->end - parser->cursor >= 4 && !strncmp( parser->cursor, "true", 4 ) ) {
		parser->cursor += 4;
		return JSON_NewNode( parser, JSON_TRUE );
	}
	if ( ch == 'f' && parser->end - parser->cursor >= 5 && !strncmp( parser->cursor, "false", 5 ) ) {
		parser->cursor += 5;
		return JSON_NewNode( parser, JSON_FALSE );
	}
	if ( ch == 'n' && parser->end - parser->cursor >= 4 && !strncmp( parser->cursor, "null", 4 ) ) {
		parser->cursor += 4;
		return JSON_NewNode( parser, JSON_NULL );
	}
	if ( ch == '-' || ( ch >= '0' && ch <= '9' ) ) {
		char	*numberEnd;
		double	value = strtod( parser->cursor, &numberEnd );
		if ( numberEnd == parser->cursor ) {
			parser->error = "bad number";
			return NULL;
		}
		parser->cursor = numberEnd;
		node = JSON_NewNode( parser, JSON_NUMBER );
		if ( node ) {
			node->number = value;
		}
		return node;
	}
	parser->error = "unexpected character";
	return NULL;
}

static jsonNode_t *JSON_Parse( glbMemory_t *memory, const char *source, int length, const char **error ) {
	jsonParser_t	parser;
	char			*text;
	jsonNode_t		*root;

	text = GLB_Alloc( memory, length + 1 );
	Com_Memcpy( text, source, length );
	text[length] = 0;

	// pass 1: count
	Com_Memset( &parser, 0, sizeof( parser ) );
	parser.cursor = text;
	parser.end = text + length;
	if ( !JSON_ParseValue( &parser, 0 ) ) {
		*error = parser.error;
		return NULL;
	}

	// pass 2: build
	parser.maxNodes = parser.numNodes;
	parser.numNodes = 0;
	parser.nodes = GLB_Alloc( memory, parser.maxNodes * sizeof( jsonNode_t ) );
	parser.cursor = text;
	root = JSON_ParseValue( &parser, 0 );
	if ( !root ) {
		*error = parser.error;
		return NULL;
	}
	return root;
}

static jsonNode_t *JSON_Get( const jsonNode_t *object, const char *key ) {
	jsonNode_t	*item;

	if ( !object || object->type != JSON_OBJECT ) {
		return NULL;
	}
	for ( item = object->child ; item ; item = item->next ) {
		if ( item->key && !strcmp( item->key, key ) ) {
			return item;
		}
	}
	return NULL;
}

static jsonNode_t *JSON_Index( const jsonNode_t *array, int index ) {
	jsonNode_t	*item;

	if ( !array || array->type != JSON_ARRAY || index < 0 ) {
		return NULL;
	}
	for ( item = array->child ; item && index > 0 ; item = item->next ) {
		index--;
	}
	return item;
}

static double JSON_Number( const jsonNode_t *node, double defaultValue ) {
	return ( node && node->type == JSON_NUMBER ) ? node->number : defaultValue;
}

static int JSON_Int( const jsonNode_t *node, int defaultValue ) {
	return ( node && node->type == JSON_NUMBER ) ? (int)node->number : defaultValue;
}

static const char *JSON_String( const jsonNode_t *node, const char *defaultValue ) {
	return ( node && node->type == JSON_STRING ) ? node->string : defaultValue;
}

static qboolean JSON_Bool( const jsonNode_t *node, qboolean defaultValue ) {
	if ( node && node->type == JSON_TRUE ) return qtrue;
	if ( node && node->type == JSON_FALSE ) return qfalse;
	return defaultValue;
}

// reads up to maxCount numbers from an array; returns how many were read
static int JSON_Floats( const jsonNode_t *array, float *out, int maxCount ) {
	jsonNode_t	*item;
	int			count = 0;

	if ( !array || array->type != JSON_ARRAY ) {
		return 0;
	}
	for ( item = array->child ; item && count < maxCount ; item = item->next ) {
		out[count++] = (float)JSON_Number( item, 0 );
	}
	return count;
}

/*
==============================================================================

MATH

Matrices are 4x4, column-major, as in glTF: m[column * 4 + row].

==============================================================================
*/

static void Mat4_Identity( float *out ) {
	Com_Memset( out, 0, 16 * sizeof( float ) );
	out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void Mat4_Multiply( const float *left, const float *right, float *out ) {
	float	result[16];
	int		column, row;

	for ( column = 0 ; column < 4 ; column++ ) {
		for ( row = 0 ; row < 4 ; row++ ) {
			result[column * 4 + row] =
				left[0 * 4 + row] * right[column * 4 + 0] +
				left[1 * 4 + row] * right[column * 4 + 1] +
				left[2 * 4 + row] * right[column * 4 + 2] +
				left[3 * 4 + row] * right[column * 4 + 3];
		}
	}
	Com_Memcpy( out, result, sizeof( result ) );
}

static void Mat4_FromTRS( const float *translation, const float *rotation, const float *scale, float *out ) {
	float	qx = rotation[0], qy = rotation[1], qz = rotation[2], qw = rotation[3];
	float	xx = qx * qx, yy = qy * qy, zz = qz * qz;
	float	xy = qx * qy, xz = qx * qz, yz = qy * qz;
	float	wx = qw * qx, wy = qw * qy, wz = qw * qz;

	out[0] = ( 1 - 2 * ( yy + zz ) ) * scale[0];
	out[1] = ( 2 * ( xy + wz ) ) * scale[0];
	out[2] = ( 2 * ( xz - wy ) ) * scale[0];
	out[3] = 0;
	out[4] = ( 2 * ( xy - wz ) ) * scale[1];
	out[5] = ( 1 - 2 * ( xx + zz ) ) * scale[1];
	out[6] = ( 2 * ( yz + wx ) ) * scale[1];
	out[7] = 0;
	out[8] = ( 2 * ( xz + wy ) ) * scale[2];
	out[9] = ( 2 * ( yz - wx ) ) * scale[2];
	out[10] = ( 1 - 2 * ( xx + yy ) ) * scale[2];
	out[11] = 0;
	out[12] = translation[0];
	out[13] = translation[1];
	out[14] = translation[2];
	out[15] = 1;
}

static void Mat4_TransformPoint( const float *matrix, const float *in, float *out ) {
	float	px = in[0], py = in[1], pz = in[2];

	out[0] = matrix[0] * px + matrix[4] * py + matrix[8] * pz + matrix[12];
	out[1] = matrix[1] * px + matrix[5] * py + matrix[9] * pz + matrix[13];
	out[2] = matrix[2] * px + matrix[6] * py + matrix[10] * pz + matrix[14];
}

static void Mat4_TransformVector( const float *matrix, const float *in, float *out ) {
	float	px = in[0], py = in[1], pz = in[2];

	out[0] = matrix[0] * px + matrix[4] * py + matrix[8] * pz;
	out[1] = matrix[1] * px + matrix[5] * py + matrix[9] * pz;
	out[2] = matrix[2] * px + matrix[6] * py + matrix[10] * pz;
}

// inverse transpose of the upper 3x3, stored as a 4x4 without translation
static void Mat4_NormalMatrix( const float *matrix, float *out ) {
	float	a = matrix[0], b = matrix[4], c = matrix[8];
	float	d = matrix[1], e = matrix[5], f = matrix[9];
	float	g = matrix[2], h = matrix[6], k = matrix[10];
	float	cofactor00 = e * k - f * h;
	float	cofactor01 = f * g - d * k;
	float	cofactor02 = d * h - e * g;
	float	determinant = a * cofactor00 + b * cofactor01 + c * cofactor02;
	float	inverseDet;

	Mat4_Identity( out );
	if ( fabs( determinant ) < 1e-12f ) {
		out[0] = a; out[4] = b; out[8] = c;
		out[1] = d; out[5] = e; out[9] = f;
		out[2] = g; out[6] = h; out[10] = k;
		return;
	}
	inverseDet = 1.0f / determinant;
	// (M^-1)^T equals the cofactor matrix divided by the determinant
	out[0] = cofactor00 * inverseDet;
	out[4] = cofactor01 * inverseDet;
	out[8] = cofactor02 * inverseDet;
	out[1] = ( c * h - b * k ) * inverseDet;
	out[5] = ( a * k - c * g ) * inverseDet;
	out[9] = ( b * g - a * h ) * inverseDet;
	out[2] = ( b * f - c * e ) * inverseDet;
	out[6] = ( c * d - a * f ) * inverseDet;
	out[10] = ( a * e - b * d ) * inverseDet;
}

static void Quat_Normalize( float *quat ) {
	float	length = (float)sqrt( quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3] );

	if ( length < 1e-12f ) {
		quat[0] = quat[1] = quat[2] = 0;
		quat[3] = 1;
		return;
	}
	quat[0] /= length;
	quat[1] /= length;
	quat[2] /= length;
	quat[3] /= length;
}

static void Quat_Slerp( const float *from, const float *to, float fraction, float *out ) {
	float	target[4];
	float	cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
	float	weightFrom, weightTo;
	int		i;

	for ( i = 0 ; i < 4 ; i++ ) {
		target[i] = to[i];
	}
	if ( cosine < 0 ) {
		cosine = -cosine;
		for ( i = 0 ; i < 4 ; i++ ) {
			target[i] = -target[i];
		}
	}
	if ( cosine > 0.9995f ) {
		weightFrom = 1.0f - fraction;
		weightTo = fraction;
	} else {
		float	angle = (float)acos( cosine );
		float	sine = (float)sin( angle );
		weightFrom = (float)sin( ( 1.0f - fraction ) * angle ) / sine;
		weightTo = (float)sin( fraction * angle ) / sine;
	}
	for ( i = 0 ; i < 4 ; i++ ) {
		out[i] = from[i] * weightFrom + target[i] * weightTo;
	}
	Quat_Normalize( out );
}

/*
==============================================================================

GLB DATA

==============================================================================
*/

typedef struct {
	char		name[MAX_QPATH];
	int			parent;
	int			mesh;
	int			skin;
	qboolean	hasMatrix;
	float		baseMatrix[16];
	float		baseTranslation[3];
	float		baseRotation[4];
	float		baseScale[3];
	float		translation[3];		// current pose
	float		rotation[4];
	float		scale[3];
	float		world[16];
	float		normalWorld[16];
	int			worldFrame;			// frame for which world[] is valid
} glbNode_t;

typedef struct {
	int			numJoints;
	int			*joints;			// node indices
	float		*inverseBind;		// numJoints * 16
	float		*jointMatrices;		// numJoints * 16, current frame
} glbSkin_t;

enum { GLB_PATH_TRANSLATION, GLB_PATH_ROTATION, GLB_PATH_SCALE };
enum { GLB_INTERP_LINEAR, GLB_INTERP_STEP, GLB_INTERP_CUBIC };

typedef struct {
	int			node;
	int			path;
	int			interpolation;
	int			numKeys;
	float		*times;
	float		*values;
	int			components;
} glbChannel_t;

typedef struct {
	char		name[64];
	float		startTime;
	float		endTime;
	float		fps;
	int			firstFrame;
	int			numFrames;
	int			firstChannel;
	int			numChannels;
} glbAnimation_t;

typedef struct {
	int			node;
	int			material;
	int			numVerts;
	float		*positions;
	float		*normals;
	float		*texcoords;
	float		*joints;			// 4 per vertex, or NULL
	float		*weights;			// 4 per vertex, or NULL
	int			numIndices;
	int			*indices;
} glbPrimitive_t;

typedef struct {
	int			material;
	int			numVerts;
	int			numTris;
	int			vertPrimitive[GLB_MAX_SURFACE_VERTS];
	int			vertIndex[GLB_MAX_SURFACE_VERTS];
	int			tris[GLB_MAX_SURFACE_TRIS * 3];
	char		name[MAX_QPATH];
	char		shader[MAX_QPATH];
} glbSurface_t;

typedef struct {
	glbMemory_t		memory;
	const char		*modelName;
	char			modelDir[MAX_QPATH];
	jsonNode_t		*root;
	const byte		*bin;
	int				binLength;
	float			scale;

	jsonNode_t		**accessors;	int numAccessors;
	jsonNode_t		**bufferViews;	int numBufferViews;
	jsonNode_t		**meshes;		int numMeshes;
	jsonNode_t		**materials;	int numMaterials;
	jsonNode_t		**textures;		int numTextures;
	jsonNode_t		**images;		int numImages;

	glbNode_t		*nodes;			int numNodes;
	glbSkin_t		*skins;			int numSkins;
	glbChannel_t	*channels;		int numChannels;
	glbAnimation_t	*animations;	int numAnimations;
	glbPrimitive_t	*primitives;	int numPrimitives;
	glbSurface_t	**surfaces;		int numSurfaces;
	int				*tagNodes;		int numTags;
	int				numFrames;
} glbContext_t;

static qboolean GLB_Fail( glbContext_t *ctx, const char *message ) {
	ri.Printf( PRINT_WARNING, "R_LoadGLB: %s: %s\n", ctx->modelName, message );
	return qfalse;
}

static jsonNode_t **GLB_CollectArray( glbContext_t *ctx, const char *key, int *count ) {
	jsonNode_t	*array = JSON_Get( ctx->root, key );
	jsonNode_t	**list, *item;
	int			index = 0;

	*count = 0;
	if ( !array || array->type != JSON_ARRAY || array->count == 0 ) {
		return NULL;
	}
	list = GLB_Alloc( &ctx->memory, array->count * sizeof( jsonNode_t * ) );
	for ( item = array->child ; item ; item = item->next ) {
		list[index++] = item;
	}
	*count = index;
	return list;
}

static int GLB_ComponentSize( int componentType ) {
	switch ( componentType ) {
	case GLB_COMP_BYTE:
	case GLB_COMP_UBYTE:
		return 1;
	case GLB_COMP_SHORT:
	case GLB_COMP_USHORT:
		return 2;
	case GLB_COMP_UINT:
	case GLB_COMP_FLOAT:
		return 4;
	}
	return 0;
}

static int GLB_TypeComponents( const char *type ) {
	if ( !strcmp( type, "SCALAR" ) ) return 1;
	if ( !strcmp( type, "VEC2" ) ) return 2;
	if ( !strcmp( type, "VEC3" ) ) return 3;
	if ( !strcmp( type, "VEC4" ) ) return 4;
	if ( !strcmp( type, "MAT4" ) ) return 16;
	return 0;
}

static float GLB_ReadComponent( const byte *data, int componentType, qboolean normalized ) {
	switch ( componentType ) {
	case GLB_COMP_FLOAT: {
		float	value;
		Com_Memcpy( &value, data, 4 );
		return LittleFloat( value );
	}
	case GLB_COMP_UBYTE:
		return normalized ? data[0] / 255.0f : (float)data[0];
	case GLB_COMP_BYTE: {
		float	value = (float)(signed char)data[0];
		if ( normalized ) {
			value /= 127.0f;
			return value < -1.0f ? -1.0f : value;
		}
		return value;
	}
	case GLB_COMP_USHORT: {
		unsigned short	value = (unsigned short)( data[0] | ( data[1] << 8 ) );
		return normalized ? value / 65535.0f : (float)value;
	}
	case GLB_COMP_SHORT: {
		float	value = (float)(short)( data[0] | ( data[1] << 8 ) );
		if ( normalized ) {
			value /= 32767.0f;
			return value < -1.0f ? -1.0f : value;
		}
		return value;
	}
	case GLB_COMP_UINT: {
		unsigned int	value = data[0] | ( data[1] << 8 ) | ( data[2] << 16 ) | ( (unsigned int)data[3] << 24 );
		return (float)value;
	}
	}
	return 0;
}

// Reads an accessor as floats.  components must match the accessor type.
// Returns NULL on failure (a warning has already been printed).
static float *GLB_ReadAccessor( glbContext_t *ctx, int accessorIndex, int components, int *outCount ) {
	jsonNode_t	*accessor, *view;
	int			componentType, count, typeComponents, elementSize, stride;
	int			viewIndex, viewOffset, viewLength, accessorOffset, element, component;
	qboolean	normalized;
	float		*result;
	char		message[128];

	*outCount = 0;
	if ( accessorIndex < 0 || accessorIndex >= ctx->numAccessors ) {
		GLB_Fail( ctx, "accessor index out of range" );
		return NULL;
	}
	accessor = ctx->accessors[accessorIndex];
	componentType = JSON_Int( JSON_Get( accessor, "componentType" ), 0 );
	count = JSON_Int( JSON_Get( accessor, "count" ), 0 );
	typeComponents = GLB_TypeComponents( JSON_String( JSON_Get( accessor, "type" ), "" ) );
	normalized = JSON_Bool( JSON_Get( accessor, "normalized" ), qfalse );

	if ( typeComponents != components || GLB_ComponentSize( componentType ) == 0 || count < 0 ) {
		Com_sprintf( message, sizeof( message ), "accessor %i has an unexpected type", accessorIndex );
		GLB_Fail( ctx, message );
		return NULL;
	}
	if ( JSON_Get( accessor, "sparse" ) ) {
		Com_sprintf( message, sizeof( message ), "accessor %i is sparse (not supported)", accessorIndex );
		GLB_Fail( ctx, message );
		return NULL;
	}
	result = GLB_Alloc( &ctx->memory, ( count ? count : 1 ) * components * sizeof( float ) );
	*outCount = count;

	viewIndex = JSON_Int( JSON_Get( accessor, "bufferView" ), -1 );
	if ( viewIndex < 0 ) {
		return result;		// all zeros, as the specification says
	}
	if ( viewIndex >= ctx->numBufferViews ) {
		GLB_Fail( ctx, "bufferView index out of range" );
		return NULL;
	}
	view = ctx->bufferViews[viewIndex];
	if ( JSON_Int( JSON_Get( view, "buffer" ), 0 ) != 0 ) {
		GLB_Fail( ctx, "only the embedded binary buffer is supported" );
		return NULL;
	}
	viewOffset = JSON_Int( JSON_Get( view, "byteOffset" ), 0 );
	viewLength = JSON_Int( JSON_Get( view, "byteLength" ), 0 );
	accessorOffset = JSON_Int( JSON_Get( accessor, "byteOffset" ), 0 );
	elementSize = GLB_ComponentSize( componentType ) * components;
	stride = JSON_Int( JSON_Get( view, "byteStride" ), 0 );
	if ( stride == 0 ) {
		stride = elementSize;
	}
	if ( count > 0 ) {
		int		lastByte = accessorOffset + stride * ( count - 1 ) + elementSize;
		if ( viewOffset < 0 || viewLength < 0 || accessorOffset < 0 || lastByte > viewLength
			|| viewOffset + viewLength > ctx->binLength ) {
			Com_sprintf( message, sizeof( message ), "accessor %i reads outside the binary chunk", accessorIndex );
			GLB_Fail( ctx, message );
			return NULL;
		}
	}
	for ( element = 0 ; element < count ; element++ ) {
		const byte	*source = ctx->bin + viewOffset + accessorOffset + element * stride;
		for ( component = 0 ; component < components ; component++ ) {
			result[element * components + component] = GLB_ReadComponent(
				source + component * GLB_ComponentSize( componentType ), componentType, normalized );
		}
	}
	return result;
}

/*
==============================================================================

SCENE SETUP

==============================================================================
*/

static void GLB_SanitizeName( const char *source, char *dest, int destSize ) {
	int		length = 0;

	while ( *source && length < destSize - 1 ) {
		char	ch = *source++;
		if ( ch >= 'A' && ch <= 'Z' ) {
			ch = ch - 'A' + 'a';
		}
		if ( !( ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-' ) ) {
			ch = '_';
		}
		dest[length++] = ch;
	}
	dest[length] = 0;
}

static qboolean GLB_SetupNodes( glbContext_t *ctx ) {
	jsonNode_t	**nodeList;
	int			index, child;

	nodeList = GLB_CollectArray( ctx, "nodes", &ctx->numNodes );
	if ( ctx->numNodes == 0 ) {
		return GLB_Fail( ctx, "no nodes" );
	}
	ctx->nodes = GLB_Alloc( &ctx->memory, ctx->numNodes * sizeof( glbNode_t ) );
	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		ctx->nodes[index].parent = -1;
	}
	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		jsonNode_t	*source = nodeList[index];
		glbNode_t	*node = &ctx->nodes[index];
		jsonNode_t	*children, *item;

		Q_strncpyz( node->name, JSON_String( JSON_Get( source, "name" ), "" ), sizeof( node->name ) );
		node->mesh = JSON_Int( JSON_Get( source, "mesh" ), -1 );
		node->skin = JSON_Int( JSON_Get( source, "skin" ), -1 );
		node->baseRotation[3] = 1;
		node->baseScale[0] = node->baseScale[1] = node->baseScale[2] = 1;
		JSON_Floats( JSON_Get( source, "translation" ), node->baseTranslation, 3 );
		JSON_Floats( JSON_Get( source, "rotation" ), node->baseRotation, 4 );
		JSON_Floats( JSON_Get( source, "scale" ), node->baseScale, 3 );
		if ( JSON_Floats( JSON_Get( source, "matrix" ), node->baseMatrix, 16 ) == 16 ) {
			node->hasMatrix = qtrue;
		}
		node->worldFrame = -1;

		children = JSON_Get( source, "children" );
		if ( children && children->type == JSON_ARRAY ) {
			for ( item = children->child ; item ; item = item->next ) {
				child = JSON_Int( item, -1 );
				if ( child < 0 || child >= ctx->numNodes || child == index || ctx->nodes[child].parent != -1 ) {
					return GLB_Fail( ctx, "invalid node hierarchy" );
				}
				ctx->nodes[child].parent = index;
			}
		}
	}
	// reject cycles
	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		int		walk = ctx->nodes[index].parent, steps = 0;
		while ( walk >= 0 ) {
			if ( ++steps > ctx->numNodes ) {
				return GLB_Fail( ctx, "node hierarchy has a cycle" );
			}
			walk = ctx->nodes[walk].parent;
		}
	}
	return qtrue;
}

static qboolean GLB_SetupSkins( glbContext_t *ctx ) {
	jsonNode_t	**skinList;
	int			index, joint;

	skinList = GLB_CollectArray( ctx, "skins", &ctx->numSkins );
	if ( ctx->numSkins == 0 ) {
		return qtrue;
	}
	ctx->skins = GLB_Alloc( &ctx->memory, ctx->numSkins * sizeof( glbSkin_t ) );
	for ( index = 0 ; index < ctx->numSkins ; index++ ) {
		glbSkin_t	*skin = &ctx->skins[index];
		jsonNode_t	*joints = JSON_Get( skinList[index], "joints" );
		jsonNode_t	*item;
		int			bindAccessor, bindCount;

		if ( !joints || joints->type != JSON_ARRAY || joints->count == 0 ) {
			return GLB_Fail( ctx, "skin without joints" );
		}
		skin->numJoints = joints->count;
		skin->joints = GLB_Alloc( &ctx->memory, skin->numJoints * sizeof( int ) );
		skin->jointMatrices = GLB_Alloc( &ctx->memory, skin->numJoints * 16 * sizeof( float ) );
		joint = 0;
		for ( item = joints->child ; item ; item = item->next ) {
			int		nodeIndex = JSON_Int( item, -1 );
			if ( nodeIndex < 0 || nodeIndex >= ctx->numNodes ) {
				return GLB_Fail( ctx, "skin joint index out of range" );
			}
			skin->joints[joint++] = nodeIndex;
		}
		bindAccessor = JSON_Int( JSON_Get( skinList[index], "inverseBindMatrices" ), -1 );
		if ( bindAccessor >= 0 ) {
			skin->inverseBind = GLB_ReadAccessor( ctx, bindAccessor, 16, &bindCount );
			if ( !skin->inverseBind ) {
				return qfalse;
			}
			if ( bindCount < skin->numJoints ) {
				return GLB_Fail( ctx, "too few inverse bind matrices" );
			}
		} else {
			skin->inverseBind = GLB_Alloc( &ctx->memory, skin->numJoints * 16 * sizeof( float ) );
			for ( joint = 0 ; joint < skin->numJoints ; joint++ ) {
				Mat4_Identity( skin->inverseBind + joint * 16 );
			}
		}
	}
	return qtrue;
}

static qboolean GLB_SetupAnimations( glbContext_t *ctx ) {
	jsonNode_t	**animationList;
	int			index, totalChannels = 0, frame = 0;

	animationList = GLB_CollectArray( ctx, "animations", &ctx->numAnimations );
	if ( ctx->numAnimations == 0 ) {
		ctx->numFrames = 1;
		return qtrue;
	}
	for ( index = 0 ; index < ctx->numAnimations ; index++ ) {
		jsonNode_t	*channels = JSON_Get( animationList[index], "channels" );
		if ( channels && channels->type == JSON_ARRAY ) {
			totalChannels += channels->count;
		}
	}
	ctx->animations = GLB_Alloc( &ctx->memory, ctx->numAnimations * sizeof( glbAnimation_t ) );
	ctx->channels = GLB_Alloc( &ctx->memory, ( totalChannels ? totalChannels : 1 ) * sizeof( glbChannel_t ) );

	for ( index = 0 ; index < ctx->numAnimations ; index++ ) {
		jsonNode_t		*source = animationList[index];
		jsonNode_t		*channels = JSON_Get( source, "channels" );
		jsonNode_t		*samplers = JSON_Get( source, "samplers" );
		glbAnimation_t	*animation = &ctx->animations[index];
		jsonNode_t		*item;
		qboolean		anyKeys = qfalse;
		float			fps;

		Q_strncpyz( animation->name, JSON_String( JSON_Get( source, "name" ), "" ), sizeof( animation->name ) );
		fps = (float)JSON_Number( JSON_Get( JSON_Get( source, "extras" ), "fps" ), GLB_DEFAULT_FPS );
		if ( fps <= 0 ) {
			fps = GLB_DEFAULT_FPS;
		}
		animation->fps = fps;
		animation->firstChannel = ctx->numChannels;

		for ( item = channels ? channels->child : NULL ; item ; item = item->next ) {
			jsonNode_t		*target = JSON_Get( item, "target" );
			const char		*path = JSON_String( JSON_Get( target, "path" ), "" );
			int				nodeIndex = JSON_Int( JSON_Get( target, "node" ), -1 );
			jsonNode_t		*sampler = JSON_Index( samplers, JSON_Int( JSON_Get( item, "sampler" ), -1 ) );
			const char		*interpolation = JSON_String( JSON_Get( sampler, "interpolation" ), "LINEAR" );
			glbChannel_t	*channel;
			int				pathIndex, components, timeCount, valueCount, expectedValues;

			if ( !strcmp( path, "translation" ) ) {
				pathIndex = GLB_PATH_TRANSLATION;
				components = 3;
			} else if ( !strcmp( path, "rotation" ) ) {
				pathIndex = GLB_PATH_ROTATION;
				components = 4;
			} else if ( !strcmp( path, "scale" ) ) {
				pathIndex = GLB_PATH_SCALE;
				components = 3;
			} else {
				continue;		// morph weights are ignored
			}
			if ( nodeIndex < 0 || nodeIndex >= ctx->numNodes || !sampler ) {
				return GLB_Fail( ctx, "invalid animation channel" );
			}
			channel = &ctx->channels[ctx->numChannels];
			channel->node = nodeIndex;
			channel->path = pathIndex;
			channel->components = components;
			if ( !strcmp( interpolation, "STEP" ) ) {
				channel->interpolation = GLB_INTERP_STEP;
			} else if ( !strcmp( interpolation, "CUBICSPLINE" ) ) {
				channel->interpolation = GLB_INTERP_CUBIC;
			} else {
				channel->interpolation = GLB_INTERP_LINEAR;
			}
			channel->times = GLB_ReadAccessor( ctx, JSON_Int( JSON_Get( sampler, "input" ), -1 ), 1, &timeCount );
			if ( !channel->times ) {
				return qfalse;
			}
			channel->values = GLB_ReadAccessor( ctx, JSON_Int( JSON_Get( sampler, "output" ), -1 ), components, &valueCount );
			if ( !channel->values ) {
				return qfalse;
			}
			expectedValues = ( channel->interpolation == GLB_INTERP_CUBIC ) ? timeCount * 3 : timeCount;
			if ( timeCount < 1 || valueCount < expectedValues ) {
				return GLB_Fail( ctx, "animation sampler has too few values" );
			}
			channel->numKeys = timeCount;
			if ( !anyKeys || channel->times[0] < animation->startTime ) {
				animation->startTime = channel->times[0];
			}
			if ( !anyKeys || channel->times[timeCount - 1] > animation->endTime ) {
				animation->endTime = channel->times[timeCount - 1];
			}
			anyKeys = qtrue;
			ctx->numChannels++;
		}
		animation->numChannels = ctx->numChannels - animation->firstChannel;
		animation->numFrames = (int)floor( ( animation->endTime - animation->startTime ) * fps + 0.5f ) + 1;
		if ( animation->numFrames < 1 ) {
			animation->numFrames = 1;
		}
		animation->firstFrame = frame;
		frame += animation->numFrames;
	}
	ctx->numFrames = frame;
	if ( ctx->numFrames > MD3_MAX_FRAMES ) {
		return GLB_Fail( ctx, va( "%i frames, the MD3 limit is %i", ctx->numFrames, MD3_MAX_FRAMES ) );
	}
	return qtrue;
}

static void GLB_ComputeNormals( glbPrimitive_t *primitive ) {
	int		tri, vert;

	for ( tri = 0 ; tri + 2 < primitive->numIndices ; tri += 3 ) {
		float	*p0 = primitive->positions + primitive->indices[tri] * 3;
		float	*p1 = primitive->positions + primitive->indices[tri + 1] * 3;
		float	*p2 = primitive->positions + primitive->indices[tri + 2] * 3;
		vec3_t	edge1, edge2, normal;
		int		corner;

		VectorSubtract( p1, p0, edge1 );
		VectorSubtract( p2, p0, edge2 );
		CrossProduct( edge1, edge2, normal );
		for ( corner = 0 ; corner < 3 ; corner++ ) {
			float	*out = primitive->normals + primitive->indices[tri + corner] * 3;
			VectorAdd( out, normal, out );
		}
	}
	for ( vert = 0 ; vert < primitive->numVerts ; vert++ ) {
		float	*normal = primitive->normals + vert * 3;
		if ( VectorNormalize( normal ) == 0 ) {
			normal[1] = 1;
		}
	}
}

// Merges vertices whose attributes are bit-identical (many exporters write
// one vertex per triangle corner) and rewrites the index list.
static void GLB_WeldPrimitive( glbContext_t *ctx, glbPrimitive_t *primitive ) {
	int			numVerts = primitive->numVerts;
	int			tableSize = 1, vert, index, newCount = 0, attribute;
	int			*table, *chain, *newIndex;
	float		*attributes[5];
	int			sizes[5];

	attributes[0] = primitive->positions;	sizes[0] = 3;
	attributes[1] = primitive->normals;		sizes[1] = 3;
	attributes[2] = primitive->texcoords;	sizes[2] = 2;
	attributes[3] = primitive->joints;		sizes[3] = 4;
	attributes[4] = primitive->weights;		sizes[4] = 4;

	while ( tableSize < numVerts * 2 ) {
		tableSize <<= 1;
	}
	table = GLB_Alloc( &ctx->memory, tableSize * sizeof( int ) );
	chain = GLB_Alloc( &ctx->memory, numVerts * sizeof( int ) );
	newIndex = GLB_Alloc( &ctx->memory, numVerts * sizeof( int ) );
	for ( index = 0 ; index < tableSize ; index++ ) {
		table[index] = -1;
	}

	for ( vert = 0 ; vert < numVerts ; vert++ ) {
		unsigned int	hash = 2166136261u;
		int				candidate, slot;

		for ( attribute = 0 ; attribute < 5 ; attribute++ ) {
			const byte	*bytes;
			int			count;
			if ( !attributes[attribute] ) {
				continue;
			}
			bytes = (const byte *)( attributes[attribute] + vert * sizes[attribute] );
			for ( count = 0 ; count < sizes[attribute] * (int)sizeof( float ) ; count++ ) {
				hash = ( hash ^ bytes[count] ) * 16777619u;
			}
		}
		slot = hash & ( tableSize - 1 );
		for ( candidate = table[slot] ; candidate >= 0 ; candidate = chain[candidate] ) {
			qboolean	same = qtrue;
			// kept vertices have already been moved to their compacted slot
			for ( attribute = 0 ; attribute < 5 && same ; attribute++ ) {
				if ( attributes[attribute] && memcmp( attributes[attribute] + newIndex[candidate] * sizes[attribute],
					attributes[attribute] + vert * sizes[attribute], sizes[attribute] * sizeof( float ) ) ) {
					same = qfalse;
				}
			}
			if ( same ) {
				break;
			}
		}
		if ( candidate >= 0 ) {
			newIndex[vert] = newIndex[candidate];
			continue;
		}
		// keep this vertex; chain stores original indices of kept vertices
		chain[vert] = table[slot];
		table[slot] = vert;
		newIndex[vert] = newCount;
		// compact in place: newCount <= vert, so nothing unread is overwritten
		for ( attribute = 0 ; attribute < 5 ; attribute++ ) {
			if ( attributes[attribute] && newCount != vert ) {
				Com_Memcpy( attributes[attribute] + newCount * sizes[attribute],
					attributes[attribute] + vert * sizes[attribute], sizes[attribute] * sizeof( float ) );
			}
		}
		newCount++;
	}
	for ( index = 0 ; index < primitive->numIndices ; index++ ) {
		primitive->indices[index] = newIndex[primitive->indices[index]];
	}
	primitive->numVerts = newCount;
}

static qboolean GLB_SetupPrimitives( glbContext_t *ctx ) {
	int		nodeIndex, total = 0;

	// count
	for ( nodeIndex = 0 ; nodeIndex < ctx->numNodes ; nodeIndex++ ) {
		int			meshIndex = ctx->nodes[nodeIndex].mesh;
		jsonNode_t	*primitives;

		if ( meshIndex < 0 ) {
			continue;
		}
		if ( meshIndex >= ctx->numMeshes ) {
			return GLB_Fail( ctx, "mesh index out of range" );
		}
		primitives = JSON_Get( ctx->meshes[meshIndex], "primitives" );
		if ( primitives && primitives->type == JSON_ARRAY ) {
			total += primitives->count;
		}
	}
	if ( total == 0 ) {
		return GLB_Fail( ctx, "no meshes" );
	}
	ctx->primitives = GLB_Alloc( &ctx->memory, total * sizeof( glbPrimitive_t ) );

	for ( nodeIndex = 0 ; nodeIndex < ctx->numNodes ; nodeIndex++ ) {
		int			meshIndex = ctx->nodes[nodeIndex].mesh;
		jsonNode_t	*primitiveList, *item;

		if ( meshIndex < 0 ) {
			continue;
		}
		primitiveList = JSON_Get( ctx->meshes[meshIndex], "primitives" );
		if ( !primitiveList || primitiveList->type != JSON_ARRAY ) {
			continue;
		}
		for ( item = primitiveList->child ; item ; item = item->next ) {
			jsonNode_t		*attributes = JSON_Get( item, "attributes" );
			glbPrimitive_t	*primitive = &ctx->primitives[ctx->numPrimitives];
			int				mode = JSON_Int( JSON_Get( item, "mode" ), 4 );
			int				positionAccessor = JSON_Int( JSON_Get( attributes, "POSITION" ), -1 );
			int				normalAccessor = JSON_Int( JSON_Get( attributes, "NORMAL" ), -1 );
			int				uvAccessor = JSON_Int( JSON_Get( attributes, "TEXCOORD_0" ), -1 );
			int				jointAccessor = JSON_Int( JSON_Get( attributes, "JOINTS_0" ), -1 );
			int				weightAccessor = JSON_Int( JSON_Get( attributes, "WEIGHTS_0" ), -1 );
			int				indexAccessor = JSON_Int( JSON_Get( item, "indices" ), -1 );
			int				count, index;

			if ( mode != 4 ) {
				ri.Printf( PRINT_DEVELOPER, "R_LoadGLB: %s: skipping a primitive with mode %i (only triangles)\n",
					ctx->modelName, mode );
				continue;
			}
			if ( positionAccessor < 0 ) {
				continue;
			}
			primitive->node = nodeIndex;
			primitive->material = JSON_Int( JSON_Get( item, "material" ), -1 );
			if ( primitive->material >= ctx->numMaterials ) {
				primitive->material = -1;
			}
			primitive->positions = GLB_ReadAccessor( ctx, positionAccessor, 3, &primitive->numVerts );
			if ( !primitive->positions ) {
				return qfalse;
			}
			if ( primitive->numVerts == 0 ) {
				continue;
			}
			if ( uvAccessor >= 0 ) {
				primitive->texcoords = GLB_ReadAccessor( ctx, uvAccessor, 2, &count );
				if ( !primitive->texcoords || count < primitive->numVerts ) {
					return GLB_Fail( ctx, "TEXCOORD_0 is shorter than POSITION" );
				}
			} else {
				primitive->texcoords = GLB_Alloc( &ctx->memory, primitive->numVerts * 2 * sizeof( float ) );
			}
			if ( indexAccessor >= 0 ) {
				float	*indexFloats = GLB_ReadAccessor( ctx, indexAccessor, 1, &primitive->numIndices );
				if ( !indexFloats ) {
					return qfalse;
				}
				primitive->indices = GLB_Alloc( &ctx->memory, ( primitive->numIndices + 1 ) * sizeof( int ) );
				for ( index = 0 ; index < primitive->numIndices ; index++ ) {
					primitive->indices[index] = (int)indexFloats[index];
					if ( primitive->indices[index] < 0 || primitive->indices[index] >= primitive->numVerts ) {
						return GLB_Fail( ctx, "triangle index out of range" );
					}
				}
			} else {
				primitive->numIndices = primitive->numVerts;
				primitive->indices = GLB_Alloc( &ctx->memory, ( primitive->numIndices + 1 ) * sizeof( int ) );
				for ( index = 0 ; index < primitive->numIndices ; index++ ) {
					primitive->indices[index] = index;
				}
			}
			primitive->numIndices -= primitive->numIndices % 3;
			if ( normalAccessor >= 0 ) {
				primitive->normals = GLB_ReadAccessor( ctx, normalAccessor, 3, &count );
				if ( !primitive->normals || count < primitive->numVerts ) {
					return GLB_Fail( ctx, "NORMAL is shorter than POSITION" );
				}
			}
			if ( jointAccessor >= 0 && weightAccessor >= 0 && ctx->nodes[nodeIndex].skin >= 0 ) {
				if ( ctx->nodes[nodeIndex].skin >= ctx->numSkins ) {
					return GLB_Fail( ctx, "skin index out of range" );
				}
				primitive->joints = GLB_ReadAccessor( ctx, jointAccessor, 4, &count );
				if ( !primitive->joints || count < primitive->numVerts ) {
					return GLB_Fail( ctx, "JOINTS_0 is shorter than POSITION" );
				}
				primitive->weights = GLB_ReadAccessor( ctx, weightAccessor, 4, &count );
				if ( !primitive->weights || count < primitive->numVerts ) {
					return GLB_Fail( ctx, "WEIGHTS_0 is shorter than POSITION" );
				}
				for ( index = 0 ; index < primitive->numVerts * 4 ; index++ ) {
					int		joint = (int)primitive->joints[index];
					if ( joint < 0 || joint >= ctx->skins[ctx->nodes[nodeIndex].skin].numJoints ) {
						return GLB_Fail( ctx, "joint index out of range" );
					}
				}
			}
			GLB_WeldPrimitive( ctx, primitive );
			if ( !primitive->normals ) {
				primitive->normals = GLB_Alloc( &ctx->memory, primitive->numVerts * 3 * sizeof( float ) );
				GLB_ComputeNormals( primitive );
			}
			ctx->numPrimitives++;
		}
	}
	if ( ctx->numPrimitives == 0 ) {
		return GLB_Fail( ctx, "no triangle primitives" );
	}
	return qtrue;
}

static void GLB_SurfaceNames( glbContext_t *ctx, int materialIndex, char *surfaceName, char *shaderName ) {
	char		materialName[MAX_QPATH];
	char		uriName[MAX_QPATH];
	jsonNode_t	*material = ( materialIndex >= 0 ) ? ctx->materials[materialIndex] : NULL;
	const char	*customShader;

	if ( material ) {
		GLB_SanitizeName( JSON_String( JSON_Get( material, "name" ), va( "material%i", materialIndex ) ),
			materialName, sizeof( materialName ) );
	} else {
		Q_strncpyz( materialName, "default", sizeof( materialName ) );
	}
	if ( !materialName[0] ) {
		Com_sprintf( materialName, sizeof( materialName ), "material%i", materialIndex );
	}
	Q_strncpyz( surfaceName, materialName, MAX_QPATH );

	// 1. explicit shader name
	customShader = JSON_String( JSON_Get( JSON_Get( material, "extras" ), "q3_shader" ), NULL );
	if ( customShader && customShader[0] ) {
		Q_strncpyz( shaderName, customShader, MAX_QPATH );
		return;
	}

	// 2. external base colour image
	if ( material ) {
		jsonNode_t	*baseTexture = JSON_Get( JSON_Get( material, "pbrMetallicRoughness" ), "baseColorTexture" );
		int			textureIndex = JSON_Int( JSON_Get( baseTexture, "index" ), -1 );
		if ( textureIndex >= 0 && textureIndex < ctx->numTextures ) {
			int		imageIndex = JSON_Int( JSON_Get( ctx->textures[textureIndex], "source" ), -1 );
			if ( imageIndex >= 0 && imageIndex < ctx->numImages ) {
				const char	*uri = JSON_String( JSON_Get( ctx->images[imageIndex], "uri" ), NULL );
				if ( uri && Q_strncmp( uri, "data:", 5 ) && !strchr( uri, ':' ) && !strstr( uri, ".." ) ) {
					COM_StripExtension( uri, uriName );
					Com_sprintf( shaderName, MAX_QPATH, "%s/%s", ctx->modelDir, uriName );
					return;
				}
				ri.Printf( PRINT_DEVELOPER, "R_LoadGLB: %s: embedded image of material '%s' is ignored, "
					"put %s/%s.tga or .jpg next to the model\n", ctx->modelName, materialName, ctx->modelDir, materialName );
			}
		}
	}

	// 3. material name inside the model directory
	Com_sprintf( shaderName, MAX_QPATH, "%s/%s", ctx->modelDir, materialName );
}

// merges primitives by material and splits the result into surfaces
static qboolean GLB_BuildSurfaces( glbContext_t *ctx ) {
	int				*remap = NULL;
	int				remapSize = 0;
	int				primitiveIndex, materialPass;
	int				*materialOrder;
	int				numMaterialsUsed = 0;

	ctx->surfaces = GLB_Alloc( &ctx->memory, GLB_MAX_SURFACES * sizeof( glbSurface_t * ) );
	materialOrder = GLB_Alloc( &ctx->memory, ( ctx->numPrimitives + 1 ) * sizeof( int ) );

	// materials in order of first use
	for ( primitiveIndex = 0 ; primitiveIndex < ctx->numPrimitives ; primitiveIndex++ ) {
		int		material = ctx->primitives[primitiveIndex].material, known;
		for ( known = 0 ; known < numMaterialsUsed ; known++ ) {
			if ( materialOrder[known] == material ) {
				break;
			}
		}
		if ( known == numMaterialsUsed ) {
			materialOrder[numMaterialsUsed++] = material;
		}
		if ( ctx->primitives[primitiveIndex].numVerts > remapSize ) {
			remapSize = ctx->primitives[primitiveIndex].numVerts;
		}
	}
	remap = GLB_Alloc( &ctx->memory, remapSize * sizeof( int ) );

	for ( materialPass = 0 ; materialPass < numMaterialsUsed ; materialPass++ ) {
		int				material = materialOrder[materialPass];
		glbSurface_t	*surface = NULL;

		for ( primitiveIndex = 0 ; primitiveIndex < ctx->numPrimitives ; primitiveIndex++ ) {
			glbPrimitive_t	*primitive = &ctx->primitives[primitiveIndex];
			int				tri, vert;

			if ( primitive->material != material ) {
				continue;
			}
			for ( vert = 0 ; vert < primitive->numVerts ; vert++ ) {
				remap[vert] = -1;
			}
			for ( tri = 0 ; tri < primitive->numIndices ; tri += 3 ) {
				int		corner, newVerts = 0;
				int		*corners = primitive->indices + tri;

				if ( corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2] ) {
					continue;		// degenerate
				}
				if ( surface ) {
					for ( corner = 0 ; corner < 3 ; corner++ ) {
						if ( remap[corners[corner]] < 0 ) {
							newVerts++;
						}
					}
				}
				if ( !surface || surface->numVerts + newVerts > GLB_MAX_SURFACE_VERTS
					|| surface->numTris + 1 > GLB_MAX_SURFACE_TRIS ) {
					if ( ctx->numSurfaces >= GLB_MAX_SURFACES ) {
						return GLB_Fail( ctx, "too many surfaces" );
					}
					surface = GLB_Alloc( &ctx->memory, sizeof( glbSurface_t ) );
					surface->material = material;
					GLB_SurfaceNames( ctx, material, surface->name, surface->shader );
					ctx->surfaces[ctx->numSurfaces++] = surface;
					for ( vert = 0 ; vert < primitive->numVerts ; vert++ ) {
						remap[vert] = -1;
					}
				}
				for ( corner = 0 ; corner < 3 ; corner++ ) {
					int		source = corners[corner];
					if ( remap[source] < 0 ) {
						remap[source] = surface->numVerts;
						surface->vertPrimitive[surface->numVerts] = primitiveIndex;
						surface->vertIndex[surface->numVerts] = source;
						surface->numVerts++;
					}
				}
				// reverse the winding: glTF is counter-clockwise, Quake is clockwise
				surface->tris[surface->numTris * 3 + 0] = remap[corners[0]];
				surface->tris[surface->numTris * 3 + 1] = remap[corners[2]];
				surface->tris[surface->numTris * 3 + 2] = remap[corners[1]];
				surface->numTris++;
			}
		}
	}
	if ( ctx->numSurfaces == 0 ) {
		return GLB_Fail( ctx, "no triangles" );
	}
	if ( ctx->numSurfaces > MD3_MAX_SURFACES ) {
		ri.Printf( PRINT_WARNING, "R_LoadGLB: %s: %i surfaces (MD3 tools expect at most %i)\n",
			ctx->modelName, ctx->numSurfaces, MD3_MAX_SURFACES );
	}
	return qtrue;
}

static void GLB_SetupTags( glbContext_t *ctx ) {
	int		index;

	ctx->tagNodes = GLB_Alloc( &ctx->memory, MD3_MAX_TAGS * sizeof( int ) );
	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		if ( Q_stricmpn( ctx->nodes[index].name, "tag_", 4 ) ) {
			continue;
		}
		if ( ctx->numTags >= MD3_MAX_TAGS ) {
			ri.Printf( PRINT_WARNING, "R_LoadGLB: %s: more than %i tags, '%s' ignored\n",
				ctx->modelName, MD3_MAX_TAGS, ctx->nodes[index].name );
			continue;
		}
		ctx->tagNodes[ctx->numTags++] = index;
	}
}

/*
==============================================================================

POSE EVALUATION

==============================================================================
*/

static void GLB_SampleChannel( const glbChannel_t *channel, float time, float *out ) {
	int		components = channel->components;
	int		last = channel->numKeys - 1;
	int		low, high, key, i;
	float	t0, t1, fraction;
	qboolean cubic = ( channel->interpolation == GLB_INTERP_CUBIC );

	#define KEY_VALUE( k ) ( channel->values + ( cubic ? ( (k) * 3 + 1 ) : (k) ) * components )

	if ( last == 0 || time <= channel->times[0] ) {
		Com_Memcpy( out, KEY_VALUE( 0 ), components * sizeof( float ) );
		return;
	}
	if ( time >= channel->times[last] ) {
		Com_Memcpy( out, KEY_VALUE( last ), components * sizeof( float ) );
		return;
	}
	low = 0;
	high = last;
	while ( high - low > 1 ) {
		int		middle = ( low + high ) / 2;
		if ( channel->times[middle] <= time ) {
			low = middle;
		} else {
			high = middle;
		}
	}
	key = low;
	t0 = channel->times[key];
	t1 = channel->times[key + 1];
	fraction = ( t1 > t0 ) ? ( time - t0 ) / ( t1 - t0 ) : 0;

	if ( channel->interpolation == GLB_INTERP_STEP ) {
		Com_Memcpy( out, KEY_VALUE( key ), components * sizeof( float ) );
		return;
	}
	if ( cubic ) {
		float	duration = t1 - t0;
		float	s2 = fraction * fraction, s3 = s2 * fraction;
		float	h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + fraction;
		float	h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
		const float	*value0 = channel->values + ( key * 3 + 1 ) * components;
		const float	*outTangent0 = channel->values + ( key * 3 + 2 ) * components;
		const float	*value1 = channel->values + ( ( key + 1 ) * 3 + 1 ) * components;
		const float	*inTangent1 = channel->values + ( ( key + 1 ) * 3 + 0 ) * components;
		for ( i = 0 ; i < components ; i++ ) {
			out[i] = h00 * value0[i] + h10 * duration * outTangent0[i] + h01 * value1[i] + h11 * duration * inTangent1[i];
		}
		if ( channel->path == GLB_PATH_ROTATION ) {
			Quat_Normalize( out );
		}
		return;
	}
	if ( channel->path == GLB_PATH_ROTATION ) {
		Quat_Slerp( KEY_VALUE( key ), KEY_VALUE( key + 1 ), fraction, out );
		return;
	}
	for ( i = 0 ; i < components ; i++ ) {
		out[i] = KEY_VALUE( key )[i] + ( KEY_VALUE( key + 1 )[i] - KEY_VALUE( key )[i] ) * fraction;
	}
	#undef KEY_VALUE
}

static void GLB_ComputeWorld( glbContext_t *ctx, int nodeIndex, int frame ) {
	glbNode_t	*node = &ctx->nodes[nodeIndex];
	float		local[16];

	if ( node->worldFrame == frame ) {
		return;
	}
	if ( node->hasMatrix ) {
		Com_Memcpy( local, node->baseMatrix, sizeof( local ) );
	} else {
		Mat4_FromTRS( node->translation, node->rotation, node->scale, local );
	}
	if ( node->parent >= 0 ) {
		GLB_ComputeWorld( ctx, node->parent, frame );
		Mat4_Multiply( ctx->nodes[node->parent].world, local, node->world );
	} else {
		Com_Memcpy( node->world, local, sizeof( local ) );
	}
	Mat4_NormalMatrix( node->world, node->normalWorld );
	node->worldFrame = frame;
}

static void GLB_EvaluateFrame( glbContext_t *ctx, int frame ) {
	int				index, channelIndex;
	glbAnimation_t	*animation = NULL;

	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		glbNode_t	*node = &ctx->nodes[index];
		VectorCopy( node->baseTranslation, node->translation );
		Com_Memcpy( node->rotation, node->baseRotation, sizeof( node->rotation ) );
		VectorCopy( node->baseScale, node->scale );
	}
	for ( index = 0 ; index < ctx->numAnimations ; index++ ) {
		if ( frame >= ctx->animations[index].firstFrame
			&& frame < ctx->animations[index].firstFrame + ctx->animations[index].numFrames ) {
			animation = &ctx->animations[index];
			break;
		}
	}
	if ( animation ) {
		float	time = animation->startTime + ( frame - animation->firstFrame ) / animation->fps;
		if ( time > animation->endTime ) {
			time = animation->endTime;
		}
		for ( channelIndex = 0 ; channelIndex < animation->numChannels ; channelIndex++ ) {
			glbChannel_t	*channel = &ctx->channels[animation->firstChannel + channelIndex];
			glbNode_t		*node = &ctx->nodes[channel->node];
			float			value[4];

			GLB_SampleChannel( channel, time, value );
			if ( channel->path == GLB_PATH_TRANSLATION ) {
				VectorCopy( value, node->translation );
			} else if ( channel->path == GLB_PATH_ROTATION ) {
				Com_Memcpy( node->rotation, value, sizeof( node->rotation ) );
				Quat_Normalize( node->rotation );
			} else {
				VectorCopy( value, node->scale );
			}
		}
	}
	for ( index = 0 ; index < ctx->numNodes ; index++ ) {
		GLB_ComputeWorld( ctx, index, frame );
	}
	for ( index = 0 ; index < ctx->numSkins ; index++ ) {
		glbSkin_t	*skin = &ctx->skins[index];
		int			joint;
		for ( joint = 0 ; joint < skin->numJoints ; joint++ ) {
			Mat4_Multiply( ctx->nodes[skin->joints[joint]].world, skin->inverseBind + joint * 16,
				skin->jointMatrices + joint * 16 );
		}
	}
}

// glTF (x left, y up, z forward) -> Quake (x forward, y left, z up)
static void GLB_ToQuake( const float *in, float scale, float *out ) {
	out[0] = in[2] * scale;
	out[1] = in[0] * scale;
	out[2] = in[1] * scale;
}

static void GLB_VertexPose( glbContext_t *ctx, const glbPrimitive_t *primitive, int vert, float *position, float *normal ) {
	const float	*sourcePosition = primitive->positions + vert * 3;
	const float	*sourceNormal = primitive->normals + vert * 3;
	float		worldPosition[3], worldNormal[3];

	if ( primitive->joints ) {
		glbSkin_t	*skin = &ctx->skins[ctx->nodes[primitive->node].skin];
		float		blend[16], weightSum = 0, normalBlend[16];
		int			influence, element;

		Com_Memset( blend, 0, sizeof( blend ) );
		for ( influence = 0 ; influence < 4 ; influence++ ) {
			float	weight = primitive->weights[vert * 4 + influence];
			int		joint = (int)primitive->joints[vert * 4 + influence];
			if ( weight <= 0 ) {
				continue;
			}
			for ( element = 0 ; element < 16 ; element++ ) {
				blend[element] += skin->jointMatrices[joint * 16 + element] * weight;
			}
			weightSum += weight;
		}
		if ( weightSum <= 0 ) {
			Com_Memcpy( blend, ctx->nodes[primitive->node].world, sizeof( blend ) );
		} else if ( weightSum != 1.0f ) {
			for ( element = 0 ; element < 16 ; element++ ) {
				blend[element] /= weightSum;
			}
		}
		Mat4_TransformPoint( blend, sourcePosition, worldPosition );
		Mat4_NormalMatrix( blend, normalBlend );
		Mat4_TransformVector( normalBlend, sourceNormal, worldNormal );
	} else {
		glbNode_t	*node = &ctx->nodes[primitive->node];
		Mat4_TransformPoint( node->world, sourcePosition, worldPosition );
		Mat4_TransformVector( node->normalWorld, sourceNormal, worldNormal );
	}
	GLB_ToQuake( worldPosition, ctx->scale, position );
	GLB_ToQuake( worldNormal, 1.0f, normal );
	if ( VectorNormalize( normal ) == 0 ) {
		normal[2] = 1;
	}
}

static short GLB_EncodeNormal( const float *normal ) {
	float	azimuth, polar;
	int		latIndex, lngIndex;
	float	z = normal[2];

	if ( z > 1 ) z = 1;
	if ( z < -1 ) z = -1;
	// decoded by the renderer as x = cos(lat) * sin(lng), y = sin(lat) * sin(lng), z = cos(lng)
	polar = (float)acos( z );
	azimuth = (float)atan2( normal[1], normal[0] );
	lngIndex = (int)floor( polar * 256.0f / ( 2.0f * M_PI ) + 0.5f ) & 0xff;
	latIndex = (int)floor( azimuth * 256.0f / ( 2.0f * M_PI ) + 0.5f ) & 0xff;
	return (short)( ( latIndex << 8 ) | lngIndex );
}

static short GLB_EncodeCoord( float value ) {
	float	scaled = value / MD3_XYZ_SCALE;

	if ( scaled > 32767.0f ) scaled = 32767.0f;
	if ( scaled < -32768.0f ) scaled = -32768.0f;
	return (short)floor( scaled + 0.5f );
}

/*
==============================================================================

MD3 IMAGE

==============================================================================
*/

static void *GLB_WriteMD3( glbContext_t *ctx, int *outSize ) {
	int				size, surfaceIndex, frame, tagIndex, vert, i;
	int				ofsFrames, ofsTags, ofsSurfaces;
	byte			*buffer;
	md3Header_t		*header;
	md3Frame_t		*frames;
	md3Tag_t		*tags;
	byte			*surfaceCursor;
	qboolean		coordsClamped = qfalse;

	ofsFrames = sizeof( md3Header_t );
	ofsTags = ofsFrames + ctx->numFrames * sizeof( md3Frame_t );
	ofsSurfaces = ofsTags + ctx->numFrames * ctx->numTags * sizeof( md3Tag_t );
	size = ofsSurfaces;
	for ( surfaceIndex = 0 ; surfaceIndex < ctx->numSurfaces ; surfaceIndex++ ) {
		glbSurface_t	*surface = ctx->surfaces[surfaceIndex];
		size += sizeof( md3Surface_t ) + sizeof( md3Shader_t ) + surface->numTris * sizeof( md3Triangle_t )
			+ surface->numVerts * sizeof( md3St_t ) + surface->numVerts * ctx->numFrames * sizeof( md3XyzNormal_t );
	}

	buffer = GLB_Alloc( &ctx->memory, size );
	header = (md3Header_t *)buffer;
	frames = (md3Frame_t *)( buffer + ofsFrames );
	tags = (md3Tag_t *)( buffer + ofsTags );

	header->ident = LittleLong( MD3_IDENT );
	header->version = LittleLong( MD3_VERSION );
	Q_strncpyz( header->name, ctx->modelName, sizeof( header->name ) );
	header->numFrames = LittleLong( ctx->numFrames );
	header->numTags = LittleLong( ctx->numTags );
	header->numSurfaces = LittleLong( ctx->numSurfaces );
	header->numSkins = 0;
	header->ofsFrames = LittleLong( ofsFrames );
	header->ofsTags = LittleLong( ofsTags );
	header->ofsSurfaces = LittleLong( ofsSurfaces );
	header->ofsEnd = LittleLong( size );

	// surface headers and static data
	surfaceCursor = buffer + ofsSurfaces;
	for ( surfaceIndex = 0 ; surfaceIndex < ctx->numSurfaces ; surfaceIndex++ ) {
		glbSurface_t	*surface = ctx->surfaces[surfaceIndex];
		md3Surface_t	*out = (md3Surface_t *)surfaceCursor;
		md3Shader_t		*shader;
		md3Triangle_t	*triangles;
		md3St_t			*st;
		int				ofsShaders = sizeof( md3Surface_t );
		int				ofsTriangles = ofsShaders + sizeof( md3Shader_t );
		int				ofsSt = ofsTriangles + surface->numTris * sizeof( md3Triangle_t );
		int				ofsXyz = ofsSt + surface->numVerts * sizeof( md3St_t );
		int				ofsEnd = ofsXyz + surface->numVerts * ctx->numFrames * sizeof( md3XyzNormal_t );

		out->ident = LittleLong( MD3_IDENT );
		Q_strncpyz( out->name, surface->name, sizeof( out->name ) );
		out->flags = 0;
		out->numFrames = LittleLong( ctx->numFrames );
		out->numShaders = LittleLong( 1 );
		out->numVerts = LittleLong( surface->numVerts );
		out->numTriangles = LittleLong( surface->numTris );
		out->ofsTriangles = LittleLong( ofsTriangles );
		out->ofsShaders = LittleLong( ofsShaders );
		out->ofsSt = LittleLong( ofsSt );
		out->ofsXyzNormals = LittleLong( ofsXyz );
		out->ofsEnd = LittleLong( ofsEnd );

		shader = (md3Shader_t *)( surfaceCursor + ofsShaders );
		Q_strncpyz( shader->name, surface->shader, sizeof( shader->name ) );

		triangles = (md3Triangle_t *)( surfaceCursor + ofsTriangles );
		for ( i = 0 ; i < surface->numTris ; i++ ) {
			triangles[i].indexes[0] = LittleLong( surface->tris[i * 3 + 0] );
			triangles[i].indexes[1] = LittleLong( surface->tris[i * 3 + 1] );
			triangles[i].indexes[2] = LittleLong( surface->tris[i * 3 + 2] );
		}
		st = (md3St_t *)( surfaceCursor + ofsSt );
		for ( vert = 0 ; vert < surface->numVerts ; vert++ ) {
			glbPrimitive_t	*primitive = &ctx->primitives[surface->vertPrimitive[vert]];
			st[vert].st[0] = LittleFloat( primitive->texcoords[surface->vertIndex[vert] * 2 + 0] );
			st[vert].st[1] = LittleFloat( primitive->texcoords[surface->vertIndex[vert] * 2 + 1] );
		}
		surfaceCursor += ofsEnd;
	}

	// per frame data
	for ( frame = 0 ; frame < ctx->numFrames ; frame++ ) {
		vec3_t		mins, maxs;
		float		radius = 0;

		GLB_EvaluateFrame( ctx, frame );
		ClearBounds( mins, maxs );

		surfaceCursor = buffer + ofsSurfaces;
		for ( surfaceIndex = 0 ; surfaceIndex < ctx->numSurfaces ; surfaceIndex++ ) {
			glbSurface_t	*surface = ctx->surfaces[surfaceIndex];
			md3Surface_t	*out = (md3Surface_t *)surfaceCursor;
			md3XyzNormal_t	*xyz = (md3XyzNormal_t *)( surfaceCursor + LittleLong( out->ofsXyzNormals ) )
				+ frame * surface->numVerts;

			for ( vert = 0 ; vert < surface->numVerts ; vert++ ) {
				float	position[3], normal[3];
				float	length;

				GLB_VertexPose( ctx, &ctx->primitives[surface->vertPrimitive[vert]], surface->vertIndex[vert], position, normal );
				for ( i = 0 ; i < 3 ; i++ ) {
					if ( position[i] > 511.0f || position[i] < -512.0f ) {
						coordsClamped = qtrue;
					}
					xyz[vert].xyz[i] = LittleShort( GLB_EncodeCoord( position[i] ) );
				}
				xyz[vert].normal = LittleShort( GLB_EncodeNormal( normal ) );
				AddPointToBounds( position, mins, maxs );
				length = VectorLength( position );
				if ( length > radius ) {
					radius = length;
				}
			}
			surfaceCursor += LittleLong( out->ofsEnd );
		}

		for ( i = 0 ; i < 3 ; i++ ) {
			frames[frame].bounds[0][i] = LittleFloat( mins[i] );
			frames[frame].bounds[1][i] = LittleFloat( maxs[i] );
			frames[frame].localOrigin[i] = 0;
		}
		frames[frame].radius = LittleFloat( radius );
		Q_strncpyz( frames[frame].name, "glb", sizeof( frames[frame].name ) );

		for ( tagIndex = 0 ; tagIndex < ctx->numTags ; tagIndex++ ) {
			glbNode_t	*node = &ctx->nodes[ctx->tagNodes[tagIndex]];
			md3Tag_t	*tag = &tags[frame * ctx->numTags + tagIndex];
			static const float	localForward[3] = { 0, 0, 1 }, localLeft[3] = { 1, 0, 0 }, localUp[3] = { 0, 1, 0 };
			float		origin[3], direction[3], axis[3][3];
			int			axisIndex;

			Q_strncpyz( tag->name, node->name, sizeof( tag->name ) );
			GLB_ToQuake( node->world + 12, ctx->scale, origin );
			// the tag's own glTF +Z, +X, +Y become Quake forward, left, up
			Mat4_TransformVector( node->world, localForward, direction );
			GLB_ToQuake( direction, 1.0f, axis[0] );
			Mat4_TransformVector( node->world, localLeft, direction );
			GLB_ToQuake( direction, 1.0f, axis[1] );
			Mat4_TransformVector( node->world, localUp, direction );
			GLB_ToQuake( direction, 1.0f, axis[2] );
			for ( axisIndex = 0 ; axisIndex < 3 ; axisIndex++ ) {
				VectorNormalize( axis[axisIndex] );
			}
			for ( i = 0 ; i < 3 ; i++ ) {
				tag->origin[i] = LittleFloat( origin[i] );
				tag->axis[0][i] = LittleFloat( axis[0][i] );
				tag->axis[1][i] = LittleFloat( axis[1][i] );
				tag->axis[2][i] = LittleFloat( axis[2][i] );
			}
		}
	}
	if ( coordsClamped ) {
		ri.Printf( PRINT_WARNING, "R_LoadGLB: %s: vertices outside +-512 units were clamped, check the scale\n",
			ctx->modelName );
	}
	*outSize = size;
	return buffer;
}

/*
==============================================================================

ENTRY POINTS

==============================================================================
*/

qboolean R_IsGLB( const void *buffer, int length ) {
	return ( length >= 12 && LittleLong( *(const int *)buffer ) == GLB_MAGIC );
}

static qboolean GLB_Parse( glbContext_t *ctx, const byte *data, int length ) {
	int			offset, jsonLength = 0, declaredLength, version;
	const char	*jsonText = NULL;
	const char	*error = "unknown error";
	jsonNode_t	*required, *extras;
	char		*slash;
	cvar_t		*scaleCvar;

	version = LittleLong( ((const int *)data)[1] );
	declaredLength = LittleLong( ((const int *)data)[2] );
	if ( version != 2 ) {
		return GLB_Fail( ctx, va( "glTF container version %i (only 2 is supported)", version ) );
	}
	if ( declaredLength > length ) {
		return GLB_Fail( ctx, "file is truncated" );
	}
	offset = 12;
	while ( offset + 8 <= declaredLength ) {
		int		chunkLength = LittleLong( *(const int *)( data + offset ) );
		int		chunkType = LittleLong( *(const int *)( data + offset + 4 ) );

		if ( chunkLength < 0 || offset + 8 + chunkLength > declaredLength ) {
			return GLB_Fail( ctx, "bad chunk length" );
		}
		if ( chunkType == GLB_CHUNK_JSON && !jsonText ) {
			jsonText = (const char *)( data + offset + 8 );
			jsonLength = chunkLength;
		} else if ( chunkType == GLB_CHUNK_BIN && !ctx->bin ) {
			ctx->bin = data + offset + 8;
			ctx->binLength = chunkLength;
		}
		offset += 8 + ( ( chunkLength + 3 ) & ~3 );
	}
	if ( !jsonText ) {
		return GLB_Fail( ctx, "no JSON chunk" );
	}

	ctx->root = JSON_Parse( &ctx->memory, jsonText, jsonLength, &error );
	if ( !ctx->root || ctx->root->type != JSON_OBJECT ) {
		return GLB_Fail( ctx, va( "JSON error: %s", error ) );
	}

	required = JSON_Get( ctx->root, "extensionsRequired" );
	if ( required && required->type == JSON_ARRAY && required->count > 0 ) {
		return GLB_Fail( ctx, va( "requires the extension %s (not supported)", JSON_String( required->child, "?" ) ) );
	}

	// model directory for shader names
	Q_strncpyz( ctx->modelDir, ctx->modelName, sizeof( ctx->modelDir ) );
	slash = strrchr( ctx->modelDir, '/' );
	if ( slash ) {
		*slash = 0;
	} else {
		ctx->modelDir[0] = 0;
	}

	scaleCvar = ri.Cvar_Get( "r_glbScale", GLB_DEFAULT_SCALE, CVAR_ARCHIVE );
	extras = JSON_Get( JSON_Get( ctx->root, "asset" ), "extras" );
	ctx->scale = (float)JSON_Number( JSON_Get( extras, "q3_scale" ), scaleCvar->value );
	if ( ctx->scale <= 0 ) {
		ctx->scale = 39.37f;
	}

	ctx->accessors = GLB_CollectArray( ctx, "accessors", &ctx->numAccessors );
	ctx->bufferViews = GLB_CollectArray( ctx, "bufferViews", &ctx->numBufferViews );
	ctx->meshes = GLB_CollectArray( ctx, "meshes", &ctx->numMeshes );
	ctx->materials = GLB_CollectArray( ctx, "materials", &ctx->numMaterials );
	ctx->textures = GLB_CollectArray( ctx, "textures", &ctx->numTextures );
	ctx->images = GLB_CollectArray( ctx, "images", &ctx->numImages );

	if ( !GLB_SetupNodes( ctx ) ) return qfalse;
	if ( !GLB_SetupSkins( ctx ) ) return qfalse;
	if ( !GLB_SetupAnimations( ctx ) ) return qfalse;
	if ( !GLB_SetupPrimitives( ctx ) ) return qfalse;
	if ( !GLB_BuildSurfaces( ctx ) ) return qfalse;
	GLB_SetupTags( ctx );
	return qtrue;
}

/*
=================
R_ConvertGLB

Converts a .glb file into an MD3 image.  All memory comes from the temporary
hunk; *handle receives the conversion, which the caller releases with
R_FreeGLB after R_LoadMD3 has copied the image (and before freeing the file
buffer, to keep the temporary hunk in stack order).  Returns NULL on failure,
in which case nothing has to be freed.
=================
*/
void *R_ConvertGLB( const char *modelName, const void *buffer, int length, int *outSize, void **handle ) {
	glbContext_t	*ctx;
	void			*md3 = NULL;
	cvar_t			*dumpCvar;
	int				index, surfaceVerts = 0, surfaceTris = 0;

	*outSize = 0;
	*handle = NULL;
	if ( !R_IsGLB( buffer, length ) ) {
		return NULL;
	}
	ctx = ri.Hunk_AllocateTempMemory( sizeof( glbContext_t ) );
	Com_Memset( ctx, 0, sizeof( *ctx ) );
	ctx->modelName = modelName;
	ctx->memory.modelName = modelName;

	if ( !GLB_Parse( ctx, (const byte *)buffer, length ) ) {
		R_FreeGLB( ctx );
		return NULL;
	}
	md3 = GLB_WriteMD3( ctx, outSize );

	for ( index = 0 ; index < ctx->numSurfaces ; index++ ) {
		surfaceVerts += ctx->surfaces[index]->numVerts;
		surfaceTris += ctx->surfaces[index]->numTris;
	}
	ri.Printf( PRINT_DEVELOPER, "R_LoadGLB: %s: %i surfaces, %i verts, %i tris, %i tags, %i frames\n",
		modelName, ctx->numSurfaces, surfaceVerts, surfaceTris, ctx->numTags, ctx->numFrames );
	for ( index = 0 ; index < ctx->numAnimations ; index++ ) {
		ri.Printf( PRINT_DEVELOPER, "  animation %i '%s': first frame %i, %i frames\n", index,
			ctx->animations[index].name, ctx->animations[index].firstFrame, ctx->animations[index].numFrames );
	}

	dumpCvar = ri.Cvar_Get( "r_glbDump", "0", 0 );
	if ( dumpCvar->integer ) {
		char	dumpName[MAX_QPATH];
		char	baseName[MAX_QPATH];
		COM_StripExtension( modelName, baseName );
		Com_sprintf( dumpName, sizeof( dumpName ), "glbdump/%s.md3", baseName );
		ri.FS_WriteFile( dumpName, md3, *outSize );
		ri.Printf( PRINT_ALL, "R_LoadGLB: wrote %s\n", dumpName );
	}
	*handle = ctx;
	return md3;
}

/*
=================
R_FreeGLB
=================
*/
void R_FreeGLB( void *handle ) {
	glbContext_t	*ctx = handle;

	if ( !ctx ) {
		return;
	}
	GLB_FreeAll( &ctx->memory );		// newest first, the MD3 image included
	ri.Hunk_FreeTempMemory( ctx );
}
