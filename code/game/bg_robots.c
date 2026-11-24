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

const robotInfo_t bg_robots[ROBOT_COUNT] = {
	{ "k7", "K7", "Medium walker, heavy machine gun", "walker",
		62000, 15, 8000, 38, RWEAPON_AUTOCANNON, 3000 },
	{ "strizh", "STRIZH", "Light scout, strong shield, two fast guns", "strizh",
		42000, 5, 18000, 56, RWEAPON_RAPIDGUNS, 6000 },
	{ "molot", "MOLOT", "Heavy assault walker, thick armor, cannon", "molot",
		98000, 30, 0, 28, RWEAPON_CANNON, 9000 },
};

// damage and structure are real values: the game divides them by ROBOT_HEALTH_SCALE
const robotWeaponInfo_t bg_robotWeapons[RWEAPON_COUNT] = {
	{ "autocannon", "Autocannon",	300,	100,	1600,	0,		0 },
	{ "rapidguns",	"Rapid guns",	140,	60,		1300,	0,		0 },
	{ "cannon",		"Cannon",		3800,	1000,	2000,	90,		2400 },
};

// seconds and development points from level to level + 1 (from 10 minutes to 24 hours)
static const int upgradeTime[ROBOT_LEVELS - 1] = {
	600, 1000, 1600, 2700, 4400, 7200, 12000, 19800, 32400, 53400, 86400
};
static const int upgradeCost[ROBOT_LEVELS - 1] = {
	60, 180, 350, 550, 790, 1050, 1350, 1670, 2010, 2380, 2770
};

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

int BG_UpgradeCost( qboolean weapon, int level ) {
	int		cost = upgradeCost[ClampLevel( level, ROBOT_LEVELS - 1 ) - 1];
	return weapon ? cost * 2 / 3 : cost;
}

int BG_UpgradeTime( qboolean weapon, int level ) {
	return upgradeTime[ClampLevel( level, ROBOT_LEVELS - 1 ) - 1];
}

int BG_SpeedUpCost( int secondsLeft ) {
	// one unit of currency for each minute left
	return secondsLeft <= 0 ? 0 : ( secondsLeft + 59 ) / 60;
}

int BG_PlayerLevelXP( int level ) {
	return 400 * ( level < 1 ? 1 : level );
}
