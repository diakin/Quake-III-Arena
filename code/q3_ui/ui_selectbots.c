/*
===========================================================================
WALKER: bot selection menu (replaces ADD BOTS and REMOVE BOTS of the in-game menu).

Lists every bot from scripts/bots.txt with its state. A click on a bot that is
not in the game adds it with the chosen skill ("addbot"), a click on a bot in
the game removes it ("clientkick").

While a menu is open in a game with one human, the server is paused, so the
player configstrings (the real state) change only after the menu is closed.
The list therefore shows the requested change at once (JOINING / LEAVING) and
switches to the real state when the server confirms it.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "ui_local.h"

#define ART_BACKGROUND		"menu/art/addbotframe"
#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"

#define MAX_LISTED_BOTS		10
#define LIST_TOP			112
#define LIST_STEP			24
#define NAME_X				176
#define STATE_X				360
#define REQUEST_NONE		0
#define REQUEST_JOIN		1
#define REQUEST_LEAVE		2

#define ID_BACK				10
#define ID_SKILL			11
#define ID_BOT0				20

typedef struct {
	menuframework_s	menu;
	menutext_s		banner;
	menubitmap_s	background;
	menutext_s		bots[MAX_LISTED_BOTS];
	menulist_s		skill;
	menubitmap_s	back;

	int				numBots;
	char			names[MAX_LISTED_BOTS][32];
	int				clientNums[MAX_LISTED_BOTS];	// -1: not in the game
	int				request[MAX_LISTED_BOTS];		// command sent, waiting for the server
} selectBotsMenuInfo_t;

static selectBotsMenuInfo_t	selectBotsMenuInfo;

static const char *selectBotsSkillNames[] = {
	"Recruit",
	"Regular",
	"Veteran",
	"Elite",
	"Ace",
	NULL
};

static vec4_t selectBotsInGameColor = { 0.55f, 0.85f, 0.45f, 1.00f };
static vec4_t selectBotsReserveColor = { 0.45f, 0.50f, 0.54f, 1.00f };


/*
=================
SelectBots_UpdateState

finds which listed bots are in the game and under which client number
=================
*/
static void SelectBots_UpdateState( void ) {
	char	info[MAX_INFO_STRING];
	char	playerName[32];
	int		maxClients;
	int		client;
	int		index;

	trap_GetConfigString( CS_SERVERINFO, info, sizeof( info ) );
	maxClients = atoi( Info_ValueForKey( info, "sv_maxclients" ) );

	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		selectBotsMenuInfo.clientNums[index] = -1;
	}

	for ( client = 0; client < maxClients; client++ ) {
		trap_GetConfigString( CS_PLAYERS + client, info, sizeof( info ) );
		if ( !atoi( Info_ValueForKey( info, "skill" ) ) ) {
			continue;		// empty slot or a human
		}
		Q_strncpyz( playerName, Info_ValueForKey( info, "n" ), sizeof( playerName ) );
		Q_CleanStr( playerName );
		for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
			if ( selectBotsMenuInfo.clientNums[index] < 0 && !Q_stricmp( playerName, selectBotsMenuInfo.names[index] ) ) {
				selectBotsMenuInfo.clientNums[index] = client;
				break;
			}
		}
	}

	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		qboolean inGame = ( selectBotsMenuInfo.clientNums[index] >= 0 );
		// the server has done what was asked
		if ( ( selectBotsMenuInfo.request[index] == REQUEST_JOIN && inGame ) ||
			( selectBotsMenuInfo.request[index] == REQUEST_LEAVE && !inGame ) ) {
			selectBotsMenuInfo.request[index] = REQUEST_NONE;
		}
		if ( selectBotsMenuInfo.request[index] == REQUEST_JOIN ) {
			inGame = qtrue;
		} else if ( selectBotsMenuInfo.request[index] == REQUEST_LEAVE ) {
			inGame = qfalse;
		}
		selectBotsMenuInfo.bots[index].color = inGame ? text_color_normal : selectBotsReserveColor;
	}
}


/*
=================
SelectBots_BotEvent
=================
*/
static void SelectBots_BotEvent( void *ptr, int event ) {
	int		index;

	if ( event != QM_ACTIVATED ) {
		return;
	}
	index = ( (menucommon_s *)ptr )->id - ID_BOT0;
	if ( index < 0 || index >= selectBotsMenuInfo.numBots ) {
		return;
	}
	if ( selectBotsMenuInfo.request[index] != REQUEST_NONE ) {
		return;		// the previous command is still waiting for the server
	}
	if ( selectBotsMenuInfo.clientNums[index] >= 0 ) {
		// WALKER: the player manages bots by hand now; otherwise the server
		// brings a removed bot back to keep bot_minplayers players
		trap_Cvar_SetValue( "bot_minplayers", 0 );
		trap_Cmd_ExecuteText( EXEC_APPEND, va( "clientkick %i\n", selectBotsMenuInfo.clientNums[index] ) );
		selectBotsMenuInfo.request[index] = REQUEST_LEAVE;
	} else {
		trap_Cvar_SetValue( "bot_minplayers", 0 );
		trap_Cmd_ExecuteText( EXEC_APPEND, va( "addbot %s %i\n", selectBotsMenuInfo.names[index], selectBotsMenuInfo.skill.curvalue + 1 ) );
		selectBotsMenuInfo.request[index] = REQUEST_JOIN;
	}
	SelectBots_UpdateState();
}


/*
=================
SelectBots_BackEvent
=================
*/
static void SelectBots_BackEvent( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}
	UI_PopMenu();
}


/*
=================
SelectBots_Draw
=================
*/
static void SelectBots_Draw( void ) {
	int			index;
	int			rowY;
	const char	*state;
	float		*color;

	SelectBots_UpdateState();
	Menu_Draw( &selectBotsMenuInfo.menu );

	UI_DrawProportionalString( NAME_X, LIST_TOP - 28, "PILOT", UI_LEFT | UI_SMALLFONT, color_white );
	UI_DrawProportionalString( STATE_X, LIST_TOP - 28, "STATE", UI_LEFT | UI_SMALLFONT, color_white );

	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		rowY = LIST_TOP + index * LIST_STEP;
		if ( selectBotsMenuInfo.request[index] == REQUEST_JOIN ) {
			state = "JOINING";
			color = color_white;
		} else if ( selectBotsMenuInfo.request[index] == REQUEST_LEAVE ) {
			state = "LEAVING";
			color = color_white;
		} else if ( selectBotsMenuInfo.clientNums[index] >= 0 ) {
			state = "IN BATTLE";
			color = selectBotsInGameColor;
		} else {
			state = "RESERVE";
			color = selectBotsReserveColor;
		}
		UI_DrawProportionalString( STATE_X, rowY, state, UI_LEFT | UI_SMALLFONT, color );
	}

	UI_DrawString( 320, 404, "click a pilot to add it or call it back", UI_CENTER | UI_SMALLFONT, selectBotsReserveColor );
}


/*
=================
UI_SelectBots_Cache
=================
*/
void UI_SelectBots_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_BACKGROUND );
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
}


/*
=================
SelectBots_Init
=================
*/
static void SelectBots_Init( void ) {
	int		index;
	int		skill;
	char	*botInfo;

	memset( &selectBotsMenuInfo, 0, sizeof( selectBotsMenuInfo ) );
	selectBotsMenuInfo.menu.fullscreen = qfalse;
	selectBotsMenuInfo.menu.wrapAround = qtrue;
	selectBotsMenuInfo.menu.draw = SelectBots_Draw;

	UI_SelectBots_Cache();

	selectBotsMenuInfo.numBots = UI_GetNumBots();
	if ( selectBotsMenuInfo.numBots > MAX_LISTED_BOTS ) {
		selectBotsMenuInfo.numBots = MAX_LISTED_BOTS;
	}
	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		botInfo = UI_GetBotInfoByNumber( index );
		Q_strncpyz( selectBotsMenuInfo.names[index], Info_ValueForKey( botInfo, "name" ), sizeof( selectBotsMenuInfo.names[index] ) );
		Q_CleanStr( selectBotsMenuInfo.names[index] );
		selectBotsMenuInfo.clientNums[index] = -1;
	}

	selectBotsMenuInfo.banner.generic.type	= MTYPE_BTEXT;
	selectBotsMenuInfo.banner.generic.x		= 320;
	selectBotsMenuInfo.banner.generic.y		= 16;
	selectBotsMenuInfo.banner.string		= "BOTS";
	selectBotsMenuInfo.banner.color			= color_white;
	selectBotsMenuInfo.banner.style			= UI_CENTER;

	selectBotsMenuInfo.background.generic.type	= MTYPE_BITMAP;
	selectBotsMenuInfo.background.generic.name	= ART_BACKGROUND;
	selectBotsMenuInfo.background.generic.flags	= QMF_INACTIVE;
	selectBotsMenuInfo.background.generic.x		= 320 - 233;
	selectBotsMenuInfo.background.generic.y		= 60;
	selectBotsMenuInfo.background.width			= 466;
	selectBotsMenuInfo.background.height		= 368;

	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		selectBotsMenuInfo.bots[index].generic.type		= MTYPE_PTEXT;
		selectBotsMenuInfo.bots[index].generic.flags	= QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
		selectBotsMenuInfo.bots[index].generic.id		= ID_BOT0 + index;
		selectBotsMenuInfo.bots[index].generic.x		= NAME_X;
		selectBotsMenuInfo.bots[index].generic.y		= LIST_TOP + index * LIST_STEP;
		selectBotsMenuInfo.bots[index].generic.callback	= SelectBots_BotEvent;
		selectBotsMenuInfo.bots[index].string			= selectBotsMenuInfo.names[index];
		selectBotsMenuInfo.bots[index].color			= selectBotsReserveColor;
		selectBotsMenuInfo.bots[index].style			= UI_LEFT | UI_SMALLFONT;
	}

	skill = (int)trap_Cvar_VariableValue( "g_spSkill" ) - 1;
	if ( skill < 0 || skill > 4 ) {
		skill = 1;
	}
	selectBotsMenuInfo.skill.generic.type		= MTYPE_SPINCONTROL;
	selectBotsMenuInfo.skill.generic.name		= "Skill of new bots:";
	selectBotsMenuInfo.skill.generic.flags		= QMF_PULSEIFFOCUS | QMF_SMALLFONT;
	selectBotsMenuInfo.skill.generic.id			= ID_SKILL;
	selectBotsMenuInfo.skill.generic.x			= 330;
	selectBotsMenuInfo.skill.generic.y			= LIST_TOP + MAX_LISTED_BOTS * LIST_STEP - 4;
	selectBotsMenuInfo.skill.itemnames			= selectBotsSkillNames;
	selectBotsMenuInfo.skill.curvalue			= skill;

	selectBotsMenuInfo.back.generic.type		= MTYPE_BITMAP;
	selectBotsMenuInfo.back.generic.name		= ART_BACK0;
	selectBotsMenuInfo.back.generic.flags		= QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
	selectBotsMenuInfo.back.generic.id			= ID_BACK;
	selectBotsMenuInfo.back.generic.callback	= SelectBots_BackEvent;
	selectBotsMenuInfo.back.generic.x			= 320 - 64;
	selectBotsMenuInfo.back.generic.y			= 416;
	selectBotsMenuInfo.back.width				= 128;
	selectBotsMenuInfo.back.height				= 64;
	selectBotsMenuInfo.back.focuspic			= ART_BACK1;

	Menu_AddItem( &selectBotsMenuInfo.menu, &selectBotsMenuInfo.background );
	Menu_AddItem( &selectBotsMenuInfo.menu, &selectBotsMenuInfo.banner );
	for ( index = 0; index < selectBotsMenuInfo.numBots; index++ ) {
		Menu_AddItem( &selectBotsMenuInfo.menu, &selectBotsMenuInfo.bots[index] );
	}
	Menu_AddItem( &selectBotsMenuInfo.menu, &selectBotsMenuInfo.skill );
	Menu_AddItem( &selectBotsMenuInfo.menu, &selectBotsMenuInfo.back );

	SelectBots_UpdateState();
}


/*
=================
UI_SelectBotsMenu
=================
*/
void UI_SelectBotsMenu( void ) {
	SelectBots_Init();
	UI_PushMenu( &selectBotsMenuInfo.menu );
}
