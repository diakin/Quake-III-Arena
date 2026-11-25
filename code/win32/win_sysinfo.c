/*
===========================================================================
WALKER: system report for the benchmark (cl_bench.c, "sysinfo").

Windows version, processor, memory, power plan, screen, video adapters with
their drivers (from the registry), and checks: OpenGL through the driver of
the video card and not the Windows software one, the driver from the maker
and not a generic one, the game on the discrete card of a laptop, and so on.
Everything is read with plain Win32 calls available on Windows XP and later;
functions of newer Windows are looked up at run time.

This file is part of WALKER, based on the Quake III Arena source code
(GPL v2, see COPYING.txt).
===========================================================================
*/
#include "../game/q_shared.h"
#include "../qcommon/qcommon.h"
#include <windows.h>

#ifndef KEY_WOW64_64KEY
#define KEY_WOW64_64KEY		0x0100
#endif

#define DISPLAY_CLASS_KEY	"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}"
#define MAX_ADAPTERS		8

typedef struct {
	char		description[128];
	char		provider[64];
	char		version[64];
	char		date[32];
	unsigned	memoryMB;
} adapterInfo_t;

static adapterInfo_t	adapters[MAX_ADAPTERS];
static int				numAdapters;

static char		*reportBuffer;
static int		reportSize;
static int		reportErrors, reportWarnings;


static void Report( const char *format, ... ) {
	va_list		argptr;
	char		text[1024];

	va_start( argptr, format );
	Q_vsnprintf( text, sizeof( text ), format, argptr );
	va_end( argptr );
	Q_strcat( reportBuffer, reportSize, text );
}


/*
=================
Registry helpers
=================
*/
static qboolean RegString( HKEY root, const char *path, const char *name, char *value, int size, REGSAM extra ) {
	HKEY	key;
	DWORD	type, length = size - 1;
	LONG	result;

	value[0] = 0;
	if ( RegOpenKeyExA( root, path, 0, KEY_READ | extra, &key ) != ERROR_SUCCESS
		&& ( !extra || RegOpenKeyExA( root, path, 0, KEY_READ, &key ) != ERROR_SUCCESS ) ) {
		return qfalse;
	}
	result = RegQueryValueExA( key, name, NULL, &type, (LPBYTE)value, &length );
	RegCloseKey( key );
	if ( result != ERROR_SUCCESS || ( type != REG_SZ && type != REG_EXPAND_SZ ) ) {
		value[0] = 0;
		return qfalse;
	}
	value[length < (DWORD)size ? length : (DWORD)size - 1] = 0;
	return qtrue;
}

static qboolean RegNumber( HKEY root, const char *path, const char *name, unsigned long long *value ) {
	HKEY	key;
	DWORD	type, length;
	BYTE	data[8];

	*value = 0;
	if ( RegOpenKeyExA( root, path, 0, KEY_READ, &key ) != ERROR_SUCCESS ) {
		return qfalse;
	}
	length = sizeof( data );
	memset( data, 0, sizeof( data ) );
	if ( RegQueryValueExA( key, name, NULL, &type, data, &length ) != ERROR_SUCCESS ) {
		RegCloseKey( key );
		return qfalse;
	}
	RegCloseKey( key );
	if ( type == REG_QWORD || length == 8 ) {
		*value = *(unsigned long long *)data;
	} else {
		*value = *(DWORD *)data;		// REG_DWORD or 4 bytes of REG_BINARY
	}
	return qtrue;
}


/*
=================
ContainsNoCase
=================
*/
static qboolean ContainsNoCase( const char *text, const char *part ) {
	char	lowerText[512], lowerPart[64];
	int		index;

	Q_strncpyz( lowerText, text, sizeof( lowerText ) );
	Q_strncpyz( lowerPart, part, sizeof( lowerPart ) );
	for ( index = 0; lowerText[index]; index++ ) {
		lowerText[index] = tolower( (unsigned char)lowerText[index] );
	}
	for ( index = 0; lowerPart[index]; index++ ) {
		lowerPart[index] = tolower( (unsigned char)lowerPart[index] );
	}
	return strstr( lowerText, lowerPart ) != NULL;
}


/*
=================
Brand

maker of a video chip from its name: "nvidia", "amd", "intel", or ""
=================
*/
static const char *Brand( const char *name ) {
	if ( ContainsNoCase( name, "nvidia" ) || ContainsNoCase( name, "geforce" ) || ContainsNoCase( name, "quadro" ) ) {
		return "nvidia";
	}
	if ( ContainsNoCase( name, "amd" ) || ContainsNoCase( name, "radeon" ) || ContainsNoCase( name, "ati " ) || ContainsNoCase( name, "ati technologies" ) ) {
		return "amd";
	}
	if ( ContainsNoCase( name, "intel" ) ) {
		return "intel";
	}
	return "";
}


/*
=================
Report_Windows
=================
*/
static void Report_Windows( void ) {
	typedef LONG ( WINAPI *rtlGetVersion_t )( OSVERSIONINFOEXW * );
	typedef BOOL ( WINAPI *isWow64Process_t )( HANDLE, PBOOL );
	const char			*currentVersion = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	OSVERSIONINFOEXW	version;
	rtlGetVersion_t		rtlGetVersion;
	isWow64Process_t	isWow64Process;
	BOOL				wow64 = FALSE;
	char				product[128], servicePack[64], release[32];

	memset( &version, 0, sizeof( version ) );
	version.dwOSVersionInfoSize = sizeof( version );
	// GetVersionEx reports Windows 8 on newer systems to programs without a manifest
	rtlGetVersion = (rtlGetVersion_t)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlGetVersion" );
	if ( rtlGetVersion ) {
		rtlGetVersion( &version );
	}
	isWow64Process = (isWow64Process_t)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "IsWow64Process" );
	if ( isWow64Process ) {
		isWow64Process( GetCurrentProcess(), &wow64 );
	}
	RegString( HKEY_LOCAL_MACHINE, currentVersion, "ProductName", product, sizeof( product ), KEY_WOW64_64KEY );
	RegString( HKEY_LOCAL_MACHINE, currentVersion, "CSDVersion", servicePack, sizeof( servicePack ), KEY_WOW64_64KEY );
	RegString( HKEY_LOCAL_MACHINE, currentVersion, "DisplayVersion", release, sizeof( release ), KEY_WOW64_64KEY );
	if ( !release[0] ) {
		RegString( HKEY_LOCAL_MACHINE, currentVersion, "ReleaseId", release, sizeof( release ), KEY_WOW64_64KEY );
	}
	// Windows 11 still names itself Windows 10 in ProductName
	if ( version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000 && !strncmp( product, "Windows 10", 10 ) ) {
		product[8] = '1';
		product[9] = '1';
	}
	Report( "  Windows: %s%s%s%s%s (%lu.%lu.%lu), %s; the game is 32-bit\n",
		product[0] ? product : "unknown", servicePack[0] ? " " : "", servicePack,
		release[0] ? " " : "", release,
		version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
		wow64 ? "64-bit" : "32-bit" );
}


/*
=================
Report_Processor
=================
*/
static int Report_Processor( void ) {
	typedef BOOL ( WINAPI *getLogicalProcessorInformation_t )( PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD );
	const char			*cpuKey = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
	getLogicalProcessorInformation_t	getInfo;
	SYSTEM_INFO			system;
	char				name[128];
	unsigned long long	megahertz;
	int					cores = 0;
	char				*start;

	RegString( HKEY_LOCAL_MACHINE, cpuKey, "ProcessorNameString", name, sizeof( name ), 0 );
	RegNumber( HKEY_LOCAL_MACHINE, cpuKey, "~MHz", &megahertz );
	GetSystemInfo( &system );

	getInfo = (getLogicalProcessorInformation_t)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "GetLogicalProcessorInformation" );
	if ( getInfo ) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION	info[64];
		DWORD	length = sizeof( info );
		if ( getInfo( info, &length ) ) {
			int		index;
			for ( index = 0; index < (int)( length / sizeof( info[0] ) ); index++ ) {
				if ( info[index].Relationship == RelationProcessorCore ) {
					cores++;
				}
			}
		}
	}
	for ( start = name; *start == ' '; start++ ) {
	}
	if ( cores > 0 ) {
		Report( "  processor: %s, %i cores / %lu threads, %u MHz\n", start[0] ? start : "unknown", cores, system.dwNumberOfProcessors, (unsigned)megahertz );
	} else {
		Report( "  processor: %s, %lu threads, %u MHz\n", start[0] ? start : "unknown", system.dwNumberOfProcessors, (unsigned)megahertz );
	}
	return cores > 0 ? cores : (int)system.dwNumberOfProcessors;
}


/*
=================
Report_Memory
=================
*/
static void Report_Memory( unsigned *totalMB, unsigned *freeMB ) {
	MEMORYSTATUSEX	memory;

	memset( &memory, 0, sizeof( memory ) );
	memory.dwLength = sizeof( memory );
	*totalMB = *freeMB = 0;
	if ( GlobalMemoryStatusEx( &memory ) ) {
		*totalMB = (unsigned)( memory.ullTotalPhys / ( 1024 * 1024 ) );
		*freeMB = (unsigned)( memory.ullAvailPhys / ( 1024 * 1024 ) );
	}
	Report( "  memory: %.1f GB, free %.1f GB\n", *totalMB / 1024.0f, *freeMB / 1024.0f );
}


/*
=================
Report_PowerPlan

"" when not known
=================
*/
static const char *Report_PowerPlan( void ) {
	typedef DWORD ( WINAPI *powerGetActiveScheme_t )( HKEY, GUID ** );
	static const struct {
		const char	*guid;
		const char	*name;
	} plans[] = {
		{ "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c", "high performance" },
		{ "381b4222-f694-41f0-9685-ff5bb260df2e", "balanced" },
		{ "a1841308-3541-4fab-bc81-f71556f20b4a", "power saver" },
		{ "e9a42b02-d5df-448d-aa00-03f14749eb61", "ultimate performance" },
	};
	HMODULE					library = LoadLibraryA( "powrprof.dll" );
	powerGetActiveScheme_t	getActive;
	GUID					*scheme = NULL;
	const char				*name = "";
	char					text[64];
	int						index;

	if ( !library ) {
		return "";
	}
	getActive = (powerGetActiveScheme_t)GetProcAddress( library, "PowerGetActiveScheme" );		// Vista and later
	if ( getActive && getActive( NULL, &scheme ) == ERROR_SUCCESS && scheme ) {
		Com_sprintf( text, sizeof( text ), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			scheme->Data1, scheme->Data2, scheme->Data3, scheme->Data4[0], scheme->Data4[1], scheme->Data4[2],
			scheme->Data4[3], scheme->Data4[4], scheme->Data4[5], scheme->Data4[6], scheme->Data4[7] );
		name = "custom";
		for ( index = 0; index < (int)( sizeof( plans ) / sizeof( plans[0] ) ); index++ ) {
			if ( !Q_stricmp( text, plans[index].guid ) ) {
				name = plans[index].name;
			}
		}
		LocalFree( scheme );
	}
	FreeLibrary( library );
	if ( name[0] ) {
		Report( "  power plan: %s\n", name );
	}
	return name;
}


/*
=================
Report_Adapters

the video adapters with their drivers, from the class key of display devices
=================
*/
static void Report_Adapters( void ) {
	HKEY	classKey;
	DEVMODEA	mode;
	DWORD	index;
	char	subkey[16];
	char	path[256];

	memset( &mode, 0, sizeof( mode ) );
	mode.dmSize = sizeof( mode );
	if ( EnumDisplaySettingsA( NULL, ENUM_CURRENT_SETTINGS, &mode ) ) {
		Report( "  screen: %lux%lu, %lu Hz, %lu bit\n", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, mode.dmBitsPerPel );
	}

	numAdapters = 0;
	if ( RegOpenKeyExA( HKEY_LOCAL_MACHINE, DISPLAY_CLASS_KEY, 0, KEY_READ, &classKey ) != ERROR_SUCCESS ) {
		Report( "  video adapters: could not read the registry\n" );
		return;
	}
	for ( index = 0; numAdapters < MAX_ADAPTERS; index++ ) {
		DWORD				length = sizeof( subkey );
		adapterInfo_t		*adapter = &adapters[numAdapters];
		unsigned long long	memory;

		if ( RegEnumKeyExA( classKey, index, subkey, &length, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) {
			break;
		}
		if ( subkey[0] < '0' || subkey[0] > '9' ) {
			continue;		// "Properties" and the like
		}
		Com_sprintf( path, sizeof( path ), "%s\\%s", DISPLAY_CLASS_KEY, subkey );
		memset( adapter, 0, sizeof( *adapter ) );
		if ( !RegString( HKEY_LOCAL_MACHINE, path, "DriverDesc", adapter->description, sizeof( adapter->description ), 0 ) ) {
			continue;
		}
		RegString( HKEY_LOCAL_MACHINE, path, "ProviderName", adapter->provider, sizeof( adapter->provider ), 0 );
		RegString( HKEY_LOCAL_MACHINE, path, "DriverVersion", adapter->version, sizeof( adapter->version ), 0 );
		RegString( HKEY_LOCAL_MACHINE, path, "DriverDate", adapter->date, sizeof( adapter->date ), 0 );
		if ( RegNumber( HKEY_LOCAL_MACHINE, path, "HardwareInformation.qwMemorySize", &memory )
			|| RegNumber( HKEY_LOCAL_MACHINE, path, "HardwareInformation.MemorySize", &memory ) ) {
			adapter->memoryMB = (unsigned)( memory / ( 1024 * 1024 ) );
		}
		numAdapters++;
	}
	RegCloseKey( classKey );

	Report( "  video adapters:\n" );
	for ( index = 0; index < (DWORD)numAdapters; index++ ) {
		adapterInfo_t	*adapter = &adapters[index];
		char			nvidia[16] = "";

		// NVIDIA: driver 30.0.15.1179 is the release 511.79 people know
		if ( !strcmp( Brand( adapter->description ), "nvidia" ) ) {
			char	digits[32];
			int		length = 0, position;
			for ( position = 0; adapter->version[position] && length < (int)sizeof( digits ) - 1; position++ ) {
				if ( adapter->version[position] >= '0' && adapter->version[position] <= '9' ) {
					digits[length++] = adapter->version[position];
				}
			}
			digits[length] = 0;
			if ( length >= 5 ) {
				Com_sprintf( nvidia, sizeof( nvidia ), " (%.3s.%.2s)", digits + length - 5, digits + length - 2 );
			}
		}
		Report( "    %s%s, driver %s %s%s, %s\n", adapter->description,
			adapter->memoryMB ? va( ", %u MB", adapter->memoryMB ) : "",
			adapter->provider[0] ? adapter->provider : "?", adapter->version[0] ? adapter->version : "?", nvidia,
			adapter->date[0] ? adapter->date : "date unknown" );
	}
	if ( !numAdapters ) {
		Report( "    none found\n" );
	}
}


/*
=================
Check
=================
*/
static void Check( int level, const char *format, ... ) {
	va_list		argptr;
	char		text[1024];

	va_start( argptr, format );
	Q_vsnprintf( text, sizeof( text ), format, argptr );
	va_end( argptr );
	if ( level == 2 ) {
		reportErrors++;
	} else if ( level == 1 ) {
		reportWarnings++;
	}
	Report( "    %s %s\n", level == 2 ? "ERROR:" : ( level == 1 ? "WARNING:" : "note:" ), text );
}


/*
=================
Sys_SystemReport
=================
*/
void Sys_SystemReport( char *buffer, int size, const char *glVendor, const char *glRenderer, const char *glVersion ) {
	const char	*glBrand = Brand( va( "%s %s", glVendor, glRenderer ) );
	const char	*powerPlan;
	unsigned	totalMB, freeMB;
	int			cores, index, glMajor = 0;
	qboolean	software = qfalse;
	SYSTEMTIME	today;

	reportBuffer = buffer;
	reportSize = size;
	reportErrors = reportWarnings = 0;
	buffer[0] = 0;

	Report( "system:\n" );
	Report_Windows();
	cores = Report_Processor();
	Report_Memory( &totalMB, &freeMB );
	powerPlan = Report_PowerPlan();
	Report_Adapters();
	Report( "  OpenGL: %s / %s / %s\n", glVendor, glRenderer, glVersion );
	glMajor = atoi( glVersion );

	Report( "  checks:\n" );
	// OpenGL through the driver of the video card
	if ( ContainsNoCase( glRenderer, "gdi generic" ) || ContainsNoCase( glVendor, "microsoft" ) ) {
		Check( 2, "OpenGL is the Windows software one (GDI Generic): the video card driver is not installed or has no OpenGL; install the driver from the maker of the card (nvidia.com, amd.com, intel.com)" );
		software = qtrue;
	} else if ( ContainsNoCase( glRenderer, "llvmpipe" ) || ContainsNoCase( glRenderer, "softpipe" ) || ContainsNoCase( glRenderer, "swiftshader" ) || ContainsNoCase( glRenderer, "software" ) ) {
		Check( 2, "OpenGL is drawn by the processor (%s), not by the video card", glRenderer );
		software = qtrue;
	} else {
		Check( 0, "OpenGL works through the video card driver" );
	}
	for ( index = 0; index < numAdapters; index++ ) {
		adapterInfo_t	*adapter = &adapters[index];
		qboolean		generic = ContainsNoCase( adapter->description, "basic display" ) || ContainsNoCase( adapter->description, "vga" );

		if ( ContainsNoCase( adapter->description, "basic render" )
			|| ( !generic && ContainsNoCase( adapter->provider, "microsoft" ) && !Brand( adapter->description )[0] ) ) {
			continue;		// Windows' own software devices (names are translated on other languages)
		}
		if ( generic ) {
			Check( 2, "%s: Windows' generic driver, the video card has no driver of its own; install it from the maker", adapter->description );
		} else if ( ContainsNoCase( adapter->provider, "microsoft" ) && Brand( adapter->description )[0] ) {
			Check( 1, "%s: the driver is from Windows Update (%s), such drivers may lack full OpenGL; better the one from the maker of the card", adapter->description, adapter->provider );
		}
		// driver date "m-d-yyyy"
		if ( adapter->date[0] ) {
			const char	*year = strrchr( adapter->date, '-' );
			GetLocalTime( &today );
			if ( year && atoi( year + 1 ) > 1990 && today.wYear - atoi( year + 1 ) >= 5 ) {
				Check( 0, "%s: the driver is from %s, more than 5 years old; a newer one may be faster (if the card and Windows still get them)", adapter->description, year + 1 );
			}
		}
	}
	// a laptop with two video cards: the game should run on the discrete one
	if ( !software && glBrand[0] && !strcmp( glBrand, "intel" ) ) {
		for ( index = 0; index < numAdapters; index++ ) {
			const char	*brand = Brand( adapters[index].description );
			if ( !strcmp( brand, "nvidia" ) || !strcmp( brand, "amd" ) ) {
				Check( 1, "the game runs on the built-in Intel video while %s is present; give Walker.exe the discrete card (NVIDIA Control Panel -> Manage 3D settings -> Program settings, or Windows Settings -> Display -> Graphics)",
					adapters[index].description );
				break;
			}
		}
	}
	if ( glMajor > 0 && glMajor < 2 && !software ) {
		Check( 1, "OpenGL %s is very old: the video card or its driver is old", glVersion );
	}
	if ( totalMB && totalMB < 3000 ) {
		Check( 1, "%u MB of memory is little for Windows and the game together", totalMB );
	}
	if ( freeMB && freeMB < 700 ) {
		Check( 1, "only %u MB of memory is free: close other programs", freeMB );
	}
	if ( !Q_stricmp( powerPlan, "power saver" ) ) {
		Check( 1, "the power plan is \"power saver\": the processor and the video card are slowed down; choose \"high performance\" or \"balanced\"" );
	}
	if ( cores <= 2 ) {
		Check( 0, "%i processor cores: the game runs its server, bots and drawing in one thread, the frequency of one core matters most", cores );
	}
	if ( Cvar_VariableIntegerValue( "r_swapInterval" ) ) {
		Check( 0, "r_swapInterval 1 (vertical sync): the frame rate never exceeds the refresh rate of the screen" );
	}
	if ( Cvar_VariableIntegerValue( "com_maxfps" ) > 0 ) {
		Check( 0, "com_maxfps %i caps the frame rate", Cvar_VariableIntegerValue( "com_maxfps" ) );
	}
	Report( "  result: %i errors, %i warnings\n", reportErrors, reportWarnings );
}
