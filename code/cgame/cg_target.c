/*
===========================================================================
WALKER: target lock and weapon labels.

The nearest enemy robot in sight and within the range of the own weapons is
taken into a frame and held while it stays valid (alive, in sight, in range);
"locknext" (TAB) moves the frame to the next one by distance. Under the frame:
the distance (1 unit = 1 m) and the damage done to this robot so far (server
command "dmg <client> <total>", g_combat.c; reset when it gets a new robot).

Behind each own weapon (at its mount, the camera looks from behind) its name,
range and damage per shot are written; grey when the target in the frame is out of
its range.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "cg_local.h"

#define TARGET_KEEP_HIDDEN		1500	// msec a target out of sight stays in the frame
#define TARGET_CONE				0.5f	// a new target: within 60 degrees of the view
#define TARGET_BOX_HALF			22		// frame around the robot: half width
#define TARGET_BOX_BOTTOM		-24
#define TARGET_BOX_TOP			60

static int		targetClient = -1;		// in the frame, -1 none
static int		targetSeenTime;
static int		targetDamage[MAX_CLIENTS];
static vec3_t	slotLabelOrigin[ROBOT_MAX_SLOTS];
static int		slotLabelFrame[ROBOT_MAX_SLOTS];	// cg.clientFrame of the last placement


/*
=================
CG_TargetDamage

server command "dmg <client> <total>"
=================
*/
void CG_TargetDamage( int clientNum, int total ) {
	if ( clientNum >= 0 && clientNum < MAX_CLIENTS ) {
		targetDamage[clientNum] = total;
	}
}


/*
=================
CG_WeaponReach

the longest range of the own weapons
=================
*/
static int CG_WeaponReach( void ) {
	clientInfo_t	*ci = &cgs.clientinfo[cg.snap->ps.clientNum];
	int				slot, reach = 0;

	for ( slot = 0; slot < ROBOT_MAX_SLOTS; slot++ ) {
		if ( ci->slotWeapons[slot] >= 0 && ci->slotWeapons[slot] < RWEAPON_COUNT && bg_robotWeapons[ci->slotWeapons[slot]].range > reach ) {
			reach = bg_robotWeapons[ci->slotWeapons[slot]].range;
		}
	}
	return reach;
}


/*
=================
CG_TargetEntity

the robot of a client in the current snapshot, NULL when not there or dead
(the entity whose number is the client number: a corpse has the clientNum too)
=================
*/
static centity_t *CG_TargetEntity( int clientNum ) {
	int		index;

	for ( index = 0; index < cg.snap->numEntities; index++ ) {
		entityState_t	*state = &cg.snap->entities[index];
		if ( state->eType == ET_PLAYER && state->number == clientNum ) {
			return ( state->eFlags & EF_DEAD ) ? NULL : &cg_entities[state->number];
		}
	}
	return NULL;
}


/*
=================
CG_TargetEnemy
=================
*/
static qboolean CG_TargetEnemy( int clientNum ) {
	if ( clientNum == cg.snap->ps.clientNum ) {
		return qfalse;
	}
	if ( cgs.gametype >= GT_TEAM && cgs.clientinfo[clientNum].team == cg.snap->ps.persistant[PERS_TEAM] ) {
		return qfalse;
	}
	return cgs.clientinfo[clientNum].infoValid;
}


/*
=================
CG_TargetVisible
=================
*/
static qboolean CG_TargetVisible( centity_t *cent ) {
	vec3_t		point;

	VectorCopy( cent->lerpOrigin, point );
	point[2] += 24;
	return CG_VisibleFromView( cent->currentState.number, point, 1 );	// traced a few times a second
}


/*
=================
CG_TargetDistance
=================
*/
static float CG_TargetDistance( centity_t *cent ) {
	// metres, as the weapon ranges and the frame label
	return Distance( cent->lerpOrigin, cg.predictedPlayerState.origin ) / ROBOT_UNITS_PER_METRE;
}


/*
=================
CG_TargetCandidates

enemies in sight and in range, nearest first; with cone, only those ahead
=================
*/
static int CG_TargetCandidates( int *list, float *distances, qboolean cone ) {
	int		index, count = 0, reach = CG_WeaponReach();

	for ( index = 0; index < cg.snap->numEntities; index++ ) {
		entityState_t	*state = &cg.snap->entities[index];
		centity_t		*cent;
		vec3_t			delta;
		float			distance;
		int				position;

		if ( state->eType != ET_PLAYER || ( state->eFlags & EF_DEAD ) || !CG_TargetEnemy( state->clientNum ) ) {
			continue;
		}
		cent = &cg_entities[state->number];
		distance = CG_TargetDistance( cent );
		if ( distance > reach ) {
			continue;
		}
		VectorSubtract( cent->lerpOrigin, cg.refdef.vieworg, delta );
		VectorNormalize( delta );
		if ( cone && DotProduct( delta, cg.refdef.viewaxis[0] ) < TARGET_CONE ) {
			continue;
		}
		if ( !CG_TargetVisible( cent ) ) {
			continue;
		}
		// insert by distance
		for ( position = count; position > 0 && distances[position - 1] > distance; position-- ) {
			list[position] = list[position - 1];
			distances[position] = distances[position - 1];
		}
		list[position] = state->clientNum;
		distances[position] = distance;
		count++;
	}
	return count;
}


static qboolean CG_WorldToScreen( const vec3_t point, float *screenX, float *screenY );

/*
=================
CG_TargetUnderCrosshair

the nearest visible enemy whose frame (the box drawn around the robot) holds the
crosshair, -1 when none; up to the distance a target is kept at
=================
*/
static int CG_TargetUnderCrosshair( void ) {
	int		index, best = -1;
	float	bestDistance = 0, reach = CG_WeaponReach() * 1.25f;
	float	crossX = 320 + cg_crosshairX.integer, crossY = 240 + cg_crosshairY.integer;	// as CG_DrawCrosshair

	for ( index = 0; index < cg.snap->numEntities; index++ ) {
		entityState_t	*state = &cg.snap->entities[index];
		centity_t		*cent;
		vec3_t			low, high;
		float			distance, lowX, lowY, highX, highY, halfWidth;

		if ( state->eType != ET_PLAYER || state->number != state->clientNum || ( state->eFlags & EF_DEAD )
			|| !CG_TargetEnemy( state->clientNum ) ) {
			continue;
		}
		cent = &cg_entities[state->number];
		distance = CG_TargetDistance( cent );
		if ( distance > reach || ( best >= 0 && distance >= bestDistance ) ) {
			continue;
		}
		VectorCopy( cent->lerpOrigin, low );
		VectorCopy( cent->lerpOrigin, high );
		low[2] += TARGET_BOX_BOTTOM;
		high[2] += TARGET_BOX_TOP;
		if ( !CG_WorldToScreen( low, &lowX, &lowY ) || !CG_WorldToScreen( high, &highX, &highY ) ) {
			continue;
		}
		// the half width on the screen by the height of the box; at least 6 points to hit
		halfWidth = ( lowY - highY ) * TARGET_BOX_HALF / ( TARGET_BOX_TOP - TARGET_BOX_BOTTOM );
		if ( halfWidth < 6 ) {
			halfWidth = 6;
		}
		if ( crossX < lowX - halfWidth || crossX > lowX + halfWidth || crossY < highY - 3 || crossY > lowY + 3 ) {
			continue;
		}
		if ( !CG_TargetVisible( cent ) ) {
			continue;
		}
		best = state->clientNum;
		bestDistance = distance;
	}
	return best;
}

/*
=================
CG_TargetUpdate

the enemy under the crosshair takes the frame (27.11.2026); otherwise the target is
kept while it is valid, and without one the nearest enemy ahead is taken
=================
*/
void CG_TargetUpdate( void ) {
	centity_t	*cent;
	int			list[MAX_CLIENTS];
	float		distances[MAX_CLIENTS];
	int			under;

	if ( !cg.snap || cg.snap->ps.persistant[PERS_TEAM] == TEAM_SPECTATOR || cg.snap->ps.stats[STAT_HEALTH] <= 0 ) {
		targetClient = -1;
		return;
	}
	under = CG_TargetUnderCrosshair();
	if ( under >= 0 && under != targetClient ) {
		targetClient = under;
		targetSeenTime = cg.time;
	}
	if ( targetClient >= 0 ) {
		cent = CG_TargetEntity( targetClient );
		if ( !cent || !CG_TargetEnemy( targetClient ) || CG_TargetDistance( cent ) > CG_WeaponReach() * 1.25f ) {
			targetClient = -1;
		} else if ( CG_TargetVisible( cent ) ) {
			targetSeenTime = cg.time;
		} else if ( cg.time - targetSeenTime > TARGET_KEEP_HIDDEN ) {
			targetClient = -1;
		}
	}
	if ( targetClient < 0 && CG_TargetCandidates( list, distances, qtrue ) > 0 ) {
		targetClient = list[0];
		targetSeenTime = cg.time;
	}
}


/*
=================
CG_TargetNext_f

"locknext" (TAB): the next target in range by distance
=================
*/
void CG_TargetNext_f( void ) {
	int		list[MAX_CLIENTS];
	float	distances[MAX_CLIENTS];
	int		count, index;

	if ( !cg.snap ) {
		return;
	}
	count = CG_TargetCandidates( list, distances, qfalse );
	if ( !count ) {
		return;
	}
	for ( index = 0; index < count; index++ ) {
		if ( list[index] == targetClient ) {
			break;
		}
	}
	targetClient = list[( index + 1 ) % count];		// not found: index == count, takes the nearest
	targetSeenTime = cg.time;
}


/*
=================
CG_WorldToScreen
=================
*/
static qboolean CG_WorldToScreen( const vec3_t point, float *screenX, float *screenY ) {
	vec3_t	delta;
	float	forward;

	VectorSubtract( point, cg.refdef.vieworg, delta );
	forward = DotProduct( delta, cg.refdef.viewaxis[0] );
	if ( forward < 4 ) {
		return qfalse;
	}
	*screenX = 320 - DotProduct( delta, cg.refdef.viewaxis[1] ) / forward * ( 320.0f / tan( cg.refdef.fov_x / 360.0f * M_PI ) );
	*screenY = 240 - DotProduct( delta, cg.refdef.viewaxis[2] ) / forward * ( 240.0f / tan( cg.refdef.fov_y / 360.0f * M_PI ) );
	return qtrue;
}


/*
=================
CG_TargetDistanceNow

distance to the target in the frame, -1 without one
=================
*/
static float CG_TargetDistanceNow( void ) {
	centity_t	*cent;

	if ( targetClient < 0 || !cg.snap ) {
		return -1;
	}
	cent = CG_TargetEntity( targetClient );
	return cent ? CG_TargetDistance( cent ) : -1;
}


/*
=================
CG_DrawTarget

corner brackets around the target, distance and damage done under them
=================
*/
void CG_DrawTarget( void ) {
	static const vec4_t	frameColor = { 1.0f, 0.3f, 0.2f, 0.95f };
	static const vec4_t	textColor = { 1.0f, 0.85f, 0.75f, 1.0f };
	centity_t	*cent;
	float		left = 640, right = 0, top = 480, bottom = 0, x, y, length;
	int			corner, projected = 0;
	vec3_t		point;

	if ( targetClient < 0 || !cg.snap ) {
		return;
	}
	cent = CG_TargetEntity( targetClient );
	if ( !cent ) {
		return;
	}
	for ( corner = 0; corner < 8; corner++ ) {
		point[0] = cent->lerpOrigin[0] + ( ( corner & 1 ) ? TARGET_BOX_HALF : -TARGET_BOX_HALF );
		point[1] = cent->lerpOrigin[1] + ( ( corner & 2 ) ? TARGET_BOX_HALF : -TARGET_BOX_HALF );
		point[2] = cent->lerpOrigin[2] + ( ( corner & 4 ) ? TARGET_BOX_TOP : TARGET_BOX_BOTTOM );
		if ( !CG_WorldToScreen( point, &x, &y ) ) {
			continue;
		}
		projected++;
		if ( x < left ) left = x;
		if ( x > right ) right = x;
		if ( y < top ) top = y;
		if ( y > bottom ) bottom = y;
	}
	if ( projected < 8 ) {
		return;
	}
	length = ( right - left < bottom - top ? right - left : bottom - top ) * 0.25f;
	if ( length < 4 ) {
		length = 4;
	}
	CG_FillRect( left, top, length, 2, frameColor );
	CG_FillRect( left, top, 2, length, frameColor );
	CG_FillRect( right - length, top, length, 2, frameColor );
	CG_FillRect( right - 2, top, 2, length, frameColor );
	CG_FillRect( left, bottom - 2, length, 2, frameColor );
	CG_FillRect( left, bottom - length, 2, length, frameColor );
	CG_FillRect( right - length, bottom - 2, length, 2, frameColor );
	CG_FillRect( right - 2, bottom - length, 2, length, frameColor );

	{
		const char	*distance = va( "%i m", (int)CG_TargetDistance( cent ) );
		CG_DrawStringExt( ( left + right ) * 0.5f - strlen( distance ) * 3, bottom + 3, distance, textColor, qtrue, qtrue, 6, 10, 0 );
	}
	if ( targetDamage[targetClient] > 0 ) {
		const char	*damage = va( "DMG %i", targetDamage[targetClient] );
		CG_DrawStringExt( ( left + right ) * 0.5f - strlen( damage ) * 3, bottom + 14, damage, textColor, qtrue, qtrue, 6, 10, 0 );
	}
}


/*
=================
CG_PlaceWeaponLabel

the mount of an own weapon this frame (cg_weapons.c, CG_AddRobotWeapons)
=================
*/
void CG_PlaceWeaponLabel( int slot, const vec3_t origin ) {
	if ( slot >= 0 && slot < ROBOT_MAX_SLOTS ) {
		VectorCopy( origin, slotLabelOrigin[slot] );
		slotLabelFrame[slot] = cg.clientFrame;
	}
}


/*
=================
CG_DrawWeaponLabels

range and damage behind each own weapon; grey when the target is out of its range
=================
*/
void CG_DrawWeaponLabels( void ) {
	static const vec4_t	readyColor = { 1.0f, 0.85f, 0.4f, 0.95f };
	static const vec4_t	farColor = { 0.55f, 0.55f, 0.55f, 0.8f };
	clientInfo_t	*ci;
	float			distance, x, y;
	int				slot;

	if ( !cg.snap || cg.snap->ps.persistant[PERS_TEAM] == TEAM_SPECTATOR || cg.snap->ps.stats[STAT_HEALTH] <= 0
		|| !cg.renderingThirdPerson ) {
		return;
	}
	ci = &cgs.clientinfo[cg.snap->ps.clientNum];
	distance = CG_TargetDistanceNow();
	for ( slot = 0; slot < ROBOT_MAX_SLOTS; slot++ ) {
		int			type = ci->slotWeapons[slot];
		const char	*range, *damage;
		const float	*color;

		if ( type < 0 || type >= RWEAPON_COUNT || slotLabelFrame[slot] != cg.clientFrame ) {
			continue;
		}
		if ( !CG_WorldToScreen( slotLabelOrigin[slot], &x, &y ) ) {
			continue;
		}
		color = ( distance >= 0 && distance > bg_robotWeapons[type].range ) ? farColor : readyColor;
		CG_DrawStringExt( x - strlen( bg_robotWeapons[type].title ) * 3, y - 23, bg_robotWeapons[type].title, color, qtrue, qtrue, 6, 10, 0 );
		range = va( "%i m", bg_robotWeapons[type].range );
		CG_DrawStringExt( x - strlen( range ) * 3, y - 12, range, color, qtrue, qtrue, 6, 10, 0 );
		damage = va( "%i", BG_WeaponDamage( type, ci->slotLevels[slot] ) );
		CG_DrawStringExt( x - strlen( damage ) * 3, y - 1, damage, color, qtrue, qtrue, 6, 10, 0 );
	}
}
