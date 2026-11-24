/*
===========================================================================
WALKER: balance menu for tests.

All robots and weapons in one column: a parameter, an edit field with the new
value and the stock value; the robot and the weapons of the player in the battle
are marked. Up/Down (Tab) choose a field, typing replaces its
value, Backspace edits it, Enter applies. ESC and BACK apply too and close.
The values go to the cvars bal_<id> (bg_robots.c), which the server sends to the
clients; applies at once to all robots in the battle, bots included.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "ui_local.h"

#define BALANCE_FILE	"balance.cfg"	// in basewalker; written on every change, read at start

#define ID_APPLY		10
#define ID_RESET		11
#define ID_BACK			12

#define INFO_ROWS		2				// under each weapon: time to destroy
#define MAX_ROWS		( 3 + CALC_COUNT + ROBOT_COUNT * ( RPARAM_COUNT + 1 ) + RWEAPON_COUNT * ( WPARAM_COUNT + 1 + INFO_ROWS ) )
#define TEXT_SIZE		16

#define TOPBAR_H		22
#define HEADER_Y		32
#define LIST_X			120
#define LIST_W			400
#define LIST_Y			48
#define ROW_H			14
#define VISIBLE_ROWS	26
#define LABEL_X			( LIST_X + 24 )
#define FIELD_X			( LIST_X + 170 )
#define FIELD_W			96
#define OLD_X			( LIST_X + 290 )
#define BOTTOM_Y		( LIST_Y + VISIBLE_ROWS * ROW_H + 6 )

typedef enum {
	ROW_SECTION,	// CALCULATOR, ROBOTS, WEAPONS (param)
	ROW_OBJECT,		// a robot or a weapon
	ROW_VALUE,
	ROW_INFO		// computed for a weapon, read only (param: line)
} rowKind_t;

// calculator: the time a weapon needs to destroy a structure, and the damage per shot
// for the norm time; kept in the cvar bal_calc "<structure> <seconds>"
typedef enum {
	CALC_NONE,
	CALC_TARGET,
	CALC_NORM,
	CALC_COUNT = CALC_NORM
} calcParam_t;

static int		calcTarget;		// structure
static int		calcNorm;		// seconds

typedef struct {
	int		kind;
	qboolean weapon;
	int		index;
	int		param;
	int		calc;					// calcParam_t of a calculator row
	char	text[TEXT_SIZE];		// in the field
	char	loaded[TEXT_SIZE];		// as it was put in the field
} balanceRow_t;

typedef struct {
	menuframework_s	menu;
	menutext_s		apply;
	menutext_s		reset;
	menutext_s		back;

	balanceRow_t	rows[MAX_ROWS];
	int				numRows;
	int				selected;		// row index
	int				top;			// first visible row
	qboolean		fresh;			// the next character replaces the field
	char			status[64];
	int				statusTime;
} balanceMenu_t;

static balanceMenu_t	balanceMenu;

// the robot and the weapons of the player in the battle, marked in the list
static int				ownRobot = -1;
static char				ownWeapons[RWEAPON_COUNT][48];	// "RIGHT L2, LEFT L1", "" when not mounted

static const vec4_t	balanceBackColor = { 0.04f, 0.05f, 0.07f, 1.0f };
static const vec4_t	balanceBarColor = { 0.09f, 0.11f, 0.14f, 1.0f };
static const vec4_t	balanceRuleColor = { 0.25f, 0.30f, 0.36f, 1.0f };
static const vec4_t	balanceAccentColor = { 1.0f, 0.72f, 0.25f, 1.0f };
static const vec4_t	balanceTextColor = { 0.88f, 0.90f, 0.92f, 1.0f };
static const vec4_t	balanceDimColor = { 0.52f, 0.56f, 0.60f, 1.0f };
static const vec4_t	balanceChangedColor = { 0.45f, 0.85f, 1.0f, 1.0f };	// differs from the stock value
static const vec4_t	balancePendingColor = { 1.0f, 0.95f, 0.35f, 1.0f };	// typed, not applied
static const vec4_t	balanceFieldColor = { 0.10f, 0.12f, 0.15f, 1.0f };
static const vec4_t	balanceSelectedColor = { 0.18f, 0.24f, 0.32f, 1.0f };
static const vec4_t	balanceOwnColor = { 1.0f, 0.72f, 0.25f, 0.14f };	// band of an own robot or weapon


/*
=================
Balance_FindOwn

the robot and the weapons of the player from his configstring ("model", "rw")
=================
*/
static void Balance_FindOwn( void ) {
	uiClientState_t	state;
	char			info[MAX_INFO_STRING];
	char			model[MAX_QPATH], list[64];
	char			*slash, *cursor;
	int				index, slot;

	ownRobot = -1;
	memset( ownWeapons, 0, sizeof( ownWeapons ) );
	trap_GetClientState( &state );
	if ( state.connState != CA_ACTIVE ) {
		return;
	}
	trap_GetConfigString( CS_PLAYERS + state.clientNum, info, sizeof( info ) );
	if ( atoi( Info_ValueForKey( info, "t" ) ) == TEAM_SPECTATOR ) {
		return;
	}
	Q_strncpyz( model, Info_ValueForKey( info, "model" ), sizeof( model ) );
	slash = strchr( model, '/' );
	if ( slash ) {
		*slash = 0;
	}
	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		if ( !Q_stricmp( model, bg_robots[index].model ) ) {
			ownRobot = index;
		}
	}
	if ( ownRobot < 0 ) {
		return;
	}

	// "osa.2,osa.1": by slot, "-" for an empty one
	Q_strncpyz( list, Info_ValueForKey( info, "rw" ), sizeof( list ) );
	cursor = list;
	for ( slot = 0; slot < bg_robots[ownRobot].numSlots && cursor && *cursor; slot++ ) {
		char		*next = strchr( cursor, ',' );
		char		*dot;
		float		side = bg_robots[ownRobot].slotOffset[slot][1];
		int			weaponLevel = 1;
		int			weapon;

		if ( next ) {
			*next++ = 0;
		}
		dot = strchr( cursor, '.' );
		if ( dot ) {
			*dot = 0;
			weaponLevel = atoi( dot + 1 );
		}
		weapon = BG_WeaponByName( cursor );
		if ( weapon >= 0 && weapon < RWEAPON_COUNT ) {
			Q_strcat( ownWeapons[weapon], sizeof( ownWeapons[weapon] ), va( "%s%s L%i", ownWeapons[weapon][0] ? ", " : "",
				side > 2 ? "LEFT" : ( side < -2 ? "RIGHT" : "CENTER" ), weaponLevel ) );
		}
		cursor = next;
	}
}


/*
=================
Balance_IsOwn
=================
*/
static qboolean Balance_IsOwn( const balanceRow_t *row ) {
	if ( row->calc ) {
		return qfalse;
	}
	return row->weapon ? ownWeapons[row->index][0] != 0 : row->index == ownRobot;
}


/*
=================
Balance_Value

a parameter in table units
=================
*/
static int Balance_Value( const balanceRow_t *row, qboolean stock ) {
	if ( row->calc ) {
		return row->calc == CALC_TARGET ? calcTarget : calcNorm;
	}
	return row->weapon ? BG_WeaponParam( row->index, row->param, stock ) : BG_RobotParam( row->index, row->param, stock );
}


/*
=================
Balance_Format

as shown: the interval as shots per second, the rest as is
=================
*/
static void Balance_Format( const balanceRow_t *row, int value, char *buffer, int size ) {
	if ( row->weapon && row->param == WPARAM_INTERVAL ) {
		int		hundredths = ( 100000 + value / 2 ) / ( value > 0 ? value : 1 );	// shots per second * 100
		if ( hundredths % 100 == 0 ) {
			Com_sprintf( buffer, size, "%i", hundredths / 100 );
		} else if ( hundredths % 10 == 0 ) {
			Com_sprintf( buffer, size, "%i.%i", hundredths / 100, ( hundredths / 10 ) % 10 );
		} else {
			Com_sprintf( buffer, size, "%i.%02i", hundredths / 100, hundredths % 100 );
		}
	} else {
		Com_sprintf( buffer, size, "%i", value );
	}
}


/*
=================
Balance_Parse

the field to table units; qfalse when there is no number in it
=================
*/
static qboolean Balance_Parse( const balanceRow_t *row, int *value ) {
	char	number[TEXT_SIZE];
	int		position;
	float	parsed;

	Q_strncpyz( number, row->text, sizeof( number ) );
	for ( position = 0; number[position]; position++ ) {
		if ( number[position] == ',' ) {
			number[position] = '.';
		}
	}
	if ( !number[0] || !strcmp( number, "." ) ) {
		return qfalse;
	}
	parsed = atof( number );
	if ( row->weapon && row->param == WPARAM_INTERVAL ) {
		if ( parsed <= 0 ) {
			return qfalse;
		}
		*value = (int)( 1000.0f / parsed + 0.5f );
	} else {
		*value = (int)( parsed + 0.5f );
	}
	return qtrue;
}


/*
=================
Balance_Load

current values into the fields
=================
*/
static void Balance_Load( void ) {
	int		index;

	for ( index = 0; index < balanceMenu.numRows; index++ ) {
		balanceRow_t	*row = &balanceMenu.rows[index];
		if ( row->kind == ROW_VALUE ) {
			Balance_Format( row, Balance_Value( row, qfalse ), row->text, sizeof( row->text ) );
			Q_strncpyz( row->loaded, row->text, sizeof( row->loaded ) );
		}
	}
	balanceMenu.fresh = qtrue;
}


/*
=================
Balance_CalcLoad / Balance_CalcSave
=================
*/
static void Balance_CalcLoad( void ) {
	char	text[MAX_CVAR_VALUE_STRING];
	char	*cursor = text, *token;

	trap_Cvar_VariableStringBuffer( "bal_calc", text, sizeof( text ) );
	token = COM_ParseExt( &cursor, qfalse );
	calcTarget = token[0] ? atoi( token ) : 70000;
	token = COM_ParseExt( &cursor, qfalse );
	calcNorm = token[0] ? atoi( token ) : 15;
	if ( calcTarget < 1 ) {
		calcTarget = 1;
	}
	if ( calcNorm < 1 ) {
		calcNorm = 1;
	}
}

static void Balance_CalcSave( void ) {
	trap_Cvar_Set( "bal_calc", va( "%i %i", calcTarget, calcNorm ) );
}


/*
=================
Balance_Tenths

"12.3"
=================
*/
static const char *Balance_Tenths( float value ) {
	int		tenths = (int)( value * 10.0f + 0.5f );
	return va( "%i.%i", tenths / 10, tenths % 10 );
}


/*
=================
Balance_InfoText

line 0: damage per second, time to destroy the calculator structure, damage per shot
for the norm time; line 1: time to destroy each robot of level 1 with its shield and armor
=================
*/
static void Balance_InfoText( const balanceRow_t *row, char *buffer, int size ) {
	const robotWeaponInfo_t	*info = &bg_robotWeapons[row->index];
	float	perSecond = (float)info->damage * 1000.0f / ( info->interval > 0 ? info->interval : 1 );
	int		robot;

	buffer[0] = 0;
	if ( perSecond <= 0 ) {
		Q_strncpyz( buffer, "no damage", size );
		return;
	}
	if ( row->param == 0 ) {
		Q_strcat( buffer, size, va( "DPS %i   TO %i: %s s   ", (int)( perSecond + 0.5f ), calcTarget, Balance_Tenths( calcTarget / perSecond ) ) );
		Q_strcat( buffer, size, va( "FOR %i s: DAMAGE %i", calcNorm,
			(int)( (float)calcTarget * info->interval / ( calcNorm * 1000.0f ) + 0.5f ) ) );
		return;
	}
	Q_strcat( buffer, size, "WITH SHIELD, ARMOR:" );
	for ( robot = 0; robot < ROBOT_COUNT; robot++ ) {
		const robotInfo_t	*target = &bg_robots[robot];
		float	effective = target->shield + (float)target->structure * 100.0f / ( 100 - target->armor );
		Q_strcat( buffer, size, va( "  %s %s", target->title, Balance_Tenths( effective / perSecond ) ) );
	}
}


/*
=================
Balance_SetStatus
=================
*/
static void Balance_SetStatus( const char *text ) {
	Q_strncpyz( balanceMenu.status, text, sizeof( balanceMenu.status ) );
	balanceMenu.statusTime = uis.realtime;
}


/*
=================
Balance_WriteCvars
=================
*/
static void Balance_WriteCvars( void ) {
	char	buffer[MAX_CVAR_VALUE_STRING];
	int		index;

	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		BG_BalanceString( qfalse, index, buffer, sizeof( buffer ) );
		trap_Cvar_Set( BG_BalanceCvarName( qfalse, index ), buffer );
	}
	for ( index = 0; index < RWEAPON_COUNT; index++ ) {
		BG_BalanceString( qtrue, index, buffer, sizeof( buffer ) );
		trap_Cvar_Set( BG_BalanceCvarName( qtrue, index ), buffer );
	}
	BG_BalanceUpdate();
}


/*
=================
Balance_SaveFile

all values to balance.cfg: a readable table in comments (new, stock, * changed) and
the cvars, which the game reads from it at start
=================
*/
static void Balance_SaveFile( void ) {
	fileHandle_t	file;
	balanceRow_t	row;
	qtime_t			now;
	char			buffer[MAX_CVAR_VALUE_STRING];
	char			value[TEXT_SIZE], stock[TEXT_SIZE];
	int				index, param;

	trap_FS_FOpenFile( BALANCE_FILE, &file, FS_WRITE );
	if ( !file ) {
		Balance_SetStatus( "could not write " BALANCE_FILE );
		return;
	}
#define BALANCE_PUT( text ) trap_FS_Write( text, strlen( text ), file )
	trap_RealTime( &now );
	BALANCE_PUT( va( "// WALKER balance from the BALANCE (TEST) menu, %04i-%02i-%02i %02i:%02i\n",
		now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min ) );
	BALANCE_PUT( "// read at start; the values are for level 1, * - differs from the stock one\n//\n" );
	memset( &row, 0, sizeof( row ) );
	for ( index = 0; index < ROBOT_COUNT + RWEAPON_COUNT; index++ ) {
		row.weapon = index >= ROBOT_COUNT;
		row.index = row.weapon ? index - ROBOT_COUNT : index;
		BALANCE_PUT( va( "// %s\n", row.weapon ? bg_robotWeapons[row.index].title : bg_robots[row.index].title ) );
		for ( param = 0; param < ( row.weapon ? WPARAM_COUNT : RPARAM_COUNT ); param++ ) {
			row.param = param;
			if ( row.weapon && !BG_WeaponHasParam( row.index, param ) ) {
				continue;
			}
			Balance_Format( &row, Balance_Value( &row, qfalse ), value, sizeof( value ) );
			Balance_Format( &row, Balance_Value( &row, qtrue ), stock, sizeof( stock ) );
			BALANCE_PUT( va( "//   %-14s %-10s stock %-10s%s\n",
				( row.weapon && param == WPARAM_INTERVAL ) ? "SHOTS/S" : ( row.weapon ? bg_weaponParams[param].name : bg_robotParams[param].name ),
				value, stock, strcmp( value, stock ) ? " *" : "" ) );
		}
	}
	BALANCE_PUT( "\n" );
	for ( index = 0; index < ROBOT_COUNT + RWEAPON_COUNT; index++ ) {
		qboolean	weapon = index >= ROBOT_COUNT;
		BG_BalanceString( weapon, weapon ? index - ROBOT_COUNT : index, buffer, sizeof( buffer ) );
		BALANCE_PUT( va( "seta %s \"%s\"\n", BG_BalanceCvarName( weapon, weapon ? index - ROBOT_COUNT : index ), buffer ) );
	}
	BALANCE_PUT( va( "seta bal_calc \"%i %i\"\n", calcTarget, calcNorm ) );
#undef BALANCE_PUT
	trap_FS_FCloseFile( file );
}


/*
=================
UI_BalanceLoadFile

at the ui start: the values saved by the menu
=================
*/
void UI_BalanceLoadFile( void ) {
	fileHandle_t	file;
	int				length = trap_FS_FOpenFile( BALANCE_FILE, &file, FS_READ );

	if ( file ) {
		trap_FS_FCloseFile( file );
	}
	if ( length > 0 ) {
		trap_Cmd_ExecuteText( EXEC_APPEND, "exec " BALANCE_FILE "\n" );
	}
}


/*
=================
Balance_Apply

the changed fields into the tables and the cvars
=================
*/
static void Balance_Apply( void ) {
	int		index, changed = 0;

	for ( index = 0; index < balanceMenu.numRows; index++ ) {
		balanceRow_t	*row = &balanceMenu.rows[index];
		int				value;

		if ( row->kind != ROW_VALUE || !strcmp( row->text, row->loaded ) ) {
			continue;
		}
		if ( row->calc ) {
			if ( Balance_Parse( row, &value ) ) {
				if ( row->calc == CALC_TARGET ) {
					calcTarget = value < 1 ? 1 : ( value > 100000000 ? 100000000 : value );
				} else {
					calcNorm = value < 1 ? 1 : ( value > 3600 ? 3600 : value );
				}
				Balance_CalcSave();
			}
			continue;
		}
		if ( Balance_Parse( row, &value ) ) {
			if ( row->weapon ) {
				BG_SetWeaponParam( row->index, row->param, value );
			} else {
				BG_SetRobotParam( row->index, row->param, value );
			}
			changed++;
		}
	}
	if ( changed ) {
		Balance_WriteCvars();
		Balance_SaveFile();
		Balance_SetStatus( va( "applied: %i, saved to " BALANCE_FILE, changed ) );
	}
	Balance_Load();
}


/*
=================
Balance_Reset

all to the stock values
=================
*/
static void Balance_Reset( void ) {
	int		index;

	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		trap_Cvar_Set( BG_BalanceCvarName( qfalse, index ), "" );
	}
	for ( index = 0; index < RWEAPON_COUNT; index++ ) {
		trap_Cvar_Set( BG_BalanceCvarName( qtrue, index ), "" );
	}
	BG_BalanceUpdate();
	Balance_Load();
	Balance_SaveFile();
	Balance_SetStatus( "all values are stock, saved to " BALANCE_FILE );
}


/*
=================
Balance_Select

the nearest value row from a row in a direction
=================
*/
static void Balance_Select( int row, int direction ) {
	if ( row < 0 ) {
		row = 0;
	}
	if ( row >= balanceMenu.numRows ) {
		row = balanceMenu.numRows - 1;
	}
	{
		int		start = row;
		while ( row >= 0 && row < balanceMenu.numRows && balanceMenu.rows[row].kind != ROW_VALUE ) {
			row += direction;
		}
		if ( row < 0 || row >= balanceMenu.numRows ) {
			// nothing that way (the list ends with computed rows): the nearest the other way
			for ( row = start; row >= 0 && row < balanceMenu.numRows && balanceMenu.rows[row].kind != ROW_VALUE; row -= direction ) {
			}
			if ( row < 0 || row >= balanceMenu.numRows ) {
				return;
			}
		}
	}
	balanceMenu.selected = row;
	balanceMenu.fresh = qtrue;
	// keep it in sight, with its title when there is room
	if ( row - 1 < balanceMenu.top ) {
		balanceMenu.top = row - 1;
	}
	{
		int		last = row;		// and the computed rows right under it
		while ( last + 1 < balanceMenu.numRows && balanceMenu.rows[last + 1].kind == ROW_INFO ) {
			last++;
		}
		if ( last >= balanceMenu.top + VISIBLE_ROWS ) {
			balanceMenu.top = last - VISIBLE_ROWS + 1;
		}
	}
}


/*
=================
Balance_Scroll
=================
*/
static void Balance_Scroll( int rows ) {
	int		maximum = balanceMenu.numRows - VISIBLE_ROWS;

	balanceMenu.top += rows;
	if ( balanceMenu.top > maximum ) {
		balanceMenu.top = maximum;
	}
	if ( balanceMenu.top < 0 ) {
		balanceMenu.top = 0;
	}
}


/*
=================
Balance_Text
=================
*/
static void Balance_Text( int x, int y, const char *text, int align, const vec4_t color ) {
	UI_DrawString( x, y, text, align | UI_TINYFONT, (float *)color );
}


/*
=================
Balance_DrawRow
=================
*/
static void Balance_DrawRow( int index, int y ) {
	balanceRow_t	*row = &balanceMenu.rows[index];
	char			stock[TEXT_SIZE];
	const float		*color;

	switch ( row->kind ) {
	case ROW_SECTION:
		{
			static const char	*titles[] = { "CALCULATOR", "ROBOTS", "WEAPONS" };
			int					width = ( strlen( titles[row->param] ) + 1 ) * TINYFONT_WIDTH;
			Balance_Text( LIST_X, y + 1, titles[row->param], UI_LEFT, balanceAccentColor );
			UI_FillRect( LIST_X + width, y + 7, LIST_W - width, 1, balanceRuleColor );
		}
		return;
	case ROW_INFO:
		{
			char	text[128];
			if ( Balance_IsOwn( row ) ) {
				UI_FillRect( LIST_X, y, 3, ROW_H, balanceAccentColor );
			}
			Balance_InfoText( row, text, sizeof( text ) );
			Balance_Text( LABEL_X, y + 1, text, UI_LEFT, balanceChangedColor );
		}
		return;
	case ROW_OBJECT:
		if ( Balance_IsOwn( row ) ) {
			UI_FillRect( LIST_X, y, LIST_W, ROW_H - 1, balanceOwnColor );
			UI_FillRect( LIST_X, y, 3, ROW_H - 1, balanceAccentColor );
			Balance_Text( LIST_X + LIST_W - 4, y + 1, row->weapon ? va( "YOURS: %s", ownWeapons[row->index] ) : "YOUR ROBOT",
				UI_RIGHT, balanceAccentColor );
		}
		if ( row->weapon ) {
			const robotWeaponInfo_t	*info = &bg_robotWeapons[row->index];
			Balance_Text( LIST_X + 8, y + 1, info->title, UI_LEFT, balanceTextColor );
			Balance_Text( LIST_X + 8 + 8 * TINYFONT_WIDTH, y + 1, va( "%s, %s", bg_weaponClassNames[info->weaponClass], info->description ),
				UI_LEFT, balanceDimColor );
		} else {
			const robotInfo_t	*info = &bg_robots[row->index];
			Balance_Text( LIST_X + 8, y + 1, info->title, UI_LEFT, balanceTextColor );
			Balance_Text( LIST_X + 8 + 8 * TINYFONT_WIDTH, y + 1, info->description, UI_LEFT, balanceDimColor );
		}
		return;
	}

	if ( Balance_IsOwn( row ) ) {
		UI_FillRect( LIST_X, y, 3, ROW_H, balanceAccentColor );
	}
	Balance_Text( LABEL_X, y + 1, row->calc ? ( row->calc == CALC_TARGET ? "TARGET STRUCTURE" : "NORM TIME, S" )
		: ( ( row->weapon && row->param == WPARAM_INTERVAL ) ? "SHOTS/S"
		: ( row->weapon ? bg_weaponParams[row->param].name : bg_robotParams[row->param].name ) ), UI_LEFT, balanceDimColor );

	UI_FillRect( FIELD_X, y, FIELD_W, ROW_H - 1, index == balanceMenu.selected ? balanceSelectedColor : balanceFieldColor );
	if ( index == balanceMenu.selected ) {
		UI_FillRect( FIELD_X, y + ROW_H - 2, FIELD_W, 1, balanceAccentColor );
	}
	if ( strcmp( row->text, row->loaded ) ) {
		color = balancePendingColor;
	} else if ( !row->calc && Balance_Value( row, qfalse ) != Balance_Value( row, qtrue ) ) {
		color = balanceChangedColor;
	} else {
		color = balanceTextColor;
	}
	Balance_Text( FIELD_X + 4, y + 1, row->text, UI_LEFT, color );
	if ( index == balanceMenu.selected && ( uis.realtime / 300 ) % 2 ) {
		Balance_Text( FIELD_X + 4 + strlen( row->text ) * TINYFONT_WIDTH, y + 1, "_", UI_LEFT, balanceAccentColor );
	}

	if ( !row->calc ) {
		Balance_Format( row, Balance_Value( row, qtrue ), stock, sizeof( stock ) );
		Balance_Text( OLD_X, y + 1, stock, UI_LEFT, balanceDimColor );
	}
}


/*
=================
Balance_Draw
=================
*/
static void Balance_Draw( void ) {
	int		index, y;

	UI_FillRect( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, balanceBackColor );
	UI_FillRect( 0, 0, SCREEN_WIDTH, TOPBAR_H, balanceBarColor );
	UI_FillRect( 0, TOPBAR_H, SCREEN_WIDTH, 1, balanceRuleColor );
	Balance_Text( 10, 5, "BALANCE (TEST)", UI_LEFT, balanceAccentColor );
	Balance_Text( 630, 5, "applies at once to all robots in the battle, bots included", UI_RIGHT, balanceDimColor );

	Balance_Text( LABEL_X, HEADER_Y, "PARAMETER", UI_LEFT, balanceDimColor );
	Balance_Text( FIELD_X + 4, HEADER_Y, "NEW", UI_LEFT, balanceDimColor );
	Balance_Text( OLD_X, HEADER_Y, "OLD (STOCK)", UI_LEFT, balanceDimColor );
	UI_FillRect( LIST_X, HEADER_Y + 13, LIST_W, 1, balanceRuleColor );

	for ( index = balanceMenu.top, y = LIST_Y; index < balanceMenu.numRows && index < balanceMenu.top + VISIBLE_ROWS; index++, y += ROW_H ) {
		Balance_DrawRow( index, y );
	}

	// scroll bar
	if ( balanceMenu.numRows > VISIBLE_ROWS ) {
		int		height = VISIBLE_ROWS * ROW_H;
		int		thumb = height * VISIBLE_ROWS / balanceMenu.numRows;
		UI_FillRect( LIST_X + LIST_W + 8, LIST_Y, 3, height, balanceFieldColor );
		UI_FillRect( LIST_X + LIST_W + 8, LIST_Y + ( height - thumb ) * balanceMenu.top / ( balanceMenu.numRows - VISIBLE_ROWS ),
			3, thumb, balanceDimColor );
	}

	UI_FillRect( 0, BOTTOM_Y, SCREEN_WIDTH, SCREEN_HEIGHT - BOTTOM_Y, balanceBarColor );
	UI_FillRect( 0, BOTTOM_Y, SCREEN_WIDTH, 1, balanceRuleColor );
	Balance_Text( 10, BOTTOM_Y + 6, "Up/Down, Tab: field   type: new value   Enter: apply   wheel, PgUp/PgDn: scroll", UI_LEFT, balanceDimColor );
	if ( balanceMenu.status[0] && uis.realtime - balanceMenu.statusTime < 3000 ) {
		Balance_Text( 630, BOTTOM_Y + 6, balanceMenu.status, UI_RIGHT, balancePendingColor );
	}

	Menu_Draw( &balanceMenu.menu );
}


/*
=================
Balance_Close
=================
*/
static void Balance_Close( void ) {
	Balance_Apply();
	UI_PopMenu();
}


/*
=================
Balance_Key
=================
*/
static sfxHandle_t Balance_Key( int key ) {
	balanceRow_t	*row = &balanceMenu.rows[balanceMenu.selected];
	int				length = strlen( row->text );

	if ( key & K_CHAR_FLAG ) {
		int		character = key & ~K_CHAR_FLAG;
		if ( ( character >= '0' && character <= '9' ) || character == '.' || character == ',' ) {
			if ( balanceMenu.fresh ) {
				row->text[0] = 0;
				length = 0;
				balanceMenu.fresh = qfalse;
			}
			if ( length < TEXT_SIZE - 1 ) {
				row->text[length] = character;
				row->text[length + 1] = 0;
			}
		}
		return 0;
	}

	switch ( key ) {
	case K_UPARROW:
	case K_KP_UPARROW:
		Balance_Select( balanceMenu.selected - 1, -1 );
		return 0;
	case K_DOWNARROW:
	case K_KP_DOWNARROW:
	case K_TAB:
		Balance_Select( balanceMenu.selected + 1, 1 );
		return 0;
	case K_PGUP:
	case K_KP_PGUP:
		Balance_Select( balanceMenu.selected - VISIBLE_ROWS + 2, -1 );
		return 0;
	case K_PGDN:
	case K_KP_PGDN:
		Balance_Select( balanceMenu.selected + VISIBLE_ROWS - 2, 1 );
		return 0;
	case K_MWHEELUP:
		Balance_Scroll( -3 );
		return 0;
	case K_MWHEELDOWN:
		Balance_Scroll( 3 );
		return 0;
	case K_BACKSPACE:
		if ( length > 0 ) {
			row->text[length - 1] = 0;
		}
		balanceMenu.fresh = qfalse;
		return 0;
	case K_DEL:
	case K_KP_DEL:
		row->text[0] = 0;
		balanceMenu.fresh = qfalse;
		return 0;
	case K_ENTER:
	case K_KP_ENTER:
		Balance_Apply();
		return menu_move_sound;
	case K_ESCAPE:
		Balance_Close();
		return menu_out_sound;
	case K_MOUSE1:
		if ( uis.cursorx >= LIST_X && uis.cursorx < LIST_X + LIST_W && uis.cursory >= LIST_Y && uis.cursory < LIST_Y + VISIBLE_ROWS * ROW_H ) {
			int		clicked = balanceMenu.top + ( uis.cursory - LIST_Y ) / ROW_H;
			if ( clicked < balanceMenu.numRows && balanceMenu.rows[clicked].kind == ROW_VALUE ) {
				Balance_Select( clicked, 1 );
			}
			return 0;
		}
		break;
	}
	if ( key < 128 && key != K_ENTER && key != K_ESCAPE ) {
		return 0;		// printable keys come again as characters
	}
	return Menu_DefaultKey( &balanceMenu.menu, key );
}


/*
=================
Balance_Event
=================
*/
static void Balance_Event( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}
	switch ( ( (menucommon_s *)ptr )->id ) {
	case ID_APPLY:
		Balance_Apply();
		break;
	case ID_RESET:
		Balance_Reset();
		break;
	case ID_BACK:
		Balance_Close();
		break;
	}
}


/*
=================
Balance_AddRow
=================
*/
static void Balance_AddRow( int kind, qboolean weapon, int index, int param ) {
	balanceRow_t	*row;

	if ( balanceMenu.numRows >= MAX_ROWS ) {
		return;
	}
	row = &balanceMenu.rows[balanceMenu.numRows++];
	row->kind = kind;
	row->weapon = weapon;
	row->index = index;
	row->param = param;
}


/*
=================
Balance_InitButton
=================
*/
static void Balance_InitButton( menutext_s *button, int id, int x, const char *text ) {
	button->generic.type = MTYPE_PTEXT;
	button->generic.flags = QMF_CENTER_JUSTIFY | QMF_PULSEIFFOCUS;
	button->generic.id = id;
	button->generic.callback = Balance_Event;
	button->generic.x = x;
	button->generic.y = BOTTOM_Y + 24;
	button->string = (char *)text;
	button->color = color_white;
	button->style = UI_CENTER | UI_SMALLFONT;
	Menu_AddItem( &balanceMenu.menu, button );
}


/*
=================
UI_BalanceMenu
=================
*/
void UI_BalanceMenu( void ) {
	int		index, param;

	memset( &balanceMenu, 0, sizeof( balanceMenu ) );
	balanceMenu.menu.fullscreen = qtrue;
	balanceMenu.menu.wrapAround = qtrue;
	balanceMenu.menu.draw = Balance_Draw;
	balanceMenu.menu.key = Balance_Key;

	BG_BalanceUpdate();
	Balance_FindOwn();
	Balance_CalcLoad();
	Balance_AddRow( ROW_SECTION, qfalse, 0, 0 );
	Balance_AddRow( ROW_VALUE, qfalse, -1, 0 );
	balanceMenu.rows[balanceMenu.numRows - 1].calc = CALC_TARGET;
	Balance_AddRow( ROW_VALUE, qfalse, -1, 0 );
	balanceMenu.rows[balanceMenu.numRows - 1].calc = CALC_NORM;
	Balance_AddRow( ROW_SECTION, qfalse, 0, 1 );
	for ( index = 0; index < ROBOT_COUNT; index++ ) {
		Balance_AddRow( ROW_OBJECT, qfalse, index, 0 );
		for ( param = 0; param < RPARAM_COUNT; param++ ) {
			Balance_AddRow( ROW_VALUE, qfalse, index, param );
		}
	}
	Balance_AddRow( ROW_SECTION, qtrue, 0, 2 );
	for ( index = 0; index < RWEAPON_COUNT; index++ ) {
		Balance_AddRow( ROW_OBJECT, qtrue, index, 0 );
		for ( param = 0; param < WPARAM_COUNT; param++ ) {
			if ( BG_WeaponHasParam( index, param ) ) {
				Balance_AddRow( ROW_VALUE, qtrue, index, param );
			}
		}
		for ( param = 0; param < INFO_ROWS; param++ ) {
			Balance_AddRow( ROW_INFO, qtrue, index, param );
		}
	}
	Balance_Load();
	Balance_Select( 0, 1 );
	balanceMenu.top = 0;

	Balance_InitButton( &balanceMenu.apply, ID_APPLY, 160, "APPLY" );
	Balance_InitButton( &balanceMenu.reset, ID_RESET, 320, "ALL TO STOCK" );
	Balance_InitButton( &balanceMenu.back, ID_BACK, 480, "BACK" );

	UI_PushMenu( &balanceMenu.menu );
}
