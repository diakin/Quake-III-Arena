/*
===========================================================================
WALKER: benchmark.

"benchmark [seconds]" in a running battle: goes through the steps of
benchmark.txt (basewalker, the default one is in pak0), each one changes some
settings for a while, and measures the frame rate and where the frame time
goes: bot AI, the rest of the server game, the client and cgame, the renderer
front end (scene, culling) and back end (GL calls; with r_finish 1 also the
wait for the video card). Every step starts from the settings the benchmark
was started with; at the end they are restored. The results go to the console
and are appended to benchmark.log in basewalker.

benchmark.txt: one step a line, "name: command; command; ...". A command of
two words is a cvar and its value, "vid_restart" restarts the renderer (for
latched cvars: textures, resolution), anything else is executed as is.
"//" starts a comment. "frozen" in a step: it runs on the battle stopped at one
moment (no game time passes, bots do not think), every frame the same picture,
so the step differs from the frozen base by its settings only; the battle is
stopped when the first frozen step starts and goes on at the next live one.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "client.h"

char	*FS_BuildOSPath( const char *base, const char *game, const char *qpath );	// files.c
extern vm_t	*gvm;		// server.h: the server game

#define BENCH_FILE			"benchmark.txt"
#define BENCH_LOG			"benchmark.log"
#define SYSINFO_FILE		"sysinfo.txt"
#define BENCH_MAX_STEPS		48
#define BENCH_MAX_SETS		8			// cvars a step changes
#define BENCH_MAX_FRAMES	16384		// frame times kept per step (1 % low)
#define BENCH_WARMUP		3000		// msec after a step is applied, not measured
#define BENCH_SETTLE_FRAMES	3			// and these frames (a vid_restart takes one long frame)
#define BENCH_FROZEN_WARMUP	1000		// a frozen step: the picture does not change, less warmup
#define BENCH_FROZEN_MIN	3000		// and half the time of a step, not less than this

// pmtype_t and the health stat of the game (bg_public.h)
#define BENCH_PM_SPECTATOR		2
#define BENCH_PM_DEAD			3
#define BENCH_PM_INTERMISSION	5

typedef struct {
	char	name[MAX_CVAR_VALUE_STRING];
	char	value[MAX_CVAR_VALUE_STRING];
	char	saved[MAX_CVAR_VALUE_STRING];
} benchSet_t;

typedef struct {
	char		name[32];
	char		commands[256];
	benchSet_t	sets[BENCH_MAX_SETS];
	int			numSets;
	qboolean	restart;
	qboolean	frozen;			// on the stopped battle

	// results
	int			frames;
	int			msec;			// measured wall time
	double		bots, server, game, client, cgame, front, back;	// msec summed over the frames
	double		batches, flushes, draws, triangles;	// batches: surfaces drawn; flushes: shader runs; draws: GL draw calls
	int			low;			// 99th percentile frame time, msec
	int			median;			// half of the frames are faster
	int			worst;
	int			maxLag;			// msec the server was behind the newest command (Connection Interrupted past ~2 s)
	int			lagFrames;		// frames with the server more than a second behind
	int			deadFrames;		// the own robot destroyed: another view
	int			spectatorFrames;
} benchStep_t;

typedef enum {
	BENCH_IDLE,
	BENCH_APPLY,
	BENCH_WARMUP_STATE,
	BENCH_MEASURE
} benchState_t;

static benchStep_t	benchSteps[BENCH_MAX_STEPS];
static int			benchNumSteps;
static int			benchStep;
static int			benchState;
static int			benchSeconds;
static int			benchStateTime;
static int			benchSettle;
static int			benchFrameTimes[BENCH_MAX_FRAMES];

// settings held for the whole run: frame rate not capped, no vsync, the back end
// waits for the video card so that its time shows
static benchSet_t	benchGlobal[] = {
	{ "com_maxfps", "0" },
	{ "r_swapInterval", "0" },
	{ "r_finish", "1" },
};
#define BENCH_NUM_GLOBAL	( sizeof( benchGlobal ) / sizeof( benchGlobal[0] ) )

// the automatic run of the benchmark exe (see CL_BenchAutoStart)
typedef enum {
	AUTO_OFF,
	AUTO_WAIT,			// for the battle
	AUTO_RAMP,			// adds the bots one by one and measures each number
	AUTO_SPECTATE,		// joined the spectators, the view settles
	AUTO_RUNNING,
	AUTO_DONE
} benchAuto_t;

static int		benchAuto;
static int		benchAutoTime;
static int		benchAutoPrinted;

static void Bench_Start( int seconds );

/*
the maps of the benchmark exe (27.11.2026): bench_map is one map, a list ("wk_islands
wk_bigbeacons") or "all"; one after another, each with the bots entering and all the
steps, the reports one after another in benchmark.log and a summary at the end
*/
#define BENCH_ALL_MAPS		"wk_islands wk_beacons wk_bigbeacons"
#define BENCH_MAX_MAPS		8

typedef struct {
	char	map[MAX_QPATH];
	float	liveFps, liveMedian, liveLow, liveFrame, liveBots, liveBack, liveDraws;
	float	frozenFps, frozenBack, frozenDraws;
} benchMapResult_t;

static benchMapResult_t	benchMapResults[BENCH_MAX_MAPS];
static int				benchMapIndex;		// the map running now

// the map with this number in bench_map, NULL past the end
const char *CL_BenchMapName( int number ) {
	static char	word[MAX_QPATH];
	const char	*list = Cvar_VariableString( "bench_map" );
	int			index = 0;

	if ( !list[0] ) {
		list = "wk_islands";
	} else if ( !Q_stricmp( list, "all" ) ) {
		list = BENCH_ALL_MAPS;
	}
	while ( *list ) {
		int		length = 0;
		while ( *list == ' ' || *list == ',' ) {
			list++;
		}
		if ( !*list ) {
			break;
		}
		while ( list[length] && list[length] != ' ' && list[length] != ',' && length < (int)sizeof( word ) - 1 ) {
			word[length] = list[length];
			length++;
		}
		word[length] = 0;
		list += length;
		while ( *list && *list != ' ' && *list != ',' ) {
			list++;		// a name too long: the rest dropped
		}
		if ( index == number && number < BENCH_MAX_MAPS ) {
			return word;
		}
		index++;
	}
	return NULL;
}

// bots entering one by one (the benchmark exe): a row for each number of robots in the battle
#define BENCH_MAX_RAMP		( MAX_CLIENTS / 2 + 1 )
static benchStep_t	benchRamp[BENCH_MAX_RAMP];
static int			benchNumRamp;
static qboolean		benchGlobalsApplied;

static void Bench_ApplyGlobals( void ) {
	int		index;

	if ( benchGlobalsApplied ) {
		return;
	}
	for ( index = 0; index < (int)BENCH_NUM_GLOBAL; index++ ) {
		Q_strncpyz( benchGlobal[index].saved, Cvar_VariableString( benchGlobal[index].name ), sizeof( benchGlobal[0].saved ) );
		Cvar_Set( benchGlobal[index].name, benchGlobal[index].value );
	}
	benchGlobalsApplied = qtrue;
}

static const char *benchDefaultSteps =
	"base:\n"
	"no_robots: r_drawentities 0\n"
	"no_world: r_drawworld 0\n"
	"no_textures: r_noTextures 1; vid_restart\n"
	"640x480: r_mode 3; vid_restart\n";


/*
=================
Bench_Parse
=================
*/
static void Bench_Parse( const char *text ) {
	char	line[512];
	int		length;

	benchNumSteps = 0;
	while ( *text && benchNumSteps < BENCH_MAX_STEPS ) {
		const char	*end = strchr( text, '\n' );
		char		*colon, *comment, *command, *next;
		benchStep_t	*step;

		length = end ? end - text : strlen( text );
		if ( length > (int)sizeof( line ) - 1 ) {
			length = sizeof( line ) - 1;
		}
		Com_Memcpy( line, text, length );
		line[length] = 0;
		text = end ? end + 1 : text + strlen( text );

		comment = strstr( line, "//" );
		if ( comment ) {
			*comment = 0;
		}
		colon = strchr( line, ':' );
		if ( !colon ) {
			continue;
		}
		*colon = 0;
		step = &benchSteps[benchNumSteps];
		Com_Memset( step, 0, sizeof( *step ) );
		{
			char	*start = line;
			while ( *start && *start <= ' ' ) {
				start++;
			}
			Q_strncpyz( step->name, start, sizeof( step->name ) );
		}
		while ( step->name[0] && step->name[strlen( step->name ) - 1] <= ' ' ) {
			step->name[strlen( step->name ) - 1] = 0;
		}
		if ( !step->name[0] ) {
			continue;
		}
		Q_strncpyz( step->commands, colon + 1, sizeof( step->commands ) );

		// the cvars to change and the restart
		for ( command = colon + 1; command && *command; command = next ) {
			char	*parse, *token, first[MAX_CVAR_VALUE_STRING];

			next = strchr( command, ';' );
			if ( next ) {
				*next++ = 0;
			}
			parse = command;
			token = COM_Parse( &parse );
			if ( !token[0] ) {
				continue;
			}
			Q_strncpyz( first, token, sizeof( first ) );
			if ( !Q_stricmp( first, "vid_restart" ) ) {
				step->restart = qtrue;
				continue;
			}
			if ( !Q_stricmp( first, "frozen" ) ) {
				step->frozen = qtrue;
				continue;
			}
			token = COM_Parse( &parse );
			if ( token[0] && !COM_Parse( &parse )[0] && step->numSets < BENCH_MAX_SETS ) {
				Q_strncpyz( step->sets[step->numSets].name, first, sizeof( step->sets[0].name ) );
				Q_strncpyz( step->sets[step->numSets].value, token, sizeof( step->sets[0].value ) );
				step->numSets++;
			}
		}
		// a stopped server sends no snapshot, and the cgame restarted by vid_restart waits for one
		if ( step->restart ) {
			step->frozen = qfalse;
		}
		benchNumSteps++;
	}
}


/*
=================
Bench_Apply / Bench_Restore
=================
*/
static void Bench_Apply( benchStep_t *step ) {
	int		index;

	// WALKER: the values to restore were taken at the start (Bench_Start). Read here,
	// a latched cvar (r_smp) still showed the previous step's value while its restore
	// waited for the vid_restart, and stayed changed for all the following steps
	for ( index = 0; index < step->numSets; index++ ) {
		Cvar_Set( step->sets[index].name, step->sets[index].value );
	}
	if ( step->restart ) {
		Cbuf_AddText( "vid_restart\n" );
	}
}

static void Bench_Restore( benchStep_t *step ) {
	int		index;

	for ( index = step->numSets - 1; index >= 0; index-- ) {
		Cvar_Set( step->sets[index].name, step->sets[index].saved );
	}
	if ( step->restart ) {
		Cbuf_AddText( "vid_restart\n" );
	}
}


/*
=================
Bench_Compare
=================
*/
static int QDECL Bench_Compare( const void *first, const void *second ) {
	return *(const int *)first - *(const int *)second;
}


static int Bench_CountWords( const char *text ) {
	int		count = 0;
	while ( *text ) {
		while ( *text == ' ' ) {
			text++;
		}
		if ( *text ) {
			count++;
		}
		while ( *text && *text != ' ' ) {
			text++;
		}
	}
	return count;
}


/*
=================
Bench_SystemReport

the system part of the reports: Windows, hardware, drivers, OpenGL limits
=================
*/
static void Bench_SystemReport( char *buffer, int size ) {
	Sys_SystemReport( buffer, size, cls.glconfig.vendor_string, cls.glconfig.renderer_string, cls.glconfig.version_string );
	Q_strcat( buffer, size, va( "  OpenGL limits: textures up to %i, %i texture units, %s compressed textures, %i extensions\n",
		cls.glconfig.maxTextureSize, cls.glconfig.maxActiveTextures,
		cls.glconfig.textureCompression != TC_NONE ? "with" : "no",
		Bench_CountWords( cls.glconfig.extensions_string ) ) );
}


/*
=================
Bench_Print

long text to the console in parts (one print takes a few kilobytes at most)
=================
*/
static void Bench_Print( const char *text ) {
	char	part[1024];
	int		length;

	while ( *text ) {
		length = strlen( text );
		if ( length > (int)sizeof( part ) - 1 ) {
			length = sizeof( part ) - 1;
		}
		Com_Memcpy( part, text, length );
		part[length] = 0;
		Com_Printf( "%s", part );
		text += length;
	}
}


/*
=================
CL_SystemInfo_f

"sysinfo": the system report to the console and sysinfo.txt
=================
*/
static char		systemReport[16384];

static qboolean Bench_WriteSystemFile( void ) {
	fileHandle_t	file;

	Bench_SystemReport( systemReport, sizeof( systemReport ) );
	file = FS_FOpenFileWrite( SYSINFO_FILE );
	if ( !file ) {
		return qfalse;
	}
	FS_Write( systemReport, strlen( systemReport ), file );
	FS_FCloseFile( file );
	return qtrue;
}

void CL_SystemInfo_f( void ) {
	qboolean	written = Bench_WriteSystemFile();
	Bench_Print( systemReport );
	if ( written ) {
		Com_Printf( "sysinfo: written to %s\n", SYSINFO_FILE );
	}
}

/*
=================
CL_SystemInfoFile

at the first start of the renderer: sysinfo.txt, without printing
=================
*/
void CL_SystemInfoFile( void ) {
	static qboolean	done;

	if ( !done ) {
		done = qtrue;
		Bench_WriteSystemFile();
	}
}


// how long a step is measured
static int Bench_MeasureMsec( qboolean frozen ) {
	int		msec = benchSeconds * 1000;

	if ( frozen ) {
		msec /= 2;
		if ( msec < BENCH_FROZEN_MIN ) {
			msec = BENCH_FROZEN_MIN;
		}
	}
	return msec;
}

/*
=================
Bench_Line

a row of the table; baseFps 0: this is the base itself
=================
*/
static void Bench_Line( benchStep_t *step, float baseFps, char *line, int size ) {
	float	fps, frames;

	if ( !step->frames || !step->msec ) {
		Com_sprintf( line, size, "%-20s  not measured\n", step->name );
		return;
	}
	frames = step->frames;
	fps = step->frames * 1000.0f / step->msec;
	Com_sprintf( line, size, "%-20s %5.1f  %5.1f  %5.1f  %5.1f  %5i  %5i  %4i %%  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %6.0f  %6.0f  %6.0f  %5.0f  %+7.0f %%  %s\n",
		step->name, fps, step->median > 0 ? 1000.0f / step->median : 0.0f, step->low > 0 ? 1000.0f / step->low : 0.0f, step->msec / frames,
		step->worst, step->maxLag, step->lagFrames * 100 / step->frames,
		step->bots / frames, ( step->server - step->bots ) / frames,
		( step->client - step->front - step->back ) / frames, ( step->cgame - step->front ) / frames, step->front / frames, step->back / frames,
		step->batches / frames, step->flushes / frames, step->draws / frames, step->triangles / frames / 1000.0f,
		baseFps > 0 ? ( fps / baseFps - 1.0f ) * 100.0f : 0.0f,
		step->deadFrames ? va( "dead %i %%", step->deadFrames * 100 / step->frames )
		: ( step->spectatorFrames * 2 > step->frames ? ( step->frozen ? "spect frozen" : "spect" ) : ( step->frozen ? "frozen" : "" ) ) );
}


/*
=================
Bench_Report
=================
*/
static void Bench_Report( void ) {
	static char		report[49152];
	fileHandle_t	file;
	qtime_t			now;
	char			line[512];
	float			baseFps = 0, frozenFps = 0;
	int				index, players = 0;

	for ( index = 0; index < MAX_CLIENTS; index++ ) {
		if ( cl.gameState.stringOffsets[CS_PLAYERS + index] ) {
			players++;
		}
	}
	Com_RealTime( &now );
	report[0] = 0;
	Q_strcat( report, sizeof( report ), va( "==== WALKER benchmark %04i-%02i-%02i %02i:%02i\n",
		now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min ) );
	{
		static char	system[16384];
		Bench_SystemReport( system, sizeof( system ) );
		Q_strcat( report, sizeof( report ), system );
	}
	Q_strcat( report, sizeof( report ), va( "game %ix%i, map %s, players %i, %i s a step (frozen %i s), r_finish 1 (see real_fps)\n",
		cls.glconfig.vidWidth, cls.glconfig.vidHeight, Cvar_VariableString( "mapname" ), players, benchSeconds, Bench_MeasureMsec( qtrue ) / 1000 ) );
	Q_strcat( report, sizeof( report ), va( "settings: r_picmip %s, r_sunShadows %s, cg_shadows %s, r_noportals %s, r_drawfog %s, r_dynamiclight %s, r_mode %s, r_vertexLight %s, r_glsl %s, r_smp %s, r_vbo %s\n",
		Cvar_VariableString( "r_picmip" ), Cvar_VariableString( "r_sunShadows" ), Cvar_VariableString( "cg_shadows" ),
		Cvar_VariableString( "r_noportals" ), Cvar_VariableString( "r_drawfog" ), Cvar_VariableString( "r_dynamiclight" ),
		Cvar_VariableString( "r_mode" ), Cvar_VariableString( "r_vertexLight" ), Cvar_VariableString( "r_glsl" ), Cvar_VariableString( "r_smp" ), Cvar_VariableString( "r_vbo" ) ) );
	Q_strcat( report, sizeof( report ), va( "effects: r_ext_multisample %s%s, r_ext_texture_filter_anisotropic %s, r_textureMode %s, r_texturebits %s, r_lodbias %s, r_waterRipple %s, r_mirrorScale %s, cg_marks %s, com_maxfps %s\n",
		Cvar_VariableString( "r_ext_multisample" ),
		!Cvar_VariableIntegerValue( "r_ext_multisample" ) ? ""
			: Cvar_VariableIntegerValue( "r_multisampleActive" ) > 0 ? va( " (in use: %ix)", Cvar_VariableIntegerValue( "r_multisampleActive" ) ) : " (not available on this card)",
		Cvar_VariableString( "r_ext_texture_filter_anisotropic" ),
		Cvar_VariableString( "r_textureMode" ), Cvar_VariableString( "r_texturebits" ), Cvar_VariableString( "r_lodbias" ),
		Cvar_VariableString( "r_waterRipple" ), Cvar_VariableString( "r_mirrorScale" ), Cvar_VariableString( "cg_marks" ),
		Cvar_VariableString( "com_maxfps" ) ) );
	Q_strcat( report, sizeof( report ), Cvar_VariableIntegerValue( "bench_mysettings" )
		? "the player's own settings (q3config.cfg, WalkerBench.exe +set bench_mysettings 1)\n"
		: "the stock settings (the player's own: WalkerBench.exe +set bench_mysettings 1)\n" );
	Q_strcat( report, sizeof( report ), va( "game code: server game %s, cgame %s\n",
		VM_IsNative( gvm ) ? "dll" : "qvm (interpreted)", VM_IsNative( cgvm ) ? "dll" : "qvm (interpreted)" ) );
	Q_strcat( report, sizeof( report ), "times: msec a frame on average; bots = bot AI, server = the rest of the server game,\n"
		"client = client and cgame, cgame = its part in the cgame (dll or qvm: see game code), front = renderer scene,\n"
		"back = GL calls and the video card\n" );
	Q_strcat( report, sizeof( report ), "worst = the longest frame, msec; lag = how far the server answer fell behind at most, msec\n"
		"(\"Connection Interrupted\" shows past about 64 frames), lag>1s = share of frames with it over a second\n" );
	Q_strcat( report, sizeof( report ), "view: spect = spectator, dead N % = the own robot destroyed for this share of the step\n" );
	Q_strcat( report, sizeof( report ), "median = fps of a typical frame (half are faster), what the fps counter on the screen mostly shows\n" );
	Q_strcat( report, sizeof( report ), "frozen = the battle stopped at one moment, every frame the same picture: the step differs by its\n"
		"settings only; vs base of a frozen step = against the first frozen step, of a live one = against base\n" );
	if ( benchNumRamp > 0 ) {
		float	rampBase = 0;
		Q_strcat( report, sizeof( report ), "bots entering the battle, two at a time (vs base = against no bots):\n" );
		Q_strcat( report, sizeof( report ), "robots in battle      fps median  1%low  frame  worst    lag lag>1s   bots server client  cgame  front   back   surfs shader  draws  ktris   vs base  view\n" );
		for ( index = 0; index < benchNumRamp; index++ ) {
			Bench_Line( &benchRamp[index], index == 0 ? 0 : rampBase, line, sizeof( line ) );
			if ( index == 0 && benchRamp[0].frames && benchRamp[0].msec ) {
				rampBase = benchRamp[0].frames * 1000.0f / benchRamp[0].msec;
			}
			Q_strcat( report, sizeof( report ), line );
		}
		Q_strcat( report, sizeof( report ), "then the steps, with all the bots:\n" );
	}
	Q_strcat( report, sizeof( report ), "step                  fps median  1%low  frame  worst    lag lag>1s   bots server client  cgame  front   back   surfs shader  draws  ktris   vs base  view\n" );

	for ( index = 0; index < benchNumSteps; index++ ) {
		benchStep_t	*step = &benchSteps[index];

		float		*kindBase = step->frozen ? &frozenFps : &baseFps;

		// the frozen steps compare with the step named "frozen" (a settling step may come first)
		if ( step->frozen && !Q_stricmp( step->name, "frozen" ) && step->frames && step->msec ) {
			frozenFps = 0;
		}
		Bench_Line( step, *kindBase, line, sizeof( line ) );
		if ( *kindBase == 0 && step->frames && step->msec ) {
			*kindBase = step->frames * 1000.0f / step->msec;
		}
		Q_strcat( report, sizeof( report ), line );
	}
	Q_strcat( report, sizeof( report ), "steps:\n" );
	for ( index = 0; index < benchNumSteps; index++ ) {
		Q_strcat( report, sizeof( report ), va( "  %-20s %s\n", benchSteps[index].name,
			benchSteps[index].commands[0] ? benchSteps[index].commands : " (as set)" ) );
	}
	Q_strcat( report, sizeof( report ), "\n" );

	Bench_Print( report );
	FS_FOpenFileByMode( BENCH_LOG, &file, FS_APPEND );
	if ( file ) {
		FS_Write( report, strlen( report ), file );
		FS_FCloseFile( file );
		Com_Printf( "benchmark: written to %s\n", BENCH_LOG );
	} else {
		Com_Printf( "benchmark: could not write %s\n", BENCH_LOG );
	}
}


/*
=================
Bench_Finish
=================
*/
// the numbers of the map just measured, for the summary
static void Bench_KeepMapResult( void ) {
	benchMapResult_t	*result;
	int					index;

	if ( benchMapIndex >= BENCH_MAX_MAPS ) {
		return;
	}
	result = &benchMapResults[benchMapIndex];
	Com_Memset( result, 0, sizeof( *result ) );
	Q_strncpyz( result->map, Cvar_VariableString( "mapname" ), sizeof( result->map ) );
	for ( index = 0; index < benchNumSteps; index++ ) {
		benchStep_t	*step = &benchSteps[index];
		float		frames = step->frames;

		if ( !step->frames || !step->msec ) {
			continue;
		}
		if ( !Q_stricmp( step->name, "base" ) ) {
			result->liveFps = step->frames * 1000.0f / step->msec;
			result->liveMedian = step->median > 0 ? 1000.0f / step->median : 0;
			result->liveLow = step->low > 0 ? 1000.0f / step->low : 0;
			result->liveFrame = step->msec / frames;
			result->liveBots = step->bots / frames;
			result->liveBack = step->back / frames;
			result->liveDraws = step->draws / frames;
		} else if ( !Q_stricmp( step->name, "frozen" ) ) {
			result->frozenFps = step->frames * 1000.0f / step->msec;
			result->frozenBack = step->back / frames;
			result->frozenDraws = step->draws / frames;
		}
	}
}

static void Bench_WriteSummary( int count ) {
	static char		summary[4096];
	fileHandle_t	file;
	int				index;

	summary[0] = 0;
	Q_strcat( summary, sizeof( summary ), va( "==== WALKER benchmark summary, %i maps (%s settings): the base step\n"
		"(the battle with all the bots) and the frozen one of each report above\n",
		count, Cvar_VariableIntegerValue( "bench_mysettings" ) ? "the player's own" : "the stock" ) );
	Q_strcat( summary, sizeof( summary ), "map                 live: fps median  1%low  frame   bots   back  draws   frozen: fps   back  draws\n" );
	for ( index = 0; index < count && index < BENCH_MAX_MAPS; index++ ) {
		benchMapResult_t	*result = &benchMapResults[index];
		Q_strcat( summary, sizeof( summary ), va( "%-18s  %9.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.0f   %11.1f  %5.1f  %5.0f\n",
			result->map, result->liveFps, result->liveMedian, result->liveLow, result->liveFrame, result->liveBots,
			result->liveBack, result->liveDraws, result->frozenFps, result->frozenBack, result->frozenDraws ) );
	}
	Q_strcat( summary, sizeof( summary ), "\n" );
	Bench_Print( summary );
	FS_FOpenFileByMode( BENCH_LOG, &file, FS_APPEND );
	if ( file ) {
		FS_Write( summary, strlen( summary ), file );
		FS_FCloseFile( file );
	}
}

static void Bench_Finish( qboolean report ) {
	int		index;

	if ( benchGlobalsApplied ) {
		for ( index = BENCH_NUM_GLOBAL - 1; index >= 0; index-- ) {
			Cvar_Set( benchGlobal[index].name, benchGlobal[index].saved );
		}
		benchGlobalsApplied = qfalse;
	}
	com_profiling = 0;
	com_frozen = 0;
	benchState = BENCH_IDLE;
	if ( report ) {
		Bench_Report();
	}
	if ( benchAuto == AUTO_RUNNING ) {
		const char	*next;

		if ( report ) {
			Bench_KeepMapResult();
		}
		next = CL_BenchMapName( benchMapIndex + 1 );
		if ( report && next ) {
			// the next map: the bots out (the ramp counts them again from none), the same steps
			benchMapIndex++;
			Com_Printf( "benchmark: the next map, %s\n", next );
			Cbuf_AddText( va( "kick allbots\nmap %s\n", next ) );
			benchAuto = AUTO_WAIT;
			benchAutoTime = 0;
			com_profiling = 1;		// CL_BenchFrame every frame again (switched off above)
			return;
		}
		if ( report && benchMapIndex > 0 ) {
			Bench_WriteSummary( benchMapIndex + 1 );
		}
		benchAuto = AUTO_DONE;
		Sys_ShowFile( FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), BASEGAME, BENCH_LOG ) );
		Cbuf_AddText( "quit\n" );
	}
}


/*
=================
Bench_Accumulate / Bench_Close

one frame into a step; at its end the 1 % low and the median
=================
*/
static void Bench_Accumulate( benchStep_t *step, int frameMsec, int serverMsec, int clientMsec ) {
	if ( cl.snap.ps.pm_type == BENCH_PM_SPECTATOR ) {
		step->spectatorFrames++;
	} else if ( cl.snap.ps.pm_type == BENCH_PM_DEAD || cl.snap.ps.stats[0] <= 0 ) {
		step->deadFrames++;
	}
	{
		// how far the snapshots fall behind the commands sent (what cgame shows as Connection Interrupted)
		int		lag = cl.cmds[cl.cmdNumber & CMD_MASK].serverTime - cl.snap.ps.commandTime;
		if ( lag > step->maxLag ) {
			step->maxLag = lag;
		}
		if ( lag > 1000 ) {
			step->lagFrames++;
		}
	}
	if ( step->frames < BENCH_MAX_FRAMES ) {
		benchFrameTimes[step->frames] = frameMsec;
	}
	step->frames++;
	step->msec += frameMsec;
	if ( frameMsec > step->worst ) {
		step->worst = frameMsec;
	}
	step->bots += time_bots;
	step->game += time_game;
	step->server += serverMsec;
	step->client += clientMsec;
	step->cgame += time_cgame;
	step->front += time_frontend;
	step->back += time_backend;
	{
		int		batches = 0, flushes = 0, draws = 0, triangles = 0;
		if ( re.GetFrameStats ) {
			re.GetFrameStats( &batches, &flushes, &draws, &triangles );
		}
		step->batches += batches;
		step->flushes += flushes;
		step->draws += draws;
		step->triangles += triangles;
	}
}

static void Bench_Close( benchStep_t *step ) {
	int		count = step->frames < BENCH_MAX_FRAMES ? step->frames : BENCH_MAX_FRAMES;

	if ( count <= 0 ) {
		return;
	}
	// 1 % low: the frame time 99 % of the frames are faster than
	qsort( benchFrameTimes, count, sizeof( int ), Bench_Compare );
	step->low = benchFrameTimes[count * 99 / 100 < count ? count * 99 / 100 : count - 1];
	step->median = benchFrameTimes[count / 2];
}


/*
===========================================================================
WALKER: the benchmark exe (WalkerBench.exe, a copy of Walker.exe)

benchmode.cfg (pak0) starts a fixed battle with the stock settings; here the
run waits for the bots (bench_players in teams, at most two minutes), turns the
player into a spectator (the camera stays at the intermission point of the
map), runs the benchmark with bench_seconds a step, opens benchmark.log and
quits.
===========================================================================
*/

void CL_BenchAutoStart( void ) {
	benchMapIndex = 0;
	benchAuto = AUTO_WAIT;
	benchAutoTime = 0;
	com_profiling = 1;		// CL_BenchFrame is called every frame
}

static int Bench_PlayersInTeams( void ) {
	int		index, count = 0;

	for ( index = 0; index < MAX_CLIENTS; index++ ) {
		int		offset = cl.gameState.stringOffsets[CS_PLAYERS + index];
		if ( offset && atoi( Info_ValueForKey( cl.gameState.stringData + offset, "t" ) ) != 3 ) {		// TEAM_SPECTATOR (bg_public.h)
			count++;
		}
	}
	return count;
}

#define RAMP_SETTLE		2500		// msec after a bot entered, not measured (it loads, spawns)
#define RAMP_MEASURE	6000
#define RAMP_JOIN_WAIT	15000		// a bot that does not enter by then is given up
#define RAMP_STEP		2			// bots added between two measurements

static const char	*benchBotNames[] = { "Anvil", "Bastion", "Cinder", "Drake", "Ember", "Flint", "Granite", "Hornet", "Jackal" };
static int			benchRampPhase;		// 0 settle, 1 measure, 2 waiting for the next bot
static int			benchRampTime;
static int			benchRampTarget;	// players wanted before the next measurement

static void Bench_AutoFrame( int frameMsec, int serverMsec, int clientMsec ) {
	int		now = Sys_Milliseconds();
	int		wanted = Cvar_VariableIntegerValue( "bench_players" );
	int		players;

	switch ( benchAuto ) {
	case AUTO_WAIT:
		if ( cls.state != CA_ACTIVE ) {
			benchAutoTime = 0;
			return;
		}
		if ( !benchAutoTime ) {
			benchAutoTime = now;
			Cbuf_AddText( "team spectator\n" );
			return;
		}
		if ( now - benchAutoTime < 2000 ) {
			return;
		}
		// the view settled: measure from no bots on
		Bench_ApplyGlobals();
		Com_Memset( benchRamp, 0, sizeof( benchRamp ) );
		benchNumRamp = 1;
		Com_sprintf( benchRamp[0].name, sizeof( benchRamp[0].name ), "%i", Bench_PlayersInTeams() );
		benchRampPhase = 0;
		benchRampTime = now;
		benchAuto = AUTO_RAMP;
		Com_Printf( "benchmark: measuring as the bots enter\n" );
		return;

	case AUTO_RAMP:
		if ( cls.state != CA_ACTIVE ) {
			return;
		}
		players = Bench_PlayersInTeams();
		if ( benchRampPhase == 0 ) {
			if ( now - benchRampTime >= RAMP_SETTLE ) {
				benchRampPhase = 1;
				benchRampTime = now;
			}
			return;
		}
		if ( benchRampPhase == 1 ) {
			benchStep_t	*row = &benchRamp[benchNumRamp - 1];
			Bench_Accumulate( row, frameMsec, serverMsec, clientMsec );
			if ( now - benchRampTime < RAMP_MEASURE ) {
				return;
			}
			Bench_Close( row );
			Com_Printf( "benchmark: %s robots %.1f fps\n", row->name, row->frames * 1000.0f / ( row->msec > 0 ? row->msec : 1 ) );
			if ( players >= wanted || benchNumRamp >= BENCH_MAX_RAMP ) {
				benchAuto = AUTO_SPECTATE;
				benchAutoTime = now;
				return;
			}
			// the next bots (two: the cost of a robot is about even), teams in turn
			benchRampTarget = players + RAMP_STEP < wanted ? players + RAMP_STEP : wanted;
			{
				int		next;
				for ( next = players; next < benchRampTarget; next++ ) {
					Cbuf_AddText( va( "addbot %s 3 %s\n", benchBotNames[next % ( sizeof( benchBotNames ) / sizeof( benchBotNames[0] ) )],
						( next & 1 ) ? "blue" : "red" ) );
				}
			}
			benchRampPhase = 2;
			benchRampTime = now;
			return;
		}
		// waiting for the bot to enter
		if ( players >= benchRampTarget || now - benchRampTime > RAMP_JOIN_WAIT ) {
			benchStep_t	*row = &benchRamp[benchNumRamp++];
			Com_Memset( row, 0, sizeof( *row ) );
			Com_sprintf( row->name, sizeof( row->name ), "%i", players );
			benchRampPhase = 0;
			benchRampTime = now;
		}
		return;

	case AUTO_SPECTATE:
		if ( now - benchAutoTime > 1000 ) {
			benchAuto = AUTO_RUNNING;
			Bench_Start( Cvar_VariableIntegerValue( "bench_seconds" ) > 0 ? Cvar_VariableIntegerValue( "bench_seconds" ) : 10 );
		}
		return;
	default:
		return;
	}
}


/*
=================
CL_BenchFrame

every frame while the benchmark runs (Com_Frame)
=================
*/
void CL_BenchFrame( int frameMsec, int serverMsec, int clientMsec ) {
	benchStep_t	*step;
	int			now = Sys_Milliseconds();

	if ( benchState == BENCH_IDLE ) {
		if ( benchAuto == AUTO_WAIT || benchAuto == AUTO_RAMP || benchAuto == AUTO_SPECTATE ) {
			Bench_AutoFrame( frameMsec, serverMsec, clientMsec );
		}
		return;
	}
	step = &benchSteps[benchStep];
	if ( cls.state < CA_LOADING ) {
		Com_Printf( "benchmark: stopped, not in a battle\n" );
		if ( benchState != BENCH_APPLY ) {
			Bench_Restore( step );
		}
		Bench_Finish( qtrue );
		return;
	}
	if ( cls.state != CA_ACTIVE ) {
		// a vid_restart reloads the cgame: wait, and measure again from the start
		if ( benchState == BENCH_MEASURE ) {
			step->frames = step->msec = 0;
			step->bots = step->server = step->game = step->client = step->cgame = step->front = step->back = 0;
			step->batches = step->flushes = step->draws = step->triangles = 0;
			step->worst = step->deadFrames = step->spectatorFrames = 0;
			step->maxLag = step->lagFrames = 0;
			benchState = BENCH_WARMUP_STATE;
		}
		benchSettle = BENCH_SETTLE_FRAMES;
		return;
	}

	switch ( benchState ) {
	case BENCH_APPLY:
		Com_Printf( "benchmark: step %i/%i %s%s\n", benchStep + 1, benchNumSteps, step->name, step->frozen ? " (frozen)" : "" );
		// the first frozen step stops the battle, the following ones keep the same moment
		com_frozen = step->frozen;
		Bench_Apply( step );
		benchState = BENCH_WARMUP_STATE;
		benchSettle = BENCH_SETTLE_FRAMES;
		benchStateTime = now;
		return;

	case BENCH_WARMUP_STATE:
		if ( benchSettle > 0 ) {
			benchSettle--;
			benchStateTime = now;		// the warmup counts from after the restart
			return;
		}
		if ( now - benchStateTime < ( step->frozen ? BENCH_FROZEN_WARMUP : BENCH_WARMUP ) ) {
			return;
		}
		benchState = BENCH_MEASURE;
		benchStateTime = now;
		return;

	case BENCH_MEASURE:
		if ( cl.snap.ps.pm_type == BENCH_PM_INTERMISSION ) {
			// the battle is over, the view stands still: the rest would not compare
			Com_Printf( "benchmark: the battle ended, stopped at step %s\n", step->name );
			step->frames = 0;
			Bench_Restore( step );
			Bench_Finish( qtrue );
			return;
		}
		Bench_Accumulate( step, frameMsec, serverMsec, clientMsec );
		if ( now - benchStateTime < Bench_MeasureMsec( step->frozen ) ) {
			return;
		}
		Bench_Close( step );
		Com_Printf( "benchmark: %s %.1f fps\n", step->name, step->frames * 1000.0f / ( step->msec > 0 ? step->msec : 1 ) );
		Bench_Restore( step );
		benchStep++;
		if ( benchStep >= benchNumSteps ) {
			Bench_Finish( qtrue );
			return;
		}
		benchState = BENCH_APPLY;
		return;
	}
}


/*
=================
CL_Benchmark_f
=================
*/
void CL_Benchmark_f( void ) {
	if ( benchState != BENCH_IDLE ) {
		if ( !Q_stricmp( Cmd_Argv( 1 ), "stop" ) ) {
			Com_Printf( "benchmark: stopped\n" );
			if ( benchState != BENCH_APPLY ) {
				Bench_Restore( &benchSteps[benchStep] );
			}
			Bench_Finish( qtrue );
		} else {
			Com_Printf( "benchmark: running (step %i/%i); \"benchmark stop\" to stop\n", benchStep + 1, benchNumSteps );
		}
		return;
	}
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "benchmark: start a battle first (best 5 x 5 with bots), then \"benchmark [seconds a step]\"\n" );
		return;
	}
	Bench_Start( Cmd_Argc() > 1 ? atoi( Cmd_Argv( 1 ) ) : 10 );
}


/*
=================
Bench_Start
=================
*/
static void Bench_Start( int seconds ) {
	char	*text = NULL;
	int		index;

	benchSeconds = seconds < 2 ? 2 : seconds;
	if ( FS_ReadFile( BENCH_FILE, (void **)&text ) > 0 && text ) {
		Bench_Parse( text );
		FS_FreeFile( text );
	} else {
		Bench_Parse( benchDefaultSteps );
	}
	if ( !benchNumSteps ) {
		Com_Printf( "benchmark: no steps in %s\n", BENCH_FILE );
		return;
	}
	Bench_ApplyGlobals();
	for ( index = 0; index < benchNumSteps; index++ ) {
		int		setIndex;
		for ( setIndex = 0; setIndex < benchSteps[index].numSets; setIndex++ ) {
			benchSet_t	*set = &benchSteps[index].sets[setIndex];
			Q_strncpyz( set->saved, Cvar_VariableString( set->name ), sizeof( set->saved ) );
		}
	}
	Com_Printf( "benchmark: %i steps, about %i s; do not touch the game; \"benchmark stop\" stops\n",
		benchNumSteps, benchNumSteps * ( benchSeconds + BENCH_WARMUP / 1000 ) );
	benchStep = 0;
	benchState = BENCH_APPLY;
	com_profiling = 1;
}
