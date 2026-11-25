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
"//" starts a comment.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "client.h"

#define BENCH_FILE			"benchmark.txt"
#define BENCH_LOG			"benchmark.log"
#define SYSINFO_FILE		"sysinfo.txt"
#define BENCH_MAX_STEPS		48
#define BENCH_MAX_SETS		8			// cvars a step changes
#define BENCH_MAX_FRAMES	16384		// frame times kept per step (1 % low)
#define BENCH_WARMUP		3000		// msec after a step is applied, not measured
#define BENCH_SETTLE_FRAMES	3			// and these frames (a vid_restart takes one long frame)

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

	// results
	int			frames;
	int			msec;			// measured wall time
	double		bots, server, game, client, front, back;	// msec summed over the frames
	double		batches, triangles;
	int			low;			// 99th percentile frame time, msec
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
			token = COM_Parse( &parse );
			if ( token[0] && !COM_Parse( &parse )[0] && step->numSets < BENCH_MAX_SETS ) {
				Q_strncpyz( step->sets[step->numSets].name, first, sizeof( step->sets[0].name ) );
				Q_strncpyz( step->sets[step->numSets].value, token, sizeof( step->sets[0].value ) );
				step->numSets++;
			}
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

	for ( index = 0; index < step->numSets; index++ ) {
		Q_strncpyz( step->sets[index].saved, Cvar_VariableString( step->sets[index].name ), sizeof( step->sets[0].saved ) );
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
	float			baseFps = 0;
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
	Q_strcat( report, sizeof( report ), va( "game %ix%i, map %s, players %i, %i s a step, r_finish 1 (see real_fps)\n",
		cls.glconfig.vidWidth, cls.glconfig.vidHeight, Cvar_VariableString( "mapname" ), players, benchSeconds ) );
	Q_strcat( report, sizeof( report ), va( "settings: r_picmip %s, r_sunShadows %s, cg_shadows %s, r_noportals %s, r_drawfog %s, r_dynamiclight %s, r_mode %s, r_vertexLight %s\n",
		Cvar_VariableString( "r_picmip" ), Cvar_VariableString( "r_sunShadows" ), Cvar_VariableString( "cg_shadows" ),
		Cvar_VariableString( "r_noportals" ), Cvar_VariableString( "r_drawfog" ), Cvar_VariableString( "r_dynamiclight" ),
		Cvar_VariableString( "r_mode" ), Cvar_VariableString( "r_vertexLight" ) ) );
	Q_strcat( report, sizeof( report ), "times: msec a frame on average; bots = bot AI, server = the rest of the server game,\n"
		"client = client and cgame, front = renderer scene, back = GL calls and the video card\n" );
	Q_strcat( report, sizeof( report ), "worst = the longest frame, msec; lag = how far the server answer fell behind at most, msec\n"
		"(\"Connection Interrupted\" shows past about 64 frames), lag>1s = share of frames with it over a second\n" );
	Q_strcat( report, sizeof( report ), "view: spect = spectator, dead N % = the own robot destroyed for this share of the step\n" );
	Q_strcat( report, sizeof( report ), "step                  fps  1%low  frame  worst    lag lag>1s   bots server client  front   back  batches  ktris   vs base  view\n" );

	for ( index = 0; index < benchNumSteps; index++ ) {
		benchStep_t	*step = &benchSteps[index];
		float		fps, frames;

		if ( !step->frames || !step->msec ) {
			Com_sprintf( line, sizeof( line ), "%-20s  not measured\n", step->name );
			Q_strcat( report, sizeof( report ), line );
			continue;
		}
		frames = step->frames;
		fps = step->frames * 1000.0f / step->msec;
		if ( index == 0 ) {
			baseFps = fps;
		}
		Com_sprintf( line, sizeof( line ), "%-20s %5.1f  %5.1f  %5.1f  %5i  %5i  %4i %%  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %7.0f  %5.0f  %+7.0f %%  %s\n",
			step->name, fps, step->low > 0 ? 1000.0f / step->low : 0.0f, step->msec / frames,
			step->worst, step->maxLag, step->lagFrames * 100 / step->frames,
			step->bots / frames, ( step->server - step->bots ) / frames,
			( step->client - step->front - step->back ) / frames, step->front / frames, step->back / frames,
			step->batches / frames, step->triangles / frames / 1000.0f,
			baseFps > 0 ? ( fps / baseFps - 1.0f ) * 100.0f : 0.0f,
			step->deadFrames ? va( "dead %i %%", step->deadFrames * 100 / step->frames )
			: ( step->spectatorFrames * 2 > step->frames ? "spect" : "" ) );
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
static void Bench_Finish( qboolean report ) {
	int		index;

	for ( index = BENCH_NUM_GLOBAL - 1; index >= 0; index-- ) {
		Cvar_Set( benchGlobal[index].name, benchGlobal[index].saved );
	}
	com_profiling = 0;
	benchState = BENCH_IDLE;
	if ( report ) {
		Bench_Report();
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
			step->bots = step->server = step->game = step->client = step->front = step->back = 0;
			step->batches = step->triangles = 0;
			step->worst = step->deadFrames = step->spectatorFrames = 0;
			step->maxLag = step->lagFrames = 0;
			benchState = BENCH_WARMUP_STATE;
		}
		benchSettle = BENCH_SETTLE_FRAMES;
		return;
	}

	switch ( benchState ) {
	case BENCH_APPLY:
		Com_Printf( "benchmark: step %i/%i %s\n", benchStep + 1, benchNumSteps, step->name );
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
		if ( now - benchStateTime < BENCH_WARMUP ) {
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
		step->front += time_frontend;
		step->back += time_backend;
		{
			int		batches = 0, triangles = 0;
			if ( re.GetFrameStats ) {
				re.GetFrameStats( &batches, &triangles );
			}
			step->batches += batches;
			step->triangles += triangles;
		}
		if ( now - benchStateTime < benchSeconds * 1000 ) {
			return;
		}
		// 1 % low: the frame time 99 % of the frames are faster than
		{
			int		count = step->frames < BENCH_MAX_FRAMES ? step->frames : BENCH_MAX_FRAMES;
			qsort( benchFrameTimes, count, sizeof( int ), Bench_Compare );
			step->low = benchFrameTimes[count * 99 / 100 < count ? count * 99 / 100 : count - 1];
		}
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
	char	*text = NULL;
	int		index;

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
	benchSeconds = Cmd_Argc() > 1 ? atoi( Cmd_Argv( 1 ) ) : 10;
	if ( benchSeconds < 2 ) {
		benchSeconds = 2;
	}
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
	for ( index = 0; index < (int)BENCH_NUM_GLOBAL; index++ ) {
		Q_strncpyz( benchGlobal[index].saved, Cvar_VariableString( benchGlobal[index].name ), sizeof( benchGlobal[0].saved ) );
		Cvar_Set( benchGlobal[index].name, benchGlobal[index].value );
	}
	Com_Printf( "benchmark: %i steps, about %i s; do not touch the game; \"benchmark stop\" stops\n",
		benchNumSteps, benchNumSteps * ( benchSeconds + BENCH_WARMUP / 1000 ) );
	benchStep = 0;
	benchState = BENCH_APPLY;
	com_profiling = 1;
}
