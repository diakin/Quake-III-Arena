/*
===========================================================================
WALKER: hangar (prototype).

Up to five robots stand in bays along the bottom of the screen. The selected
robot is shown large in the middle. It can be turned around every axis: drag
with the left mouse button (left/right - turn, up/down - tilt), drag with the
right button (roll), arrow keys and PgUp/PgDn; HOME or "RESET VIEW" puts it back.
"AUTO ROTATE" switches the slow turn by itself on and off (cvar ui_hangarAutoRotate).
"PILOT" makes the selected robot the player's robot (cvars model/headmodel).

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "ui_local.h"

#define ART_BACKGROUND		"menu/art/hangar"
#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"
#define ART_PILOT0			"menu/art/pilot_0"
#define ART_PILOT1			"menu/art/pilot_1"
#define ART_BAY				"menu/art/maps_select"
#define ART_BAY_SELECTED	"menu/art/maps_selected"

#define MAX_BAYS			5

#define ID_BACK				10
#define ID_PILOT			11
#define ID_AUTOROTATE		12
#define ZOOM_STEP			1.12f	// one wheel notch
#define ZOOM_MIN			0.6f
#define ZOOM_MAX			2.6f
#define ID_RESETVIEW		13
#define ID_BAY0				20

// big preview
#define PREVIEW_X			150
#define PREVIEW_Y			44
#define PREVIEW_W			340
#define PREVIEW_H			282

// bays
#define BAY_Y				334
#define BAY_W				104
#define BAY_H				80
#define BAY_GAP				8

#define TURN_SPEED			24.0f	// degrees per second when nobody turns it
#define DRAG_SPEED			1.2f	// degrees per virtual pixel of mouse travel
#define KEY_STEP			15.0f

typedef struct {
	const char	*title;
	const char	*model;		// model/skin as in the "model" cvar
	int			armor;
	int			speed;
	const char	*weapon;
} hangarRobot_t;

// WALKER: the prototype has one robot model in five paint schemes; later each
// bay can hold a different robot
static const hangarRobot_t hangarRobots[MAX_BAYS] = {
	{ "K7 FOREST",	"walker",			100, 110, "Autocannon" },
	{ "K7 DESERT",	"walker/desert",	100, 110, "Autocannon" },
	{ "K7 URBAN",	"walker/urban",		100, 110, "Autocannon" },
	{ "K7 RED",		"walker/red",		100, 110, "Autocannon" },
	{ "K7 BLUE",	"walker/blue",		100, 110, "Autocannon" },
};

typedef struct {
	menuframework_s	menu;
	menutext_s		banner;
	menubitmap_s	preview;
	menubitmap_s	bays[MAX_BAYS];
	menubitmap_s	back;
	menubitmap_s	pilot;
	menutext_s		autoRotate;
	menutext_s		resetView;

	playerInfo_t	players[MAX_BAYS];
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
} hangarMenuInfo_t;

static hangarMenuInfo_t	hangarMenuInfo;

static vec4_t hangarDimColor = { 0.55f, 0.62f, 0.66f, 1.00f };


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
Hangar_DrawPreview
=================
*/
static void Hangar_DrawPreview( void *self ) {
	const hangarRobot_t	*robot;
	char				line[64];

	robot = &hangarRobots[hangarMenuInfo.selected];
	Hangar_FaceYaw( &hangarMenuInfo.players[hangarMenuInfo.selected], hangarMenuInfo.previewYaw );
	uiPlayerOrbit = qtrue;
	uiPlayerOrbitPitch = hangarMenuInfo.previewPitch;
	uiPlayerOrbitRoll = hangarMenuInfo.previewRoll;
	uiPlayerOrbitZoom = hangarMenuInfo.previewZoom;
	UI_DrawPlayer( PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H, &hangarMenuInfo.players[hangarMenuInfo.selected], uis.realtime / 2 );
	uiPlayerOrbit = qfalse;

	UI_DrawProportionalString( 320, PREVIEW_Y + PREVIEW_H - 44, robot->title, UI_CENTER | UI_SMALLFONT, color_white );
	Com_sprintf( line, sizeof( line ), "ARMOR %i   SPEED %i   %s", robot->armor, robot->speed, robot->weapon );
	UI_DrawString( 320, PREVIEW_Y + PREVIEW_H - 22, line, UI_CENTER | UI_SMALLFONT, hangarDimColor );
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

	bay = (menubitmap_s *)self;
	index = bay->generic.id - ID_BAY0;
	focus = ( bay->generic.parent->cursor == bay->generic.menuPosition );

	UI_FillRect( bay->generic.x, bay->generic.y, bay->width, bay->height, color_black );
	Hangar_FaceYaw( &hangarMenuInfo.players[index], hangarMenuInfo.bayYaw + index * 25 );
	UI_DrawPlayer( bay->generic.x + 4, bay->generic.y + 2, bay->width - 8, bay->height - 18, &hangarMenuInfo.players[index], uis.realtime / 2 );

	UI_DrawHandlePic( bay->generic.x, bay->generic.y, bay->width, bay->height,
		index == hangarMenuInfo.selected ? hangarMenuInfo.baySelectedShader : hangarMenuInfo.bayShader );

	color = ( index == hangarMenuInfo.selected ) ? text_color_normal : ( focus ? text_color_highlight : hangarDimColor );
	UI_DrawString( bay->generic.x + bay->width / 2, bay->generic.y + bay->height - 16, hangarRobots[index].title, UI_CENTER | UI_SMALLFONT, color );
}


/*
=================
Hangar_Draw
=================
*/
static void Hangar_Draw( void ) {
	int		now;
	float	seconds;

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
	Menu_Draw( &hangarMenuInfo.menu );
	UI_DrawString( 320, 450, "drag: left button - turn and tilt, right button - roll", UI_CENTER | UI_SMALLFONT, hangarDimColor );
	UI_DrawString( 320, 466, "wheel: zoom   arrows, PgUp/PgDn: turn   HOME: reset", UI_CENTER | UI_SMALLFONT, hangarDimColor );
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
	case ID_PILOT:
		trap_Cvar_Set( "model", hangarRobots[hangarMenuInfo.selected].model );
		trap_Cvar_Set( "headmodel", hangarRobots[hangarMenuInfo.selected].model );
		trap_Cvar_Set( "team_model", "walker" );
		trap_Cvar_Set( "team_headmodel", "walker" );
		UI_PopMenu();
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
	trap_R_RegisterShaderNoMip( ART_PILOT0 );
	trap_R_RegisterShaderNoMip( ART_PILOT1 );
	trap_R_RegisterShaderNoMip( ART_BAY );
	trap_R_RegisterShaderNoMip( ART_BAY_SELECTED );
}


/*
=================
Hangar_Init
=================
*/
static void Hangar_Init( void ) {
	int		index;
	char	current[MAX_QPATH];
	vec3_t	angles;

	memset( &hangarMenuInfo, 0, sizeof( hangarMenuInfo ) );
	hangarMenuInfo.menu.fullscreen = qtrue;
	hangarMenuInfo.menu.wrapAround = qtrue;
	hangarMenuInfo.menu.draw = Hangar_Draw;
	hangarMenuInfo.menu.key = Hangar_Key;

	UI_Hangar_Cache();
	hangarMenuInfo.backgroundShader = trap_R_RegisterShaderNoMip( ART_BACKGROUND );
	hangarMenuInfo.bayShader = trap_R_RegisterShaderNoMip( ART_BAY );
	hangarMenuInfo.baySelectedShader = trap_R_RegisterShaderNoMip( ART_BAY_SELECTED );

	// start with the robot the player flies now
	trap_Cvar_VariableStringBuffer( "model", current, sizeof( current ) );
	VectorClear( angles );
	angles[YAW] = 150;
	for ( index = 0; index < MAX_BAYS; index++ ) {
		UI_PlayerInfo_SetModel( &hangarMenuInfo.players[index], (char *)hangarRobots[index].model );
		UI_PlayerInfo_SetInfo( &hangarMenuInfo.players[index], LEGS_IDLE, TORSO_STAND, angles, angles, WP_MACHINEGUN, qfalse );
		if ( !Q_stricmp( current, hangarRobots[index].model ) ) {
			hangarMenuInfo.selected = index;
		}
	}
	hangarMenuInfo.previewYaw = 150;
	hangarMenuInfo.previewZoom = 1;

	hangarMenuInfo.banner.generic.type	= MTYPE_BTEXT;
	hangarMenuInfo.banner.generic.x		= 320;
	hangarMenuInfo.banner.generic.y		= 10;
	hangarMenuInfo.banner.string		= "HANGAR";
	hangarMenuInfo.banner.color			= color_white;
	hangarMenuInfo.banner.style			= UI_CENTER;

	hangarMenuInfo.preview.generic.type		= MTYPE_BITMAP;
	hangarMenuInfo.preview.generic.flags	= QMF_INACTIVE;
	hangarMenuInfo.preview.generic.ownerdraw = Hangar_DrawPreview;
	hangarMenuInfo.preview.generic.x		= PREVIEW_X;
	hangarMenuInfo.preview.generic.y		= PREVIEW_Y;
	hangarMenuInfo.preview.width			= PREVIEW_W;
	hangarMenuInfo.preview.height			= PREVIEW_H;

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
	}

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

	hangarMenuInfo.pilot.generic.type		= MTYPE_BITMAP;
	hangarMenuInfo.pilot.generic.name		= ART_PILOT0;
	hangarMenuInfo.pilot.generic.flags		= QMF_RIGHT_JUSTIFY | QMF_PULSEIFFOCUS;
	hangarMenuInfo.pilot.generic.id			= ID_PILOT;
	hangarMenuInfo.pilot.generic.callback	= Hangar_Event;
	hangarMenuInfo.pilot.generic.x			= 640;
	hangarMenuInfo.pilot.generic.y			= 480 - 64;
	hangarMenuInfo.pilot.width				= 128;
	hangarMenuInfo.pilot.height				= 64;
	hangarMenuInfo.pilot.focuspic			= ART_PILOT1;

	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.banner );
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.preview );
	for ( index = 0; index < MAX_BAYS; index++ ) {
		Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.bays[index] );
	}
	hangarMenuInfo.autoRotate.generic.type		= MTYPE_PTEXT;
	hangarMenuInfo.autoRotate.generic.flags		= QMF_RIGHT_JUSTIFY | QMF_PULSEIFFOCUS;
	hangarMenuInfo.autoRotate.generic.id		= ID_AUTOROTATE;
	hangarMenuInfo.autoRotate.generic.callback	= Hangar_Event;
	hangarMenuInfo.autoRotate.generic.x			= 632;
	hangarMenuInfo.autoRotate.generic.y			= 60;
	hangarMenuInfo.autoRotate.string			= "AUTO ROTATE: ON";
	hangarMenuInfo.autoRotate.style				= UI_RIGHT | UI_SMALLFONT;
	hangarMenuInfo.autoRotate.color				= color_red;

	hangarMenuInfo.resetView.generic.type		= MTYPE_PTEXT;
	hangarMenuInfo.resetView.generic.flags		= QMF_RIGHT_JUSTIFY | QMF_PULSEIFFOCUS;
	hangarMenuInfo.resetView.generic.id			= ID_RESETVIEW;
	hangarMenuInfo.resetView.generic.callback	= Hangar_Event;
	hangarMenuInfo.resetView.generic.x			= 632;
	hangarMenuInfo.resetView.generic.y			= 84;
	hangarMenuInfo.resetView.string				= "RESET VIEW";
	hangarMenuInfo.resetView.style				= UI_RIGHT | UI_SMALLFONT;
	hangarMenuInfo.resetView.color				= color_red;

	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.back );
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.pilot );
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.autoRotate );
	Menu_AddItem( &hangarMenuInfo.menu, &hangarMenuInfo.resetView );
}


/*
=================
UI_HangarMenu
=================
*/
void UI_HangarMenu( void ) {
	Hangar_Init();
	UI_PushMenu( &hangarMenuInfo.menu );
}
