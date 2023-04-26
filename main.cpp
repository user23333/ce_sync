#include "main.h"
#include "tunnel.h"

ExportedFunctions ef;
LPCSTR   sync_plugin_host = "127.0.0.1";
LPCSTR   sync_plugin_port = "9100";
bool     sync_plugin_enable = false;
int      sync_plugin_state = 0;
UINT_PTR sync_plugin_base = 0;
UINT_PTR sync_plugin_offs = 0;

HRESULT LoadConfigurationFile()
{
	DWORD count = 0;
	HRESULT hRes = S_OK;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	CHAR lpProfile[MAX_PATH] = { 0 };
	LPSTR lpConfHost = NULL;
	LPSTR lpConfPort = NULL;

	count = GetEnvironmentVariableA("userprofile", lpProfile, MAX_PATH);
	if (count == 0 || count > MAX_PATH)
	{
		return E_FAIL;
	}

	hRes = strcat_s(lpProfile, MAX_PATH, "\\.sync");
	if FAILED(hRes)
	{
		return E_FAIL;
	}

	hFile = CreateFileA(lpProfile, GENERIC_READ, NULL, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		return E_FAIL;
	}

	CloseHandle(hFile);

	lpConfHost = (LPSTR)malloc(MAX_PATH);
	lpConfPort = (LPSTR)malloc(MAX_PATH);
	if (lpConfHost == NULL || lpConfPort == NULL)
	{
		goto failed;
	}

	count = GetPrivateProfileStringA("INTERFACE", "host", "127.0.0.1", lpConfHost, MAX_PATH, lpProfile);
	if ((count > 0) && (count < (MAX_PATH - 2)))
	{
		sync_plugin_host = lpConfHost;
	}

	count = GetPrivateProfileStringA("INTERFACE", "port", "9100", lpConfPort, MAX_PATH, lpProfile);
	if ((count > 0) && (count < (MAX_PATH - 2)))
	{
		sync_plugin_port = lpConfPort;
	}

	return hRes;

failed:
	if (lpConfHost != NULL) { free(lpConfHost); }
	if (lpConfPort != NULL) { free(lpConfPort); }

	return E_FAIL;
}

bool IsDebugging()
{
	lua_State* l = ef.GetLuaState();
	lua_getglobal(l, "debug_isDebugging");
	if (lua_pcall(l, 0, 1, 0) != LUA_OK) { lua_pop(l, 1); return false; }
	bool result = lua_toboolean(l, -1);
	lua_pop(l, 1);
	return result;
}

bool IsBroken()
{
	lua_State* l = ef.GetLuaState();
	lua_getglobal(l, "debug_isBroken");
	if (lua_pcall(l, 0, 1, 0) != LUA_OK) { lua_pop(l, 1); return false; }
	bool result = lua_toboolean(l, -1);
	lua_pop(l, 1);
	return result;
}

bool Is64bit()
{
	lua_State* l = ef.GetLuaState();
	lua_getglobal(l, "targetIs64Bit");
	if (lua_pcall(l, 0, 1, 0) != LUA_OK) { lua_pop(l, 1); return false; }
	bool result = lua_toboolean(l, -1);
	lua_pop(l, 1);
	return result;
}

UINT_PTR GetXIP()
{
	const char* xip = Is64bit() ? "RIP" : "EIP";
	lua_State* l = ef.GetLuaState();
	lua_getglobal(l, xip);
	lua_Number result = lua_tonumber(l, 1);
	lua_pop(l, 1);
	return static_cast<UINT_PTR>(result);
}

bool GetAddressModuleInfo(UINT_PTR address, const char*& module_name, UINT_PTR& module_base)
{
	lua_State* l = ef.GetLuaState();
	lua_getglobal(l, "__get_address_module_info");
	lua_pushnumber(l, static_cast<lua_Number>(address));
	if (lua_pcall(l, 1, 2, 0) != LUA_OK || lua_isnil(l, -2) || lua_isnil(l, -1)) { lua_pop(l, 2); return false; }
	module_name = lua_tostring(l, -2);
	module_base = static_cast<UINT_PTR>(lua_tonumber(l, -1));
	lua_pop(l, 2);
	return true;
}

bool SyncConnect()
{
	sync_plugin_base = 0;
	sync_plugin_offs = 0;

	if (FAILED(TunnelCreate(sync_plugin_host, sync_plugin_port)))
		return false;

	if (FAILED(TunnelSend("[notice]{\"type\":\"new_dbg\",\"msg\":\"dbg connect - CheatEngine\",\"dialect\":\"CheatEngine\"}\n")))
		return false;

	return true;
}

void SyncDisconnect()
{
	TunnelClose();
}

bool SyncUpdate()
{
	UINT_PTR prev_base = sync_plugin_base;
	UINT_PTR prev_offs = sync_plugin_offs;
	const char* name;

	sync_plugin_offs = GetXIP();
	if (sync_plugin_offs)
	{
		if (GetAddressModuleInfo(sync_plugin_offs, name, sync_plugin_base))
		{
			if (sync_plugin_base != prev_base)
			{
				if (FAILED(TunnelSend("[notice]{\"type\":\"module\",\"path\":\"%s\"}\n", name)))
				{
					return false;
				}
			}
			if (sync_plugin_offs != prev_offs)
			{
				return SUCCEEDED(TunnelSend("[sync]{\"type\":\"loc\",\"base\":%llu,\"offset\":%llu}\n", sync_plugin_base, sync_plugin_offs));
			}
			return false;
		}
	}

	if (sync_plugin_base)
	{
		TunnelSend("[notice]{\"type\":\"dbg_err\"}\n");
		sync_plugin_base = 0;
	}

	return false;
}

void CALLBACK OnTimer(HWND hwnd, UINT msg, UINT_PTR wp, DWORD lp)
{
	if (IsDebugging() && IsBroken())
	{
		if (sync_plugin_state == 0)
		{
			sync_plugin_state = SyncConnect() ? 1 : 2;
		}

		if (sync_plugin_state == 1)
		{
			SyncUpdate();
		}
	}
	else if (sync_plugin_state == 2)
	{
		sync_plugin_state = 0;
	}
}

void __stdcall CEPlugin_MainMenu()
{
	static char title[256];
	char new_title[256]; new_title[0] = '\0';
	HWND hwnd = reinterpret_cast<HWND>(ef.GetMainWindowHandle());

	if (!sync_plugin_enable)
	{
		LoadConfigurationFile();
		sync_plugin_enable = true;
		sync_plugin_state = 0;
		SetTimer(hwnd, 12130, 100, OnTimer);
	}
	else
	{
		KillTimer(hwnd, 12130);
		SyncDisconnect();
		sync_plugin_enable = false;
		sync_plugin_state = 0;
	}

	if (!title[0]) GetWindowTextA(hwnd, title, 256);

	strcpy_s(new_title, title);
	strcat_s(new_title, sync_plugin_enable ? " [Syncing]" : "");
	SetWindowTextA(hwnd, new_title);
}

BOOL __stdcall CEPlugin_GetVersion(PPluginVersion pv, int size)
{
	pv->version = CESDK_VERSION;
	pv->pluginname = const_cast<char*>("SyncPlugin");
	return TRUE;
}

BOOL __stdcall CEPlugin_InitializePlugin(PExportedFunctions exported_functions, int plugin_id)
{
	if (exported_functions->sizeofExportedFunctions != sizeof(ef))
		return FALSE;

	ef = *exported_functions;

	MAINMENUPLUGIN_INIT menu_init = { const_cast<char*>("SyncPlugin"), CEPlugin_MainMenu, nullptr };
	if (ef.RegisterFunction(plugin_id, ptMainMenu, &menu_init) == -1)
		return FALSE;

	const char* script =
		"function __get_address_module_info(address)\n"
		"	if address then\n"
		"		local mods = enumModules()\n"
		"		for k, v in pairs(mods) do\n"
		"			local mod_base = v[\"Address\"]\n"
		"			if mod_base then\n"
		"				local mod_size = getModuleSize(v[\"Name\"])\n"
		"				if mod_size then\n"
		"					if address >= mod_base and address < (mod_base + mod_size) then\n"
		"						return v[\"Name\"], mod_base\n"
		"					end\n"
		"				end\n"
		"			end\n"
		"		end\n"
		"	end\n"
		"	return nil, nil\n"
		"end";

	luaL_dostring(ef.GetLuaState(), script);

	return TRUE;
}

BOOL __stdcall CEPlugin_DisablePlugin()
{
	if (ef.sizeofExportedFunctions)
	{
		if (sync_plugin_enable)
		{
			CEPlugin_MainMenu();
		}
	}
	return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	return TRUE;
}