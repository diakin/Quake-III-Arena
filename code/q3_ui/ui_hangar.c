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

Layout (virtual 640x480): top bar with the pilot and his money, preview on the
left, characteristics panel on the right, bays, bottom bar with BACK. Texts use
the small 6x12 font (UI_TINYFONT); buttons are drawn by Hangar_DrawButton.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/

#include "ui_local.h"

#define ART_BACKGROUND		"menu/art/hangar"
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

#define TOPBAR_H			22

// big preview
#define PREVIEW_X			8
#define PREVIEW_Y			26
#define PREVIEW_W			384
#define PREVIEW_H			304

// characteristics
#define PANEL_X				400
#define PANEL_Y				30
#define PANEL_W				232
#define PANEL_H				250
#define PANEL_PAD			8
#define LINE				14
#define BUTTON_H			18
#define ROW_FIRST			( PANEL_Y + 176 )	// first line under the characteristics

// bays
#define BAY_Y				338
#define BAY_W				104
#define BAY_H				80
#define BAY_GAP				8

// bottom bar
#define BOTTOM_Y			430

typedef struct {
	menubitmap_s	item;			// first: the menu code sees a bitmap item
	char			label[48];
	char			detail[48];		// right-aligned part (cost, time), may be empty
	qboolean		enabled;		// enough points / credits: bright, otherwise dim
} hangarButton_t;

typedef struct {
	menuframework_s	menu;
	menubitmap_s	preview;
	menubitmap_s	bays[MAX_BAYS];
	hangarButton_t	back;
	hangarButton_t	autoRotate;
	hangarButton_t	resetView;
	hangarButton_t	upgradeRobot;
	hangarButton_t	upgradeWeapon;
	hangarButton_t	finish;
	hangarButton_t	buy;
	hangarButton_t	shopPrev;
	hangarButton_t	shopNext;

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

	char			message[96];
} hangarMenuInfo_t;

static hangarMenuInfo_t	hangarMenuInfo;

static vec4_t hangarDimColor = { 0.55f, 0.62f, 0.66f, 1.00f };
static vec4_t hangarTextColor = { 0.88f, 0.90f, 0.92f, 1.00f };
static vec4_t hangarAccentColor = { 1.00f, 0.55f, 0.15f, 1.00f };
static vec4_t hangarPanelColor = { 0.0f, 0.0f, 0.0f, 0.60f };
static vec4_t hangarBarColor = { 0.0f, 0.0f, 0.0f, 0.70f };
static vec4_t hangarRuleColor = { 0.55f, 0.62f, 0.66f, 0.35f };
static vec4_t hangarGreenColor = { 0.45f, 1.0f, 0.45f, 1.0f };
static vec4_t hangarMoneyColor = { 1.0f, 0.85f, 0.30f, 1.0f };
static vec4_t hangarButtonColor = { 0.06f, 0.08f, 0.10f, 0.85f };
static vec4_t hangarButtonFocusColor = { 0.30f, 0.15f, 0.04f, 0.90f };
static vec4_t hangarBorderColor = { 0.40f, 0.46f, 0.50f, 1.0f };


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
static void Hangar_Show( hangarButton_t *button, qboolean show ) {
	if ( show ) {
		button->item.generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
	} else {
		button->item.generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
	}
}


/*
=================
Hangar_PlaceButton

moves a button inside the panel (its row depends on the state of the robot)
=================
*/
static void Hangar_PlaceButton( hangarButton_t *button, int y ) {
	button->item.generic.y = y;
	button->item.generic.top = y;
	button->item.generic.bottom = y + button->item.height;
}


/*
=================
Hangar_SetButton
=================
*/
static void Hangar_SetButton( hangarButton_t *button, const char *label, const char *detail, qboolean enabled ) {
	Q_strncpyz( button->label, label, sizeof( button->label ) );
	Q_strncpyz( button->detail, detail, sizeof( button->detail ) );
	button->enabled = enabled;
	Hangar_Show( button, qtrue );
}


/*
=================
Hangar_UpdateButtons
=================
*/
static void Hangar_UpdateButtons( void ) {
	accountRobot_t	*robot = Hangar_SelectedRobot();
	int				now = UI_AccountTime();

	Hangar_SetButton( &hangarMenuInfo.autoRotate,
		trap_Cvar_VariableValue( "ui_hangarAutoRotate" ) ? "AUTO ROTATE: ON" : "AUTO ROTATE: OFF", "", qtrue );

	// rows under the characteristics: upgrades right after them; the shop and the
	// finish button leave a line for the text above them (Hangar_DrawPanel)
	Hangar_PlaceButton( &hangarMenuInfo.upgradeRobot, ROW_FIRST );
	Hangar_PlaceButton( &hangarMenuInfo.upgradeWeapon, ROW_FIRST + BUTTON_H + 6 );
	Hangar_PlaceButton( &hangarMenuInfo.finish, ROW_FIRST + 2 * LINE + 4 );
	Hangar_PlaceButton( &hangarMenuInfo.shopPrev, ROW_FIRST + LINE + 6 );
	Hangar_PlaceButton( &hangarMenuInfo.buy, ROW_FIRST + LINE + 6 );
	Hangar_PlaceButton( &hangarMenuInfo.shopNext, ROW_FIRST + LINE + 6 );

	if ( !robot ) {
		const robotInfo_t	*info = &bg_robots[hangarMenuInfo.shopType];
		Hangar_SetButton( &hangarMenuInfo.buy, va( "BUY  %s CR", Hangar_Number( info->price ) ), "",
			uiAccount.currency >= info->price );
		Hangar_SetButton( &hangarMenuInfo.shopPrev, "<", "", qtrue );
		Hangar_SetButton( &hangarMenuInfo.shopNext, ">", "", qtrue );
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
		Hangar_Show( &hangarMenuInfo.finish, qfalse );
		return;
	}
	Hangar_Show( &hangarMenuInfo.buy, qfalse );
	Hangar_Show( &hangarMenuInfo.shopPrev, qfalse );
	Hangar_Show( &hangarMenuInfo.shopNext, qfalse );

	if ( robot->upgrade != UPGRADE_NONE ) {
		int		cost = BG_SpeedUpCost( robot->upgradeEnd - now );
		Hangar_SetButton( &hangarMenuInfo.finish, "FINISH NOW", va( "%i CR", cost ), uiAccount.currency >= cost );
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
		return;
	}
	Hangar_Show( &hangarMenuInfo.finish, qfalse );

	if ( robot->level < ROBOT_LEVELS ) {
		int		cost = BG_UpgradeCost( qfalse, robot->level );
		Hangar_SetButton( &hangarMenuInfo.upgradeRobot, "UPGRADE ROBOT",
			va( "%i PTS  %s", cost, Hangar_Duration( BG_UpgradeTime( qfalse, robot->level ) ) ), uiAccount.points >= cost );
	} else {
		Hangar_Show( &hangarMenuInfo.upgradeRobot, qfalse );
	}
	if ( robot->weaponLevel < WEAPON_LEVELS ) {
		int		cost = BG_UpgradeCost( qtrue, robot->weaponLevel );
		Hangar_SetButton( &hangarMenuInfo.upgradeWeapon, "UPGRADE WEAPON",
			va( "%i PTS  %s", cost, Hangar_Duration( BG_UpgradeTime( qtrue, robot->weaponLevel ) ) ), uiAccount.points >= cost );
	} else {
		Hangar_Show( &hangarMenuInfo.upgradeWeapon, qfalse );
	}
}


/*
=================
Hangar_Text
=================
*/
static void Hangar_Text( int x, int y, const char *text, int align, float *color ) {
	UI_DrawString( x, y, text, align | UI_TINYFONT, color );
}


/*
=================
Hangar_Rule

thin separator line across the panel
=================
*/
static void Hangar_Rule( int y ) {
	UI_FillRect( PANEL_X + PANEL_PAD, y, PANEL_W - 2 * PANEL_PAD, 1, hangarRuleColor );
}


/*
=================
Hangar_DrawLine

a characteristic: name on the left, value and the gain of the next level on the right
=================
*/
static void Hangar_DrawLine( int y, const char *name, const char *value, const char *next ) {
	Hangar_Text( PANEL_X + PANEL_PAD, y, name, UI_LEFT, hangarDimColor );
	Hangar_Text( PANEL_X + 168, y, value, UI_RIGHT, hangarTextColor );
	if ( next && next[0] ) {
		Hangar_Text( PANEL_X + PANEL_W - PANEL_PAD, y, next, UI_RIGHT, hangarGreenColor );
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

	UI_FillRect( PANEL_X, PANEL_Y, PANEL_W, PANEL_H, hangarPanelColor );
	UI_FillRect( PANEL_X, PANEL_Y, PANEL_W, 2, hangarAccentColor );

	y = PANEL_Y + 8;
	UI_DrawString( PANEL_X + PANEL_PAD, y, info->title, UI_LEFT | UI_SMALLFONT, hangarAccentColor );
	Hangar_Text( PANEL_X + PANEL_W - PANEL_PAD, y + 3, robot ? va( "LEVEL %i / %i", level, ROBOT_LEVELS ) : "IN SHOP",
		UI_RIGHT, hangarAccentColor );
	y += 18;
	Hangar_Text( PANEL_X + PANEL_PAD, y, info->description, UI_LEFT, hangarDimColor );
	y += LINE + 4;
	Hangar_Rule( y - 3 );

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
	y += LINE + 6;

	Hangar_Rule( y - 3 );
	Hangar_Text( PANEL_X + PANEL_PAD, y, weapon->title, UI_LEFT, hangarAccentColor );
	Hangar_Text( PANEL_X + PANEL_W - PANEL_PAD, y, va( "LEVEL %i / %i", weaponLevel, WEAPON_LEVELS ), UI_RIGHT, hangarAccentColor );
	y += LINE + 2;
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
	y += LINE + 6;
	Hangar_Rule( y - 3 );

	// the rows under the characteristics start at ROW_FIRST (Hangar_UpdateButtons)
	y = ROW_FIRST + 2;
	if ( robot && robot->upgrade != UPGRADE_NONE ) {
		Hangar_Text( PANEL_X + PANEL_PAD, y, va( "UPGRADING %s", robot->upgrade == UPGRADE_ROBOT ? "ROBOT" : "WEAPON" ),
			UI_LEFT, color_yellow );
		Hangar_Text( PANEL_X + PANEL_W - PANEL_PAD, y, va( "%s left", Hangar_Duration( robot->upgradeEnd - UI_AccountTime() ) ),
			UI_RIGHT, color_yellow );
		Hangar_Text( PANEL_X + PANEL_PAD, y + LINE, "stays out of battles until done", UI_LEFT, hangarDimColor );
	} else if ( !robot ) {
		Hangar_Text( PANEL_X + PANEL_PAD, y, "Empty bay: choose a robot to buy", UI_LEFT, hangarDimColor );
	}
	if ( hangarMenuInfo.message[0] ) {
		Hangar_Text( PANEL_X + PANEL_W / 2, PANEL_Y + PANEL_H - 14, hangarMenuInfo.message, UI_CENTER, color_red );
	}
}


/*
=================
Hangar_DrawButton
=================
*/
static void Hangar_DrawButton( void *self ) {
	hangarButton_t	*button = (hangarButton_t *)self;
	menucommon_s	*item = &button->item.generic;
	qboolean		focus = ( item->parent->cursor == item->menuPosition );
	float			*color;
	int				textY = item->y + ( button->item.height - TINYFONT_HEIGHT ) / 2;

	UI_FillRect( item->x, item->y, button->item.width, button->item.height, focus ? hangarButtonFocusColor : hangarButtonColor );
	UI_DrawRect( item->x, item->y, button->item.width, button->item.height, focus ? hangarAccentColor : hangarBorderColor );
	if ( !button->enabled ) {
		color = hangarDimColor;
	} else {
		color = focus ? colorWhite : hangarAccentColor;
	}
	if ( button->detail[0] ) {
		Hangar_Text( item->x + 6, textY, button->label, UI_LEFT, color );
		Hangar_Text( item->x + button->item.width - 6, textY, button->detail, UI_RIGHT, button->enabled ? hangarTextColor : hangarDimColor );
	} else {
		Hangar_Text( item->x + button->item.width / 2, textY, button->label, UI_CENTER, color );
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
	UI_DrawPlayer( PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H - 24, player, uis.realtime / 2 );
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

	UI_FillRect( bay->generic.x, bay->generic.y, bay->width, bay->height, hangarPanelColor );
	if ( index < uiAccount.numRobots ) {
		accountRobot_t	*robot = &uiAccount.robots[index];
		Hangar_FaceYaw( &hangarMenuInfo.players[index], hangarMenuInfo.bayYaw + index * 25 );
		UI_DrawPlayer( bay->generic.x + 4, bay->generic.y + 2, bay->width - 8, bay->height - 16, &hangarMenuInfo.players[index], uis.realtime / 2 );
		Com_sprintf( label, sizeof( label ), "%s  LV %i", bg_robots[robot->type].title, robot->level );
		if ( robot->upgrade != UPGRADE_NONE ) {
			Hangar_Text( bay->generic.x + bay->width / 2, bay->generic.y + 4, "UPGRADING", UI_CENTER, color_yellow );
		}
	} else {
		Q_strncpyz( label, "+ BUY", sizeof( label ) );
	}

	UI_DrawHandlePic( bay->generic.x, bay->generic.y, bay->width, bay->height,
		index == hangarMenuInfo.selected ? hangarMenuInfo.baySelectedShader : hangarMenuInfo.bayShader );

	color = ( index == hangarMenuInfo.selected ) ? hangarAccentColor : ( focus ? colorWhite : hangarDimColor );
	Hangar_Text( bay->generic.x + bay->width / 2, bay->generic.y + bay->height - 15, label, UI_CENTER, color );
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

	UI_DrawHandlePic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, hangarMenuInfo.backgroundShader );

	// top bar: pilot, money
	UI_FillRect( 0, 0, SCREEN_WIDTH, TOPBAR_H, hangarBarColor );
	UI_FillRect( 0, TOPBAR_H, SCREEN_WIDTH, 1, hangarRuleColor );
	Hangar_Text( 10, 5, uiAccount.nick, UI_LEFT, hangarTextColor );
	Hangar_Text( 10 + ( strlen( uiAccount.nick ) + 2 ) * TINYFONT_WIDTH, 5, va( "PILOT LEVEL %i", uiAccount.level ), UI_LEFT, hangarDimColor );
	Hangar_Text( 630, 5, va( "%s PTS", Hangar_Number( uiAccount.points ) ), UI_RIGHT, hangarGreenColor );
	Hangar_Text( 630 - 14 * TINYFONT_WIDTH, 5, va( "%s CR", Hangar_Number( uiAccount.currency ) ), UI_RIGHT, hangarMoneyColor );

	Hangar_DrawPanel();

	// bottom bar
	UI_FillRect( 0, BOTTOM_Y, SCREEN_WIDTH, SCREEN_HEIGHT - BOTTOM_Y, hangarBarColor );
	UI_FillRect( 0, BOTTOM_Y, SCREEN_WIDTH, 1, hangarRuleColor );
	Hangar_Text( 630, BOTTOM_Y + 10, va( "%i of %i robots ready for battle", UI_AccountAvailableRobots(), uiAccount.numRobots ),
		UI_RIGHT, hangarDimColor );
	Hangar_Text( 630, BOTTOM_Y + 26, "LMB drag: turn   RMB drag: roll   wheel: zoom", UI_RIGHT, hangarDimColor );

	Menu_Draw( &hangarMenuInfo.menu );
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
			uis.cursory >= PREVIEW_Y && uis.cursory <= PREVIEW_Y + PREVIEW_H - 24 ) {
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
	trap_R_RegisterShaderNoMip( ART_BAY );
	trap_R_RegisterShaderNoMip( ART_BAY_SELECTED );
}


/*
=================
Hangar_InitButton
=================
*/
static void Hangar_InitButton( hangarButton_t *button, int id, int x, int y, int width, int height ) {
	menubitmap_s	*item = &button->item;

	item->generic.type		= MTYPE_BITMAP;
	item->generic.flags		= QMF_NODEFAULTINIT;
	item->generic.id		= id;
	item->generic.callback	= Hangar_Event;
	item->generic.ownerdraw	= Hangar_DrawButton;
	item->generic.x			= x;
	item->generic.y			= y;
	item->width				= width;
	item->height			= height;
	item->generic.left		= x;
	item->generic.top		= y;
	item->generic.right		= x + width;
	item->generic.bottom	= y + height;
	button->enabled			= qtrue;
	Menu_AddItem( &hangarMenuInfo.menu, item );
}


/*
=================
Hangar_Init
=================
*/
static void Hangar_Init( void ) {
	int		index, buttonY, buttonX, buttonW;

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

	// actions at the bottom of the characteristics panel
	buttonX = PANEL_X + PANEL_PAD;
	buttonW = PANEL_W - 2 * PANEL_PAD;
	buttonY = ROW_FIRST;		// rows are set again by Hangar_UpdateButtons
	Hangar_InitButton( &hangarMenuInfo.upgradeRobot, ID_UPGRADE_ROBOT, buttonX, buttonY, buttonW, BUTTON_H );
	Hangar_InitButton( &hangarMenuInfo.upgradeWeapon, ID_UPGRADE_WEAPON, buttonX, buttonY + BUTTON_H + 6, buttonW, BUTTON_H );
	Hangar_InitButton( &hangarMenuInfo.finish, ID_FINISH, buttonX, buttonY + BUTTON_H + 6, buttonW, BUTTON_H );
	Hangar_InitButton( &hangarMenuInfo.shopPrev, ID_SHOP_PREV, buttonX, buttonY + BUTTON_H + 6, 24, BUTTON_H );
	Hangar_InitButton( &hangarMenuInfo.buy, ID_BUY, buttonX + 30, buttonY + BUTTON_H + 6, buttonW - 60, BUTTON_H );
	Hangar_InitButton( &hangarMenuInfo.shopNext, ID_SHOP_NEXT, buttonX + buttonW - 24, buttonY + BUTTON_H + 6, 24, BUTTON_H );

	// preview controls under the robot
	Hangar_InitButton( &hangarMenuInfo.autoRotate, ID_AUTOROTATE, PREVIEW_X + 8, PREVIEW_Y + PREVIEW_H - 20, 116, 16 );
	Hangar_InitButton( &hangarMenuInfo.resetView, ID_RESETVIEW, PREVIEW_X + 130, PREVIEW_Y + PREVIEW_H - 20, 80, 16 );
	Hangar_SetButton( &hangarMenuInfo.resetView, "RESET VIEW", "", qtrue );

	Hangar_InitButton( &hangarMenuInfo.back, ID_BACK, 10, BOTTOM_Y + 12, 96, 24 );
	Hangar_SetButton( &hangarMenuInfo.back, "BACK", "", qtrue );
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
