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

static qboolean	glslAvailable;

// the program of models in video memory (tr_vbo.c)
#define	MESH_ATTRIB_FRAME		0		// generic 0 is the vertex position
#define	MESH_ATTRIB_OLDFRAME	6		// 1, 6, 7: not taken by NVIDIA's fixed arrays
#define	MESH_ATTRIB_TEXCOORD	7
static GLuint	meshProgram;
static GLint	meshBackLerp, meshLightDir, meshAmbient, meshDirected, meshViewOrigin, meshSpecularOrigin;
static GLint	meshColorMode, meshAlphaMode, meshTcMode, meshColor, meshFogOn, meshTexture;

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
	GLuint	fragment = GLSL_Compile( GL_FRAGMENT_SHADER, fragmentSource, name );
	GLuint	program;
	GLint	status = 0;

	if ( !vertex || !fragment ) {
		return 0;
	}
	program = qglCreateProgram();
	qglAttachShader( program, vertex );
	qglAttachShader( program, fragment );
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
GLSL_Init

after every start of OpenGL (programs live in its context)
=================
*/
void GLSL_Init( void ) {
	qboolean	ok = qtrue;

	r_glsl = ri.Cvar_Get( "r_glsl", "1", CVAR_ARCHIVE );
	glslAvailable = qfalse;
	dlightProgram = 0;
	meshProgram = 0;

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
	glslAvailable = qtrue;
	ri.Printf( PRINT_ALL, "GLSL: on%s%s\n", dlightProgram ? ", dynamic lights in one pass" : "",
		meshProgram ? ", models in video memory" : "" );
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
