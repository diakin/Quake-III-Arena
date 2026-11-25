/*
===========================================================================
WALKER: beacon capture, client side (see game/g_beacons.c).

Each beacon is drawn as a beam of light going up from its middle and a ring on
the ground. The colour depends on who looks: a beacon of the viewer's team is
blue, of the other team red, a free one white; while it is being taken the
colour goes over smoothly. The HUD shows the beacons as letters at the top of
the screen, the points of both teams with the time left, and a bar of the
capture when the player stands on a beacon.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "cg_local.h"

#define BEAM_HEIGHT			2400
#define BEAM_GLOW_WIDTH		56
#define BEAM_CORE_WIDTH		14

static const vec3_t	colorAlly = { 0.25f, 0.55f, 1.0f };
static const vec3_t	colorEnemy = { 1.0f, 0.22f, 0.16f };
static const vec3_t	colorFree = { 1.0f, 1.0f, 1.0f };

/*
=================
CG_BeaconRelative

Control of the beacon from the viewer's side: +1 own team, -1 the other team.
A spectator sees blue as +1.
=================
*/
static float CG_BeaconRelative( const entityState_t *state ) {
	float	control = state->angles2[0];

	if ( cg.snap && cg.snap->ps.persistant[PERS_TEAM] == TEAM_RED ) {
		return -control;
	}
	return control;
}

static void CG_BeaconColor( float relative, vec3_t color ) {
	float	amount = fabs( relative );
	const float	*team;

	if ( amount > 1 ) {
		amount = 1;
	}
	team = relative > 0 ? colorAlly : colorEnemy;

	color[0] = colorFree[0] + ( team[0] - colorFree[0] ) * amount;
	color[1] = colorFree[1] + ( team[1] - colorFree[1] ) * amount;
	color[2] = colorFree[2] + ( team[2] - colorFree[2] ) * amount;
}

/*
=================
CG_BeamQuad

A vertical quad turned to the camera.
=================
*/
static void CG_BeamQuad( const vec3_t base, float width, const vec3_t color, float strength ) {
	polyVert_t	verts[4];
	vec3_t		toEye, right, up = { 0, 0, 1 };
	byte		red, green, blue;
	int			index;

	VectorSubtract( cg.refdef.vieworg, base, toEye );
	toEye[2] = 0;
	if ( VectorNormalize( toEye ) < 0.001f ) {
		toEye[0] = 1;
	}
	CrossProduct( up, toEye, right );
	VectorScale( right, width * 0.5f, right );

	red = (byte)( Com_Clamp( 0, 1, color[0] * strength ) * 255 );
	green = (byte)( Com_Clamp( 0, 1, color[1] * strength ) * 255 );
	blue = (byte)( Com_Clamp( 0, 1, color[2] * strength ) * 255 );

	VectorSubtract( base, right, verts[0].xyz );
	VectorAdd( base, right, verts[1].xyz );
	VectorAdd( base, right, verts[2].xyz );
	verts[2].xyz[2] += BEAM_HEIGHT;
	VectorSubtract( base, right, verts[3].xyz );
	verts[3].xyz[2] += BEAM_HEIGHT;
	verts[0].st[0] = 0; verts[0].st[1] = 1;
	verts[1].st[0] = 1; verts[1].st[1] = 1;
	verts[2].st[0] = 1; verts[2].st[1] = 0;
	verts[3].st[0] = 0; verts[3].st[1] = 0;
	for ( index = 0 ; index < 4 ; index++ ) {
		verts[index].modulate[0] = red;
		verts[index].modulate[1] = green;
		verts[index].modulate[2] = blue;
		verts[index].modulate[3] = 255;
	}
	trap_R_AddPolyToScene( cgs.media.beaconBeamShader, 4, verts );
}

/*
=================
Beacon rings

The ring on the ground follows the relief: CM_MarkFragments clips it against the
brushes under the beacon. On wk_islands the ground is made of small triangular
brushes, one ring gave up to 256 fragments, and five rings every frame used up the
renderer limit of polygons (r_maxpolys): the beams of the last beacons were
dropped. The ground does not change, so the fragments are computed once per
beacon and only coloured each frame; the rings are drawn after all beams
(CG_AddBeaconRings, after the packet entities).
=================
*/
#define MAX_RING_BEACONS	8
#define MAX_RING_FRAGMENTS	256
#define MAX_RING_POINTS		1536

typedef struct {
	int			entityNum;			// -1 free
	vec3_t		origin;
	float		radius;
	int			numFragments;
	int			fragmentFirst[MAX_RING_FRAGMENTS];
	int			fragmentPoints[MAX_RING_FRAGMENTS];
	int			numPoints;
	vec3_t		points[MAX_RING_POINTS];
	float		texture[MAX_RING_POINTS][2];
} beaconRing_t;

static beaconRing_t	beaconRings[MAX_RING_BEACONS];
static qboolean		beaconRingsCleared;
static centity_t	*beaconsThisFrame[MAX_RING_BEACONS];
static int			numBeaconsThisFrame;

static beaconRing_t *CG_BeaconRing( const centity_t *cent ) {
	static markFragment_t	fragments[MAX_RING_FRAGMENTS];	// static: too large for the QVM stack
	static vec3_t			markPoints[MAX_RING_POINTS];
	beaconRing_t	*ring, *freeRing = NULL;
	vec3_t			axis[3], corners[4], projection, delta;
	float			radius = cent->currentState.origin2[0];
	int				index, corner, point;

	if ( !beaconRingsCleared ) {
		for ( index = 0 ; index < MAX_RING_BEACONS ; index++ ) {
			beaconRings[index].entityNum = -1;
		}
		beaconRingsCleared = qtrue;
	}
	for ( index = 0 ; index < MAX_RING_BEACONS ; index++ ) {
		ring = &beaconRings[index];
		if ( ring->entityNum == cent->currentState.number ) {
			if ( VectorCompare( ring->origin, cent->lerpOrigin ) && ring->radius == radius ) {
				return ring;
			}
			freeRing = ring;		// moved or resized (a new map): compute again
			break;
		}
		if ( ring->entityNum < 0 && !freeRing ) {
			freeRing = ring;
		}
	}
	if ( !freeRing || radius <= 0 ) {
		return NULL;
	}
	ring = freeRing;
	ring->entityNum = cent->currentState.number;
	VectorCopy( cent->lerpOrigin, ring->origin );
	ring->radius = radius;

	// the same square as CG_ImpactMark with the normal straight up
	VectorSet( axis[0], 0, 0, 1 );
	VectorSet( axis[1], 0, 1, 0 );
	VectorSet( axis[2], -1, 0, 0 );
	for ( index = 0 ; index < 3 ; index++ ) {
		corners[0][index] = ring->origin[index] - radius * axis[1][index] - radius * axis[2][index];
		corners[1][index] = ring->origin[index] + radius * axis[1][index] - radius * axis[2][index];
		corners[2][index] = ring->origin[index] + radius * axis[1][index] + radius * axis[2][index];
		corners[3][index] = ring->origin[index] - radius * axis[1][index] + radius * axis[2][index];
	}
	VectorSet( projection, 0, 0, -20 );
	ring->numFragments = trap_CM_MarkFragments( 4, (void *)corners, projection, MAX_RING_POINTS, markPoints[0],
		MAX_RING_FRAGMENTS, fragments );
	ring->numPoints = 0;
	for ( index = 0 ; index < ring->numFragments ; index++ ) {
		int		count = fragments[index].numPoints;

		if ( count > MAX_VERTS_ON_POLY ) {
			count = MAX_VERTS_ON_POLY;
		}
		ring->fragmentFirst[index] = ring->numPoints;
		ring->fragmentPoints[index] = count;
		for ( corner = 0 ; corner < count ; corner++ ) {
			point = ring->numPoints++;
			VectorCopy( markPoints[fragments[index].firstPoint + corner], ring->points[point] );
			VectorSubtract( ring->points[point], ring->origin, delta );
			ring->texture[point][0] = 0.5f + DotProduct( delta, axis[1] ) * 0.5f / radius;
			ring->texture[point][1] = 0.5f + DotProduct( delta, axis[2] ) * 0.5f / radius;
		}
	}
	return ring;
}

static void CG_DrawBeaconRing( centity_t *cent, const vec3_t color ) {
	beaconRing_t	*ring = CG_BeaconRing( cent );
	polyVert_t		verts[MAX_VERTS_ON_POLY];
	byte			red, green, blue;
	int				fragment, corner, point;

	if ( !ring ) {
		return;
	}
	red = (byte)( Com_Clamp( 0, 1, color[0] ) * 255 );
	green = (byte)( Com_Clamp( 0, 1, color[1] ) * 255 );
	blue = (byte)( Com_Clamp( 0, 1, color[2] ) * 255 );
	for ( fragment = 0 ; fragment < ring->numFragments ; fragment++ ) {
		for ( corner = 0 ; corner < ring->fragmentPoints[fragment] ; corner++ ) {
			point = ring->fragmentFirst[fragment] + corner;
			VectorCopy( ring->points[point], verts[corner].xyz );
			verts[corner].st[0] = ring->texture[point][0];
			verts[corner].st[1] = ring->texture[point][1];
			verts[corner].modulate[0] = red;
			verts[corner].modulate[1] = green;
			verts[corner].modulate[2] = blue;
			verts[corner].modulate[3] = 255;
		}
		trap_R_AddPolyToScene( cgs.media.beaconRingShader, ring->fragmentPoints[fragment], verts );
	}
}

/*
=================
CG_BeaconPulse
=================
*/
static float CG_BeaconPulse( const entityState_t *state ) {
	// a beacon being fought for flickers a little
	return ( state->generic1 == 3 ) ? 0.75f + 0.25f * sin( cg.time * 0.02f ) : 1.0f;
}

/*
=================
CG_Beacon

the beam; the ring follows in CG_AddBeaconRings
=================
*/
void CG_Beacon( centity_t *cent ) {
	const entityState_t	*state = &cent->currentState;
	vec3_t				color;
	float				pulse = CG_BeaconPulse( state );

	CG_BeaconColor( CG_BeaconRelative( state ), color );
	CG_BeamQuad( cent->lerpOrigin, BEAM_GLOW_WIDTH, color, 0.45f * pulse );
	CG_BeamQuad( cent->lerpOrigin, BEAM_CORE_WIDTH, color, 1.0f * pulse );
	if ( numBeaconsThisFrame < MAX_RING_BEACONS ) {
		beaconsThisFrame[numBeaconsThisFrame++] = cent;
	}
}

/*
=================
CG_AddBeaconRings

after CG_AddPacketEntities: rings of the beacons drawn this frame
=================
*/
void CG_AddBeaconRings( void ) {
	int		index;
	vec3_t	color;
	float	pulse;

	for ( index = 0 ; index < numBeaconsThisFrame ; index++ ) {
		centity_t	*cent = beaconsThisFrame[index];

		pulse = CG_BeaconPulse( &cent->currentState );
		CG_BeaconColor( CG_BeaconRelative( &cent->currentState ), color );
		VectorScale( color, pulse, color );
		if ( cg_addMarks.integer ) {
			CG_DrawBeaconRing( cent, color );
		}
	}
	numBeaconsThisFrame = 0;
}

/*
=================
CG_DrawBeacons

HUD of the beacon game.
=================
*/
void CG_DrawBeacons( void ) {
	int				index, count = 0, letter, seconds;
	const entityState_t	*states[8];
	const entityState_t	*standing = NULL;
	float			x, y, size = 26, gap = 6, relative;
	vec4_t			fill, frame = { 0, 0, 0, 0.8f }, text = { 1, 1, 1, 1 };
	int				allyPoints, enemyPoints;
	const char		*label;

	if ( cgs.gametype != GT_BEACONS || !cg.snap ) {
		return;
	}
	for ( index = 0 ; index < cg.snap->numEntities && count < 8 ; index++ ) {
		if ( cg.snap->entities[index].eType == ET_BEACON ) {
			states[count++] = &cg.snap->entities[index];
		}
	}
	// beacons in order of their letters
	for ( index = 1 ; index < count ; index++ ) {
		const entityState_t	*moved = states[index];
		int					place = index;
		while ( place > 0 && states[place - 1]->modelindex2 > moved->modelindex2 ) {
			states[place] = states[place - 1];
			place--;
		}
		states[place] = moved;
	}

	// letters at the top
	x = 320 - ( count * size + ( count - 1 ) * gap ) * 0.5f;
	y = 6;
	for ( index = 0 ; index < count ; index++ ) {
		vec3_t	color;
		char	name[2];

		relative = CG_BeaconRelative( states[index] );
		CG_BeaconColor( relative, color );
		VectorCopy( color, fill );
		fill[3] = 0.85f;
		CG_FillRect( x - 2, y - 2, size + 4, size + 4, frame );
		CG_FillRect( x, y, size, size, fill );
		if ( states[index]->generic1 == 3 && ( cg.time / 250 ) % 2 ) {
			CG_DrawRect( x - 2, y - 2, size + 4, size + 4, 2, colorWhite );
		}
		name[0] = 'A' + states[index]->modelindex2;
		name[1] = 0;
		CG_DrawStringExt( x + ( size - 16 ) * 0.5f, y + ( size - 16 ) * 0.5f, name, colorBlack, qtrue, qfalse, 16, 16, 1 );
		x += size + gap;

		// is the player on this beacon?
		if ( cg.snap->ps.stats[STAT_HEALTH] > 0 && cg.snap->ps.persistant[PERS_TEAM] != TEAM_SPECTATOR ) {
			float	dx = cg.predictedPlayerState.origin[0] - states[index]->pos.trBase[0];
			float	dy = cg.predictedPlayerState.origin[1] - states[index]->pos.trBase[1];
			float	dz = cg.predictedPlayerState.origin[2] - states[index]->pos.trBase[2];
			float	radius = states[index]->origin2[0];
			if ( dx * dx + dy * dy <= radius * radius && fabs( dz ) <= 120 ) {
				standing = states[index];
			}
		}
	}

	// points and time under the letters: own team on the left
	if ( cg.snap->ps.persistant[PERS_TEAM] == TEAM_RED ) {
		allyPoints = cgs.scores1;
		enemyPoints = cgs.scores2;
	} else {
		allyPoints = cgs.scores2;
		enemyPoints = cgs.scores1;
	}
	y += size + 8;
	VectorCopy( colorAlly, fill ); fill[3] = 1;
	label = va( "%i", allyPoints );
	CG_DrawStringExt( 320 - 60 - CG_DrawStrlen( label ) * 12, y, label, fill, qtrue, qtrue, 12, 16, 0 );
	VectorCopy( colorEnemy, fill ); fill[3] = 1;
	label = va( "%i", enemyPoints );
	CG_DrawStringExt( 320 + 60, y, label, fill, qtrue, qtrue, 12, 16, 0 );
	if ( cgs.timelimit > 0 ) {
		seconds = ( cgs.timelimit * 60000 - ( cg.time - cgs.levelStartTime ) ) / 1000;
		if ( seconds < 0 ) {
			seconds = 0;
		}
		label = va( "%i:%02i", seconds / 60, seconds % 60 );
		CG_DrawStringExt( 320 - CG_DrawStrlen( label ) * 6, y, label, text, qtrue, qtrue, 12, 16, 0 );
	}

	// capture bar when standing on a beacon
	if ( standing ) {
		vec3_t	color;
		float	width = 200, height = 10, amount;

		vec4_t	track = { 0.25f, 0.28f, 0.30f, 0.6f };

		relative = CG_BeaconRelative( standing );
		if ( relative >= 0 ) {
			VectorCopy( colorAlly, color );
		} else {
			VectorCopy( colorEnemy, color );
		}
		VectorCopy( color, fill );
		fill[3] = 0.9f;
		amount = fabs( relative );
		x = 320 - width * 0.5f;
		y = 300;
		// one bar filled from the left by the control of the side that holds the
		// beacon now: blue for the own team, red for the other (it shrinks while
		// the beacon is being taken from them, then the blue part grows)
		CG_FillRect( x - 2, y - 2, width + 4, height + 4, frame );
		CG_FillRect( x, y, width, height, track );
		CG_FillRect( x, y, width * amount, height, fill );
		letter = 'A' + standing->modelindex2;
		if ( standing->generic1 == 3 ) {
			label = va( "BEACON %c: CONTESTED", letter );
		} else if ( relative >= 1 ) {
			label = va( "BEACON %c: OURS", letter );
		} else {
			label = va( "BEACON %c: CAPTURING", letter );
		}
		CG_DrawStringExt( 320 - CG_DrawStrlen( label ) * 4, y + height + 6, label, text, qtrue, qtrue, 8, 12, 0 );
	}
}

/*
=================
CG_DrawHealthBars

WALKER: a bar of health over every robot in sight: green over the own team,
red over the others (in a game without teams all are others). The game sends
health in percent in entityState generic1 (g_active.c, ClientEndFrame).
=================
*/
#define HEALTH_BAR_HEIGHT		108		// over the robot's origin
#define HEALTH_BAR_DISTANCE		3000
#define HEALTH_BAR_WIDTH		40
#define SET_COLOR( c, r, g, b, a )	( (c)[0] = (r), (c)[1] = (g), (c)[2] = (b), (c)[3] = (a) )

void CG_DrawHealthBars( void ) {
	int				index, myTeam, team;
	const entityState_t	*state;
	centity_t		*cent;
	vec3_t			point, delta;
	float			forward, across, upward, screenX, screenY, scaleX, scaleY, distance, width;
	vec4_t			back = { 0, 0, 0, 0.6f }, fill;
	trace_t			trace;

	if ( !cg_drawHealthBars.integer || !cg.snap ) {
		return;
	}
	myTeam = cg.snap->ps.persistant[PERS_TEAM];
	scaleX = 320.0f / tan( cg.refdef.fov_x / 360.0f * M_PI );
	scaleY = 240.0f / tan( cg.refdef.fov_y / 360.0f * M_PI );
	for ( index = 0 ; index < cg.snap->numEntities ; index++ ) {
		state = &cg.snap->entities[index];
		if ( state->eType != ET_PLAYER || ( state->eFlags & EF_DEAD ) || state->clientNum == cg.snap->ps.clientNum ) {
			continue;
		}
		cent = &cg_entities[state->number];
		VectorCopy( cent->lerpOrigin, point );
		point[2] += HEALTH_BAR_HEIGHT;
		VectorSubtract( point, cg.refdef.vieworg, delta );
		distance = VectorLength( delta );
		forward = DotProduct( delta, cg.refdef.viewaxis[0] );
		if ( forward < 16 || distance > HEALTH_BAR_DISTANCE ) {
			continue;
		}
		// hidden behind the world (WALKER: the trace a few times a second, CG_VisibleFromView)
		if ( !CG_VisibleFromView( state->number, point, 0 ) ) {
			continue;
		}
		across = DotProduct( delta, cg.refdef.viewaxis[1] );
		upward = DotProduct( delta, cg.refdef.viewaxis[2] );
		screenX = 320 - across / forward * scaleX;
		screenY = 240 - upward / forward * scaleY;
		// a little smaller far away, never smaller than half
		width = HEALTH_BAR_WIDTH * ( distance < 600 ? 1.0f : ( distance > 2400 ? 0.5f : 1.0f - ( distance - 600 ) / 3600 ) );

		team = cgs.clientinfo[state->clientNum].team;
		if ( cgs.gametype >= GT_TEAM && myTeam == TEAM_SPECTATOR ) {
			SET_COLOR( fill, team == TEAM_BLUE ? 0.3f : 1.0f, 0.3f, team == TEAM_BLUE ? 1.0f : 0.25f, 0.95f );
		} else if ( cgs.gametype >= GT_TEAM && team == myTeam ) {
			SET_COLOR( fill, 0.25f, 0.95f, 0.3f, 0.95f );
		} else {
			SET_COLOR( fill, 1.0f, 0.25f, 0.2f, 0.95f );
		}
		CG_FillRect( screenX - width * 0.5f - 1, screenY - 1, width + 2, 6, back );
		CG_FillRect( screenX - width * 0.5f, screenY, width * Com_Clamp( 0, 100, state->generic1 ) / 100.0f, 4, fill );
		// WALKER: the shield above the structure (entityState time2, percent)
		if ( state->time2 > 0 ) {
			vec4_t	shieldColor = { 0.45f, 0.85f, 1.0f, 0.95f };
			CG_FillRect( screenX - width * 0.5f - 1, screenY - 5, width + 2, 4, back );
			CG_FillRect( screenX - width * 0.5f, screenY - 4, width * Com_Clamp( 0, 100, state->time2 ) / 100.0f, 2, shieldColor );
		}
	}

	// robots left in the hangar (beacon capture)
	if ( cgs.gametype == GT_BEACONS && myTeam != TEAM_SPECTATOR ) {
		const char	*label = va( "ROBOTS LEFT %i", cg.snap->ps.stats[STAT_ROBOTS] > 0 ? cg.snap->ps.stats[STAT_ROBOTS] : 0 );
		vec4_t		color = { 1, 1, 1, 0.9f };
		CG_DrawStringExt( 8, 420, label, color, qtrue, qtrue, 10, 14, 0 );
		if ( cg.snap->ps.stats[STAT_HEALTH] <= 0 && cg.snap->ps.stats[STAT_ROBOTS] > 1 ) {
			CG_DrawStringExt( 320 - 13 * 6, 200, "NEXT ROBOT...", color, qtrue, qtrue, 12, 16, 0 );
		}
	}
}

/*
=================
CG_AddBarrier

WALKER: the edge of the playing field (worldspawn "barrier": half its size,
square around the centre) is a wall of player clip. When the player comes
closer than BARRIER_FADE to it, a shimmering grid shows on the wall around him,
brighter as he comes closer.
=================
*/
#define BARRIER_FADE		180
#define BARRIER_SPAN		360		// half width of the shown patch
#define BARRIER_COLUMNS		9
#define BARRIER_ROWS		5

static float	barrierHalf = -1;

static void CG_ParseBarrier( void ) {
	char	key[MAX_TOKEN_CHARS], value[MAX_TOKEN_CHARS];

	barrierHalf = 0;
	if ( !trap_GetEntityToken( key, sizeof( key ) ) || key[0] != '{' ) {
		return;
	}
	while ( trap_GetEntityToken( key, sizeof( key ) ) && key[0] != '}' ) {
		if ( !trap_GetEntityToken( value, sizeof( value ) ) ) {
			return;
		}
		if ( !Q_stricmp( key, "barrier" ) ) {
			barrierHalf = atof( value );
		}
	}
}

void CG_AddBarrier( void ) {
	int			side, column, row, index;
	float		distance, closeness, along, height;
	vec3_t		eye, corner;
	polyVert_t	grid[BARRIER_ROWS][BARRIER_COLUMNS], quad[4];

	if ( barrierHalf < 0 ) {
		CG_ParseBarrier();
	}
	if ( barrierHalf <= 0 || !cg.snap ) {
		return;
	}
	VectorCopy( cg.refdef.vieworg, eye );
	for ( side = 0 ; side < 4 ; side++ ) {
		int		axis = side < 2 ? 0 : 1;			// the wall is across this axis
		float	sign = ( side & 1 ) ? -1.0f : 1.0f;
		float	wall = sign * barrierHalf;

		distance = fabs( wall - eye[axis] );
		if ( distance > BARRIER_FADE || fabs( eye[axis] ) > barrierHalf + 8 ) {
			continue;
		}
		closeness = 1.0f - distance / BARRIER_FADE;
		for ( row = 0 ; row < BARRIER_ROWS ; row++ ) {
			for ( column = 0 ; column < BARRIER_COLUMNS ; column++ ) {
				polyVert_t	*vert = &grid[row][column];
				float		strength, reach;

				along = eye[!axis] + ( (float)column / ( BARRIER_COLUMNS - 1 ) * 2 - 1 ) * BARRIER_SPAN;
				height = eye[2] - 120 + (float)row / ( BARRIER_ROWS - 1 ) * 360;
				corner[axis] = wall;
				corner[!axis] = along;
				corner[2] = height;
				VectorCopy( corner, vert->xyz );
				vert->st[0] = along / 64.0f;
				vert->st[1] = height / 64.0f;
				// brightest in front of the player, fading out around
				reach = Distance( corner, eye ) / BARRIER_SPAN;
				strength = closeness * Com_Clamp( 0, 1, 1.2f - reach );
				vert->modulate[0] = (byte)( 90 * strength );
				vert->modulate[1] = (byte)( 190 * strength );
				vert->modulate[2] = (byte)( 255 * strength );
				vert->modulate[3] = 255;
			}
		}
		for ( row = 0 ; row < BARRIER_ROWS - 1 ; row++ ) {
			for ( column = 0 ; column < BARRIER_COLUMNS - 1 ; column++ ) {
				quad[0] = grid[row][column];
				quad[1] = grid[row][column + 1];
				quad[2] = grid[row + 1][column + 1];
				quad[3] = grid[row + 1][column];
				for ( index = 0 ; index < 4 ; index++ ) {
					if ( quad[index].modulate[0] || quad[index].modulate[1] || quad[index].modulate[2] ) {
						break;
					}
				}
				if ( index < 4 ) {
					trap_R_AddPolyToScene( cgs.media.barrierShader, 4, quad );
				}
			}
		}
	}
}
