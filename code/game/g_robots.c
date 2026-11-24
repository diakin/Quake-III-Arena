/*
===========================================================================
WALKER: robots in the battle.

A player brings the robots of his account (userinfo walker_robots,
"k7.3:osa.2:osa.1,molot.1:taran.1": type.level and weapon.level of every slot,
"-" for an empty slot, ui_account.c), a bot gets g_botRobots random robots of
g_botLevel with random weapons that fit their slots; the bots of the team
against the human players are one level higher. Each new life takes the next
robot.

The characteristics come from bg_robots.c: the structure is the health (kept in
tenths, ROBOT_HEALTH_SCALE), the armor takes its percent off every hit on the
structure, the shield takes the damage first and recovers after SHIELD_DELAY
without hits, the speed sets ps.speed (km/h * g_speed / 38). All weapons of the robot fire
while the attack button is held, each at its own rate (G_RobotFire, the shot
itself in g_weapon.c); pmove does not fire (STAT_FIRE_INTERVAL -1). The weapons
of the current robot go to the clients in the player configstring ("w").

At the end of a battle every human player gets his reward: development points,
currency and experience for the damage, destroyed robots, captured beacons and
the victory (server command "reward", cgame hands it to the ui).

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "g_local.h"

#define SHIELD_DELAY		4000	// msec without hits before the shield recovers
#define SHIELD_RECOVERY		12		// percent of the full shield per second

/*
=================
G_RobotParseWeapon

"osa.2" or "-"
=================
*/
static void G_RobotParseWeapon( battleWeapon_t *weapon, const char *text ) {
	char	name[32];
	char	*dot;

	weapon->type = -1;
	weapon->level = 1;
	Q_strncpyz( name, text, sizeof( name ) );
	dot = strchr( name, '.' );
	if ( dot ) {
		*dot = 0;
		weapon->level = atoi( dot + 1 );
	}
	weapon->type = BG_WeaponByName( name );
	if ( weapon->level < 1 || weapon->level > WEAPON_LEVELS ) {
		weapon->level = 1;
	}
}


/*
=================
G_RobotParse

robots of a player from his userinfo
=================
*/
void G_RobotParse( gclient_t *client, const char *list ) {
	char	entry[128];
	int		length, count = 0;

	while ( *list && count < ROBOT_HANGAR_SIZE ) {
		battleRobot_t	*robot = &client->pers.robots[count];
		char			*part, *next, *dot;
		int				type, slot;

		for ( length = 0; list[length] && list[length] != ','; length++ ) {
		}
		Q_strncpyz( entry, list, length + 1 < (int)sizeof( entry ) ? length + 1 : sizeof( entry ) );
		list += length;
		if ( *list == ',' ) {
			list++;
		}
		// "k7.3:osa.2:osa.1"
		next = strchr( entry, ':' );
		if ( next ) {
			*next++ = 0;
		}
		dot = strchr( entry, '.' );
		if ( !dot ) {
			continue;
		}
		*dot = 0;
		type = BG_RobotByName( entry );
		if ( type < 0 ) {
			continue;
		}
		robot->type = type;
		robot->level = atoi( dot + 1 );
		if ( robot->level < 1 || robot->level > ROBOT_LEVELS ) {
			robot->level = 1;
		}
		for ( slot = 0; slot < ROBOT_MAX_SLOTS; slot++ ) {
			robot->weapons[slot].type = -1;
			robot->weapons[slot].level = 1;
			if ( !next ) {
				continue;
			}
			part = next;
			next = strchr( part, ':' );
			if ( next ) {
				*next++ = 0;
			}
			G_RobotParseWeapon( &robot->weapons[slot], part );
			if ( robot->weapons[slot].type >= 0 && !BG_WeaponFits( type, slot, robot->weapons[slot].type ) ) {
				robot->weapons[slot].type = -1;		// a weapon of another class
			}
		}
		count++;
	}
	client->pers.numRobots = count;
}


/*
=================
G_RobotRandomWeapon

a weapon of the given class
=================
*/
static int G_RobotRandomWeapon( int weaponClass ) {
	int		choices[RWEAPON_COUNT];
	int		count = 0, index;

	for ( index = 0; index < RWEAPON_COUNT; index++ ) {
		if ( bg_robotWeapons[index].weaponClass == weaponClass ) {
			choices[count++] = index;
		}
	}
	return count ? choices[rand() % count] : -1;
}


/*
=================
G_RobotGiveBot

random robots for a bot, about as strong as the human players
=================
*/
static void G_RobotGiveBot( gentity_t *ent ) {
	gclient_t	*client = ent->client;
	int			index, slot, robotLevel, count, humanTeam = TEAM_FREE;

	for ( index = 0; index < level.maxclients; index++ ) {
		gclient_t	*other = &level.clients[index];
		if ( other->pers.connected == CON_CONNECTED && !( g_entities[index].r.svFlags & SVF_BOT )
			&& ( other->sess.sessionTeam == TEAM_RED || other->sess.sessionTeam == TEAM_BLUE ) ) {
			humanTeam = other->sess.sessionTeam;
			break;
		}
	}
	robotLevel = g_botLevel.integer;
	if ( humanTeam != TEAM_FREE && client->sess.sessionTeam != humanTeam ) {
		robotLevel++;		// the enemies are a little stronger
	}
	if ( robotLevel < 1 ) {
		robotLevel = 1;
	} else if ( robotLevel > ROBOT_LEVELS ) {
		robotLevel = ROBOT_LEVELS;
	}
	count = g_botRobots.integer;
	if ( count < 1 ) {
		count = 1;
	} else if ( count > ROBOT_HANGAR_SIZE ) {
		count = ROBOT_HANGAR_SIZE;
	}
	for ( index = 0; index < count; index++ ) {
		battleRobot_t		*robot = &client->pers.robots[index];
		const robotInfo_t	*info;

		robot->type = rand() % ROBOT_COUNT;
		robot->level = robotLevel;
		info = &bg_robots[robot->type];
		for ( slot = 0; slot < ROBOT_MAX_SLOTS; slot++ ) {
			robot->weapons[slot].type = slot < info->numSlots ? G_RobotRandomWeapon( info->slotClass[slot] ) : -1;
			robot->weapons[slot].level = robotLevel;
		}
	}
	client->pers.numRobots = count;
}


/*
=================
G_RobotCurrent

the robot this player flies now (the next one of his hangar after each loss)
=================
*/
battleRobot_t *G_RobotCurrent( gclient_t *client ) {
	static battleRobot_t	standard = { ROBOT_K7, 1, { { RWEAPON_OSA, 1 }, { RWEAPON_OSA, 1 }, { -1, 1 } } };
	int						index;

	if ( client->pers.numRobots <= 0 ) {
		return &standard;
	}
	index = client->pers.robotsLost;
	if ( g_gametype.integer != GT_BEACONS ) {
		index %= client->pers.numRobots;		// other modes: the robots take turns
	} else if ( index >= client->pers.numRobots ) {
		index = client->pers.numRobots - 1;
	}
	return &client->pers.robots[index];
}


/*
=================
G_RobotModel

player model of the current robot, keeping the paint ("walker/red" -> "strizh/red")
=================
*/
void G_RobotModel( gclient_t *client, char *model, int size ) {
	const char	*skin = strchr( model, '/' );
	char		paint[MAX_QPATH];

	paint[0] = 0;
	if ( skin ) {
		Q_strncpyz( paint, skin, sizeof( paint ) );
	}
	Com_sprintf( model, size, "%s%s", bg_robots[G_RobotCurrent( client )->type].model, paint );
}


/*
=================
G_RobotSpawn

characteristics of the current robot on a new life
=================
*/
void G_RobotSpawn( gentity_t *ent ) {
	gclient_t		*client = ent->client;
	battleRobot_t	*robot;
	const robotInfo_t *info;

	if ( ( ent->r.svFlags & SVF_BOT ) && client->pers.numRobots <= 0 ) {
		G_RobotGiveBot( ent );
		ClientUserinfoChanged( ent - g_entities );
	}
	robot = G_RobotCurrent( client );
	info = &bg_robots[robot->type];

	client->pers.maxHealth = BG_RobotStructure( robot->type, robot->level ) / ROBOT_HEALTH_SCALE;
	client->ps.stats[STAT_MAX_HEALTH] = client->pers.maxHealth;
	ent->health = client->ps.stats[STAT_HEALTH] = client->pers.maxHealth;
	client->ps.stats[STAT_MAX_SHIELD] = BG_RobotShield( robot->type, robot->level ) / ROBOT_HEALTH_SCALE;
	client->ps.stats[STAT_SHIELD] = client->ps.stats[STAT_MAX_SHIELD];
	client->ps.stats[STAT_FIRE_INTERVAL] = -1;		// pmove does not fire: G_RobotFire

	// a new robot: the damage the others did to the previous one is forgotten
	{
		int		other;

		for ( other = 0; other < level.maxclients; other++ ) {
			gclient_t	*attacker = &level.clients[other];
			if ( attacker->pers.connected == CON_CONNECTED && attacker->damageTo[ent->s.number] ) {
				attacker->damageTo[ent->s.number] = 0;
				if ( !( g_entities[other].r.svFlags & SVF_BOT ) ) {
					trap_SendServerCommand( other, va( "dmg %i 0", ent->s.number ) );
				}
			}
		}
	}
	client->shieldHitTime = 0;
	client->shieldResidual = 0;
	memset( client->slotTime, 0, sizeof( client->slotTime ) );
	ent->flags |= FL_NO_KNOCKBACK;		// bullets do not push a walking robot

	// the HUD and the bots see one Quake weapon; the real ones are in the slots
	client->ps.stats[STAT_WEAPONS] = ( 1 << WP_MACHINEGUN );
	client->ps.ammo[WP_MACHINEGUN] = -1;
	client->ps.weapon = WP_MACHINEGUN;
	(void)info;
}


/*
=================
G_RobotSpeed

ps.speed of the current robot
=================
*/
float G_RobotSpeed( gclient_t *client ) {
	battleRobot_t	*robot = G_RobotCurrent( client );
	return BG_RobotSpeed( robot->type, robot->level ) * g_speed.value / ROBOT_SPEED_UNITS;
}


/*
=================
G_RobotFire

every slot fires at its own rate while the attack button is held
=================
*/
void G_RobotFire( gentity_t *ent, int msec ) {
	gclient_t		*client = ent->client;
	battleRobot_t	*robot;
	qboolean		attacking;
	int				slot;

	if ( ent->health <= 0 || client->sess.sessionTeam == TEAM_SPECTATOR || level.intermissiontime ) {
		return;
	}
	robot = G_RobotCurrent( client );
	attacking = ( client->buttons & BUTTON_ATTACK ) != 0;
	for ( slot = 0; slot < bg_robots[robot->type].numSlots; slot++ ) {
		battleWeapon_t	*weapon = &robot->weapons[slot];

		if ( weapon->type < 0 ) {
			continue;
		}
		if ( client->slotTime[slot] > 0 ) {
			client->slotTime[slot] -= msec;
		}
		if ( !attacking ) {
			if ( client->slotTime[slot] < 0 ) {
				client->slotTime[slot] = 0;
			}
			continue;
		}
		while ( client->slotTime[slot] <= 0 ) {
			G_RobotFireWeapon( ent, slot, weapon->type, weapon->level );
			client->slotTime[slot] += BG_WeaponInterval( weapon->type, weapon->level );
		}
	}
	if ( attacking ) {
		client->ps.eFlags |= EF_FIRING;
	}
}


/*
=================
G_RobotBalanceChanged

the test balance changed (bg_robots.c): the robots in the battle take the new
structure and shield at once, keeping the share they have left
=================
*/
void G_RobotBalanceChanged( void ) {
	int		index;

	for ( index = 0; index < level.maxclients; index++ ) {
		gentity_t		*ent = &g_entities[index];
		gclient_t		*client = ent->client;
		battleRobot_t	*robot;
		int				structure, shield;

		if ( !ent->inuse || client->pers.connected != CON_CONNECTED || client->sess.sessionTeam == TEAM_SPECTATOR ) {
			continue;
		}
		robot = G_RobotCurrent( client );
		structure = BG_RobotStructure( robot->type, robot->level ) / ROBOT_HEALTH_SCALE;
		shield = BG_RobotShield( robot->type, robot->level ) / ROBOT_HEALTH_SCALE;
		if ( structure < 1 ) {
			structure = 1;
		}
		if ( ent->health > 0 && client->ps.stats[STAT_MAX_HEALTH] > 0 && structure != client->ps.stats[STAT_MAX_HEALTH] ) {
			ent->health = (int)( (float)ent->health * structure / client->ps.stats[STAT_MAX_HEALTH] + 0.5f );
			if ( ent->health < 1 ) {
				ent->health = 1;
			}
			client->ps.stats[STAT_HEALTH] = ent->health;
		}
		if ( client->ps.stats[STAT_MAX_SHIELD] > 0 ) {
			client->ps.stats[STAT_SHIELD] = (int)( (float)client->ps.stats[STAT_SHIELD] * shield / client->ps.stats[STAT_MAX_SHIELD] + 0.5f );
		} else if ( ent->health > 0 ) {
			client->ps.stats[STAT_SHIELD] = shield;		// had none: full
		}
		client->pers.maxHealth = structure;
		client->ps.stats[STAT_MAX_HEALTH] = structure;
		client->ps.stats[STAT_MAX_SHIELD] = shield;
	}
}


/*
=================
G_RobotWeaponList

"osa.2,osa.1" - the weapons of the current robot by slot with their levels, for the
player configstring
=================
*/
const char *G_RobotWeaponList( gclient_t *client ) {
	static char		list[64];
	battleRobot_t	*robot = G_RobotCurrent( client );
	int				slot;

	list[0] = 0;
	for ( slot = 0; slot < bg_robots[robot->type].numSlots; slot++ ) {
		int		type = robot->weapons[slot].type;
		if ( type >= 0 ) {
			Q_strcat( list, sizeof( list ), va( "%s%s.%i", slot ? "," : "", bg_robotWeapons[type].id, robot->weapons[slot].level ) );
		} else {
			Q_strcat( list, sizeof( list ), slot ? ",-" : "-" );
		}
	}
	return list;
}


/*
=================
G_RobotAbsorb

the shield takes the damage first, the armor takes its percent off the rest;
returns what reaches the structure
=================
*/
int G_RobotAbsorb( gentity_t *targ, int damage ) {
	gclient_t		*client = targ->client;
	battleRobot_t	*robot;
	int				absorbed, armor;

	if ( !client || damage <= 0 ) {
		return damage;
	}
	client->shieldHitTime = level.time;
	absorbed = client->ps.stats[STAT_SHIELD] < damage ? client->ps.stats[STAT_SHIELD] : damage;
	client->ps.stats[STAT_SHIELD] -= absorbed;
	damage -= absorbed;
	if ( damage <= 0 ) {
		return 0;
	}
	robot = G_RobotCurrent( client );
	armor = BG_RobotArmor( robot->type, robot->level );
	damage = damage * ( 100 - armor ) / 100;
	return damage > 0 ? damage : 1;
}


/*
=================
G_RobotThink

the shield recovers after SHIELD_DELAY without hits
=================
*/
void G_RobotThink( gentity_t *ent, int msec ) {
	gclient_t	*client = ent->client;
	int			maximum = client->ps.stats[STAT_MAX_SHIELD];

	if ( maximum <= 0 || ent->health <= 0 || client->ps.stats[STAT_SHIELD] >= maximum
		|| level.time - client->shieldHitTime < SHIELD_DELAY ) {
		return;
	}
	client->shieldResidual += maximum * SHIELD_RECOVERY * msec;
	client->ps.stats[STAT_SHIELD] += client->shieldResidual / 100000;
	client->shieldResidual %= 100000;
	if ( client->ps.stats[STAT_SHIELD] > maximum ) {
		client->ps.stats[STAT_SHIELD] = maximum;
	}
}


/*
=================
G_RobotRewards

at the start of the intermission: the reward of every human player
=================
*/
void G_RobotRewards( void ) {
	int		index, winner = TEAM_FREE, bestScore = -99999;
	char	battle[32];

	// the QVM printf has no %x: two decimal numbers
	Com_sprintf( battle, sizeof( battle ), "%i_%i", rand() & 0x7fff, ( level.startTime ^ trap_Milliseconds() ) & 0x7fffffff );

	if ( g_gametype.integer >= GT_TEAM ) {
		if ( level.teamScores[TEAM_RED] > level.teamScores[TEAM_BLUE] ) {
			winner = TEAM_RED;
		} else if ( level.teamScores[TEAM_BLUE] > level.teamScores[TEAM_RED] ) {
			winner = TEAM_BLUE;
		}
	} else {
		for ( index = 0; index < level.maxclients; index++ ) {
			if ( level.clients[index].pers.connected == CON_CONNECTED
				&& level.clients[index].ps.persistant[PERS_SCORE] > bestScore ) {
				bestScore = level.clients[index].ps.persistant[PERS_SCORE];
			}
		}
	}

	for ( index = 0; index < level.maxclients; index++ ) {
		gclient_t	*client = &level.clients[index];
		int			damage, points, currency, xp;
		qboolean	won;

		if ( client->pers.connected != CON_CONNECTED || ( g_entities[index].r.svFlags & SVF_BOT ) ) {
			continue;
		}
		if ( g_gametype.integer >= GT_TEAM ) {
			won = winner != TEAM_FREE && client->sess.sessionTeam == winner;
			if ( client->sess.sessionTeam == TEAM_SPECTATOR && client->pers.robotsLost > 0 ) {
				won = winner != TEAM_FREE && client->pers.playedTeam == winner;	// lost all robots, watched
			}
		} else {
			won = client->ps.persistant[PERS_SCORE] >= bestScore;
		}
		damage = client->pers.damageDealt;
		points = 40 + client->pers.kills * 25 + damage / 2000 + client->pers.captures * 20 + ( won ? 60 : 0 );
		currency = 100 + client->pers.kills * 15 + client->pers.captures * 10 + ( won ? 150 : 0 );
		xp = 50 + client->pers.kills * 20 + damage / 1500 + client->pers.captures * 15 + ( won ? 100 : 0 );
		trap_SendServerCommand( index, va( "reward %s %i %i %i %i %i %i %i", battle, points, currency, xp,
			client->pers.kills, damage, client->pers.captures, won ) );
	}
}
