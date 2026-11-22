/*
===========================================================================
WALKER: effects menu (SETUP -> EFFECTS)

Switches for the expensive parts of the picture, so the game can run on old
hardware. Each item changes a cvar right away; antialiasing, texture filtering
and texture quality take effect after APPLY (vid_restart).

  SHOW FPS            cg_drawFPS
  WATER REFLECTIONS   r_noportals (the water mirror draws the scene twice)
  HAZE                r_drawfog (1: OpenGL fog, almost free; 2: the original fog pass)
  WATER RIPPLES       r_waterRipple 0 / 0.35 / 0.65 / 1
  ROBOT SHADOWS       cg_shadows 0 / 1 (round spot) / 2 (stencil, exact shape)
  DYNAMIC LIGHTS      r_dynamiclight (shots, explosions)
  MARKS ON WALLS      cg_marks
  ANTIALIASING        r_ext_multisample 0 / 2 / 4          (after APPLY)
  TEXTURE FILTER      r_ext_texture_filter_anisotropic     (after APPLY)
  TEXTURE QUALITY     r_picmip 0 / 1 / 2                   (after APPLY)

FAST and QUALITY set all of them at once.
===========================================================================
*/

#include "ui_local.h"

#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"

#define EFFECTS_X			340
#define EFFECTS_Y			96
#define EFFECTS_STEP		22

#define ID_FPS				10
#define ID_REFLECTIONS		11
#define ID_HAZE				12
#define ID_RIPPLES			23
#define ID_SHADOWS			13
#define ID_DLIGHTS			14
#define ID_MARKS			15
#define ID_ANTIALIAS		16
#define ID_FILTER			17
#define ID_PICMIP			18
#define ID_FAST				19
#define ID_QUALITY			20
#define ID_APPLY			21
#define ID_BACK				22

typedef struct {
	menuframework_s		menu;

	menutext_s			banner;
	menulist_s			fps;
	menulist_s			reflections;
	menulist_s			haze;
	menulist_s			ripples;
	menulist_s			shadows;
	menulist_s			dlights;
	menulist_s			marks;
	menulist_s			antialias;
	menulist_s			filter;
	menulist_s			picmip;
	menutext_s			fast;
	menutext_s			quality;
	menutext_s			apply;
	menubitmap_s		back;
} effectsMenuInfo_t;

static effectsMenuInfo_t	effectsMenuInfo;

static const char *offOnNames[] = { "off", "on", NULL };
static const char *rippleNames[] = { "off", "low", "medium", "high", NULL };
static const float	rippleValues[] = { 0, 0.35f, 0.65f, 1.0f };
static const char *shadowNames[] = { "off", "simple", "full", NULL };
static const char *antialiasNames[] = { "off", "2x", "4x", NULL };
static const char *filterNames[] = { "trilinear", "anisotropic", NULL };
static const char *picmipNames[] = { "high", "medium", "low", NULL };

static const int	antialiasSamples[] = { 0, 2, 4 };

/*
=================
Effects_SetMenuItems

current cvar values into the controls
=================
*/
static void Effects_SetMenuItems( void ) {
	int		samples;
	float	ripple;

	effectsMenuInfo.fps.curvalue = trap_Cvar_VariableValue( "cg_drawFPS" ) != 0;
	effectsMenuInfo.reflections.curvalue = trap_Cvar_VariableValue( "r_noportals" ) == 0;
	effectsMenuInfo.haze.curvalue = trap_Cvar_VariableValue( "r_drawfog" ) != 0;
	ripple = trap_Cvar_VariableValue( "r_waterRipple" );
	effectsMenuInfo.ripples.curvalue = ripple < 0.15f ? 0 : ( ripple < 0.5f ? 1 : ( ripple < 0.85f ? 2 : 3 ) );
	effectsMenuInfo.shadows.curvalue = Com_Clamp( 0, 2, trap_Cvar_VariableValue( "cg_shadows" ) );
	effectsMenuInfo.dlights.curvalue = trap_Cvar_VariableValue( "r_dynamiclight" ) != 0;
	effectsMenuInfo.marks.curvalue = trap_Cvar_VariableValue( "cg_marks" ) != 0;
	samples = trap_Cvar_VariableValue( "r_ext_multisample" );
	effectsMenuInfo.antialias.curvalue = samples >= 4 ? 2 : ( samples >= 2 ? 1 : 0 );
	effectsMenuInfo.filter.curvalue = trap_Cvar_VariableValue( "r_ext_texture_filter_anisotropic" ) != 0;
	effectsMenuInfo.picmip.curvalue = Com_Clamp( 0, 2, trap_Cvar_VariableValue( "r_picmip" ) );
}

/*
=================
Effects_StoreItems

controls into the cvars
=================
*/
static void Effects_StoreItems( void ) {
	trap_Cvar_SetValue( "cg_drawFPS", effectsMenuInfo.fps.curvalue );
	trap_Cvar_SetValue( "r_noportals", !effectsMenuInfo.reflections.curvalue );
	trap_Cvar_SetValue( "r_drawfog", effectsMenuInfo.haze.curvalue );
	trap_Cvar_SetValue( "r_waterRipple", rippleValues[effectsMenuInfo.ripples.curvalue] );
	trap_Cvar_SetValue( "cg_shadows", effectsMenuInfo.shadows.curvalue );
	trap_Cvar_SetValue( "r_dynamiclight", effectsMenuInfo.dlights.curvalue );
	trap_Cvar_SetValue( "cg_marks", effectsMenuInfo.marks.curvalue );
	trap_Cvar_SetValue( "r_ext_multisample", antialiasSamples[effectsMenuInfo.antialias.curvalue] );
	trap_Cvar_SetValue( "r_ext_texture_filter_anisotropic", effectsMenuInfo.filter.curvalue );
	trap_Cvar_SetValue( "r_picmip", effectsMenuInfo.picmip.curvalue );
}

/*
=================
Effects_Preset
=================
*/
static void Effects_Preset( qboolean fast ) {
	effectsMenuInfo.fps.curvalue = 1;
	effectsMenuInfo.reflections.curvalue = !fast;
	effectsMenuInfo.haze.curvalue = 1;		// OpenGL fog: almost free
	effectsMenuInfo.ripples.curvalue = 1;
	effectsMenuInfo.shadows.curvalue = fast ? 1 : 2;
	effectsMenuInfo.dlights.curvalue = !fast;
	effectsMenuInfo.marks.curvalue = 1;
	effectsMenuInfo.antialias.curvalue = fast ? 0 : 2;
	effectsMenuInfo.filter.curvalue = 1;
	effectsMenuInfo.picmip.curvalue = 0;
	Effects_StoreItems();
}

/*
=================
Effects_Event
=================
*/
static void Effects_Event( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}

	switch ( ((menucommon_s *)ptr)->id ) {
	case ID_FAST:
		Effects_Preset( qtrue );
		break;
	case ID_QUALITY:
		Effects_Preset( qfalse );
		break;
	case ID_APPLY:
		Effects_StoreItems();
		trap_Cmd_ExecuteText( EXEC_APPEND, "vid_restart\n" );
		break;
	case ID_BACK:
		UI_PopMenu();
		break;
	default:
		Effects_StoreItems();
		break;
	}
}

/*
=================
Effects_InitSpin
=================
*/
static void Effects_InitSpin( menulist_s *item, int id, int y, const char *name, const char **names ) {
	item->generic.type = MTYPE_SPINCONTROL;
	item->generic.flags = QMF_PULSEIFFOCUS | QMF_SMALLFONT;
	item->generic.name = name;
	item->generic.id = id;
	item->generic.callback = Effects_Event;
	item->generic.x = EFFECTS_X;
	item->generic.y = y;
	item->itemnames = names;
}

/*
=================
Effects_InitText
=================
*/
static void Effects_InitText( menutext_s *item, int id, int x, int y, const char *text ) {
	item->generic.type = MTYPE_PTEXT;
	item->generic.flags = QMF_CENTER_JUSTIFY | QMF_PULSEIFFOCUS;
	item->generic.id = id;
	item->generic.callback = Effects_Event;
	item->generic.x = x;
	item->generic.y = y;
	item->string = (char *)text;
	item->color = color_red;
	item->style = UI_CENTER | UI_SMALLFONT;
}

/*
=================
Effects_Draw
=================
*/
static void Effects_Draw( void ) {
	static vec4_t	dimColor = { 0.75f, 0.75f, 0.75f, 1.0f };

	Menu_Draw( &effectsMenuInfo.menu );
	UI_DrawString( 320, 358, "reflections and full shadows cost the most", UI_CENTER | UI_SMALLFONT, dimColor );
	UI_DrawString( 320, 376, "antialiasing, filter and quality: press APPLY", UI_CENTER | UI_SMALLFONT, dimColor );
}

/*
=================
Effects_MenuInit
=================
*/
static void Effects_MenuInit( void ) {
	int		y = EFFECTS_Y;

	memset( &effectsMenuInfo, 0, sizeof( effectsMenuInfo ) );
	UI_Effects_Cache();

	effectsMenuInfo.menu.wrapAround = qtrue;
	effectsMenuInfo.menu.fullscreen = qtrue;
	effectsMenuInfo.menu.draw = Effects_Draw;

	effectsMenuInfo.banner.generic.type = MTYPE_BTEXT;
	effectsMenuInfo.banner.generic.x = 320;
	effectsMenuInfo.banner.generic.y = 16;
	effectsMenuInfo.banner.string = "EFFECTS";
	effectsMenuInfo.banner.color = color_white;
	effectsMenuInfo.banner.style = UI_CENTER;

	Effects_InitSpin( &effectsMenuInfo.fps, ID_FPS, y, "Show FPS:", offOnNames );					y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.reflections, ID_REFLECTIONS, y, "Water Reflections:", offOnNames );	y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.haze, ID_HAZE, y, "Haze:", offOnNames );						y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.ripples, ID_RIPPLES, y, "Water Ripples:", rippleNames );		y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.shadows, ID_SHADOWS, y, "Robot Shadows:", shadowNames );		y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.dlights, ID_DLIGHTS, y, "Dynamic Lights:", offOnNames );		y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.marks, ID_MARKS, y, "Marks On Walls:", offOnNames );			y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.antialias, ID_ANTIALIAS, y, "Antialiasing:", antialiasNames );	y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.filter, ID_FILTER, y, "Texture Filter:", filterNames );		y += EFFECTS_STEP;
	Effects_InitSpin( &effectsMenuInfo.picmip, ID_PICMIP, y, "Texture Quality:", picmipNames );		y += EFFECTS_STEP;

	y += 8;
	Effects_InitText( &effectsMenuInfo.fast, ID_FAST, 200, y, "FAST" );
	Effects_InitText( &effectsMenuInfo.quality, ID_QUALITY, 320, y, "QUALITY" );
	Effects_InitText( &effectsMenuInfo.apply, ID_APPLY, 440, y, "APPLY" );

	effectsMenuInfo.back.generic.type = MTYPE_BITMAP;
	effectsMenuInfo.back.generic.name = ART_BACK0;
	effectsMenuInfo.back.generic.flags = QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
	effectsMenuInfo.back.generic.callback = Effects_Event;
	effectsMenuInfo.back.generic.id = ID_BACK;
	effectsMenuInfo.back.generic.x = 0;
	effectsMenuInfo.back.generic.y = 480 - 64;
	effectsMenuInfo.back.width = 128;
	effectsMenuInfo.back.height = 64;
	effectsMenuInfo.back.focuspic = ART_BACK1;

	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.banner );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.fps );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.reflections );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.haze );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.ripples );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.shadows );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.dlights );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.marks );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.antialias );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.filter );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.picmip );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.fast );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.quality );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.apply );
	Menu_AddItem( &effectsMenuInfo.menu, &effectsMenuInfo.back );

	Effects_SetMenuItems();
}

/*
=================
UI_Effects_Cache
=================
*/
void UI_Effects_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
}

/*
=================
UI_EffectsMenu
=================
*/
void UI_EffectsMenu( void ) {
	Effects_MenuInit();
	UI_PushMenu( &effectsMenuInfo.menu );
}
