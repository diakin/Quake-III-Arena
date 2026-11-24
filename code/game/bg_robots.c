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
robotInfo_t bg_robots[ROBOT_COUNT] = {
	{ "k7", "K7", "Medium walker, two light mounts", "walker",
		65000, 40, 6000, 40, 3000,
		2, { WCLASS_LIGHT, WCLASS_LIGHT, 0 }, { RWEAPON_OSA, RWEAPON_OSA, -1 },
		{ { -6, -19.5f, -10.5f }, { -6, 19.5f, -10.5f }, { 0, 0, 0 } } },
	{ "strizh", "STRIZH", "Light scout, strong shield", "strizh",
		38000, 0, 12000, 60, 6000,
		2, { WCLASS_MEDIUM, WCLASS_LIGHT, 0 }, { RWEAPON_TOPOR, RWEAPON_OSA, -1 },
		{ { 1.5f, -13, -11.5f }, { 1.5f, 12.2f, -11.5f }, { 0, 0, 0 } } },
	{ "molot", "MOLOT", "Heavy assault, thick armor", "molot",
		125000, 150, 0, 26, 9000,
		1, { WCLASS_HEAVY, 0, 0 }, { RWEAPON_TARAN, -1, -1 },
		{ { 0, -16, -3.5f }, { 0, 0, 0 }, { 0, 0, 0 } } },
};

// damage and structure are real values: the game divides them by ROBOT_HEALTH_SCALE
robotWeaponInfo_t bg_robotWeapons[RWEAPON_COUNT] = {
	{ "osa",	"OSA",		"Light machine gun",	WCLASS_LIGHT,	WFIRE_BULLET,	290,	125,	450,	0,	0,		300,	1200,	16 },
	{ "shmel",	"SHMEL",	"Light rocket pod",		WCLASS_LIGHT,	WFIRE_SHELL,	2600,	1000,	500,	60,	1400,	0,		1800,	10 },
	{ "topor",	"TOPOR",	"Medium autocannon",	WCLASS_MEDIUM,	WFIRE_BULLET,	500,	333,	700,	0,	0,		150,	2500,	22 },
	{ "iskra",	"ISKRA",	"Medium plasma gun",	WCLASS_MEDIUM,	WFIRE_PLASMA,	700,	400,	600,	50,	1800,	0,		3000,	20 },
	{ "taran",	"TARAN",	"Heavy cannon",			WCLASS_HEAVY,	WFIRE_SHELL,	9000,	2000,	750,	90,	2400,	0,		4500,	33 },
	{ "kopyo",	"KOPYO",	"Heavy railgun",		WCLASS_HEAVY,	WFIRE_RAIL,		13000,	3333,	1000,	0,	0,		0,		6000,	33 },
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
	return Grow( bg_robots[type].armor, ClampLevel( level, ROBOT_LEVELS ), 8 );
}

int BG_ArmorHit( int damage, int armor ) {
	int		passed = damage - armor;
	return passed > damage / ARMOR_MIN_PASS ? passed : damage / ARMOR_MIN_PASS;
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


/*
===========================================================================
WALKER: balance for tests (see bg_robots.h)
===========================================================================
*/

// the syscalls of the module this file is built into (game, cgame and ui have the same ones)
void	trap_Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags );
void	trap_Cvar_Update( vmCvar_t *vmCvar );

const balanceParam_t bg_robotParams[RPARAM_COUNT] = {
	{ "STRUCTURE",		100,	10000000 },
	{ "ARMOR",			0,		100000 },
	{ "SHIELD",			0,		10000000 },
	{ "SPEED KM/H",		1,		500 },
	{ "PRICE",			0,		100000000 },
};

const balanceParam_t bg_weaponParams[WPARAM_COUNT] = {
	{ "DAMAGE",			0,		10000000 },
	{ "INTERVAL MS",	10,		60000 },		// the menu shows shots per second
	{ "RANGE M",		10,		20000 },
	{ "SPLASH",			0,		5000 },
	{ "SHELL SPEED",	50,		50000 },
	{ "SPREAD",			0,		10000 },
	{ "PRICE",			0,		100000000 },
};

static robotInfo_t			stockRobots[ROBOT_COUNT];
static robotWeaponInfo_t	stockWeapons[RWEAPON_COUNT];
static qboolean				stockSaved;
static vmCvar_t				balanceCvars[ROBOT_COUNT + RWEAPON_COUNT];
static int					balanceCounts[ROBOT_COUNT + RWEAPON_COUNT];

static void BG_SaveStock( void ) {
	if ( !stockSaved ) {
		memcpy( stockRobots, bg_robots, sizeof( stockRobots ) );
		memcpy( stockWeapons, bg_robotWeapons, sizeof( stockWeapons ) );
		stockSaved = qtrue;
	}
}

static int *BG_RobotField( robotInfo_t *info, int param ) {
	switch ( param ) {
	case RPARAM_STRUCTURE:	return &info->structure;
	case RPARAM_ARMOR:		return &info->armor;
	case RPARAM_SHIELD:		return &info->shield;
	case RPARAM_SPEED:		return &info->speed;
	default:				return &info->price;
	}
}

static int *BG_WeaponField( robotWeaponInfo_t *info, int param ) {
	switch ( param ) {
	case WPARAM_DAMAGE:				return &info->damage;
	case WPARAM_INTERVAL:			return &info->interval;
	case WPARAM_RANGE:				return &info->range;
	case WPARAM_SPLASH:				return &info->splashRadius;
	case WPARAM_PROJECTILE_SPEED:	return &info->projectileSpeed;
	case WPARAM_SPREAD:				return &info->spread;
	default:						return &info->price;
	}
}

static int BG_ClampParam( const balanceParam_t *param, int value ) {
	return value < param->minimum ? param->minimum : ( value > param->maximum ? param->maximum : value );
}

int BG_RobotParam( int robot, int param, qboolean stock ) {
	BG_SaveStock();
	return *BG_RobotField( stock ? &stockRobots[robot] : &bg_robots[robot], param );
}

int BG_WeaponParam( int weapon, int param, qboolean stock ) {
	BG_SaveStock();
	return *BG_WeaponField( stock ? &stockWeapons[weapon] : &bg_robotWeapons[weapon], param );
}

qboolean BG_WeaponHasParam( int weapon, int param ) {
	int		fire = bg_robotWeapons[weapon].fire;

	switch ( param ) {
	case WPARAM_SPLASH:
	case WPARAM_PROJECTILE_SPEED:
		return fire == WFIRE_SHELL || fire == WFIRE_PLASMA;
	case WPARAM_SPREAD:
		return fire == WFIRE_BULLET;
	default:
		return qtrue;
	}
}

void BG_SetRobotParam( int robot, int param, int value ) {
	BG_SaveStock();
	*BG_RobotField( &bg_robots[robot], param ) = BG_ClampParam( &bg_robotParams[param], value );
}

void BG_SetWeaponParam( int weapon, int param, int value ) {
	BG_SaveStock();
	// a parameter this weapon does not use (shell speed of a machine gun) keeps its 0
	*BG_WeaponField( &bg_robotWeapons[weapon], param ) = BG_WeaponHasParam( weapon, param )
		? BG_ClampParam( &bg_weaponParams[param], value ) : value;
}

const char *BG_BalanceCvarName( qboolean weapon, int index ) {
	return va( "bal_%s", weapon ? bg_robotWeapons[index].id : bg_robots[index].id );
}

void BG_BalanceString( qboolean weapon, int index, char *buffer, int size ) {
	int		param, count = weapon ? WPARAM_COUNT : RPARAM_COUNT;
	qboolean changed = qfalse;

	buffer[0] = 0;
	for ( param = 0; param < count; param++ ) {
		int		value = weapon ? BG_WeaponParam( index, param, qfalse ) : BG_RobotParam( index, param, qfalse );
		int		stock = weapon ? BG_WeaponParam( index, param, qtrue ) : BG_RobotParam( index, param, qtrue );
		if ( value != stock ) {
			changed = qtrue;
		}
		Q_strcat( buffer, size, va( param ? " %i" : BALANCE_VERSION " %i", value ) );
	}
	if ( !changed ) {
		buffer[0] = 0;
	}
}

/*
=================
BG_BalanceParse

values of a robot or a weapon from its cvar, the stock ones for the missing
=================
*/
static void BG_BalanceParse( qboolean weapon, int index, const char *text ) {
	int		param, count = weapon ? WPARAM_COUNT : RPARAM_COUNT;
	char	*token;

	token = COM_ParseExt( (char **)&text, qfalse );
	if ( strcmp( token, BALANCE_VERSION ) ) {
		text = "";		// empty, or saved for an older stock balance: all stock
	}
	for ( param = 0; param < count; param++ ) {
		int		value;

		token = COM_ParseExt( (char **)&text, qfalse );
		value = token[0] ? atoi( token ) : ( weapon ? BG_WeaponParam( index, param, qtrue ) : BG_RobotParam( index, param, qtrue ) );
		if ( weapon ) {
			BG_SetWeaponParam( index, param, value );
		} else {
			BG_SetRobotParam( index, param, value );
		}
	}
}

void BG_BalanceRegister( int flags ) {
	int		index;

	BG_SaveStock();
	for ( index = 0; index < ROBOT_COUNT + RWEAPON_COUNT; index++ ) {
		qboolean	weapon = index >= ROBOT_COUNT;
		trap_Cvar_Register( &balanceCvars[index], BG_BalanceCvarName( weapon, weapon ? index - ROBOT_COUNT : index ), "", flags );
		balanceCounts[index] = -1;
	}
	BG_BalanceUpdate();
}

qboolean BG_BalanceUpdate( void ) {
	int		index;
	qboolean changed = qfalse;

	for ( index = 0; index < ROBOT_COUNT + RWEAPON_COUNT; index++ ) {
		qboolean	weapon = index >= ROBOT_COUNT;
		trap_Cvar_Update( &balanceCvars[index] );
		if ( balanceCvars[index].modificationCount != balanceCounts[index] ) {
			balanceCounts[index] = balanceCvars[index].modificationCount;
			BG_BalanceParse( weapon, weapon ? index - ROBOT_COUNT : index, balanceCvars[index].string );
			changed = qtrue;
		}
	}
	return changed;
}
