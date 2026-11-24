/*
===========================================================================
WALKER: hangar.

The robots of the pilot account stand in five bays along the bottom of the
screen; an empty bay sells a new robot. The selected robot is shown large on the
left and can be turned around every axis: drag with the left mouse button
(left/right - turn, up/down - tilt), drag with the right button (roll), arrow
keys and PgUp/PgDn; HOME or "RESET VIEW" puts it back, the wheel zooms.

On the right: the characteristics of the robot and its weapon at their levels,
the upgrades (for development points, they take real time: from 10 minutes on
the first levels to 24 hours on the last) and finishing an upgrade at once for
currency. Robots being upgraded do not go into battles. All the others do, in
the order of the bays (ui_account.c, walker_robots).

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/

#include "ui_local.h"

#define ART_BACKGROUND		"menu/art/hangar"
#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"
#define ART_BAY				"menu/art/maps_select"
#define ART_BAY_SELECTED	"menu/art/maps_selected"

#define MAX_BAYS			ROBOT_HANGAR_SIZE

#define ID_BACK				10
#define ID_AUTOROTATE		12
#define ID_RESETVIEW		13
#define ID_UPGRADE_ROBOT	14
#define ID_UPGRADE_WEAPON	15
#define ID_FINISH			16
#define ID_BUY				17
#define ID_SHOP_PREV		18
#define ID_SHOP_NEXT		19
#define ID_BAY0				20

#define ZOOM_STEP			1.12f	// one wheel notch
#define ZOOM_MIN			0.6f
#define ZOOM_MAX			2.6f
#define DRAG_SPEED			1.2f	// degrees per virtual pixel of mouse travel
#define TURN_SPEED			20.0f	// degrees per second of the automatic turn
#define KEY_STEP			10.0f

// big preview
#define PREVIEW_X			8
#define PREVIEW_Y			44
#define PREVIEW_W			330
#define PREVIEW_H			282

// characteristics
#define PANEL_X				346
#define PANEL_Y				48
#define PANEL_W				286
#define LINE				18

// bays
#define BAY_Y				334
#define BAY_W				104
#define BAY_H				80
#define BAY_GAP				8

typedef struct {
	menuframework_s	menu;
	menutext_s		banner;
	menubitmap_s	preview;
	menubitmap_s	bays[MAX_BAYS];
	menubitmap_s	back;
	menutext_s		autoRotate;
	menutext_s		resetView;
	menutext_s		upgradeRobot;
	menutext_s		upgradeWeapon;
	menutext_s		finish;
	menutext_s		buy;
	menutext_s		shopPrev;
	menutext_s		shopNext;

	playerInfo_t	players[MAX_BAYS];
	playerInfo_t	shopPlayer;
	int				shopType;		// robot shown in an empty bay
	int				shopLoaded;		// type loaded into shopPlayer, -1 none
	int				loadedType[MAX_BAYS];
	int				selected;
	float			previewYaw;
	float			previewPitch;
	float			previewRoll;
	float			previewZoom;
	float			bayYaw;
	int				lastTime;
	qboolean		dragging;		// left button: turn and tilt
	qboolean		rolling;		// right button: roll
	float			dragLastX;
	float			dragLastY;
	qhandle_t		backgroundShader;
	qhandle_t		bayShader;
	qhandle_t		baySelectedShader;

	char			upgradeRobotLabel[64];
	char			upgradeWeaponLabel[64];
	char			finishLabel[64];
	char			buyLabel[64];
	char			message[96];
} hangarMenuInfo_t;

static hangarMenuInfo_t	hangarMenuInfo;

static vec4_t hangarDimColor = { 0.55f, 0.62f, 0.66f, 1.00f };
static vec4_t hangarPanelColor = { 0.0f, 0.0f, 0.0f, 0.55f };
static vec4_t hangarGreenColor = { 0.45f, 1.0f, 0.45f, 1.0f };


/*
=================
Hangar_Duration
=================
*/
static const char *Hangar_Duration( int seconds ) {
	if ( seconds < 0 ) {
		seconds = 0;
	}
	if ( seconds >= 3600 ) {
		return va( "%ih %02im", seconds / 3600, ( seconds / 60 ) % 60 );
	}
	return va( "%im %02is", seconds / 60, seconds % 60 );
}


/*
=================
Hangar_Number

12345 -> "12 345"
=================
*/
static const char *Hangar_Number( int value ) {
	// own rotating buffers: va() has only two, and one line of the panel
	// formats up to three numbers (value, "+" and the gain)
	static char	buffers[4][32];
	static int	current;
	char		*text = buffers[current];

	current = ( current + 1 ) & 3;
	if ( value >= 1000 ) {
		Com_sprintf( text, sizeof( buffers[0] ), "%i %03i", value / 1000, value % 1000 );
	} else {
		Com_sprintf( text, sizeof( buffers[0] ), "%i", value );
	}
	return text;
}


/*
=================
Hangar_RobotModel

the player model with the paint of the robot type
=================
*/
static const char *Hangar_RobotModel( int type ) {
	return bg_robots[type].model;
}


/*
=================
Hangar_FaceYaw

points the whole robot (legs, torso, head) at one angle, without the swing
the player preview normally uses
=================
*/
static void Hangar_FaceYaw( playerInfo_t *player, float yaw ) {
	player->viewAngles[YAW] = yaw;
	player->moveAngles[YAW] = yaw;
	player->legs.yawAngle = yaw;
	player->torso.yawAngle = yaw;
	player->legs.yawing = qfalse;
	player->torso.yawing = qfalse;
}


/*
=================
Hangar_SetPlayer
=================
*/
static void Hangar_SetPlayer( playerInfo_t *player, int type ) {
	vec3_t	angles;

	VectorClear( angles );
	angles[YAW] = 150;
	memset( player, 0, sizeof( *player ) );
	UI_PlayerInfo_SetModel( player, (char *)Hangar_RobotModel( type ) );
	UI_PlayerInfo_SetInfo( player, LEGS_IDLE, TORSO_STAND, angles, angles, WP_MACHINEGUN, qfalse );
}


/*
=================
Hangar_SyncModels

loads the models of the robots that came or changed
=================
*/
static void Hangar_SyncModels( void ) {
	int		index;

	for ( index = 0; index < MAX_BAYS; index++ ) {
		int		type = index < uiAccount.numRobots ? uiAccount.robots[index].type : -1;
		if ( type >= 0 && hangarMenuInfo.loadedType[index] != type ) {
			Hangar_SetPlayer( &hangarMenuInfo.players[index], type );
			hangarMenuInfo.loadedType[index] = type;
		}
		if ( type < 0 ) {
			hangarMenuInfo.loadedType[index] = -1;
		}
	}
	if ( hangarMenuInfo.shopLoaded != hangarMenuInfo.shopType ) {
		Hangar_SetPlayer( &hangarMenuInfo.shopPlayer, hangarMenuInfo.shopType );
		hangarMenuInfo.shopLoaded = hangarMenuInfo.shopType;
	}
}


/*
=================
Hangar_SelectedRobot

NULL: an empty bay is selected (the shop)
=================
*/
static accountRobot_t *Hangar_SelectedRobot( void ) {
	if ( hangarMenuInfo.selected < uiAccount.numRobots ) {
		return &uiAccount.robots[hangarMenuInfo.selected];
	}
	return NULL;
}


/*
=================
Hangar_Show
=================
*/
static void Hangar_Show( menutext_s *item, qboolean show ) {
	if ( show ) {
		item->generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
	} else {
		item->generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
	}
}


/*
=================
Hangar_UpdateButtons
=================
*/
static void Hangar_UpdateButtons( void ) {
	accountRobot_t	*robot = Hangar_SelectedRobot();
	int				now = UI_AccountTime();

	if ( !robot ) {
		const robotInfo_t	*info = &bg_robots[hangarMenuInfo.shopType];
		Com_sprintf( hangarMenuInfo.buyLabel, sizeof( hangarMenuInfo.buyLabel ), "BUY  %s CR", Hangar_Number( info->price ) );
		hangarMenuInfo.buy.color = uiAccount.currency >= info->price ? color_red : hangarDimColor;
		Hangar_Show( &hangarMenuInfo.buy, qtrue );
		Hangar_Show( &hangarMenuInfo.shopPrev, qtrue );
		Hangar_Show( &hangarMenuInfo.shopNext, qtrue );
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
		Hangar_Show( &hangarMenuInfo.finish, qfalse );
		return;
	}
	Hangar_Show( &hangarMenuInfo.buy, qfalse );
	Hangar_Show( &hangarMenuInfo.shopPrev, qfalse );
	Hangar_Show( &hangarMenuInfo.shopNext, qfalse );

	if ( robot->upgrade != UPGRADE_NONE ) {
		int		left = robot->upgradeEnd - now;
		Com_sprintf( hangarMenuInfo.finishLabel, sizeof( hangarMenuInfo.finishLabel ), "FINISH NOW  %i CR", BG_SpeedUpCost( left ) );
		hangarMenuInfo.finish.color = uiAccount.currency >= BG_SpeedUpCost( left ) ? color_red : hangarDimColor;
		Hangar_Show( &hangarMenuInfo.finish, qtrue );
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
		return;
	}
	Hangar_Show( &hangarMenuInfo.finish, qfalse );

	if ( robot->level < ROBOT_LEVELS ) {
		int		cost = BG_UpgradeCost( qfalse, robot->level );
		// the button carries the name only: cost and time are drawn in the small
		// fixed font beside it (Hangar_DrawPanel), the proportional font does not fit
		Q_strncpyz( hangarMenuInfo.upgradeRobotLabel, "UPGRADE ROBOT", sizeof( hangarMenuInfo.upgradeRobotLabel ) );
		hangarMenuInfo.upgradeRobot.color = uiAccount.points >= cost ? color_red : hangarDimColor;
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qtrue );
	} else {
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
	}
	if ( robot->weaponLevel < WEAPON_LEVELS ) {
		int		cost = BG_UpgradeCost( qtrue, robot->weaponLevel );
		// the button carries the name only: cost and time are drawn in the small
		// fixed font beside it (Hangar_DrawPanel), the proportional font does not fit
		Q_strncpyz( hangarMenuInfo.upgradeWeaponLabel, "UPGRADE WEAPON", sizeof( hangarMenuInfo.upgradeWeaponLabel ) );
		hangarMenuInfo.upgradeWeapon.color = uiAccount.points >= cost ? color_red : hangarDimColor;
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qtrue );
	} else {
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
	}
}


/*
=================
Hangar_DrawLine

a characteristic: name on the left, value and the gain of the next level on the right
=================
*/
static void Hangar_DrawLine( int y, const char *name, const char *value, const char *next ) {
	UI_DrawString( PANEL_X + 8, y, name, UI_LEFT | UI_SMALLFONT, hangarDimColor );
	UI_DrawString( PANEL_X + 150, y, value, UI_LEFT | UI_SMALLFONT, color_white );
	if ( next && next[0] ) {
		UI_DrawString( PANEL_X + PANEL_W - 8, y, next, UI_RIGHT | UI_SMALLFONT, hangarGreenColor );
	}
}


/*
=================
Hangar_DrawPanel
=================
*/
static void Hangar_DrawPanel( void ) {
	accountRobot_t		*robot = Hangar_SelectedRobot();
	const robotInfo_t	*info;
	const robotWeaponInfo_t	*weapon;
	int					type, level, weaponLevel, y, next, value;
	qboolean			showNext;

	if ( robot ) {
		type = robot->type;
		level = robot->level;
		weaponLevel = robot->weaponLevel;
	} else {
		type = hangarMenuInfo.shopType;
		level = 1;
		weaponLevel = 1;
	}
	info = &bg_robots[type];
	weapon = &bg_robotWeapons[info->weapon];
	showNext = robot && robot->upgrade == UPGRADE_NONE;

	UI_FillRect( PANEL_X, PANEL_Y - 6, PANEL_W, 280, hangarPanelColor );
	y = PANEL_Y;
	UI_DrawString( PANEL_X + 8, y, va( "%s   LEVEL %i", info->title, level ), UI_LEFT | UI_BIGFONT, color_orange );
	y += 30;
	UI_DrawString( PANEL_X + 8, y, info->description, UI_LEFT | UI_SMALLFONT, hangarDimColor );
	y += LINE + 6;

	next = level < ROBOT_LEVELS ? level + 1 : level;
	value = BG_RobotStructure( type, level );
	Hangar_DrawLine( y, "STRUCTURE", Hangar_Number( value ),
		showNext && next > level ? va( "+%s", Hangar_Number( BG_RobotStructure( type, next ) - value ) ) : "" );
	y += LINE;
	value = BG_RobotArmor( type, level );
	Hangar_DrawLine( y, "ARMOR", va( "%i %%", value ),
		showNext && next > level ? va( "+%i %%", BG_RobotArmor( type, next ) - value ) : "" );
	y += LINE;
	value = BG_RobotShield( type, level );
	Hangar_DrawLine( y, "SHIELD", value ? Hangar_Number( value ) : "-",
		showNext && next > level && value ? va( "+%s", Hangar_Number( BG_RobotShield( type, next ) - value ) ) : "" );
	y += LINE;
	value = BG_RobotSpeed( type, level );
	Hangar_DrawLine( y, "SPEED", va( "%i km/h", value ),
		showNext && next > level && BG_RobotSpeed( type, next ) > value ? va( "+%i", BG_RobotSpeed( type, next ) - value ) : "" );
	y += LINE + 8;

	UI_DrawString( PANEL_X + 8, y, va( "%s   LEVEL %i", weapon->title, weaponLevel ), UI_LEFT | UI_SMALLFONT, color_orange );
	y += LINE;
	next = weaponLevel < WEAPON_LEVELS ? weaponLevel + 1 : weaponLevel;
	value = BG_WeaponDamage( info->weapon, weaponLevel );
	Hangar_DrawLine( y, "DAMAGE", Hangar_Number( value ),
		showNext && next > weaponLevel ? va( "+%s", Hangar_Number( BG_WeaponDamage( info->weapon, next ) - value ) ) : "" );
	y += LINE;
	value = BG_WeaponInterval( info->weapon, weaponLevel );
	Hangar_DrawLine( y, "RATE", va( "%i.%i /s", 1000 / value, ( 10000 / value ) % 10 ),
		showNext && next > weaponLevel ? va( "%i.%i /s", 1000 / BG_WeaponInterval( info->weapon, next ),
			( 10000 / BG_WeaponInterval( info->weapon, next ) ) % 10 ) : "" );
	y += LINE;
	Hangar_DrawLine( y, "RANGE", va( "%i", weapon->range ), "" );
	y += LINE + 8;

	if ( robot && robot->upgrade != UPGRADE_NONE ) {
		UI_DrawString( PANEL_X + 8, y, va( "UPGRADING %s: %s LEFT", robot->upgrade == UPGRADE_ROBOT ? "ROBOT" : "WEAPON",
			Hangar_Duration( robot->upgradeEnd - UI_AccountTime() ) ), UI_LEFT | UI_SMALLFONT, color_yellow );
		UI_DrawString( PANEL_X + 8, y + LINE, "does not go into battles until done", UI_LEFT | UI_SMALLFONT, hangarDimColor );
	} else if ( !robot ) {
		UI_DrawString( PANEL_X + 8, y, "EMPTY BAY: buy a robot", UI_LEFT | UI_SMALLFONT, color_yellow );
	} else {
		// cost and duration of the upgrades, on the lines of their buttons
		if ( robot->level < ROBOT_LEVELS ) {
			int		cost = BG_UpgradeCost( qfalse, robot->level );
			UI_DrawString( PANEL_X + PANEL_W - 8, PANEL_Y + 218, va( "%i PTS  %s", cost, Hangar_Duration( BG_UpgradeTime( qfalse, robot->level ) ) ),
				UI_RIGHT | UI_SMALLFONT, uiAccount.points >= cost ? color_white : hangarDimColor );
		}
		if ( robot->weaponLevel < WEAPON_LEVELS ) {
			int		cost = BG_UpgradeCost( qtrue, robot->weaponLevel );
			UI_DrawString( PANEL_X + PANEL_W - 8, PANEL_Y + 238, va( "%i PTS  %s", cost, Hangar_Duration( BG_UpgradeTime( qtrue, robot->weaponLevel ) ) ),
				UI_RIGHT | UI_SMALLFONT, uiAccount.points >= cost ? color_white : hangarDimColor );
		}
	}
}


/*
=================
Hangar_DrawPreview
=================
*/
static void Hangar_DrawPreview( void *self ) {
	playerInfo_t	*player;

	player = Hangar_SelectedRobot() ? &hangarMenuInfo.players[hangarMenuInfo.selected] : &hangarMenuInfo.shopPlayer;
	Hangar_FaceYaw( player, hangarMenuInfo.previewYaw );
	uiPlayerOrbit = qtrue;
	uiPlayerOrbitPitch = hangarMenuInfo.previewPitch;
	uiPlayerOrbitRoll = hangarMenuInfo.previewRoll;
	uiPlayerOrbitZoom = hangarMenuInfo.previewZoom;
	UI_DrawPlayer( PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H, player, uis.realtime / 2 );
	uiPlayerOrbit = qfalse;
}


/*
=================
Hangar_DrawBay
=================
*/
static void Hangar_DrawBay( void *self ) {
	menubitmap_s	*bay;
	int				index;
	qboolean		focus;
	float			*color;
	char			label[64];

	bay = (menubitmap_s *)self;
	index = bay->generic.id - ID_BAY0;
	focus = ( bay->generic.parent->cursor == bay->generic.menuPosition );

	UI_FillRect( bay->generic.x, bay->generic.y, bay->width, bay->height, color_black );
	if ( index < uiAccount.numRobots ) {
		accountRobot_t	*robot = &uiAccount.robots[index];
		Hangar_FaceYaw( &hangarMenuInfo.players[index], hangarMenuInfo.bayYaw + index * 25 );
		UI_DrawPlayer( bay->generic.x + 4, bay->generic.y + 2, bay->width - 8, bay->height - 18, &hangarMenuInfo.players[index], uis.realtime / 2 );
		Com_sprintf( label, sizeof( label ), "%s %i", bg_robots[robot->type].title, robot->level );
		if ( robot->upgrade != UPGRADE_NONE ) {
			UI_DrawString( bay->generic.x + bay->width / 2, bay->generic.y + 4, "UPGRADING", UI_CENTER | UI_SMALLFONT, color_yellow );
		}
	} else {
		Q_strncpyz( label, "+ BUY", sizeof( label ) );
	}

	UI_DrawHandlePic( bay->generic.x, bay->generic.y, bay->width, bay->height,
		index == hangarMenuInfo.selected ? hangarMenuInfo.baySelectedShader : hangarMenuInfo.bayShader );

	color = ( index == hangarMenuInfo.selected ) ? text_color_normal : ( focus ? text_color_highlight : hangarDimColor );
	UI_DrawString( bay->generic.x + bay->width / 2, bay->generic.y + bay->height - 16, label, UI_CENTER | UI_SMALLFONT, color );
}


/*
=================
Hangar_Draw
=================
*/
static void Hangar_Draw( void ) {
	int		now;
	float	seconds;

	UI_AccountUpdate();		// upgrades whose time is up
	Hangar_SyncModels();
	Hangar_UpdateButtons();

	now = uis.realtime;
	seconds = hangarMenuInfo.lastTime ? ( now - hangarMenuInfo.lastTime ) * 0.001f : 0;
	hangarMenuInfo.lastTime = now;

	if ( hangarMenuInfo.dragging && !trap_Key_IsDown( K_MOUSE1 ) ) {
		hangarMenuInfo.dragging = qfalse;
	}
	if ( hangarMenuInfo.rolling && !trap_Key_IsDown( K_MOUSE2 ) ) {
		hangarMenuInfo.rolling = qfalse;
	}
	if ( hangarMenuInfo.dragging ) {
		hangarMenuInfo.previewYaw += ( uis.cursorx - hangarMenuInfo.dragLastX ) * DRAG_SPEED;
		hangarMenuInfo.previewPitch += ( uis.cursory - hangarMenuInfo.dragLastY ) * DRAG_SPEED;
	} else if ( hangarMenuInfo.rolling ) {
		hangarMenuInfo.previewRoll += ( uis.cursorx - hangarMenuInfo.dragLastX ) * DRAG_SPEED;
	} else if ( trap_Cvar_VariableValue( "ui_hangarAutoRotate" ) ) {
		hangarMenuInfo.previewYaw += TURN_SPEED * seconds;
	}
	hangarMenuInfo.dragLastX = uis.cursorx;
	hangarMenuInfo.dragLastY = uis.cursory;
	hangarMenuInfo.previewYaw = AngleMod( hangarMenuInfo.previewYaw );
	hangarMenuInfo.previewPitch = AngleNormalize180( hangarMenuInfo.previewPitch );
	hangarMenuInfo.previewRoll = AngleNormalize180( hangarMenuInfo.previewRoll );
	if ( trap_Cvar_VariableValue( "ui_hangarAutoRotate" ) ) {
		hangarMenuInfo.bayYaw = AngleMod( hangarMenuInfo.bayYaw + TURN_SPEED * 0.5f * seconds );
	}
	hangarMenuInfo.autoRotate.string = trap_Cvar_VariableValue( "ui_hangarAutoRotate" ) ? "AUTO ROTATE: ON" : "AUTO ROTATE: OFF";

	UI_DrawHandlePic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, hangarMenuInfo.backgroundShader );
	Hangar_DrawPanel();
	UI_DrawString( 8, 8, va( "%s  LV %i", uiAccount.nick, uiAccount.level ), UI_LEFT | UI_SMALLFONT, color_white );
	UI_DrawString( 632, 8, va( "%s CR    %s PTS", Hangar_Number( uiAccount.currency ), Hangar_Number( uiAccount.points ) ),
		UI_RIGHT | UI_SMALLFONT, color_yellow );
	Menu_Draw( &hangarMenuInfo.menu );
	if ( hangarMenuInfo.message[0] ) {
		UI_DrawString( 320, 422, hangarMenuInfo.message, UI_CENTER | UI_SMALLFONT, color_red );
	}
	UI_DrawString( 400, 446, "drag: left - turn, right - roll   wheel: zoom", UI_CENTER | UI_SMALLFONT, hangarDimColor );
	UI_DrawString( 400, 462, va( "%i of %i robots ready for battle", UI_AccountAvailableRobots(), uiAccount.numRobots ),
		UI_CENTER | UI_SMALLFONT, hangarDimColor );
}


/*
=================
Hangar_Key
=================
*/
static sfxHandle_t Hangar_Key( int key ) {
	switch ( key ) {
	case K_MOUSE1:
	case K_MOUSE2:
		if ( uis.cursorx >= PREVIEW_X && uis.cursorx <= PREVIEW_X + PREVIEW_W &&
			uis.cursory >= PREVIEW_Y + 20 && uis.cursory <= PREVIEW_Y + PREVIEW_H - 50 ) {
			if ( key == K_MOUSE1 ) {
				hangarMenuInfo.dragging = qtrue;
			} else {
				hangarMenuInfo.rolling = qtrue;
			}
			hangarMenuInfo.dragLastX = uis.cursorx;
			hangarMenuInfo.dragLastY = uis.cursory;
			return 0;
		}
		break;
	case K_LEFTARROW:
	case K_KP_LEFTARROW:
		hangarMenuInfo.previewYaw -= KEY_STEP;
		return 0;
	case K_RIGHTARROW:
	case K_KP_RIGHTARROW:
		hangarMenuInfo.previewYaw += KEY_STEP;
		return 0;
	case K_UPARROW:
	case K_KP_UPARROW:
		hangarMenuInfo.previewPitch -= KEY_STEP;
		return 0;
	case K_DOWNARROW:
	case K_KP_DOWNARROW:
		hangarMenuInfo.previewPitch += KEY_STEP;
		return 0;
	case K_PGUP:
	case K_KP_PGUP:
		hangarMenuInfo.previewRoll -= KEY_STEP;
		return 0;
	case K_PGDN:
	case K_KP_PGDN:
		hangarMenuInfo.previewRoll += KEY_STEP;
		return 0;
	case K_HOME:
	case K_KP_HOME:
		hangarMenuInfo.previewYaw = 150;
		hangarMenuInfo.previewPitch = 0;
		hangarMenuInfo.previewRoll = 0;
		hangarMenuInfo.previewZoom = 1;
		return 0;
	case K_MWHEELUP:
	case K_KP_PLUS:
	case '=':
		hangarMenuInfo.previewZoom *= ZOOM_STEP;
		if ( hangarMenuInfo.previewZoom > ZOOM_MAX ) {
			hangarMenuInfo.previewZoom = ZOOM_MAX;
		}
		return 0;
	case K_MWHEELDOWN:
	case K_KP_MINUS:
	case '-':
		hangarMenuInfo.previewZoom /= ZOOM_STEP;
		if ( hangarMenuInfo.previewZoom < ZOOM_MIN ) {
			hangarMenuInfo.previewZoom = ZOOM_MIN;
		}
		return 0;
	}
	return Menu_DefaultKey( &hangarMenuInfo.menu, key );
}


/*
=================
Hangar_StartUpgrade
=================
*/
static void Hangar_StartUpgrade( qboolean weapon ) {
	accountRobot_t	*robot = Hangar_SelectedRobot();
	int				level, cost;

	hangarMenuInfo.message[0] = 0;
	if ( !robot || robot->upgrade != UPGRADE_NONE ) {
		return;
	}
	level = weapon ? robot->weaponLevel : robot->level;
	if ( level >= ( weapon ? WEAPON_LEVELS : ROBOT_LEVELS ) ) {
		return;
	}
	cost = BG_UpgradeCost( weapon, level );
	if ( uiAccount.points < cost ) {
		Com_sprintf( hangarMenuInfo.message, sizeof( hangarMenuInfo.message ), "Not enough development points: %i more needed",
			cost - uiAccount.points );
		return;
	}
	uiAccount.points -= cost;
	robot->upgrade = weapon ? UPGRADE_WEAPON : UPGRADE_ROBOT;
	robot->upgradeEnd = UI_AccountTime() + BG_UpgradeTime( weapon, level );
	UI_AccountSave();
	UI_AccountUpdate();
}


/*
=================
Hangar_Finish
=================
*/
static void Hangar_Finish( void ) {
	accountRobot_t	*robot = Hangar_SelectedRobot();
	int				cost;

	hangarMenuInfo.message[0] = 0;
	if ( !robot || robot->upgrade == UPGRADE_NONE ) {
		return;
	}
	cost = BG_SpeedUpCost( robot->upgradeEnd - UI_AccountTime() );
	if ( uiAccount.currency < cost ) {
		Com_sprintf( hangarMenuInfo.message, sizeof( hangarMenuInfo.message ), "Not enough credits: %i more needed",
			cost - uiAccount.currency );
		return;
	}
	uiAccount.currency -= cost;
	robot->upgradeEnd = UI_AccountTime();
	UI_AccountUpdate();		// finishes it and saves
	UI_AccountSave();
}


/*
=================
Hangar_Buy
=================
*/
static void Hangar_Buy( void ) {
	const robotInfo_t	*info = &bg_robots[hangarMenuInfo.shopType];
	accountRobot_t		*robot;

	hangarMenuInfo.message[0] = 0;
	if ( uiAccount.numRobots >= ROBOT_HANGAR_SIZE ) {
		return;
	}
	if ( uiAccount.currency < info->price ) {
		Com_sprintf( hangarMenuInfo.message, sizeof( hangarMenuInfo.message ), "Not enough credits: %s more needed",
			Hangar_Number( info->price - uiAccount.currency ) );
		return;
	}
	uiAccount.currency -= info->price;
	robot = &uiAccount.robots[uiAccount.numRobots];
	memset( robot, 0, sizeof( *robot ) );
	robot->type = hangarMenuInfo.shopType;
	robot->level = 1;
	robot->weaponLevel = 1;
	hangarMenuInfo.selected = uiAccount.numRobots;
	uiAccount.numRobots++;
	UI_AccountSave();
	UI_AccountUpdate();
}


/*
=================
Hangar_Event
=================
*/
static void Hangar_Event( void *ptr, int event ) {
	int		id;

	if ( event != QM_ACTIVATED ) {
		return;
	}
	id = ( (menucommon_s *)ptr )->id;
	if ( id >= ID_BAY0 && id < ID_BAY0 + MAX_BAYS ) {
		hangarMenuInfo.selected = id - ID_BAY0;
		if ( hangarMenuInfo.selected > uiAccount.numRobots ) {
			hangarMenuInfo.selected = uiAccount.numRobots;		// the first empty bay sells
		}
		hangarMenuInfo.message[0] = 0;
		return;
	}
	switch ( id ) {
	case ID_AUTOROTATE:
		trap_Cvar_SetValue( "ui_hangarAutoRotate", !trap_Cvar_VariableValue( "ui_hangarAutoRotate" ) );
		break;
	case ID_RESETVIEW:
		hangarMenuInfo.previewYaw = 150;
		hangarMenuInfo.previewPitch = 0;
		hangarMenuInfo.previewRoll = 0;
		hangarMenuInfo.previewZoom = 1;
		break;
	case ID_UPGRADE_ROBOT:
		Hangar_StartUpgrade( qfalse );
		break;
	case ID_UPGRADE_WEAPON:
		Hangar_StartUpgrade( qtrue );
		break;
	case ID_FINISH:
		Hangar_Finish();
		break;
	case ID_BUY:
		Hangar_Buy();
		break;
	case ID_SHOP_PREV:
		hangarMenuInfo.shopType = ( hangarMenuInfo.shopType + ROBOT_COUNT - 1 ) % ROBOT_COUNT;
		break;
	case ID_SHOP_NEXT:
		hangarMenuInfo.shopType = ( hangarMenuInfo.shopType + 1 ) % ROBOT_COUNT;
		break;
	case ID_BACK:
		UI_PopMenu();
		break;
	}
}


/*
=================
UI_Hangar_Cache
=================
*/
void UI_Hangar_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_BACKGROUND );
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
	trap_R_RegisterShaderNoMip( ART_BAY );
	trap_R_RegisterShaderNoMip( ART_BAY_SELECTED );
}


/*
=================
Hangar_Button
=================
*/
static void Hangar_Button( menutext_s *item, int id, int x, int y, int style, const char *text ) {
	item->generic.type		= MTYPE_PTEXT;
	item->generic.flags		= ( style & UI_RIGHT ? QMF_RIGHT_JUSTIFY : ( style & UI_CENTER ? QMF_CENTER_JUSTIFY : QMF_LEFT_JUSTIFY ) ) | QMF_PULSEIFFOCUS;
	item->generic.id		= id;
	item->generic.callback	= Hangar_Event;
	item->generic.x			= x;
	item->generic.y			= y;
	item->string			= (char *)text;
	item->style				= style | UI_SMALLFONT;
	item->color				= color_red;
	Menu_AddItem( &hangarMenuInfo.menu, item );
}


/*
=================
Hangar_Init
=================
*/
static void Hangar_Init( void ) {
	int		index;

	memset( &hangarMenuInfo, 0, sizeof( hangarMenuInfo ) );
	hangarMenuInfo.menu.fullscreen = qtrue;
	hangarMenuInfo.menu.wrapAround = qtrue;
	hangarMenuInfo.menu.draw = Hangar_Draw;
	hangarMenuInfo.menu.key = Hangar_Key;

	UI_Hangar_Cache();
	hangarMenuInfo.backgroundShader = trap_R_RegisterShaderNoMip( ART_BACKGROUND );
	hangarMenuInfo.bayShader = trap_R_RegisterShaderNoMip( ART_BAY );
	hangarMenuInfo.baySelectedShader = trap_R_RegisterShaderNoMip( ART_BAY_SELECTED );

	for ( index = 0; index < MAX_BAYS; index++ ) {
		hangarMenuInfo.loadedType[index] = -1;
	}
	hangarMenuInfo.shopLoaded = -1;
	hangarMenuInfo.shopType = ROBOT_STRIZH;
	UI_AccountUpdate();
	Hangar_SyncModels();
	hangarMenuInfo.previewYaw = 150;
	hangarMenuInfo.previewZoom = 1;

	hangarMenuInfo.banner.generic.type	= MTYPE_BTEXT;
	hangarMenuInfo.banner.generic.x		= 320;
	hangarMenuInfo.banner.generic.y		= 10;
	hangarMenuInfo.banner.string		= "HANGAR";
	hangarMenuInfo.banner.color			= color_white;
	hangarMenuInfo.banner.style			= UI_CENTER;
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.banner );

	hangarMenuInfo.preview.generic.type		= MTYPE_BITMAP;
	hangarMenuInfo.preview.generic.flags	= QMF_INACTIVE;
	hangarMenuInfo.preview.generic.ownerdraw = Hangar_DrawPreview;
	hangarMenuInfo.preview.generic.x		= PREVIEW_X;
	hangarMenuInfo.preview.generic.y		= PREVIEW_Y;
	hangarMenuInfo.preview.width			= PREVIEW_W;
	hangarMenuInfo.preview.height			= PREVIEW_H;
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.preview );

	for ( index = 0; index < MAX_BAYS; index++ ) {
		hangarMenuInfo.bays[index].generic.type		= MTYPE_BITMAP;
		hangarMenuInfo.bays[index].generic.flags	= QMF_LEFT_JUSTIFY | QMF_NODEFAULTINIT;
		hangarMenuInfo.bays[index].generic.id		= ID_BAY0 + index;
		hangarMenuInfo.bays[index].generic.callback	= Hangar_Event;
		hangarMenuInfo.bays[index].generic.ownerdraw = Hangar_DrawBay;
		hangarMenuInfo.bays[index].generic.x		= ( SCREEN_WIDTH - ( MAX_BAYS * BAY_W + ( MAX_BAYS - 1 ) * BAY_GAP ) ) / 2 + index * ( BAY_W + BAY_GAP );
		hangarMenuInfo.bays[index].generic.y		= BAY_Y;
		hangarMenuInfo.bays[index].width			= BAY_W;
		hangarMenuInfo.bays[index].height			= BAY_H;
		hangarMenuInfo.bays[index].generic.left		= hangarMenuInfo.bays[index].generic.x;
		hangarMenuInfo.bays[index].generic.top		= hangarMenuInfo.bays[index].generic.y;
		hangarMenuInfo.bays[index].generic.right	= hangarMenuInfo.bays[index].generic.x + BAY_W;
		hangarMenuInfo.bays[index].generic.bottom	= hangarMenuInfo.bays[index].generic.y + BAY_H;
		Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.bays[index] );
	}

	// actions under the characteristics
	Hangar_Button( &hangarMenuInfo.upgradeRobot, ID_UPGRADE_ROBOT, PANEL_X + 8, PANEL_Y + 214, UI_LEFT, hangarMenuInfo.upgradeRobotLabel );
	Hangar_Button( &hangarMenuInfo.upgradeWeapon, ID_UPGRADE_WEAPON, PANEL_X + 8, PANEL_Y + 234, UI_LEFT, hangarMenuInfo.upgradeWeaponLabel );
	Hangar_Button( &hangarMenuInfo.finish, ID_FINISH, PANEL_X + 8, PANEL_Y + 254, UI_LEFT, hangarMenuInfo.finishLabel );
	Hangar_Button( &hangarMenuInfo.buy, ID_BUY, PANEL_X + PANEL_W / 2, PANEL_Y + 254, UI_CENTER, hangarMenuInfo.buyLabel );
	Hangar_Button( &hangarMenuInfo.shopPrev, ID_SHOP_PREV, PANEL_X + 8, PANEL_Y + 254, UI_LEFT, "<" );
	Hangar_Button( &hangarMenuInfo.shopNext, ID_SHOP_NEXT, PANEL_X + PANEL_W - 8, PANEL_Y + 254, UI_RIGHT, ">" );
	Hangar_Button( &hangarMenuInfo.autoRotate, ID_AUTOROTATE, PREVIEW_X + 4, PREVIEW_Y + PREVIEW_H - 20, UI_LEFT, "AUTO ROTATE: ON" );
	Hangar_Button( &hangarMenuInfo.resetView, ID_RESETVIEW, PREVIEW_X + PREVIEW_W - 4, PREVIEW_Y + PREVIEW_H - 20, UI_RIGHT, "RESET VIEW" );

	hangarMenuInfo.back.generic.type		= MTYPE_BITMAP;
	hangarMenuInfo.back.generic.name		= ART_BACK0;
	hangarMenuInfo.back.generic.flags		= QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
	hangarMenuInfo.back.generic.id			= ID_BACK;
	hangarMenuInfo.back.generic.callback	= Hangar_Event;
	hangarMenuInfo.back.generic.x			= 0;
	hangarMenuInfo.back.generic.y			= 480 - 64;
	hangarMenuInfo.back.width				= 128;
	hangarMenuInfo.back.height				= 64;
	hangarMenuInfo.back.focuspic			= ART_BACK1;
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.back );
	Hangar_UpdateButtons();
}


/*
=================
UI_HangarMenu
=================
*/
void UI_HangarMenu( void ) {
	if ( !uiAccount.loaded ) {
		UI_AccountMenu();
		return;
	}
	Hangar_Init();
	UI_PushMenu( &hangarMenuInfo.menu );
	Menu_SetCursorToItem( &hangarMenuInfo.menu, &hangarMenuInfo.bays[0] );
}
