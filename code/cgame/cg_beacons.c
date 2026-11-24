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
CG_Beacon
=================
*/
void CG_Beacon( centity_t *cent ) {
	const entityState_t	*state = &cent->currentState;
	vec3_t				color, up = { 0, 0, 1 };
	float				pulse;

	CG_BeaconColor( CG_BeaconRelative( state ), color );
	// a beacon being fought for flickers a little
	pulse = ( state->generic1 == 3 ) ? 0.75f + 0.25f * sin( cg.time * 0.02f ) : 1.0f;
	CG_BeamQuad( cent->lerpOrigin, BEAM_GLOW_WIDTH, color, 0.45f * pulse );
	CG_BeamQuad( cent->lerpOrigin, BEAM_CORE_WIDTH, color, 1.0f * pulse );
	// ring on the ground along the edge of the beacon
	CG_ImpactMark( cgs.media.beaconRingShader, cent->lerpOrigin, up, 0,
		color[0] * pulse, color[1] * pulse, color[2] * pulse, 1, qfalse, state->origin2[0], qtrue );
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

		relative = CG_BeaconRelative( standing );
		CG_BeaconColor( relative, color );
		VectorCopy( color, fill );
		fill[3] = 0.9f;
		amount = fabs( relative );
		x = 320 - width * 0.5f;
		y = 300;
		CG_FillRect( x - 2, y - 2, width + 4, height + 4, frame );
		if ( relative >= 0 ) {
			CG_FillRect( 320, y, width * 0.5f * amount, height, fill );
		} else {
			CG_FillRect( 320 - width * 0.5f * amount, y, width * 0.5f * amount, height, fill );
		}
		CG_FillRect( 319, y - 2, 2, height + 4, colorWhite );
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
