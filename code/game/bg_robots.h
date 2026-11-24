/*
===========================================================================
WALKER: robots and their weapons.

Shared by the game, cgame and ui modules: types of robots and weapons, their
characteristics by level, the cost and time of upgrades. The structure (health)
and shield inside the game are kept in tenths (ROBOT_HEALTH_SCALE): the 16 bit
fields of the player state hold up to 32767, a strong robot has over 100 000.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/

#ifndef BG_ROBOTS_H
#define BG_ROBOTS_H

#define	ROBOT_LEVELS			12
#define	WEAPON_LEVELS			12
#define	ROBOT_HEALTH_SCALE		10		// structure and shield in the game: real value / 10
#define	ROBOT_HANGAR_SIZE		5		// bays of a hangar, robots a player takes into a battle
#define	ROBOT_SPEED_UNITS		38.0f	// km/h that move a robot at g_speed units per second

typedef enum {
	RWEAPON_AUTOCANNON,		// K7: one heavy machine gun
	RWEAPON_RAPIDGUNS,		// light robot: two fast machine guns
	RWEAPON_CANNON,			// heavy robot: a cannon firing shells
	RWEAPON_COUNT
} robotWeapon_t;

typedef enum {
	ROBOT_K7,
	ROBOT_STRIZH,
	ROBOT_MOLOT,
	ROBOT_COUNT
} robotType_t;

typedef struct {
	const char	*id;			// in accounts and in the userinfo
	const char	*title;
	const char	*description;
	const char	*model;			// player model folder
	int			structure;		// level 1
	int			armor;			// level 1, percent taken off every hit on the structure
	int			shield;			// level 1, absorbs damage first, recovers
	int			speed;			// level 1, km/h
	int			weapon;			// robotWeapon_t
	int			price;			// currency
} robotInfo_t;

typedef struct {
	const char	*id;
	const char	*title;
	int			damage;			// level 1, per hit (a shell: at the point of impact)
	int			interval;		// level 1, msec between shots
	int			range;			// units, the same on every level
	int			splashRadius;	// shells only
	int			projectileSpeed;// shells only, units per second
} robotWeaponInfo_t;

extern const robotInfo_t	bg_robots[ROBOT_COUNT];
extern const robotWeaponInfo_t	bg_robotWeapons[RWEAPON_COUNT];

int		BG_RobotByName( const char *id );		// -1: unknown
int		BG_RobotByModel( const char *model );	// player model "strizh/red" -> ROBOT_STRIZH, K7 for unknown
int		BG_RobotStructure( int type, int level );
int		BG_RobotArmor( int type, int level );
int		BG_RobotShield( int type, int level );
int		BG_RobotSpeed( int type, int level );		// km/h
int		BG_WeaponDamage( int weapon, int level );
int		BG_WeaponInterval( int weapon, int level );
int		BG_UpgradeCost( qboolean weapon, int level );	// development points from level to level + 1
int		BG_UpgradeTime( qboolean weapon, int level );	// seconds from level to level + 1
int		BG_SpeedUpCost( int secondsLeft );				// currency to finish an upgrade at once
int		BG_PlayerLevelXP( int level );					// experience from level to level + 1

#endif
