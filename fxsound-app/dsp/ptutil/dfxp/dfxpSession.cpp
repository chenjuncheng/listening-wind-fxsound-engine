/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* dfxpSession.cpp */

#include "codedefs.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <mutex>

#include "u_dfxp.h" 

#include "dfxp.h"
#include "reg.h"
#include "DfxSdk.h"
#include "mth.h"

// CRITICAL FIX: In NO_REGISTRY mode, registry writes are disabled, but the DSP
// communication layer (dfxp_CommunicateXxx) reads knob values back via
// dfxp_GetKnobValue_MIDI -> dfxp_SessionReadIntegerValue. Without a memory
// cache, every read returns the hardcoded default, so realtime parameter
// updates silently have no effect.
//
// SAFETY FIX: std::map is NOT thread-safe. The audio thread (processTimer)
// and the main thread (processRealtimeCommand) can both access these
// functions concurrently. We protect the caches with std::mutex.
#ifdef NO_REGISTRY
static std::map<std::wstring, int> g_noRegistrySessionCache;
static std::map<std::wstring, realtype> g_noRegistrySessionCacheReal;
static std::mutex g_noRegistrySessionMutex;
#endif

/*
 * FUNCTION: dfxp_SessionWriteIntegerValue() 
 * DESCRIPTION:
 *   Saves the passed integer session value in the registry.
 */
int dfxp_SessionWriteIntegerValue(PT_HANDLE *hp_dfxp, wchar_t *wcp_key_name, int i_value)
{
#ifdef NO_REGISTRY
	// No registry available - persist in memory cache so that
	// dfxp_CommunicateXxx can read back the latest value.
	if (wcp_key_name) {
		std::lock_guard<std::mutex> lock(g_noRegistrySessionMutex);
		g_noRegistrySessionCache[wcp_key_name] = i_value;
	}
	return(OKAY);
#else
	struct dfxpHdlType *cast_handle;
	wchar_t wcp_key_value[DFXP_REGISTRY_BUFFER_LENGTH];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];

	cast_handle = (struct dfxpHdlType *)(hp_dfxp);

	if (cast_handle == NULL)
		return(OKAY);
/*
	if (cast_handle->trace.mode)
	   (cast_handle->slout1)->Message_Wide(FIRST_LINE, L"dfxp_SessionWriteIntegerValue: Entered");
*/
   if (!(cast_handle->fully_initialized))
		return(OKAY);
		
	if (cast_handle->vendor_code == 0)
      return(OKAY);

	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
									DFXP_REGISTRY_TOP_WIDE, 
									cast_handle->wcp_product_name, 
									cast_handle->major_version,
		                     cast_handle->vendor_code, 
									DFXP_REGISTRY_LASTUSED_WIDE, 
									wcp_key_name);
	
	swprintf(wcp_key_value, L"%d", i_value);
/*
	if (cast_handle->trace.mode)
	{
		swprintf(cast_handle->wcp_msg1, L"dfxp_SessionWriteIntegerValue: Calling regCreateKey(%s, %s)", wcp_full_key_path, wcp_key_value);
	   (cast_handle->slout1)->Message_Wide(FIRST_LINE, cast_handle->wcp_msg1);
	}
*/
	if (regCreateKey_Wide(REG_CURRENT_USER, wcp_full_key_path, wcp_key_value) != OKAY)
	   return(NOT_OKAY);
/*
	if (cast_handle->trace.mode)
	   (cast_handle->slout1)->Message_Wide(FIRST_LINE, L"dfxp_SessionWriteIntegerValue: Success");
*/
	return(OKAY);
#endif
}

/*
 * FUNCTION: dfxp_SessionReadIntegerValue() 
 * DESCRIPTION:
 *   Reads the session integer value from the registry.
 *
 *   If the last session info does not exist or if this is the first time
 *   run since installation, set to the default values.
 */
int dfxp_SessionReadIntegerValue(PT_HANDLE *hp_dfxp, wchar_t *wcp_key_name, int i_default_value, int *ip_value)
{
	struct dfxpHdlType *cast_handle;

	cast_handle = (struct dfxpHdlType *)(hp_dfxp);

	if (cast_handle == NULL)
		return(OKAY);

	if (wcp_key_name == NULL)
		return(NOT_OKAY);

	/* If vendor code is 0, then just use the default value */
	if (cast_handle->vendor_code == 0)
	{
		*ip_value = i_default_value;
      return(OKAY);
	}

#ifdef NO_REGISTRY
	// No registry available - read from memory cache first, fallback to default
	if (wcp_key_name) {
		std::lock_guard<std::mutex> lock(g_noRegistrySessionMutex);
		auto it = g_noRegistrySessionCache.find(wcp_key_name);
		if (it != g_noRegistrySessionCache.end()) {
			*ip_value = it->second;
			return(OKAY);
		}
	}
	*ip_value = i_default_value;
	return(OKAY);
#else
	wchar_t wcp_key_value[DFXP_REGISTRY_BUFFER_LENGTH];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];
	int key_exists;
	int use_default;
   int is_long;

	/* Create the registry full path */
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
									DFXP_REGISTRY_TOP_WIDE, 
									cast_handle->wcp_product_name, 
									cast_handle->major_version,
									cast_handle->vendor_code,
		                     DFXP_REGISTRY_LASTUSED_WIDE, 
									wcp_key_name);

	if (regReadKey_Wide(REG_CURRENT_USER, wcp_full_key_path, &key_exists, wcp_key_value,
	   (unsigned long)DFXP_REGISTRY_BUFFER_LENGTH) != OKAY)
	      return(NOT_OKAY);

	use_default = IS_TRUE;

	if (key_exists)
   {
		if (mthIsLong_Wide(wcp_key_value, &is_long) != OKAY)
			return(NOT_OKAY);
		if (is_long)
			use_default = IS_FALSE;
	}

	if (use_default)
		*ip_value = i_default_value;
	else
		*ip_value = _wtoi(wcp_key_value);

	return(OKAY);		                        
#endif
}

/*
 * FUNCTION: dfxp_SessionWriteRealValue() 
 * DESCRIPTION:
 *   Saves the passed real session value in the registry.
 */
int dfxp_SessionWriteRealValue(PT_HANDLE *hp_dfxp, wchar_t *wcp_key_name, realtype r_value)
{
#ifdef NO_REGISTRY
	// No registry available - persist real values in memory cache
	if (wcp_key_name) {
		std::lock_guard<std::mutex> lock(g_noRegistrySessionMutex);
		g_noRegistrySessionCacheReal[wcp_key_name] = r_value;
	}
	return(OKAY);
#else
	struct dfxpHdlType *cast_handle;
	wchar_t wcp_key_value[DFXP_REGISTRY_BUFFER_LENGTH];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];

	cast_handle = (struct dfxpHdlType *)(hp_dfxp);

	if (cast_handle == NULL)
		return(OKAY);

	if (cast_handle->vendor_code == 0)
      return(OKAY);

	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
									DFXP_REGISTRY_TOP_WIDE, 
									cast_handle->wcp_product_name, 
									cast_handle->major_version,
		                     cast_handle->vendor_code, 
									DFXP_REGISTRY_LASTUSED_WIDE, 
									wcp_key_name);
	
	swprintf(wcp_key_value, L"%.2f", r_value);

	if (regCreateKey_Wide(REG_CURRENT_USER, wcp_full_key_path, wcp_key_value) != OKAY)
	   return(NOT_OKAY);

	return(OKAY);
#endif
}

/*
 * FUNCTION: dfxp_SessionReadRealValue() 
 * DESCRIPTION:
 *   Reads the session real value from the registry.
 *
 *   If the last session info does not exist or if this is the first time
 *   run since installation, set to the default values.
 */
int dfxp_SessionReadRealValue(PT_HANDLE *hp_dfxp, wchar_t *wcp_key_name, realtype r_default_value, realtype *rp_value)
{
	struct dfxpHdlType *cast_handle;

	cast_handle = (struct dfxpHdlType *)(hp_dfxp);

	if (cast_handle == NULL)
		return(OKAY);

	if (wcp_key_name == NULL)
		return(NOT_OKAY);

	/* If vendor code is 0, then just use the default value */
	if (cast_handle->vendor_code == 0)
	{
		*rp_value = r_default_value;
      return(OKAY);
	}

#ifdef NO_REGISTRY
	// No registry available - read from memory cache first, fallback to default
	if (wcp_key_name) {
		std::lock_guard<std::mutex> lock(g_noRegistrySessionMutex);
		auto it = g_noRegistrySessionCacheReal.find(wcp_key_name);
		if (it != g_noRegistrySessionCacheReal.end()) {
			*rp_value = it->second;
			return(OKAY);
		}
	}
	*rp_value = r_default_value;
	return(OKAY);
#else
	wchar_t wcp_key_value[DFXP_REGISTRY_BUFFER_LENGTH];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];
	int key_exists;
	int use_default;

	/* Create the registry full path */
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
									DFXP_REGISTRY_TOP_WIDE, 
									cast_handle->wcp_product_name, 
									cast_handle->major_version,
									cast_handle->vendor_code,
		                     DFXP_REGISTRY_LASTUSED_WIDE, 
									wcp_key_name);

	if (regReadKey_Wide(REG_CURRENT_USER, wcp_full_key_path, &key_exists, wcp_key_value,
	   (unsigned long)DFXP_REGISTRY_BUFFER_LENGTH) != OKAY)
	      return(NOT_OKAY);

	use_default = IS_TRUE;

	if (key_exists)
   {
		if (wcslen(wcp_key_value) > 0)
			use_default = IS_FALSE;
	}

	if (use_default)
		*rp_value = r_default_value;
	else
		*rp_value = (realtype)_wtof(wcp_key_value);

	return(OKAY);		                        
#endif
}

