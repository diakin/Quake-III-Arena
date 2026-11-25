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

cvar_t			*r_glsl;

static qboolean	glslAvailable;

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
static GLuint GLSL_Program( const char *vertexSource, const char *fragmentSource, const char *name ) {
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
	glslAvailable = qtrue;
	ri.Printf( PRINT_ALL, "GLSL: on%s\n", dlightProgram ? ", dynamic lights in one pass" : "" );
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
