/*
 * FxSound Engine MVP - Override codedefs.h
 * 
 * Replaces the original codedefs.h to:
 * 1. Replace NOT_OKAY macro (no MessageBox, no DebugBreak)
 * 2. Keep all other defines intact
 * 
 * Original: fxsound-app/audiopassthru/include/codedefs.h
 */
#ifndef _CODEDEFS_H_
#define _CODEDEFS_H_

/*
 * This is to get rid of the following warnings:
 * warning C4996: 'sprintf' was declared deprecated
 */
#define _CRT_SECURE_NO_DEPRECATE
#define _CRT_NON_CONFORMING_SWPRINTFS

/* Add memory leak detection to debug versions */
#include <stdlib.h>
#include <stdio.h>

#ifndef __ANDROID__
#include <crtdbg.h>
#include <windows.h>
#endif

/* Turn off memory tracing */
#define PT_TURN_OFF_MEM_TRACE

/* Static Library case (same as original) */
#ifndef PT_DECLSPEC
	#define PT_DECLSPEC
#endif

/* Widen string macros */
#define PT_WIDEN2(x) L ## x
#define PT_WIDEN(x) PT_WIDEN2(x)
#define PT_WFILE PT_WIDEN(__FILE__)

/* Wide string version of __LINE__ macro */
#define PT_STRINGIZE(x) PT_STRINGIZE2(x)
#define PT_STRINGIZE2(x) PT_WIDEN(#x)
#define PT_LINE_STRING PT_STRINGIZE(__LINE__)

#define S(x) #x
#define S_(x) S(x)
#define S__LINE__ S_(__LINE__)

/* Char version of __LINE__ macro */
#define PT_STRINGIZE_CHAR2(x) #x
#define PT_STRINGIZE_CHAR(x) PT_STRINGIZE_CHAR2(x)
#define PT_LINE_STRING_CHAR PT_STRINGIZE_CHAR(__LINE__)

/* Return codes for functions */
#define OKAY 0
#define NOT_OKAY_NO_BREAK 1

/* ============================================================================
 * FXSOUND_ENGINE: Replace NOT_OKAY with stderr output instead of MessageBox
 * Original code pops up MessageBox or DebugBreak - both are unacceptable for
 * a headless command-line process.
 * This is now always active (not gated by FXSOUND_ENGINE_MVP anymore).
 * ============================================================================ */

#ifdef UNICODE
    inline int ptEngineNotOkay(wchar_t *wcp_file, wchar_t *wcp_line)
    {
        fwprintf(stderr, L"[FxSound Engine] NOT_OKAY: %s (line %s)\n", wcp_file, wcp_line);
        return(NOT_OKAY_NO_BREAK);
    }
    #define NOT_OKAY ptEngineNotOkay(PT_WFILE, PT_LINE_STRING)
#else
    inline int ptEngineNotOkay(char *cp_file, char *cp_line)
    {
        fprintf(stderr, "[FxSound Engine] NOT_OKAY: %s (line %s)\n", cp_file, cp_line);
        return(NOT_OKAY_NO_BREAK);
    }
    #define NOT_OKAY ptEngineNotOkay(__FILE__, PT_LINE_STRING_CHAR)
#endif

/* Define a PT_HANDLE */
typedef int PT_HANDLE;

/* IS_TRUE, and IS_FALSE */
#define IS_FALSE 0
#define IS_TRUE  1

#define realtype float

/* For setting the precision of cos/sin in response plotting functions */
#define respDouble realtype

/* OS Dependent defines */
#ifndef WIN32
	#ifndef HWND
		#define HWND void *
	#endif
#endif

#ifdef __ANDROID__
	#include <android/log.h>
	#define ANDROID_LOG_TAG __FILE__
	#define DPRINTF(...)  __android_log_print(ANDROID_LOG_DEBUG,ANDROID_LOG_TAG,__VA_ARGS__)
	#define IPRINTF(...)  __android_log_print(ANDROID_LOG_INFO,ANDROID_LOG_TAG,__VA_ARGS__)
	#define EPRINTF(...)  __android_log_print(ANDROID_LOG_ERROR,ANDROID_LOG_TAG,__VA_ARGS__)
#endif

#define _CRT_NON_CONFORMING_SWPRINTFS

#endif //_CODEDEFS_H_
