/*
===========================================================================
WALKER: robots and their weapons (see bg_robots.h).

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "q_shared.h"
#include "bg_public.h"
#include "bg_robots.h"

const char *bg_weaponClassNames[WCLASS_COUNT] = { "LIGHT", "MEDIUM", "HEAVY" };

// slot mounts (tag_weapon1.. of the models): forward, left, up from the eye (the view
// height 26 above the origin); the shot starts at the mount + the weapon length forward
const robotInfo_t bg_robots[ROBOT_COUNT] = {
	{ "k7", "K7", "Medium walker, two light mounts", "walker",
		62000, 15, 4000, 38, 3000,
		2, { WCLASS_LIGHT, WCLASS_LIGHT, 0 }, { RWEAPON_OSA, RWEAPON_OSA, -1 },
		{ { -6, -19.5f, -10.5f }, { -6, 19.5f, -10.5f }, { 0, 0, 0 } } },
	{ "strizh", "STRIZH", "Light scout, strong shield", "strizh",
		42000, 5, 9000, 56, 6000,
		2, { WCLASS_MEDIUM, WCLASS_LIGHT, 0 }, { RWEAPON_TOPOR, RWEAPON_OSA, -1 },
		{ { 1.5f, -13, -11.5f }, { 1.5f, 12.2f, -11.5f }, { 0, 0, 0 } } },
	{ "molot", "MOLOT", "Heavy assault, thick armor", "molot",
		98000, 30, 0, 28, 9000,
		1, { WCLASS_HEAVY, 0, 0 }, { RWEAPON_TARAN, -1, -1 },
		{ { 0, -16, -3.5f }, { 0, 0, 0 }, { 0, 0, 0 } } },
};

// damage and structure are real values: the game divides them by ROBOT_HEALTH_SCALE
const robotWeaponInfo_t bg_robotWeapons[RWEAPON_COUNT] = {
	{ "osa",	"OSA",		"Light machine gun",	WCLASS_LIGHT,	WFIRE_BULLET,	120,	125,	500,	0,	0,		300,	1200,	16 },
	{ "shmel",	"SHMEL",	"Light rocket pod",		WCLASS_LIGHT,	WFIRE_SHELL,	700,	1000,	500,	60,	1400,	0,		1800,	10 },
	{ "topor",	"TOPOR",	"Medium autocannon",	WCLASS_MEDIUM,	WFIRE_BULLET,	300,	333,	800,	0,	0,		150,	2500,	22 },
	{ "iskra",	"ISKRA",	"Medium plasma gun",	WCLASS_MEDIUM,	WFIRE_PLASMA,	650,	250,	800,	40,	1800,	0,		3000,	20 },
	{ "taran",	"TARAN",	"Heavy cannon",			WCLASS_HEAVY,	WFIRE_SHELL,	7600,	2000,	800,	90,	2400,	0,		4500,	33 },
	{ "kopyo",	"KOPYO",	"Heavy railgun",		WCLASS_HEAVY,	WFIRE_RAIL,		9000,	2200,	800,	0,	0,		0,		6000,	33 },
};

// seconds and development points from level to level + 1 (from 10 minutes to 24 hours)
static const int upgradeTime[ROBOT_LEVELS - 1] = {
	600, 1000, 1600, 2700, 4400, 7200, 12000, 19800, 32400, 53400, 86400
};
static const int upgradeCost[ROBOT_LEVELS - 1] = {
	60, 180, 350, 550, 790, 1050, 1350, 1670, 2010, 2380, 2770
};
// share of the robot upgrade cost for a weapon of each class, percent
static const int weaponCostShare[WCLASS_COUNT] = { 50, 67, 85 };

static int ClampLevel( int level, int maximum ) {
	return level < 1 ? 1 : ( level > maximum ? maximum : level );
}

int BG_RobotByName( const char *id ) {
	int		index;

	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		if ( !Q_stricmp( id, bg_robots[index].id ) ) {
			return index;
		}
	}
	return -1;
}

int BG_WeaponByName( const char *id ) {
	int		index;

	for ( index = 0; index < RWEAPON_COUNT; index++ ) {
		if ( !Q_stricmp( id, bg_robotWeapons[index].id ) ) {
			return index;
		}
	}
	return -1;
}

int BG_RobotByModel( const char *model ) {
	int		index, length;

	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		length = strlen( bg_robots[index].model );
		if ( !Q_stricmpn( model, bg_robots[index].model, length ) && ( model[length] == 0 || model[length] == '/' ) ) {
			return index;
		}
	}
	return ROBOT_K7;
}

// characteristics grow by a share of the level 1 value per level
static int Grow( int base, int level, int percentPerLevel ) {
	return base + base * percentPerLevel * ( level - 1 ) / 100;
}

int BG_RobotStructure( int type, int level ) {
	return Grow( bg_robots[type].structure, ClampLevel( level, ROBOT_LEVELS ), 8 );
}

int BG_RobotArmor( int type, int level ) {
	return bg_robots[type].armor + ( ClampLevel( level, ROBOT_LEVELS ) - 1 );
}

int BG_RobotShield( int type, int level ) {
	return Grow( bg_robots[type].shield, ClampLevel( level, ROBOT_LEVELS ), 8 );
}

int BG_RobotSpeed( int type, int level ) {
	return Grow( bg_robots[type].speed * 10, ClampLevel( level, ROBOT_LEVELS ), 1 ) / 10;
}

int BG_WeaponDamage( int weapon, int level ) {
	return Grow( bg_robotWeapons[weapon].damage, ClampLevel( level, WEAPON_LEVELS ), 8 );
}

int BG_WeaponInterval( int weapon, int level ) {
	// each level fires 3 % more often
	return bg_robotWeapons[weapon].interval * 100 / ( 100 + 3 * ( ClampLevel( level, WEAPON_LEVELS ) - 1 ) );
}

qboolean BG_WeaponFits( int robot, int slot, int weapon ) {
	if ( robot < 0 || robot >= ROBOT_COUNT || weapon < 0 || weapon >= RWEAPON_COUNT ) {
		return qfalse;
	}
	if ( slot < 0 || slot >= bg_robots[robot].numSlots ) {
		return qfalse;
	}
	return bg_robots[robot].slotClass[slot] == bg_robotWeapons[weapon].weaponClass;
}

int BG_RobotUpgradeCost( int level ) {
	return upgradeCost[ClampLevel( level, ROBOT_LEVELS - 1 ) - 1];
}

int BG_WeaponUpgradeCost( int weapon, int level ) {
	return upgradeCost[ClampLevel( level, WEAPON_LEVELS - 1 ) - 1] * weaponCostShare[bg_robotWeapons[weapon].weaponClass] / 100;
}

int BG_UpgradeTime( int level ) {
	return upgradeTime[ClampLevel( level, ROBOT_LEVELS - 1 ) - 1];
}

int BG_SpeedUpCost( int secondsLeft ) {
	// one unit of currency for each minute left
	return secondsLeft <= 0 ? 0 : ( secondsLeft + 59 ) / 60;
}

int BG_PlayerLevelXP( int level ) {
	return 400 * ( level < 1 ? 1 : level );
}
