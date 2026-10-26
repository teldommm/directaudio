/*
 * PE side of the DirectAudio driver for Wine 9.0 / Proton 9 (arm64ec).
 *
 * Wine 9's mmdevapi still expects every audio driver to ship its own small PE
 * module exporting get_device_guid / get_device_name_from_guid, plus the winmm
 * entry points. (Wine 11 made that part generic, which is why the Wine 11
 * build of this driver is just directaudio.c.) This file is dlls/winepulse.drv/
 * mmdevdrv.c from the same Wine 9.0 tree, renamed; all audio logic lives in the
 * unixlib (../directaudio.c).
 *
 * Original copyright notices retained:
 *
 * Copyright 2011-2012 Maarten Lankhorst
 * Copyright 2010-2011 Maarten Lankhorst for CodeWeavers
 * Copyright 2011 Andrew Eikum for CodeWeavers
 * Copyright 2022 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>
#include <assert.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unixlib.h"

#include "ole2.h"
#include "mimeole.h"
#include "dshow.h"
#include "dsound.h"
#include "propsys.h"
#include "propkey.h"

#include "initguid.h"
#include "ks.h"
#include "ksmedia.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"

#include "../mmdevapi/unixlib.h"
#include "../mmdevapi/mmdevdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(directaudio);

#define MAX_DEVICE_NAME_LEN 256

static struct list g_devices_cache = LIST_INIT(g_devices_cache);

struct device_cache {
    struct list entry;
    GUID guid;
    EDataFlow dataflow;
    char device_name[0];
};

static GUID da_render_guid =
{ 0xdd6d204b, 0xf518, 0x4190, { 0xae, 0xe8, 0xa9, 0x07, 0x94, 0xa5, 0x32, 0x6a } };
static GUID da_capture_guid =
{ 0x394e1666, 0xee00, 0x4fd1, { 0x8a, 0x87, 0xe2, 0x5f, 0x84, 0x04, 0x96, 0x12 } };

static WCHAR drv_key_devicesW[256];

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void *reserved)
{
    WCHAR buf[MAX_PATH];
    WCHAR *filename;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(dll);
        if (__wine_init_unix_call())
            return FALSE;

        GetModuleFileNameW(dll, buf, ARRAY_SIZE(buf));

        filename = wcsrchr(buf, '\\');
        filename = filename ? filename + 1 : buf;

        swprintf(drv_key_devicesW, ARRAY_SIZE(drv_key_devicesW),
                 L"Software\\Wine\\Drivers\\%s\\devices", filename);
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
        {
            struct device_cache *device, *device_next;

            LIST_FOR_EACH_ENTRY_SAFE(device, device_next, &g_devices_cache, struct device_cache, entry)
                free(device);
        }
        break;
    }
    return TRUE;
}

void WINAPI get_device_guid(EDataFlow flow, const char *device_name, GUID *guid)
{
    WCHAR key_name[MAX_DEVICE_NAME_LEN + 2];
    DWORD type, size = sizeof(*guid);
    LSTATUS status;
    HKEY drv_key, dev_key;

    if (!device_name[0]) {
        *guid = (flow == eRender) ? da_render_guid : da_capture_guid;
        return;
    }

    status = RegCreateKeyExW(HKEY_CURRENT_USER, drv_key_devicesW, 0, NULL, 0,
                             KEY_WRITE | KEY_WOW64_64KEY, NULL, &drv_key, NULL);
    if (status != ERROR_SUCCESS) {
        ERR("Failed to open devices registry key: %lu\n", status);
        CoCreateGuid(guid);
        return;
    }

    key_name[0] = (flow == eRender) ? '0' : '1';
    key_name[1] = ',';
    MultiByteToWideChar(CP_UNIXCP, 0, device_name, -1, key_name + 2, ARRAY_SIZE(key_name) - 2);

    status = RegCreateKeyExW(drv_key, key_name, 0, NULL, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                             NULL, &dev_key, NULL);
    if (status != ERROR_SUCCESS) {
        ERR("Failed to open registry key for device %s: %lu\n", device_name, status);
        RegCloseKey(drv_key);
        CoCreateGuid(guid);
        return;
    }

    status = RegQueryValueExW(dev_key, L"guid", 0, &type, (BYTE*)guid, &size);
    if (status != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(*guid)) {
        CoCreateGuid(guid);
        status = RegSetValueExW(dev_key, L"guid", 0, REG_BINARY, (BYTE*)guid, sizeof(*guid));
        if (status != ERROR_SUCCESS)
            ERR("Failed to store device GUID for %s to registry: %lu\n", device_name, status);
    }
    RegCloseKey(dev_key);
    RegCloseKey(drv_key);
}

BOOL WINAPI get_device_name_from_guid(GUID *guid, char **name, EDataFlow *flow)
{
    struct device_cache *device;
    WCHAR key_name[MAX_DEVICE_NAME_LEN + 2];
    DWORD key_name_size;
    DWORD index = 0;
    HKEY key;

    *name = NULL;

    /* Return empty string for the default device */
    if (IsEqualGUID(guid, &da_render_guid)) {
        *flow = eRender;
        if (!(*name = malloc(1)))
            return FALSE;
    } else if (IsEqualGUID(guid, &da_capture_guid)) {
        *flow = eCapture;
        if (!(*name = malloc(1)))
            return FALSE;
    }

    if (*name) {
        *name[0] = '\0';
        return TRUE;
    }

    /* Check the cache first */
    LIST_FOR_EACH_ENTRY(device, &g_devices_cache, struct device_cache, entry) {
        if (!IsEqualGUID(guid, &device->guid))
            continue;
        *flow = device->dataflow;
        if ((*name = strdup(device->device_name)))
            return TRUE;

        return FALSE;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, drv_key_devicesW, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        WARN("No devices found in registry\n");
        return FALSE;
    }

    for (;;) {
        DWORD size, type;
        LSTATUS status;
        GUID reg_guid;
        HKEY dev_key;
        int len;

        key_name_size = ARRAY_SIZE(key_name);
        if (RegEnumKeyExW(key, index++, key_name, &key_name_size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (RegOpenKeyExW(key, key_name, 0, KEY_READ | KEY_WOW64_64KEY, &dev_key) != ERROR_SUCCESS) {
            ERR("Couldn't open key: %s\n", wine_dbgstr_w(key_name));
            continue;
        }

        size = sizeof(reg_guid);
        status = RegQueryValueExW(dev_key, L"guid", 0, &type, (BYTE *)&reg_guid, &size);
        RegCloseKey(dev_key);

        if (status == ERROR_SUCCESS && type == REG_BINARY && size == sizeof(reg_guid) && IsEqualGUID(&reg_guid, guid)) {
            RegCloseKey(key);

            TRACE("Found matching device key: %s\n", wine_dbgstr_w(key_name));

            if (key_name[0] == '0')
                *flow = eRender;
            else if (key_name[0] == '1')
                *flow = eCapture;
            else {
                WARN("Unknown device type: %c\n", key_name[0]);
                return FALSE;
            }

            if (!(len = WideCharToMultiByte(CP_UNIXCP, 0, key_name + 2, -1, NULL, 0, NULL, NULL)))
                    return FALSE;

            if (!(*name = malloc(len)))
                return FALSE;

            if (!WideCharToMultiByte(CP_UNIXCP, 0, key_name + 2, -1, *name, len, NULL, NULL)) {
                free(*name);
                return FALSE;
            }

            if ((device = malloc(FIELD_OFFSET(struct device_cache, device_name[len])))) {
                device->guid = reg_guid;
                device->dataflow = *flow;
                memcpy(device->device_name, *name, len);
                list_add_tail(&g_devices_cache, &device->entry);
            }
            return TRUE;
        }
    }

    RegCloseKey(key);
    WARN("No matching device in registry for GUID %s\n", debugstr_guid(guid));
    return FALSE;
}
