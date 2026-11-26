/*
===========================================================================
WALKER: robots and their weapons.

Shared by the game, cgame and ui modules: types of robots and weapons, their
characteristics by level, the cost and time of upgrades. The structure (health)
and shield inside the game are kept in tenths (ROBOT_HEALTH_SCALE): the 16 bit
fields of the player state hold up to 32767, a strong robot has over 100 000.

Weapons come in three classes (light, medium, heavy). A robot has slots of
given classes (K7: two light, STRIZH: one medium and one light, MOLOT: one
heavy); a weapon goes into a slot of its own class. Weapons are separate items
of the pilot (ui_account.c) with their own levels and move between robots.
All weapons of a robot fire together with the attack button, each at its own
rate (g_robots.c).

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
#define	ROBOT_UNITS_PER_METRE	5		// scale of the world: a robot of 60 units is 12 m tall, as
										// War Robots looks at the same distance (27.11.2026)
#define	ROBOT_SPEED_UNITS		39.6f	// km/h that move a robot at g_speed units per second:
										// g_speed 55 = 39.6 km/h = 11 m/s = 55 units at 5 units per metre
#define	ROBOT_MAX_SLOTS			3		// weapon slots of a robot (tag_weapon1 .. tag_weapon3)

typedef enum {
	WCLASS_LIGHT,
	WCLASS_MEDIUM,
	WCLASS_HEAVY,
	WCLASS_COUNT
} weaponClass_t;

typedef enum {
	WFIRE_BULLET,			// hitscan, tracer
	WFIRE_SHELL,			// projectile with an explosion
	WFIRE_PLASMA,			// projectile, plasma ball
	WFIRE_RAIL				// hitscan through the line of sight, beam
} weaponFire_t;

typedef enum {
	RWEAPON_OSA,			// light: machine gun
	RWEAPON_SHMEL,			// light: rocket pod
	RWEAPON_TOPOR,			// medium: autocannon
	RWEAPON_ISKRA,			// medium: plasma gun
	RWEAPON_TARAN,			// heavy: cannon
	RWEAPON_KOPYO,			// heavy: railgun
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
	int			armor;			// level 1, taken off every hit on the structure (real units)
	int			shield;			// level 1, absorbs damage first, recovers
	int			speed;			// level 1, km/h
	int			price;			// currency
	int			numSlots;
	int			slotClass[ROBOT_MAX_SLOTS];		// weaponClass_t
	int			defaultWeapon[ROBOT_MAX_SLOTS];	// a new robot comes with these
	float		slotOffset[ROBOT_MAX_SLOTS][3];	// mount (tag_weaponN) from the eye: forward, left, up
} robotInfo_t;

typedef struct {
	const char	*id;			// in accounts and in the userinfo
	const char	*title;
	const char	*description;
	int			weaponClass;	// weaponClass_t
	int			fire;			// weaponFire_t
	int			damage;			// level 1, per hit (a shell: at the point of impact)
	int			interval;		// level 1, msec between shots
	int			range;			// metres (x ROBOT_UNITS_PER_METRE in the world), the same on every level
	int			splashRadius;	// shells, units (it goes with the size of the robots, not the range)
	int			projectileSpeed;// shells, metres per second (x ROBOT_UNITS_PER_METRE in the world)
	int			spread;			// bullets, Quake spread (the machine gun has 200)
	int			price;			// currency
	int			length;			// from the mount to the muzzle (tag_flash of the model), units
} robotWeaponInfo_t;

// changeable by the balance cvars (see below), the stock values stay in bg_robots.c
extern robotInfo_t			bg_robots[ROBOT_COUNT];
extern robotWeaponInfo_t	bg_robotWeapons[RWEAPON_COUNT];
extern const char				*bg_weaponClassNames[WCLASS_COUNT];

int		BG_RobotByName( const char *id );		// -1: unknown
int		BG_RobotByModel( const char *model );	// player model "strizh/red" -> ROBOT_STRIZH, K7 for unknown
int		BG_WeaponByName( const char *id );		// -1: unknown
int		BG_RobotStructure( int type, int level );
int		BG_RobotArmor( int type, int level );		// taken off every hit on the structure
int		BG_ArmorHit( int damage, int armor );		// what passes, at least 1 / ARMOR_MIN_PASS of the hit
int		BG_RobotShield( int type, int level );
int		BG_RobotSpeed( int type, int level );		// km/h
int		BG_WeaponDamage( int weapon, int level );
int		BG_WeaponInterval( int weapon, int level );
qboolean BG_WeaponFits( int robot, int slot, int weapon );
int		BG_RobotUpgradeCost( int level );				// development points from level to level + 1
int		BG_WeaponUpgradeCost( int weapon, int level );
int		BG_UpgradeTime( int level );					// seconds from level to level + 1
int		BG_SpeedUpCost( int secondsLeft );				// currency to finish an upgrade at once
int		BG_PlayerLevelXP( int level );					// experience from level to level + 1

// WALKER: balance for tests. Cvar "bal_<id>" of a robot or a weapon holds its parameters in
// table units, separated by spaces, in the order of the enums; empty or missing values are
// the stock ones. Game, cgame and ui read them (CVAR_SYSTEMINFO from the server); the ui
// balance menu (ui_balance.c) writes them.
#define ARMOR_MIN_PASS		5		// the armor stops at most 4/5 of a hit
#define BALANCE_VERSION		"v2"	// first word of a balance cvar; older ones are dropped

typedef enum {
	RPARAM_STRUCTURE,
	RPARAM_ARMOR,
	RPARAM_SHIELD,
	RPARAM_SPEED,
	RPARAM_PRICE,
	RPARAM_COUNT
} robotParam_t;

typedef enum {
	WPARAM_DAMAGE,
	WPARAM_INTERVAL,
	WPARAM_RANGE,
	WPARAM_SPLASH,
	WPARAM_PROJECTILE_SPEED,
	WPARAM_SPREAD,
	WPARAM_PRICE,
	WPARAM_COUNT
} weaponParam_t;

typedef struct {
	const char	*name;
	int			minimum;
	int			maximum;
} balanceParam_t;

extern const balanceParam_t	bg_robotParams[RPARAM_COUNT];
extern const balanceParam_t	bg_weaponParams[WPARAM_COUNT];

int		BG_RobotParam( int robot, int param, qboolean stock );
int		BG_WeaponParam( int weapon, int param, qboolean stock );
qboolean BG_WeaponHasParam( int weapon, int param );		// splash only for shells, spread for bullets...
void	BG_SetRobotParam( int robot, int param, int value );	// clamped
void	BG_SetWeaponParam( int weapon, int param, int value );
const char *BG_BalanceCvarName( qboolean weapon, int index );
void	BG_BalanceString( qboolean weapon, int index, char *buffer, int size );	// "" when all stock
void	BG_BalanceRegister( int flags );		// once at the module start
qboolean BG_BalanceUpdate( void );			// every frame: takes changed cvars, qtrue when any

#endif
