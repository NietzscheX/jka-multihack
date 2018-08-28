#include <Windows.h>
#include <stdlib.h>
#include <string>
#include <map>

// the hooking library
#include "mhook\mhook-lib\mhook.h"

// the official JKA SDK headers
#include "SDK\codemp\game\q_shared.h"
#include "SDK\codemp\game\bg_public.h"
#include "SDK\codemp\cgame\cg_public.h"
#include "SDK\codemp\cgame\cg_local.h"
#include "SDK\codemp\cgame\tr_types.h"

// Cvars to control the hack via the in-game console
vmCvar_t WALLHACK;
vmCvar_t CHEATS;
vmCvar_t TRIGGERBOT;
vmCvar_t GLOW;

// define a type of `pointer to GetProcAddress` to dynamically hook it
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE, LPCSTR);
// this is the unmodified call which will be used in hooking calls to internally call the
// real method
pGetProcAddress originalGetProcAddress = (pGetProcAddress)GetProcAddress(GetModuleHandle("kernel32.dll"), "GetProcAddress");

// define type of `pointer to dllEntry` to dynamically hook it
// the parameters and result values can be found in the original
// function prototype of the engine code
typedef int (*DLLENTRY)(int(QDECL *)(int, ...));
DLLENTRY originalDLLEntry;

// declaration of variadic method to hook q3 VM system calls
int(QDECL *syscall)(int arg, ...) = (int(QDECL *)(int, ...)) - 1;

// same thing as `DLLENTRY` but for the main method of the q3 VM
typedef int (*VMMain)(int, int, int, int, int, int, int, int, int, int, int, int, int);
VMMain originalVMMain;

// engine struct (e.g. to add custom shaders)
// will be populated in `CG_Init`
cgs_t *client_gameState;

// additional internal engine game state structs
// which will be populated by hooking q3 VM system calls
cg_t *client_game = 0;
gameState_t *gameState;

// complete information about the current player
playerState_t *ps;

// maps client numbers to team numbers
std::map<int, int> teamMap;

// to keep track of all entities in the VM
// this gets filled via hooking the `CG_GETDEFAULTSTATE` syscall in `dllMain`
centity_t *_cg_entities[MAX_GENTITIES];
// the currently processed entity in `_cg_entities`
int currentEntityIdx = 0;

// in team based games, we only want to add glow to enemies
// the triggerbot should only attack in case we focus an enemy
BOOL isEnemy(int currentEntityClientNumber)
{
    // do gamestate structs populated yet
    if (!ps)
    {
        return TRUE;
    }
    if (!client_gameState)
    {
        return TRUE;
    }
    // not team based - everyone is an enemy
    if (client_gameState->gametype < GT_TEAM)
    {
        return TRUE;
    }

    int ownTeam = ps->persistant[PERS_TEAM];
    if (ownTeam == TEAM_SPECTATOR)
    {
        return FALSE;
    }

    // check if team already determined
    if (teamMap.find(currentEntityClientNumber) != teamMap.end())
    {
        // dont add glow for team mates
        if (teamMap[currentEntityClientNumber] == ownTeam)
        {
            return FALSE;
        }
    }

    // determine team of specified entity
    else
    {
        // search for the team in the memory region of the `clientinfo` struct
        // using `.teamName` isn't exact because of memory hackery while determining `client_gameState`
        char red[] = "red";
        char *base = client_gameState->clientinfo[currentEntityClientNumber].teamName - 100;
        char *end = client_gameState->clientinfo[currentEntityClientNumber].teamName + 1000;

        // check if he's red
        BOOL found = FALSE;
        while (base < end)
        {
            if (_memicmp(red, base, sizeof(red)) == 0)
            {
                teamMap.insert(std::pair<int, int>(currentEntityClientNumber, TEAM_RED));
                return FALSE;
            }
            base += sizeof(red);
        }
        // then he must be blue <:
        if (!found)
        {
            teamMap.insert(std::pair<int, int>(currentEntityClientNumber, TEAM_BLUE));
        }

        if (teamMap[currentEntityClientNumber] == ownTeam)
        {
            return FALSE;
        }
    }

    return TRUE;
}

// an entity gets processed --> check if we want to add glow to it
BOOL playerGlowRequired()
{
    // glow turned off via cvar
    if (!GLOW.value)
    {
        return FALSE;
    }

    // illegal index
    if (currentEntityIdx > MAX_GENTITIES)
    {
        return FALSE;
    }

    // illegal index too
    if (_cg_entities[currentEntityIdx]->currentState.clientNum > MAX_CLIENTS)
    {
        return FALSE;
    }

    // dead player or not a player at all
    if (_cg_entities[currentEntityIdx]->currentState.eType != ET_PLAYER

        || _cg_entities[currentEntityIdx]->currentState.eFlags & EF_DEAD)

    {
        return FALSE;
    }

    // it's us
    if (_cg_entities[currentEntityIdx]->currentState.clientNum == ps->clientNum)
    {
        // TODO REMOVE ME
        ps->jetpackFuel = 500;
        return FALSE;
    }

    // we are in a duel
    if (ps->duelInProgress)
    {
        // only add glow for duel opponent
        if (!_cg_entities[currentEntityIdx]->currentState.number == ps->duelIndex)
        {
            return FALSE;
        }
    }

    // only mark enemies
    int currentEntityClientNumber = _cg_entities[currentEntityIdx]->currentState.number;
    if (!isEnemy(currentEntityClientNumber))
    {
        return FALSE;
    }

    return TRUE;
}

// this is a proxy call to the real system calls processing method of the Q3 engine
// to dynamically modify/hook all syscalls
int syscall_hook(int cmd, ...)
{
    // this is a variadic function, so this gets all parameters
    // using va_arg
    int arg[14];
    va_list arglist;
    int count;
    va_start(arglist, cmd);
    for (count = 0; count < 14; count++)
    {
        arg[count] = va_arg(arglist, int);
    }
    va_end(arglist);

    // check which syscall has been requested
    switch (cmd)
    {

    // syscall to add an entity to a rendered scene
    // --> this also adds players to the scene
    // --> check if the entity is a player and display it
    // over walls
    case CG_R_ADDREFENTITYTOSCENE:
    {
        if (WALLHACK.value)
        {
            // get the passed parameter (an entity)
            refEntity_t *ref = (refEntity_t *)arg[0];

            // add the RF_DEPTHHACK flag to display the entity
            // over walls, effectively disabling depth for players
            // --> wallhack
            ref->renderfx |= RF_DEPTHHACK | RDF_NOFOG;

            if (playerGlowRequired())
            {
                // add a glowing shader for a better view
                ref->customShader = client_gameState->media.disruptorShader;
            }
        }
        break;
    }

    // syscall to update the position of an entity
    // this gets called before `CG_R_ADDREFENTITYTOSCENE`, so this allows us
    // to determine the currently processed entity using the passed index in this function
    case CG_S_UPDATEENTITYPOSITION:
    {
        currentEntityIdx = (int)arg[0];
        break;
    }

    // Gets information on an entity using an engine struct
    case CG_GETDEFAULTSTATE:
    {
        _cg_entities[arg[0]] = (centity_t *)arg[1];
        break;
    }

    // the VM processed the current game state
    // --> fill our own data with the passed values
    case CG_GETGAMESTATE:
    {
        gameState = (gameState_t *)arg[0];
        // the gameState_t* element is wrapped by a cgs_t struct, so
        // using pointer arithmetic it's possible to get the "parent" element
        // --> the cgs_t struct
        cgs_t *_tmp = 0;
        client_gameState = (cgs_t *)((int)gameState - (int)&_tmp->gameState);
        break;
    }

    // same pointer arithmetic hack as above
    // get own playerstate
    case CG_GETSNAPSHOT:
    {
        // call the real syscall first so the struct will be prepared for the following calls
        auto result = syscall(cmd, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13]);
        snapshot_t *snap = (snapshot_t *)arg[1];
        // get the current player state
        ps = &(snap->ps);

        cg_t *tmp = 0;
        // we have an `activeSnapshots` object, so let's substract that values length and get the parent element
        client_game = (cg_t *)((int)arg[1] - (int)&tmp->activeSnapshots);

        return result;
        break;
    }

    default:
        break;
    }
    // call the real syscall using the passed parameters
    return syscall(cmd, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13]);
}

// to allow enabling and disabling things using the in-game console
void makeCvars()
{
    syscall_hook(CG_CVAR_REGISTER, &WALLHACK, "hax_wallhack", "1", 0);
    syscall_hook(CG_CVAR_REGISTER, &GLOW, "hax_glow", "1", 0);
    syscall_hook(CG_CVAR_REGISTER, &CHEATS, "hax_cheats", "1", 0);
    syscall_hook(CG_CVAR_REGISTER, &TRIGGERBOT, "hax_trigger", "1", 0);
}

// read current cvar values and act accordingly
void updateCvars()
{
    syscall_hook(CG_CVAR_UPDATE, &WALLHACK);
    syscall_hook(CG_CVAR_UPDATE, &GLOW);
    if (!GLOW.value)
    {
        teamMap.clear();
    }

    syscall_hook(CG_CVAR_UPDATE, &CHEATS);
    syscall_hook(CG_CVAR_UPDATE, &TRIGGERBOT);
}

// To debug stuff
void intToMessagebox(const char *tag, int x)
{
    char buf[1337];
    _itoa_s(x, buf, 10);
    MessageBox(0, buf, tag, 0);
}

// really good substring function
BOOL isSubstr(const char *a, const char *b)
{
    const char *output = NULL;
    output = strstr(a, b);
    if (output)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

// emulate a left click (for the triggerbot)
// https://gist.github.com/CoolOppo/9325b2fe6c2d94f7b583
void LeftClick()
{
    INPUT Input = {0};
    // left down
    Input.type = INPUT_MOUSE;
    Input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    ::SendInput(1, &Input, sizeof(INPUT));

    // left up
    ::ZeroMemory(&Input, sizeof(INPUT));
    Input.type = INPUT_MOUSE;
    Input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(1, &Input, sizeof(INPUT));
}

// register a custom shader in the game VM
qhandle_t trap_R_RegisterShader(const char *name)
{
    return syscall_hook(CG_R_REGISTERSHADER, name);
}

// hook for the q3 VM main method
int hookVMMain(int cmd, int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l)
{
    switch (cmd)
    {

    // initializing the game vm (game startup)
    case CG_INIT:
    {
        // register our cvars
        makeCvars();

        // delete data from previous game VMs
        teamMap.clear();

        // call the real method
        int result = originalVMMain(cmd, a, b, c, d, e, f, g, h, i, j, k, l);

        // Initialize custom shaders which will be used to make enemy players glow
        client_gameState->media.electricBody2Shader = trap_R_RegisterShader("gfx/misc/fullbodyelectric2");
        client_gameState->media.enlightenmentShader = trap_R_RegisterShader("powerups/enlightenmentshell");
        client_gameState->media.disruptorShader = trap_R_RegisterShader("gfx/effects/burn");
        return result;
    }

    // this gets called each time a frame will be drawn
    case CG_DRAW_ACTIVE_FRAME:
    {
        int result = originalVMMain(cmd, a, b, c, d, e, f, g, h, i, j, k, l);
        // read the (maybe) updated cvar values
        updateCvars();

        // enable cheat protected cvars
        if (CHEATS.value)
        {
            // unlock blocked and cheat protected cvars
            // by overriding the server provided cvar
            // prevent overwriting it by setting it each frame
            syscall_hook(CG_CVAR_SET, "sv_cheats", "1");
        }

        // triggerbot logic
        // checks if we look at a player entity
        if (TRIGGERBOT.value && client_game && client_game->crosshairClientNum)
        {
            // only trigger with a shootable weapon
            if (ps->weapon > WP_SABER && ps->weapon != WP_THERMAL && ps->weapon != WP_TRIP_MINE && ps->weapon != WP_DET_PACK)
            {
                // only shoot at enemies
                if (isEnemy(client_game->crosshairClientNum))
                {
                    // send a left click
                    // not using left click for fire? srsly why
                    LeftClick();
                    client_game->crosshairClientNum = 0;
                }
            }
        }

        return result;
    }

    default:
        break;
    }
    return originalVMMain(cmd, a, b, c, d, e, f, g, h, i, j, k, l);
}

// this will be called instead of the original `dllEntry`
// the result type and parameters result from the original
// engine code
void hookDLLEntry(int(QDECL *syscallptr)(int arg, ...))
{
    syscall = syscallptr;
    originalDLLEntry(syscall_hook);
}

// this gets called instead of `GetProcAddress`
FARPROC WINAPI hookGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    CHAR moduleName[MAX_PATH];

    // check in which dll the call will be executed
    if (!GetModuleFileName(hModule, moduleName, sizeof(moduleName)))
    {
        return (FARPROC)originalGetProcAddress(hModule, lpProcName);
    }

    // are we in the game vm?
    if (isSubstr(moduleName, "cgamex86.dll"))

    {
        // address of dllEntry requested --> pass our own method instead --> hook
        if (isSubstr(lpProcName, "dllEntry"))

        {
            // Modify returned function here
            // --> execute modified function instead
            // (modified function calls original function after doing hax using `originalDLLEntry`)
            originalDLLEntry = (DLLENTRY)originalGetProcAddress(hModule, lpProcName);
            return (PROC)hookDLLEntry;
        }

        // save things as above
        if (isSubstr(lpProcName, "vmMain"))

        {
            originalVMMain = (VMMain)originalGetProcAddress(hModule, lpProcName);
            return (PROC)hookVMMain;
        }
    }
    return (FARPROC)originalGetProcAddress(hModule, lpProcName);
}

// the main method of the injected dll
BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Do not need the thread based attach/detach messages in this DLL
        DisableThreadLibraryCalls(hModule);

        // create the hook which serves as entry point for this
        if (!Mhook_SetHook((PVOID *)&originalGetProcAddress, hookGetProcAddress))
        {
            MessageBox(0, "Couldn't create hook", ":(", 0);
        }

        break;
    }
    return TRUE;
}