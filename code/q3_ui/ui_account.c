/*
===========================================================================
WALKER: pilot accounts.

An account keeps the pilot: nick, level and experience, currency, development
points and the robots of the hangar with their levels and upgrades. It is a text
file accounts/<nick>.acc in the game folder of the player, saved after every
change. The game is told what to take into a battle by userinfo cvars:
walker_robots ("k7.3.2,strizh.1.1": type.level.weapon level of every robot that
is not being upgraded) and walker_level. After a battle cgame puts the reward
into walker_reward and UI_AccountUpdate adds it to the account.

The menu here lists the accounts and creates and deletes them; it comes up at
the start of the game until an account is chosen.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "ui_local.h"

#define ACCOUNT_DIR			"accounts"
#define ACCOUNT_EXT			"acc"
#define ACCOUNT_VERSION		1
#define START_CURRENCY		1500

account_t	uiAccount;


/*
=================
UI_AccountTime
=================
*/
int UI_AccountTime( void ) {
	return trap_RealTime( NULL );
}


/*
=================
Account_FileName

letters, digits, '-' and '_' of the nick; the rest become '_'
=================
*/
static void Account_FileName( const char *nick, char *path, int size ) {
	char	name[32];
	int		index;

	for ( index = 0; nick[index] && index < (int)sizeof( name ) - 1; index++ ) {
		char	letter = nick[index];
		if ( ( letter >= 'a' && letter <= 'z' ) || ( letter >= 'A' && letter <= 'Z' ) ||
			( letter >= '0' && letter <= '9' ) || letter == '-' || letter == '_' ) {
			name[index] = letter;
		} else {
			name[index] = '_';
		}
	}
	name[index] = 0;
	Com_sprintf( path, size, "%s/%s.%s", ACCOUNT_DIR, name, ACCOUNT_EXT );
}


/*
=================
Account_New
=================
*/
static void Account_New( account_t *account, const char *nick ) {
	memset( account, 0, sizeof( *account ) );
	Q_strncpyz( account->nick, nick, sizeof( account->nick ) );
	account->level = 1;
	account->currency = START_CURRENCY;
	account->numRobots = 1;
	account->robots[0].type = ROBOT_K7;
	account->robots[0].level = 1;
	account->robots[0].weaponLevel = 1;
	account->loaded = qtrue;
}


/*
=================
Account_Read

qfalse: no such file, a deleted account or not an account file
=================
*/
static qboolean Account_Read( const char *path, account_t *account ) {
	fileHandle_t	file;
	int				length;
	char			text[4096];
	char			*cursor, *token;

	length = trap_FS_FOpenFile( path, &file, FS_READ );
	if ( length <= 0 || length >= (int)sizeof( text ) ) {
		if ( length > 0 ) {
			trap_FS_FCloseFile( file );
		}
		return qfalse;
	}
	trap_FS_Read( text, length, file );
	trap_FS_FCloseFile( file );
	text[length] = 0;

	memset( account, 0, sizeof( *account ) );
	cursor = text;
	token = COM_Parse( &cursor );
	if ( Q_stricmp( token, "walker_account" ) ) {
		return qfalse;
	}
	token = COM_Parse( &cursor );
	if ( !Q_stricmp( token, "deleted" ) ) {
		return qfalse;
	}
	account->level = 1;
	while ( 1 ) {
		token = COM_Parse( &cursor );
		if ( !token[0] ) {
			break;
		}
		if ( !Q_stricmp( token, "nick" ) ) {
			Q_strncpyz( account->nick, COM_Parse( &cursor ), sizeof( account->nick ) );
		} else if ( !Q_stricmp( token, "level" ) ) {
			account->level = atoi( COM_Parse( &cursor ) );
		} else if ( !Q_stricmp( token, "xp" ) ) {
			account->xp = atoi( COM_Parse( &cursor ) );
		} else if ( !Q_stricmp( token, "currency" ) ) {
			account->currency = atoi( COM_Parse( &cursor ) );
		} else if ( !Q_stricmp( token, "points" ) ) {
			account->points = atoi( COM_Parse( &cursor ) );
		} else if ( !Q_stricmp( token, "lastReward" ) ) {
			Q_strncpyz( account->lastReward, COM_Parse( &cursor ), sizeof( account->lastReward ) );
		} else if ( !Q_stricmp( token, "robot" ) ) {
			accountRobot_t	*robot;
			int				type = BG_RobotByName( COM_Parse( &cursor ) );

			robot = &account->robots[account->numRobots];
			robot->level = atoi( COM_Parse( &cursor ) );
			robot->weaponLevel = atoi( COM_Parse( &cursor ) );
			robot->upgrade = atoi( COM_Parse( &cursor ) );
			robot->upgradeEnd = atoi( COM_Parse( &cursor ) );
			if ( type >= 0 && account->numRobots < ROBOT_HANGAR_SIZE ) {
				robot->type = type;
				if ( robot->level < 1 || robot->level > ROBOT_LEVELS ) {
					robot->level = 1;
				}
				if ( robot->weaponLevel < 1 || robot->weaponLevel > WEAPON_LEVELS ) {
					robot->weaponLevel = 1;
				}
				account->numRobots++;
			}
		}
	}
	if ( !account->nick[0] ) {
		return qfalse;
	}
	if ( account->level < 1 ) {
		account->level = 1;
	}
	account->loaded = qtrue;
	return qtrue;
}


/*
=================
Account_Write
=================
*/
static void Account_Write( const account_t *account ) {
	fileHandle_t	file;
	char			path[MAX_QPATH];
	char			text[4096];
	int				index;

	Account_FileName( account->nick, path, sizeof( path ) );
	Com_sprintf( text, sizeof( text ),
		"walker_account %i\n"
		"nick \"%s\"\n"
		"level %i\n"
		"xp %i\n"
		"currency %i\n"
		"points %i\n"
		"lastReward \"%s\"\n",
		ACCOUNT_VERSION, account->nick, account->level, account->xp, account->currency, account->points,
		account->lastReward );
	for ( index = 0; index < account->numRobots; index++ ) {
		const accountRobot_t	*robot = &account->robots[index];
		Q_strcat( text, sizeof( text ), va( "robot %s %i %i %i %i\n", bg_robots[robot->type].id,
			robot->level, robot->weaponLevel, robot->upgrade, robot->upgradeEnd ) );
	}
	if ( trap_FS_FOpenFile( path, &file, FS_WRITE ) < 0 || !file ) {
		Com_Printf( "^1Could not save the account %s\n", path );
		return;
	}
	trap_FS_Write( text, strlen( text ), file );
	trap_FS_FCloseFile( file );
}


/*
=================
UI_AccountLoad
=================
*/
qboolean UI_AccountLoad( const char *nick ) {
	char		path[MAX_QPATH];
	account_t	account;

	if ( !nick || !nick[0] ) {
		return qfalse;
	}
	Account_FileName( nick, path, sizeof( path ) );
	if ( !Account_Read( path, &account ) ) {
		return qfalse;
	}
	uiAccount = account;
	trap_Cvar_Set( "ui_account", uiAccount.nick );
	trap_Cvar_Set( "name", uiAccount.nick );
	UI_AccountUpdate();
	return qtrue;
}


/*
=================
UI_AccountSave
=================
*/
void UI_AccountSave( void ) {
	if ( uiAccount.loaded ) {
		Account_Write( &uiAccount );
	}
}


/*
=================
UI_AccountAvailableRobots

robots that are not being upgraded: they go into a battle
=================
*/
int UI_AccountAvailableRobots( void ) {
	int		index, count = 0;

	for ( index = 0; index < uiAccount.numRobots; index++ ) {
		if ( uiAccount.robots[index].upgrade == UPGRADE_NONE ) {
			count++;
		}
	}
	return count;
}


/*
=================
Account_TakeReward

walker_reward: "<battle id> <points> <currency> <xp> <kills> <damage> <captures> <won>"
=================
*/
static qboolean Account_TakeReward( void ) {
	char	text[256];
	char	*cursor, *token;
	char	battle[32];
	int		points, currency, xp, kills, damage, captures, won;

	trap_Cvar_VariableStringBuffer( "walker_reward", text, sizeof( text ) );
	if ( !text[0] ) {
		return qfalse;
	}
	trap_Cvar_Set( "walker_reward", "" );
	cursor = text;
	token = COM_Parse( &cursor );
	Q_strncpyz( battle, token, sizeof( battle ) );
	if ( !battle[0] || !Q_stricmp( battle, uiAccount.lastReward ) ) {
		return qfalse;		// already taken
	}
	points = atoi( COM_Parse( &cursor ) );
	currency = atoi( COM_Parse( &cursor ) );
	xp = atoi( COM_Parse( &cursor ) );
	kills = atoi( COM_Parse( &cursor ) );
	damage = atoi( COM_Parse( &cursor ) );
	captures = atoi( COM_Parse( &cursor ) );
	won = atoi( COM_Parse( &cursor ) );

	Q_strncpyz( uiAccount.lastReward, battle, sizeof( uiAccount.lastReward ) );
	uiAccount.points += points;
	uiAccount.currency += currency;
	uiAccount.xp += xp;
	while ( uiAccount.xp >= BG_PlayerLevelXP( uiAccount.level ) ) {
		uiAccount.xp -= BG_PlayerLevelXP( uiAccount.level );
		uiAccount.level++;
	}
	Com_sprintf( uiAccount.lastBattle, sizeof( uiAccount.lastBattle ),
		"%s: %i kills, %i damage, %i beacons  ->  +%i points, +%i credits, +%i xp",
		won ? "VICTORY" : "DEFEAT", kills, damage, captures, points, currency, xp );
	return qtrue;
}


/*
=================
UI_AccountUpdate

finishes the upgrades whose time is up, takes the reward of the last battle,
saves and tells the game what the player takes into a battle
=================
*/
void UI_AccountUpdate( void ) {
	int			index, now, total;
	qboolean	changed;
	char		robots[MAX_INFO_VALUE];

	if ( !uiAccount.loaded ) {
		return;
	}
	now = UI_AccountTime();
	changed = Account_TakeReward();
	for ( index = 0; index < uiAccount.numRobots; index++ ) {
		accountRobot_t	*robot = &uiAccount.robots[index];
		if ( robot->upgrade != UPGRADE_NONE && now >= robot->upgradeEnd ) {
			if ( robot->upgrade == UPGRADE_ROBOT && robot->level < ROBOT_LEVELS ) {
				robot->level++;
			} else if ( robot->upgrade == UPGRADE_WEAPON && robot->weaponLevel < WEAPON_LEVELS ) {
				robot->weaponLevel++;
			}
			robot->upgrade = UPGRADE_NONE;
			robot->upgradeEnd = 0;
			changed = qtrue;
		}
	}
	if ( changed ) {
		UI_AccountSave();
	}

	// what goes into a battle; the bots are about as strong as these robots
	robots[0] = 0;
	total = 0;
	for ( index = 0; index < uiAccount.numRobots; index++ ) {
		accountRobot_t	*robot = &uiAccount.robots[index];
		if ( robot->upgrade != UPGRADE_NONE ) {
			continue;
		}
		Q_strcat( robots, sizeof( robots ), va( "%s%s.%i.%i", robots[0] ? "," : "",
			bg_robots[robot->type].id, robot->level, robot->weaponLevel ) );
		total += robot->level;
	}
	trap_Cvar_Set( "walker_robots", robots );
	trap_Cvar_Set( "walker_level", va( "%i", uiAccount.level ) );
	if ( UI_AccountAvailableRobots() ) {
		int		average = ( total + UI_AccountAvailableRobots() / 2 ) / UI_AccountAvailableRobots();
		trap_Cvar_Set( "g_botLevel", va( "%i", average ) );
		trap_Cvar_Set( "g_botRobots", va( "%i", UI_AccountAvailableRobots() ) );
	}
}


/*
=================
UI_AccountEnsure

at the start: the account of the last time, or the menu to choose one
=================
*/
void UI_AccountEnsure( void ) {
	char	nick[64];

	if ( uiAccount.loaded ) {
		UI_AccountUpdate();
		return;
	}
	trap_Cvar_VariableStringBuffer( "ui_account", nick, sizeof( nick ) );
	if ( UI_AccountLoad( nick ) ) {
		return;
	}
	UI_AccountMenu();
}


/*
===============================================================================

ACCOUNT MENU

===============================================================================
*/

#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"

#define ID_ENTRY0			100
#define ID_PLAY				10
#define ID_DELETE			11
#define ID_CREATE			12
#define ID_BACK				13
#define ID_NAME				14

#define LIST_Y				86
#define LIST_STEP			22

typedef struct {
	menuframework_s	menu;
	menutext_s		banner;
	menutext_s		entries[MAX_ACCOUNTS];
	menufield_s		name;
	menutext_s		create;
	menutext_s		play;
	menutext_s		remove;
	menubitmap_s	back;

	int				count;
	int				selected;
	account_t		accounts[MAX_ACCOUNTS];
	char			labels[MAX_ACCOUNTS][64];
	char			playLabel[64];
	char			message[128];
} accountMenu_t;

static accountMenu_t	accountMenu;

static void AccountMenu_Fill( void );


/*
=================
AccountMenu_Choose
=================
*/
static void AccountMenu_Choose( int index ) {
	if ( index < 0 || index >= accountMenu.count ) {
		return;
	}
	if ( UI_AccountLoad( accountMenu.accounts[index].nick ) ) {
		UI_PopMenu();
	}
}


/*
=================
AccountMenu_DeleteAction
=================
*/
static void AccountMenu_DeleteAction( qboolean result ) {
	fileHandle_t	file;
	char			path[MAX_QPATH];
	const char		*marker = "walker_account deleted\n";
	account_t		*account;

	if ( !result || accountMenu.selected < 0 || accountMenu.selected >= accountMenu.count ) {
		return;
	}
	account = &accountMenu.accounts[accountMenu.selected];
	// the game cannot remove files: the account is marked deleted and no longer listed
	Account_FileName( account->nick, path, sizeof( path ) );
	if ( trap_FS_FOpenFile( path, &file, FS_WRITE ) >= 0 && file ) {
		trap_FS_Write( marker, strlen( marker ), file );
		trap_FS_FCloseFile( file );
	}
	if ( uiAccount.loaded && !Q_stricmp( uiAccount.nick, account->nick ) ) {
		memset( &uiAccount, 0, sizeof( uiAccount ) );
		trap_Cvar_Set( "ui_account", "" );
	}
	AccountMenu_Fill();
}


/*
=================
AccountMenu_Create
=================
*/
static void AccountMenu_Create( void ) {
	char		nick[32];
	char		path[MAX_QPATH];
	account_t	existing;
	int			index, start;

	// trim spaces
	Q_strncpyz( nick, accountMenu.name.field.buffer, sizeof( nick ) );
	for ( start = 0; nick[start] == ' '; start++ ) {
	}
	for ( index = strlen( nick ) - 1; index >= start && nick[index] == ' '; index-- ) {
		nick[index] = 0;
	}
	if ( !nick[start] ) {
		Q_strncpyz( accountMenu.message, "Type a name for the new pilot", sizeof( accountMenu.message ) );
		return;
	}
	Account_FileName( nick + start, path, sizeof( path ) );
	if ( Account_Read( path, &existing ) ) {
		Q_strncpyz( accountMenu.message, "A pilot with this name already exists", sizeof( accountMenu.message ) );
		return;
	}
	Account_New( &uiAccount, nick + start );
	UI_AccountSave();
	trap_Cvar_Set( "ui_account", uiAccount.nick );
	trap_Cvar_Set( "name", uiAccount.nick );
	UI_AccountUpdate();
	UI_PopMenu();
}


/*
=================
AccountMenu_Event
=================
*/
static void AccountMenu_Event( void *ptr, int event ) {
	int		id;

	if ( event != QM_ACTIVATED ) {
		return;
	}
	id = ( (menucommon_s *)ptr )->id;
	if ( id >= ID_ENTRY0 && id < ID_ENTRY0 + MAX_ACCOUNTS ) {
		accountMenu.selected = id - ID_ENTRY0;
		AccountMenu_Fill();
		return;
	}
	switch ( id ) {
	case ID_PLAY:
		AccountMenu_Choose( accountMenu.selected );
		break;
	case ID_DELETE:
		if ( accountMenu.selected >= 0 && accountMenu.selected < accountMenu.count ) {
			UI_ConfirmMenu( va( "DELETE %s?", accountMenu.accounts[accountMenu.selected].nick ), NULL, AccountMenu_DeleteAction );
		}
		break;
	case ID_CREATE:
		AccountMenu_Create();
		break;
	case ID_BACK:
		if ( uiAccount.loaded ) {
			UI_PopMenu();
		}
		break;
	}
}


/*
=================
AccountMenu_Fill

reads the accounts from the folder and updates the list
=================
*/
static void AccountMenu_Fill( void ) {
	char	list[4096];
	char	*name;
	char	path[MAX_QPATH];
	int		files, index, length;

	accountMenu.count = 0;
	files = trap_FS_GetFileList( ACCOUNT_DIR, ACCOUNT_EXT, list, sizeof( list ) );
	name = list;
	for ( index = 0; index < files && accountMenu.count < MAX_ACCOUNTS; index++ ) {
		length = strlen( name );
		Com_sprintf( path, sizeof( path ), "%s/%s", ACCOUNT_DIR, name );
		if ( Account_Read( path, &accountMenu.accounts[accountMenu.count] ) ) {
			accountMenu.count++;
		}
		name += length + 1;
	}
	if ( accountMenu.selected >= accountMenu.count ) {
		accountMenu.selected = accountMenu.count - 1;
	}
	if ( accountMenu.selected < 0 && accountMenu.count ) {
		accountMenu.selected = 0;
	}

	for ( index = 0; index < MAX_ACCOUNTS; index++ ) {
		if ( index < accountMenu.count ) {
			account_t	*account = &accountMenu.accounts[index];
			Com_sprintf( accountMenu.labels[index], sizeof( accountMenu.labels[index] ), "%s   LV %i   %i ROBOT%s",
				account->nick, account->level, account->numRobots, account->numRobots == 1 ? "" : "S" );
			accountMenu.entries[index].string = accountMenu.labels[index];
			accountMenu.entries[index].color = ( index == accountMenu.selected ) ? color_orange : color_white;
			accountMenu.entries[index].generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
		} else {
			accountMenu.entries[index].string = "";
			accountMenu.entries[index].generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
		}
	}
	if ( accountMenu.selected >= 0 ) {
		Com_sprintf( accountMenu.playLabel, sizeof( accountMenu.playLabel ), "FLY AS %s", accountMenu.accounts[accountMenu.selected].nick );
		accountMenu.play.generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
		accountMenu.remove.generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
	} else {
		accountMenu.playLabel[0] = 0;
		accountMenu.play.generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
		accountMenu.remove.generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
	}
	accountMenu.play.string = accountMenu.playLabel;
	if ( uiAccount.loaded ) {
		accountMenu.back.generic.flags &= ~( QMF_HIDDEN | QMF_INACTIVE );
	} else {
		accountMenu.back.generic.flags |= QMF_HIDDEN | QMF_INACTIVE;
	}
}


/*
=================
AccountMenu_Draw
=================
*/
static void AccountMenu_Draw( void ) {
	UI_DrawNamedPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, "menu/art/hangar" );
	UI_FillRect( 120, LIST_Y - 10, 400, ( accountMenu.count > 0 ? accountMenu.count : 2 ) * LIST_STEP + 16, colorMdGrey );
	if ( !accountMenu.count ) {
		UI_DrawString( 320, LIST_Y + 20, "No pilots yet: type a name below and press CREATE", UI_CENTER | UI_SMALLFONT, color_white );
	}
	Menu_Draw( &accountMenu.menu );
	if ( accountMenu.message[0] ) {
		UI_DrawString( 320, 410, accountMenu.message, UI_CENTER | UI_SMALLFONT, color_red );
	}
}


/*
=================
AccountMenu_Key
=================
*/
static sfxHandle_t AccountMenu_Key( int key ) {
	if ( ( key == K_ESCAPE || key == K_MOUSE2 ) && !uiAccount.loaded ) {
		return 0;		// an account must be chosen first
	}
	if ( ( key == K_ENTER || key == K_KP_ENTER ) && accountMenu.menu.cursor == accountMenu.name.generic.menuPosition ) {
		AccountMenu_Create();
		return 0;
	}
	return Menu_DefaultKey( &accountMenu.menu, key );
}


/*
=================
UI_AccountMenu
=================
*/
void UI_AccountMenu( void ) {
	int		index;

	memset( &accountMenu, 0, sizeof( accountMenu ) );
	accountMenu.menu.fullscreen = qtrue;
	accountMenu.menu.wrapAround = qtrue;
	accountMenu.menu.draw = AccountMenu_Draw;
	accountMenu.menu.key = AccountMenu_Key;
	accountMenu.selected = -1;

	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );

	accountMenu.banner.generic.type		= MTYPE_BTEXT;
	accountMenu.banner.generic.x		= 320;
	accountMenu.banner.generic.y		= 16;
	accountMenu.banner.string			= "PILOTS";
	accountMenu.banner.color			= color_white;
	accountMenu.banner.style			= UI_CENTER;
	Menu_AddItem( &accountMenu.menu, &accountMenu.banner );

	for ( index = 0; index < MAX_ACCOUNTS; index++ ) {
		accountMenu.entries[index].generic.type		= MTYPE_PTEXT;
		accountMenu.entries[index].generic.flags	= QMF_CENTER_JUSTIFY | QMF_PULSEIFFOCUS;
		accountMenu.entries[index].generic.id		= ID_ENTRY0 + index;
		accountMenu.entries[index].generic.callback	= AccountMenu_Event;
		accountMenu.entries[index].generic.x		= 320;
		accountMenu.entries[index].generic.y		= LIST_Y + index * LIST_STEP;
		accountMenu.entries[index].style			= UI_CENTER | UI_SMALLFONT;
		accountMenu.entries[index].color			= color_white;
		accountMenu.entries[index].string			= "";
		Menu_AddItem( &accountMenu.menu, &accountMenu.entries[index] );
	}

	accountMenu.name.generic.type		= MTYPE_FIELD;
	accountMenu.name.generic.name		= "New pilot:";
	accountMenu.name.generic.flags		= QMF_NODEFAULTINIT;
	accountMenu.name.generic.id			= ID_NAME;
	accountMenu.name.generic.x			= 250;
	accountMenu.name.generic.y			= 440;
	accountMenu.name.generic.left		= 250 - 100;
	accountMenu.name.generic.top		= 440 - 2;
	accountMenu.name.generic.right		= 250 + 200;
	accountMenu.name.generic.bottom		= 440 + 18;
	accountMenu.name.field.widthInChars	= 16;
	accountMenu.name.field.maxchars		= 20;
	Menu_AddItem( &accountMenu.menu, &accountMenu.name );

	accountMenu.create.generic.type		= MTYPE_PTEXT;
	accountMenu.create.generic.flags	= QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
	accountMenu.create.generic.id		= ID_CREATE;
	accountMenu.create.generic.callback	= AccountMenu_Event;
	accountMenu.create.generic.x		= 470;
	accountMenu.create.generic.y		= 436;
	accountMenu.create.string			= "CREATE";
	accountMenu.create.style			= UI_LEFT | UI_SMALLFONT;
	accountMenu.create.color			= color_red;
	Menu_AddItem( &accountMenu.menu, &accountMenu.create );

	accountMenu.play.generic.type		= MTYPE_PTEXT;
	accountMenu.play.generic.flags		= QMF_CENTER_JUSTIFY | QMF_PULSEIFFOCUS;
	accountMenu.play.generic.id			= ID_PLAY;
	accountMenu.play.generic.callback	= AccountMenu_Event;
	accountMenu.play.generic.x			= 320;
	accountMenu.play.generic.y			= 380;
	accountMenu.play.style				= UI_CENTER;
	accountMenu.play.color				= color_red;
	Menu_AddItem( &accountMenu.menu, &accountMenu.play );

	accountMenu.remove.generic.type		= MTYPE_PTEXT;
	accountMenu.remove.generic.flags	= QMF_RIGHT_JUSTIFY | QMF_PULSEIFFOCUS;
	accountMenu.remove.generic.id		= ID_DELETE;
	accountMenu.remove.generic.callback	= AccountMenu_Event;
	accountMenu.remove.generic.x		= 632;
	accountMenu.remove.generic.y		= 20;
	accountMenu.remove.string			= "DELETE PILOT";
	accountMenu.remove.style			= UI_RIGHT | UI_SMALLFONT;
	accountMenu.remove.color			= color_red;
	Menu_AddItem( &accountMenu.menu, &accountMenu.remove );

	accountMenu.back.generic.type		= MTYPE_BITMAP;
	accountMenu.back.generic.name		= ART_BACK0;
	accountMenu.back.generic.flags		= QMF_LEFT_JUSTIFY | QMF_PULSEIFFOCUS;
	accountMenu.back.generic.id			= ID_BACK;
	accountMenu.back.generic.callback	= AccountMenu_Event;
	accountMenu.back.generic.x			= 0;
	accountMenu.back.generic.y			= 480 - 64;
	accountMenu.back.width				= 128;
	accountMenu.back.height				= 64;
	accountMenu.back.focuspic			= ART_BACK1;
	Menu_AddItem( &accountMenu.menu, &accountMenu.back );

	AccountMenu_Fill();
	UI_PushMenu( &accountMenu.menu );
	if ( !accountMenu.count ) {
		Menu_SetCursorToItem( &accountMenu.menu, &accountMenu.name );
	}
}
