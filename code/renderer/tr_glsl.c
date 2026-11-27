/*
===========================================================================
WALKER: GLSL programs (OpenGL 2.0).

The renderer stays the fixed-function one of Quake III; programs are used
where they save work: the dynamic lights of shots and explosions (one pass
for all lights of a surface instead of a pass for each light with texture
coordinates and colours computed on the processor).

The OpenGL 2.0 functions are looked up at start; without them (very old
cards and drivers) or with r_glsl 0 everything is drawn the classic way.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "tr_local.h"

#define GL_FRAGMENT_SHADER		0x8B30
#define GL_VERTEX_SHADER		0x8B31
#define GL_COMPILE_STATUS		0x8B81
#define GL_LINK_STATUS			0x8B82

typedef char GLchar;

static GLuint	( APIENTRY *qglCreateShader )( GLenum type );
static void		( APIENTRY *qglShaderSource )( GLuint shader, GLsizei count, const GLchar **string, const GLint *length );
static void		( APIENTRY *qglCompileShader )( GLuint shader );
static void		( APIENTRY *qglGetShaderiv )( GLuint shader, GLenum name, GLint *value );
static void		( APIENTRY *qglGetShaderInfoLog )( GLuint shader, GLsizei size, GLsizei *length, GLchar *log );
static void		( APIENTRY *qglDeleteShader )( GLuint shader );
static GLuint	( APIENTRY *qglCreateProgram )( void );
static void		( APIENTRY *qglAttachShader )( GLuint program, GLuint shader );
static void		( APIENTRY *qglLinkProgram )( GLuint program );
static void		( APIENTRY *qglGetProgramiv )( GLuint program, GLenum name, GLint *value );
static void		( APIENTRY *qglGetProgramInfoLog )( GLuint program, GLsizei size, GLsizei *length, GLchar *log );
static void		( APIENTRY *qglUseProgram )( GLuint program );
static GLint	( APIENTRY *qglGetUniformLocation )( GLuint program, const GLchar *name );
static void		( APIENTRY *qglUniform1i )( GLint location, GLint value );
static void		( APIENTRY *qglUniform1f )( GLint location, GLfloat value );
static void		( APIENTRY *qglUniform4fv )( GLint location, GLsizei count, const GLfloat *value );
static void		( APIENTRY *qglUniform3f )( GLint location, GLfloat v0, GLfloat v1, GLfloat v2 );
static void		( APIENTRY *qglUniform4f )( GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3 );
static void		( APIENTRY *qglBindAttribLocation )( GLuint program, GLuint index, const GLchar *name );
static void		( APIENTRY *qglVertexAttribPointer )( GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer );
static void		( APIENTRY *qglEnableVertexAttribArray )( GLuint index );
static void		( APIENTRY *qglDisableVertexAttribArray )( GLuint index );

cvar_t			*r_glsl;
cvar_t			*r_materialPass;	// WALKER: 0: the material classes of a robot part each with its own shader
cvar_t			*r_normalMaps;		// WALKER: 0: the relief maps of the robots (materialBump) not used

static qboolean	glslAvailable;

// the program of models in video memory (tr_vbo.c)
#define	MESH_ATTRIB_FRAME		0		// generic 0 is the vertex position
#define	MESH_ATTRIB_OLDFRAME	6		// 1, 6, 7: not taken by NVIDIA's fixed arrays
#define	MESH_ATTRIB_TEXCOORD	7
static GLuint	meshProgram;
static GLint	meshBackLerp, meshLightDir, meshAmbient, meshDirected, meshViewOrigin, meshSpecularOrigin;
static GLint	meshColorMode, meshAlphaMode, meshTcMode, meshColor, meshFogOn, meshTexture;

// the material program: all material classes of a robot part in one pass; the second
// one lights every pixel with the normal tilted by the relief map (materialBump)
typedef struct {
	GLuint	program;
	GLint	backLerp, lightDir, ambient, directed, viewOrigin, specularOrigin;
	GLint	glow, fogOn, baseMap, envMap, maskMap, bumpMap, bumpScale;
} materialProgram_t;
static materialProgram_t	materialPlain, materialBump;

// the vertex program of map surfaces with deformVertexes lodshift (tr_shade.c)
static GLuint	shiftProgram;
static GLint	shiftEye, shiftBandStart, shiftBand, shiftDepth, shiftFar;
static GLint	shiftTcMode[2], shiftPlaneS[2], shiftPlaneT[2];

// the program of the dynamic lights
static GLuint	dlightProgram;
static GLint	dlightCountLocation, dlightLightsLocation, dlightColorsLocation, dlightFogLocation;


/*
=================
GLSL_Proc
=================
*/
static void *GLSL_Proc( const char *name, qboolean *ok ) {
	void	*proc = NULL;

#ifdef _WIN32
	proc = (void *)qwglGetProcAddress( name );
#endif
	if ( !proc ) {
		*ok = qfalse;
	}
	return proc;
}


/*
=================
GLSL_Compile
=================
*/
static GLuint GLSL_Compile( GLenum type, const char *source, const char *name ) {
	GLuint	shader = qglCreateShader( type );
	GLint	status = 0;

	qglShaderSource( shader, 1, &source, NULL );
	qglCompileShader( shader );
	qglGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if ( !status ) {
		char	log[2048];
		qglGetShaderInfoLog( shader, sizeof( log ), NULL, log );
		ri.Printf( PRINT_WARNING, "GLSL: %s does not compile:\n%s\n", name, log );
		qglDeleteShader( shader );
		return 0;
	}
	return shader;
}


/*
=================
GLSL_Program
=================
*/
static GLuint GLSL_ProgramAttribs( const char *vertexSource, const char *fragmentSource, const char *name, const char **attribs, const int *locations, int numAttribs );

static GLuint GLSL_Program( const char *vertexSource, const char *fragmentSource, const char *name ) {
	return GLSL_ProgramAttribs( vertexSource, fragmentSource, name, NULL, NULL, 0 );
}

static GLuint GLSL_ProgramAttribs( const char *vertexSource, const char *fragmentSource, const char *name, const char **attribs, const int *locations, int numAttribs ) {
	GLuint	vertex = GLSL_Compile( GL_VERTEX_SHADER, vertexSource, name );
	GLuint	fragment = fragmentSource ? GLSL_Compile( GL_FRAGMENT_SHADER, fragmentSource, name ) : 0;
	GLuint	program;
	GLint	status = 0;

	// no fragment source: the fixed fragment stage stays (texture environment, fog)
	if ( !vertex || ( fragmentSource && !fragment ) ) {
		return 0;
	}
	program = qglCreateProgram();
	qglAttachShader( program, vertex );
	if ( fragment ) {
		qglAttachShader( program, fragment );
	}
	{
		int		index;
		for ( index = 0; index < numAttribs; index++ ) {
			qglBindAttribLocation( program, locations[index], attribs[index] );
		}
	}
	qglLinkProgram( program );
	qglGetProgramiv( program, GL_LINK_STATUS, &status );
	if ( !status ) {
		char	log[2048];
		qglGetProgramInfoLog( program, sizeof( log ), NULL, log );
		ri.Printf( PRINT_WARNING, "GLSL: %s does not link:\n%s\n", name, log );
		return 0;
	}
	return program;
}


/*
===========================================================================
Dynamic lights

The look of Q3's light pass is kept: across (x, y) the falloff of its light
texture (bright to a quarter of the radius, then 1/d^2, cut at about half the
radius), along z full to half the radius, then down to zero at the radius;
the result brightens what is already drawn (blend dst_color, one). Under the
OpenGL haze the light fades with the same density.
===========================================================================
*/
static const char *dlightVertexSource =
	"varying vec3 position;\n"
	"varying float eyeDistance;\n"
	"void main() {\n"
	"	position = gl_Vertex.xyz;\n"
	"	eyeDistance = length( ( gl_ModelViewMatrix * gl_Vertex ).xyz );\n"
	"	gl_Position = ftransform();\n"
	"}\n";

static const char *dlightFragmentSource =
	"#define MAX_LIGHTS 16\n"	// GLSL_MAX_DLIGHTS
	"uniform int count;\n"
	"uniform vec4 lights[MAX_LIGHTS];\n"		// origin, 1 / radius
	"uniform vec4 colors[MAX_LIGHTS];\n"
	"uniform float fogDensity;\n"
	"varying vec3 position;\n"
	"varying float eyeDistance;\n"
	"void main() {\n"
	"	vec3 sum = vec3( 0.0 );\n"
	"	for ( int i = 0; i < MAX_LIGHTS; i++ ) {\n"
	"		if ( i >= count ) {\n"
	"			break;\n"
	"		}\n"
	"		vec3 delta = ( lights[i].xyz - position ) * lights[i].w;\n"	// in radii
	"		float across = length( delta.xy );\n"
	"		float height = abs( delta.z );\n"
	"		float spot = clamp( 0.0625 / max( across * across, 0.0001 ), 0.0, 1.0 ) * ( 1.0 - smoothstep( 0.42, 0.5, across ) );\n"
	"		float side = height < 0.5 ? 1.0 : max( 2.0 * ( 1.0 - height ), 0.0 );\n"
	"		sum += colors[i].rgb * ( spot * side );\n"
	"	}\n"
	"	if ( fogDensity > 0.0 ) {\n"
	"		sum *= exp( -fogDensity * eyeDistance );\n"
	"	}\n"
	"	gl_FragColor = vec4( sum, 1.0 );\n"
	"}\n";


/*
===========================================================================
Models in video memory (tr_vbo.c)

What the processor did for every vertex of a model each frame is done here:
the blend of two frames (LerpMeshVertexes; MD3 positions in 1/64 units and the
normal packed as latitude and longitude), rgbGen lightingDiffuse
(RB_CalcDiffuseColor), tcGen environment (RB_CalcEnvironmentTexCoords) and
alphaGen lightingSpecular (RB_CalcSpecularAlpha, with its fixed light point),
with the same formulas. The OpenGL haze is applied in the fragment program
(the fixed one does not apply to programs) with the colour RB_HardwareFogStage
chose for the blend of the stage.
===========================================================================
*/
static const char *meshVertexSource =
	"attribute vec4 frameNew;\n"
	"attribute vec4 frameOld;\n"
	"attribute vec2 texCoord;\n"
	"uniform float backLerp;\n"
	"uniform vec3 lightDir;\n"
	"uniform vec3 ambientLight;\n"
	"uniform vec3 directedLight;\n"
	"uniform vec3 viewOrigin;\n"
	"uniform vec3 specularOrigin;\n"
	"uniform float colorMode;\n"		// 1: lightingDiffuse
	"uniform float alphaMode;\n"		// 1: lightingSpecular
	"uniform float tcMode;\n"			// 1: environment
	"uniform vec4 constColor;\n"
	"varying vec2 tc;\n"
	"varying vec4 color;\n"
	"varying float eyeDistance;\n"
	"vec3 unpackNormal( float latLong ) {\n"
	"	float value = latLong < 0.0 ? latLong + 65536.0 : latLong;\n"
	"	float lat = floor( value / 256.0 );\n"
	"	float lng = value - lat * 256.0;\n"
	"	lat *= 6.28318530718 / 256.0;\n"
	"	lng *= 6.28318530718 / 256.0;\n"
	"	return vec3( cos( lat ) * sin( lng ), sin( lat ) * sin( lng ), cos( lng ) );\n"
	"}\n"
	"void main() {\n"
	"	vec3 position = mix( frameNew.xyz, frameOld.xyz, backLerp ) * ( 1.0 / 64.0 );\n"
	"	vec3 normal = mix( unpackNormal( frameNew.w ), unpackNormal( frameOld.w ), backLerp );\n"
	"	if ( backLerp > 0.0 ) {\n"
	"		normal = normalize( normal );\n"
	"	}\n"
	"	vec4 vertex = vec4( position, 1.0 );\n"
	"	vec4 eye = gl_ModelViewMatrix * vertex;\n"
	"	gl_Position = gl_ModelViewProjectionMatrix * vertex;\n"
	"	gl_ClipVertex = eye;\n"
	"	eyeDistance = length( eye.xyz );\n"
	"	color = constColor;\n"
	"	if ( colorMode > 0.5 ) {\n"
	"		float incoming = dot( normal, lightDir );\n"
	"		color.rgb = min( incoming <= 0.0 ? ambientLight : ambientLight + incoming * directedLight, vec3( 1.0 ) );\n"
	"	}\n"
	"	if ( alphaMode > 0.5 ) {\n"
	"		vec3 toLight = normalize( specularOrigin - position );\n"
	"		vec3 reflected = normal * 2.0 * dot( normal, toLight ) - toLight;\n"
	"		vec3 viewer = viewOrigin - position;\n"
	"		float shine = dot( reflected, viewer ) * inversesqrt( dot( viewer, viewer ) );\n"
	"		shine = max( shine, 0.0 );\n"
	"		shine *= shine;\n"
	"		color.a = min( shine * shine, 1.0 );\n"
	"	}\n"
	"	if ( tcMode > 0.5 ) {\n"
	"		vec3 viewer = normalize( viewOrigin - position );\n"
	"		vec3 reflected = normal * 2.0 * dot( normal, viewer ) - viewer;\n"
	"		tc = vec2( 0.5 + reflected.y * 0.5, 0.5 - reflected.z * 0.5 );\n"
	"	} else {\n"
	"		tc = texCoord;\n"
	"	}\n"
	"}\n";

/*
===========================================================================
Materials in one pass

A robot part has one atlas; its material classes were shaders of up to three
stages each on the same atlas (bake_walker.py):
  paint  base * diffuse + reflection * 0.40 * shine        (blend src_alpha, one)
  metal  base * diffuse, * ( 1 + reflection * 0.55 ),      (blend dst_color, one)
         + reflection * 0.55 * shine
  glass  base * diffuse + reflection * 0.35                (blend one, one)
  glow   base * wave, unlit
Each stage a draw call, and every class its own batch: up to eight draws for
a part. Here the class of every texel comes from a mask (r: multiplied in,
g: added, b: added with the shine, a: glow), the stages are summed in the same
order with the same clamps the frame buffer gave between them.
===========================================================================
*/
static const char *materialVertexSource =
	"attribute vec4 frameNew;\n"
	"attribute vec4 frameOld;\n"
	"attribute vec2 texCoord;\n"
	"uniform float backLerp;\n"
	"uniform vec3 lightDir;\n"
	"uniform vec3 ambientLight;\n"
	"uniform vec3 directedLight;\n"
	"uniform vec3 viewOrigin;\n"
	"uniform vec3 specularOrigin;\n"
	"varying vec2 tc;\n"
	"varying vec2 envTc;\n"
	"varying vec3 diffuse;\n"
	"varying float shine;\n"
	"varying float eyeDistance;\n"
	"vec3 unpackNormal( float latLong ) {\n"
	"	float value = latLong < 0.0 ? latLong + 65536.0 : latLong;\n"
	"	float lat = floor( value / 256.0 );\n"
	"	float lng = value - lat * 256.0;\n"
	"	lat *= 6.28318530718 / 256.0;\n"
	"	lng *= 6.28318530718 / 256.0;\n"
	"	return vec3( cos( lat ) * sin( lng ), sin( lat ) * sin( lng ), cos( lng ) );\n"
	"}\n"
	"void main() {\n"
	"	vec3 position = mix( frameNew.xyz, frameOld.xyz, backLerp ) * ( 1.0 / 64.0 );\n"
	"	vec3 normal = mix( unpackNormal( frameNew.w ), unpackNormal( frameOld.w ), backLerp );\n"
	"	if ( backLerp > 0.0 ) {\n"
	"		normal = normalize( normal );\n"
	"	}\n"
	"	vec4 vertex = vec4( position, 1.0 );\n"
	"	vec4 eye = gl_ModelViewMatrix * vertex;\n"
	"	gl_Position = gl_ModelViewProjectionMatrix * vertex;\n"
	"	gl_ClipVertex = eye;\n"
	"	eyeDistance = length( eye.xyz );\n"
	"	float incoming = dot( normal, lightDir );\n"
	"	diffuse = min( incoming <= 0.0 ? ambientLight : ambientLight + incoming * directedLight, vec3( 1.0 ) );\n"
	"	vec3 toLight = normalize( specularOrigin - position );\n"
	"	vec3 reflected = normal * 2.0 * dot( normal, toLight ) - toLight;\n"
	"	vec3 viewer = viewOrigin - position;\n"
	"	float lit = max( dot( reflected, viewer ) * inversesqrt( dot( viewer, viewer ) ), 0.0 );\n"
	"	lit *= lit;\n"
	"	shine = min( lit * lit, 1.0 );\n"
	"	vec3 toViewer = normalize( viewer );\n"
	"	vec3 mirrored = normal * 2.0 * dot( normal, toViewer ) - toViewer;\n"
	"	envTc = vec2( 0.5 + mirrored.y * 0.5, 0.5 - mirrored.z * 0.5 );\n"
	"	tc = texCoord;\n"
	"}\n";

static const char *materialFragmentSource =
	"uniform sampler2D baseMap;\n"
	"uniform sampler2D envMap;\n"
	"uniform sampler2D maskMap;\n"
	"uniform float glow;\n"
	"uniform float fogOn;\n"
	"varying vec2 tc;\n"
	"varying vec2 envTc;\n"
	"varying vec3 diffuse;\n"
	"varying float shine;\n"
	"varying float eyeDistance;\n"
	"void main() {\n"
	"	vec3 base = texture2D( baseMap, tc ).rgb;\n"
	"	vec4 mask = texture2D( maskMap, tc );\n"
	"	vec3 env = texture2D( envMap, envTc ).rgb;\n"
	"	vec3 lit = clamp( base * diffuse, 0.0, 1.0 );\n"
	"	lit = clamp( lit + lit * env * mask.r, 0.0, 1.0 );\n"
	"	lit = clamp( lit + env * mask.g, 0.0, 1.0 );\n"
	"	lit = clamp( lit + env * ( mask.b * shine ), 0.0, 1.0 );\n"
	"	vec3 result = mix( lit, clamp( base * glow, 0.0, 1.0 ), mask.a );\n"
	"	if ( fogOn > 0.5 ) {\n"
	"		float visible = clamp( exp( -gl_Fog.density * eyeDistance ), 0.0, 1.0 );\n"
	"		result = mix( gl_Fog.color.rgb, result, visible );\n"
	"	}\n"
	"	gl_FragColor = vec4( result, 1.0 );\n"
	"}\n";

/*
===========================================================================
Materials with relief (materialBump, 27.11.2026)

The same classes and sums, but the light is found for every pixel: the normal is
tilted by the relief map of the part (bake_walker.py: seams, rivets, bolts,
grilles). The map holds the slopes of the relief height along the atlas axes,
dh/du and dh/dv; on the surface they become the gradient of the height with the
derivatives of the position and the atlas coordinates in the pixel (surface
gradient, Mikkelsen 2010), so no tangents are stored, the frames of a part may
move as they like and the pieces of the atlas may lie at any angle. The tilted
normal is n - grad h.
===========================================================================
*/
static const char *materialBumpVertexSource =
	"attribute vec4 frameNew;\n"
	"attribute vec4 frameOld;\n"
	"attribute vec2 texCoord;\n"
	"uniform float backLerp;\n"
	"varying vec2 tc;\n"
	"varying vec3 modelPosition;\n"
	"varying vec3 modelNormal;\n"
	"varying float eyeDistance;\n"
	"vec3 unpackNormal( float latLong ) {\n"
	"	float value = latLong < 0.0 ? latLong + 65536.0 : latLong;\n"
	"	float lat = floor( value / 256.0 );\n"
	"	float lng = value - lat * 256.0;\n"
	"	lat *= 6.28318530718 / 256.0;\n"
	"	lng *= 6.28318530718 / 256.0;\n"
	"	return vec3( cos( lat ) * sin( lng ), sin( lat ) * sin( lng ), cos( lng ) );\n"
	"}\n"
	"void main() {\n"
	"	vec3 position = mix( frameNew.xyz, frameOld.xyz, backLerp ) * ( 1.0 / 64.0 );\n"
	"	modelNormal = mix( unpackNormal( frameNew.w ), unpackNormal( frameOld.w ), backLerp );\n"
	"	vec4 vertex = vec4( position, 1.0 );\n"
	"	vec4 eye = gl_ModelViewMatrix * vertex;\n"
	"	gl_Position = gl_ModelViewProjectionMatrix * vertex;\n"
	"	gl_ClipVertex = eye;\n"
	"	eyeDistance = length( eye.xyz );\n"
	"	modelPosition = position;\n"
	"	tc = texCoord;\n"
	"}\n";

static const char *materialBumpFragmentSource =
	"uniform sampler2D baseMap;\n"
	"uniform sampler2D envMap;\n"
	"uniform sampler2D maskMap;\n"
	"uniform sampler2D bumpMap;\n"
	"uniform float bumpScale;\n"		// slope of the map's full range, Quake units per atlas unit
	"uniform float glow;\n"
	"uniform float fogOn;\n"
	"uniform vec3 lightDir;\n"
	"uniform vec3 ambientLight;\n"
	"uniform vec3 directedLight;\n"
	"uniform vec3 viewOrigin;\n"
	"uniform vec3 specularOrigin;\n"
	"varying vec2 tc;\n"
	"varying vec3 modelPosition;\n"
	"varying vec3 modelNormal;\n"
	"varying float eyeDistance;\n"
	"void main() {\n"
	"	vec3 normal = normalize( modelNormal );\n"
	"	vec2 slope = ( texture2D( bumpMap, tc ).rg * 2.0 - 1.0 ) * bumpScale;\n"
	"	vec3 positionX = dFdx( modelPosition );\n"
	"	vec3 positionY = dFdy( modelPosition );\n"
	"	float heightX = dot( slope, dFdx( tc ) );\n"
	"	float heightY = dot( slope, dFdy( tc ) );\n"
	"	vec3 across1 = cross( positionY, normal );\n"
	"	vec3 across2 = cross( normal, positionX );\n"
	"	float area = dot( positionX, across1 );\n"
	"	vec3 gradient = sign( area ) * ( heightX * across1 + heightY * across2 );\n"
	"	normal = normalize( abs( area ) * normal - gradient );\n"
	"	float incoming = dot( normal, lightDir );\n"
	"	vec3 diffuse = min( incoming <= 0.0 ? ambientLight : ambientLight + incoming * directedLight, vec3( 1.0 ) );\n"
	"	vec3 toLight = normalize( specularOrigin - modelPosition );\n"
	"	vec3 reflected = normal * 2.0 * dot( normal, toLight ) - toLight;\n"
	"	vec3 viewer = viewOrigin - modelPosition;\n"
	"	float lit = max( dot( reflected, viewer ) * inversesqrt( dot( viewer, viewer ) ), 0.0 );\n"
	"	lit *= lit;\n"
	"	float shine = min( lit * lit, 1.0 );\n"
	"	vec3 toViewer = normalize( viewer );\n"
	"	vec3 mirrored = normal * 2.0 * dot( normal, toViewer ) - toViewer;\n"
	"	vec2 envTc = vec2( 0.5 + mirrored.y * 0.5, 0.5 - mirrored.z * 0.5 );\n"
	"	vec3 base = texture2D( baseMap, tc ).rgb;\n"
	"	vec4 mask = texture2D( maskMap, tc );\n"
	"	vec3 env = texture2D( envMap, envTc ).rgb;\n"
	"	vec3 color = clamp( base * diffuse, 0.0, 1.0 );\n"
	"	color = clamp( color + color * env * mask.r, 0.0, 1.0 );\n"
	"	color = clamp( color + env * mask.g, 0.0, 1.0 );\n"
	"	color = clamp( color + env * ( mask.b * shine ), 0.0, 1.0 );\n"
	"	vec3 result = mix( color, clamp( base * glow, 0.0, 1.0 ), mask.a );\n"
	"	if ( fogOn > 0.5 ) {\n"
	"		float visible = clamp( exp( -gl_Fog.density * eyeDistance ), 0.0, 1.0 );\n"
	"		result = mix( gl_Fog.color.rgb, result, visible );\n"
	"	}\n"
	"	gl_FragColor = vec4( result, 1.0 );\n"
	"}\n";

/*
===========================================================================
Map surfaces in two detail levels (deformVertexes lodshift)

RB_CalcLodShift moves every vertex on the processor while the surface is in the
band of r_lodDistance, so such surfaces could come from the map's buffers only
when wholly outside it (and big buildings seldom are). Here the same move is
made per vertex; only the vertex stage is a program, the fixed fragment stage
(texture environment, alpha test, fog by gl_FogFragCoord) stays. Texture
coordinates: the array of the unit, or tcGen vector from the moved point, then
the texture matrix (the tcMods), as on the fixed path.
===========================================================================
*/
static const char *shiftVertexSource =
	"uniform vec3 eye;\n"
	"uniform float bandStart;\n"
	"uniform float band;\n"
	"uniform float depth;\n"
	"uniform float farLevel;\n"
	"uniform float tcMode0;\n"		// 0: the array of the unit, 1: tcGen vector
	"uniform float tcMode1;\n"
	"uniform vec3 planeS0;\n"
	"uniform vec3 planeT0;\n"
	"uniform vec3 planeS1;\n"
	"uniform vec3 planeT1;\n"
	"void main() {\n"
	"	vec3 position = gl_Vertex.xyz;\n"
	"	vec3 delta = position - eye;\n"
	"	float distance = length( delta );\n"
	"	if ( distance >= 1.0 ) {\n"
	"		float fraction = clamp( ( distance - bandStart ) / band, 0.0, 1.0 );\n"
	"		float shift = depth * ( farLevel > 0.5 ? 1.0 - fraction : fraction );\n"
	"		position += delta * ( shift / distance );\n"
	"	}\n"
	"	vec4 vertex = vec4( position, 1.0 );\n"
	"	vec4 eyePosition = gl_ModelViewMatrix * vertex;\n"
	"	gl_Position = gl_ModelViewProjectionMatrix * vertex;\n"
	"	gl_ClipVertex = eyePosition;\n"
	"	gl_FogFragCoord = abs( eyePosition.z );\n"
	"	gl_FrontColor = gl_Color;\n"
	"	vec4 coord0 = tcMode0 > 0.5 ? vec4( dot( position, planeS0 ), dot( position, planeT0 ), 0.0, 1.0 ) : gl_MultiTexCoord0;\n"
	"	vec4 coord1 = tcMode1 > 0.5 ? vec4( dot( position, planeS1 ), dot( position, planeT1 ), 0.0, 1.0 ) : gl_MultiTexCoord1;\n"
	"	gl_TexCoord[0] = gl_TextureMatrix[0] * coord0;\n"
	"	gl_TexCoord[1] = gl_TextureMatrix[1] * coord1;\n"
	"}\n";

static const char *meshFragmentSource =
	"uniform sampler2D texture0;\n"
	"uniform float fogOn;\n"
	"varying vec2 tc;\n"
	"varying vec4 color;\n"
	"varying float eyeDistance;\n"
	"void main() {\n"
	"	vec4 result = texture2D( texture0, tc ) * color;\n"
	"	if ( fogOn > 0.5 ) {\n"
	"		float visible = clamp( exp( -gl_Fog.density * eyeDistance ), 0.0, 1.0 );\n"
	"		result.rgb = mix( gl_Fog.color.rgb, result.rgb, visible );\n"
	"	}\n"
	"	gl_FragColor = result;\n"
	"}\n";


/*
=================
GLSL_MaterialProgram

one of the two material programs and the places of its uniforms
=================
*/
static void GLSL_MaterialProgram( materialProgram_t *target, const char *vertexSource, const char *fragmentSource, const char *name ) {
	static const char	*attribs[3] = { "frameNew", "frameOld", "texCoord" };
	static const int	locations[3] = { MESH_ATTRIB_FRAME, MESH_ATTRIB_OLDFRAME, MESH_ATTRIB_TEXCOORD };
	GLuint				program;

	program = GLSL_ProgramAttribs( vertexSource, fragmentSource, name, attribs, locations, 3 );
	if ( !program ) {
		return;
	}
	target->program = program;
	target->backLerp = qglGetUniformLocation( program, "backLerp" );
	target->lightDir = qglGetUniformLocation( program, "lightDir" );
	target->ambient = qglGetUniformLocation( program, "ambientLight" );
	target->directed = qglGetUniformLocation( program, "directedLight" );
	target->viewOrigin = qglGetUniformLocation( program, "viewOrigin" );
	target->specularOrigin = qglGetUniformLocation( program, "specularOrigin" );
	target->glow = qglGetUniformLocation( program, "glow" );
	target->fogOn = qglGetUniformLocation( program, "fogOn" );
	target->baseMap = qglGetUniformLocation( program, "baseMap" );
	target->envMap = qglGetUniformLocation( program, "envMap" );
	target->maskMap = qglGetUniformLocation( program, "maskMap" );
	target->bumpMap = qglGetUniformLocation( program, "bumpMap" );		// -1 in the plain one
	target->bumpScale = qglGetUniformLocation( program, "bumpScale" );
}

/*
=================
GLSL_Init

after every start of OpenGL (programs live in its context)
=================
*/
void GLSL_Init( void ) {
	qboolean	ok = qtrue;

	r_glsl = ri.Cvar_Get( "r_glsl", "1", CVAR_ARCHIVE );
	r_materialPass = ri.Cvar_Get( "r_materialPass", "1", CVAR_ARCHIVE );
	r_normalMaps = ri.Cvar_Get( "r_normalMaps", "1", CVAR_ARCHIVE );
	glslAvailable = qfalse;
	dlightProgram = 0;
	meshProgram = 0;
	shiftProgram = 0;
	Com_Memset( &materialPlain, 0, sizeof( materialPlain ) );
	Com_Memset( &materialBump, 0, sizeof( materialBump ) );

	qglCreateShader = GLSL_Proc( "glCreateShader", &ok );
	qglShaderSource = GLSL_Proc( "glShaderSource", &ok );
	qglCompileShader = GLSL_Proc( "glCompileShader", &ok );
	qglGetShaderiv = GLSL_Proc( "glGetShaderiv", &ok );
	qglGetShaderInfoLog = GLSL_Proc( "glGetShaderInfoLog", &ok );
	qglDeleteShader = GLSL_Proc( "glDeleteShader", &ok );
	qglCreateProgram = GLSL_Proc( "glCreateProgram", &ok );
	qglAttachShader = GLSL_Proc( "glAttachShader", &ok );
	qglLinkProgram = GLSL_Proc( "glLinkProgram", &ok );
	qglGetProgramiv = GLSL_Proc( "glGetProgramiv", &ok );
	qglGetProgramInfoLog = GLSL_Proc( "glGetProgramInfoLog", &ok );
	qglUseProgram = GLSL_Proc( "glUseProgram", &ok );
	qglGetUniformLocation = GLSL_Proc( "glGetUniformLocation", &ok );
	qglUniform1i = GLSL_Proc( "glUniform1i", &ok );
	qglUniform1f = GLSL_Proc( "glUniform1f", &ok );
	qglUniform4fv = GLSL_Proc( "glUniform4fv", &ok );
	qglUniform3f = GLSL_Proc( "glUniform3f", &ok );
	qglUniform4f = GLSL_Proc( "glUniform4f", &ok );
	qglBindAttribLocation = GLSL_Proc( "glBindAttribLocation", &ok );
	qglVertexAttribPointer = GLSL_Proc( "glVertexAttribPointer", &ok );
	qglEnableVertexAttribArray = GLSL_Proc( "glEnableVertexAttribArray", &ok );
	qglDisableVertexAttribArray = GLSL_Proc( "glDisableVertexAttribArray", &ok );
	if ( !ok ) {
		ri.Printf( PRINT_ALL, "GLSL: OpenGL 2.0 functions not found, the classic renderer is used\n" );
		return;
	}

	dlightProgram = GLSL_Program( dlightVertexSource, dlightFragmentSource, "dynamic lights" );
	if ( dlightProgram ) {
		dlightCountLocation = qglGetUniformLocation( dlightProgram, "count" );
		dlightLightsLocation = qglGetUniformLocation( dlightProgram, "lights" );
		dlightColorsLocation = qglGetUniformLocation( dlightProgram, "colors" );
		dlightFogLocation = qglGetUniformLocation( dlightProgram, "fogDensity" );
	}
	{
		static const char	*attribs[3] = { "frameNew", "frameOld", "texCoord" };
		static const int	locations[3] = { MESH_ATTRIB_FRAME, MESH_ATTRIB_OLDFRAME, MESH_ATTRIB_TEXCOORD };
		meshProgram = GLSL_ProgramAttribs( meshVertexSource, meshFragmentSource, "models", attribs, locations, 3 );
	}
	if ( meshProgram ) {
		meshBackLerp = qglGetUniformLocation( meshProgram, "backLerp" );
		meshLightDir = qglGetUniformLocation( meshProgram, "lightDir" );
		meshAmbient = qglGetUniformLocation( meshProgram, "ambientLight" );
		meshDirected = qglGetUniformLocation( meshProgram, "directedLight" );
		meshViewOrigin = qglGetUniformLocation( meshProgram, "viewOrigin" );
		meshSpecularOrigin = qglGetUniformLocation( meshProgram, "specularOrigin" );
		meshColorMode = qglGetUniformLocation( meshProgram, "colorMode" );
		meshAlphaMode = qglGetUniformLocation( meshProgram, "alphaMode" );
		meshTcMode = qglGetUniformLocation( meshProgram, "tcMode" );
		meshColor = qglGetUniformLocation( meshProgram, "constColor" );
		meshFogOn = qglGetUniformLocation( meshProgram, "fogOn" );
		meshTexture = qglGetUniformLocation( meshProgram, "texture0" );
	}
	GLSL_MaterialProgram( &materialPlain, materialVertexSource, materialFragmentSource, "materials" );
	GLSL_MaterialProgram( &materialBump, materialBumpVertexSource, materialBumpFragmentSource, "materials with relief" );
	shiftProgram = GLSL_Program( shiftVertexSource, NULL, "lodshift" );
	if ( shiftProgram ) {
		shiftEye = qglGetUniformLocation( shiftProgram, "eye" );
		shiftBandStart = qglGetUniformLocation( shiftProgram, "bandStart" );
		shiftBand = qglGetUniformLocation( shiftProgram, "band" );
		shiftDepth = qglGetUniformLocation( shiftProgram, "depth" );
		shiftFar = qglGetUniformLocation( shiftProgram, "farLevel" );
		shiftTcMode[0] = qglGetUniformLocation( shiftProgram, "tcMode0" );
		shiftTcMode[1] = qglGetUniformLocation( shiftProgram, "tcMode1" );
		shiftPlaneS[0] = qglGetUniformLocation( shiftProgram, "planeS0" );
		shiftPlaneS[1] = qglGetUniformLocation( shiftProgram, "planeS1" );
		shiftPlaneT[0] = qglGetUniformLocation( shiftProgram, "planeT0" );
		shiftPlaneT[1] = qglGetUniformLocation( shiftProgram, "planeT1" );
	}
	glslAvailable = qtrue;
	ri.Printf( PRINT_ALL, "GLSL: on%s%s%s%s\n", dlightProgram ? ", dynamic lights in one pass" : "",
		meshProgram ? ", models in video memory" : "", materialPlain.program ? ", materials in one pass" : "",
		materialBump.program ? ", relief maps" : "" );
}


/*
=================
GLSL_DlightsAvailable
=================
*/
qboolean GLSL_DlightsAvailable( void ) {
	return glslAvailable && dlightProgram && r_glsl->integer;
}


/*
=================
GLSL_DlightsBegin / GLSL_DlightsEnd

lights (origin, 1 / radius) and colours, count up to GLSL_MAX_DLIGHTS; the
caller draws the triangles in between (tr_shade.c, RB_DlightPass)
=================
*/
void GLSL_DlightsBegin( int count, const float *lights, const float *colors, float fogDensity ) {
	qglUseProgram( dlightProgram );
	qglUniform1i( dlightCountLocation, count );
	qglUniform4fv( dlightLightsLocation, count, lights );
	qglUniform4fv( dlightColorsLocation, count, colors );
	qglUniform1f( dlightFogLocation, fogDensity );
}

void GLSL_DlightsEnd( void ) {
	qglUseProgram( 0 );
}



/*
=================
GLSL_MeshAvailable / GLSL_MeshBegin / GLSL_MeshStage / GLSL_MeshEnd

the caller binds the model's buffers, draws the ranges after every stage
=================
*/
qboolean GLSL_MeshAvailable( void ) {
	return glslAvailable && meshProgram && r_glsl->integer;
}

extern vec3_t	lightOrigin;	// tr_shade_calc.c, the light point of lightingSpecular

void GLSL_MeshBegin( const meshModelVBO_t *model, int frame, int oldFrame, float backLerp ) {
	trRefEntity_t	*ent = backEnd.currentEntity;
	int				frameSize = model->numVerts * sizeof( md3XyzNormal_t );

	qglUseProgram( meshProgram );
	qglUniform1f( meshBackLerp, backLerp );
	qglUniform3f( meshLightDir, ent->lightDir[0], ent->lightDir[1], ent->lightDir[2] );
	qglUniform3f( meshAmbient, ent->ambientLight[0] / 255.0f, ent->ambientLight[1] / 255.0f, ent->ambientLight[2] / 255.0f );
	qglUniform3f( meshDirected, ent->directedLight[0] / 255.0f, ent->directedLight[1] / 255.0f, ent->directedLight[2] / 255.0f );
	qglUniform3f( meshViewOrigin, backEnd.or.viewOrigin[0], backEnd.or.viewOrigin[1], backEnd.or.viewOrigin[2] );
	qglUniform3f( meshSpecularOrigin, lightOrigin[0], lightOrigin[1], lightOrigin[2] );
	qglUniform1i( meshTexture, 0 );

	qglVertexAttribPointer( MESH_ATTRIB_FRAME, 4, GL_SHORT, GL_FALSE, sizeof( md3XyzNormal_t ),
		(const void *)(size_t)( model->framesOffset + frame * frameSize ) );
	qglVertexAttribPointer( MESH_ATTRIB_OLDFRAME, 4, GL_SHORT, GL_FALSE, sizeof( md3XyzNormal_t ),
		(const void *)(size_t)( model->framesOffset + oldFrame * frameSize ) );
	qglVertexAttribPointer( MESH_ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 2 * sizeof( float ),
		(const void *)(size_t)model->stOffset );
	qglEnableVertexAttribArray( MESH_ATTRIB_FRAME );
	qglEnableVertexAttribArray( MESH_ATTRIB_OLDFRAME );
	qglEnableVertexAttribArray( MESH_ATTRIB_TEXCOORD );
}

void GLSL_MeshStage( qboolean diffuse, qboolean specular, qboolean environment, const byte *color, qboolean fogOn ) {
	qglUniform1f( meshColorMode, diffuse ? 1.0f : 0.0f );
	qglUniform1f( meshAlphaMode, specular ? 1.0f : 0.0f );
	qglUniform1f( meshTcMode, environment ? 1.0f : 0.0f );
	qglUniform4f( meshColor, color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f );
	qglUniform1f( meshFogOn, fogOn ? 1.0f : 0.0f );
}

void GLSL_MeshEnd( void ) {
	qglDisableVertexAttribArray( MESH_ATTRIB_OLDFRAME );
	qglDisableVertexAttribArray( MESH_ATTRIB_TEXCOORD );
	qglDisableVertexAttribArray( MESH_ATTRIB_FRAME );
	qglUseProgram( 0 );
}



/*
=================
GLSL_MaterialAvailable / GLSL_MaterialBegin / GLSL_MaterialEnd

one pass of a part with "materialImages" (tr_shade.c, RB_DrawMeshVBO); the
caller binds the atlas to unit 0, the reflection to unit 1, the mask to unit 2
=================
*/
qboolean GLSL_MaterialAvailable( void ) {
	return glslAvailable && meshProgram && materialPlain.program && r_glsl->integer && r_materialPass->integer
		&& glConfig.maxActiveTextures >= 3;
}

// the relief map can be used: the program built, a fourth texture unit, r_normalMaps
qboolean GLSL_MaterialBumpAvailable( void ) {
	return materialBump.program && r_normalMaps->integer && glConfig.maxActiveTextures >= 4;
}

// bumpScale: 0 - the plain program, else the slope of the relief map's full range in
// Quake units per atlas unit (its map bound to unit 3 by the caller)
void GLSL_MaterialBegin( const meshModelVBO_t *model, int frame, int oldFrame, float backLerp, float glow, qboolean fogOn, float bumpScale ) {
	trRefEntity_t		*ent = backEnd.currentEntity;
	int					frameSize = model->numVerts * sizeof( md3XyzNormal_t );
	materialProgram_t	*used = bumpScale > 0 && GLSL_MaterialBumpAvailable() ? &materialBump : &materialPlain;

	qglUseProgram( used->program );
	qglUniform1f( used->backLerp, backLerp );
	qglUniform3f( used->lightDir, ent->lightDir[0], ent->lightDir[1], ent->lightDir[2] );
	qglUniform3f( used->ambient, ent->ambientLight[0] / 255.0f, ent->ambientLight[1] / 255.0f, ent->ambientLight[2] / 255.0f );
	qglUniform3f( used->directed, ent->directedLight[0] / 255.0f, ent->directedLight[1] / 255.0f, ent->directedLight[2] / 255.0f );
	qglUniform3f( used->viewOrigin, backEnd.or.viewOrigin[0], backEnd.or.viewOrigin[1], backEnd.or.viewOrigin[2] );
	qglUniform3f( used->specularOrigin, lightOrigin[0], lightOrigin[1], lightOrigin[2] );
	qglUniform1f( used->glow, glow );
	qglUniform1f( used->fogOn, fogOn ? 1.0f : 0.0f );
	qglUniform1i( used->baseMap, 0 );
	qglUniform1i( used->envMap, 1 );
	qglUniform1i( used->maskMap, 2 );
	if ( used == &materialBump ) {
		qglUniform1i( used->bumpMap, 3 );
		qglUniform1f( used->bumpScale, bumpScale );
	}

	qglVertexAttribPointer( MESH_ATTRIB_FRAME, 4, GL_SHORT, GL_FALSE, sizeof( md3XyzNormal_t ),
		(const void *)(size_t)( model->framesOffset + frame * frameSize ) );
	qglVertexAttribPointer( MESH_ATTRIB_OLDFRAME, 4, GL_SHORT, GL_FALSE, sizeof( md3XyzNormal_t ),
		(const void *)(size_t)( model->framesOffset + oldFrame * frameSize ) );
	qglVertexAttribPointer( MESH_ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 2 * sizeof( float ),
		(const void *)(size_t)model->stOffset );
	qglEnableVertexAttribArray( MESH_ATTRIB_FRAME );
	qglEnableVertexAttribArray( MESH_ATTRIB_OLDFRAME );
	qglEnableVertexAttribArray( MESH_ATTRIB_TEXCOORD );
}

void GLSL_MaterialEnd( void ) {
	GLSL_MeshEnd();
}


/*
=================
GLSL_LodShiftAvailable / Begin / Unit / End

the caller sets the arrays, texture matrices and the fixed state as for the
fixed path and draws in between
=================
*/
qboolean GLSL_LodShiftAvailable( void ) {
	return glslAvailable && shiftProgram && r_glsl->integer;
}

void GLSL_LodShiftBegin( const deformStage_t *deform ) {
	float	band = r_lodBand->value < 1 ? 1 : r_lodBand->value;	// as R_LodShiftPoint

	qglUseProgram( shiftProgram );
	qglUniform3f( shiftEye, backEnd.viewParms.or.origin[0], backEnd.viewParms.or.origin[1], backEnd.viewParms.or.origin[2] );
	qglUniform1f( shiftBandStart, r_lodDistance->value - band * 0.5f );
	qglUniform1f( shiftBand, band );
	qglUniform1f( shiftDepth, R_LodShiftDepth( deform ) );
	qglUniform1f( shiftFar, deform->moveVector[0] ? 1.0f : 0.0f );
	qglUniform1f( shiftTcMode[0], 0 );
	qglUniform1f( shiftTcMode[1], 0 );
}

void GLSL_LodShiftUnit( int unit, qboolean vector, const vec3_t planeS, const vec3_t planeT ) {
	qglUniform1f( shiftTcMode[unit], vector ? 1.0f : 0.0f );
	if ( vector ) {
		qglUniform3f( shiftPlaneS[unit], planeS[0], planeS[1], planeS[2] );
		qglUniform3f( shiftPlaneT[unit], planeT[0], planeT[1], planeT[2] );
	}
}

void GLSL_LodShiftEnd( void ) {
	qglUseProgram( 0 );
}
