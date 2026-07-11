/* ================================================================================================
 * File: input.cpp
 * Brief: Gamepad input backend, replacing null/in_null.c. Drives a single GamePad
 *        (see pad.h) and maps its state onto the engine: buttons become key events
 *        routed by input focus (menu/console navigation vs. rebindable game action
 *        keys), the left stick rotates the camera and the right stick moves the
 *        player - following the axis handling of the original win32/in_win.c.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/input/pad.h"

// The input backend is client code (the engine's own win32/in_win.c is the same):
// sticks write straight into cl.viewangles and the outgoing usercmd_t, and button
// routing depends on cls.key_dest, so it reaches into the client internals. The
// legacy headers redeclare a few q_common.h functions, hence the pragma.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
}
#pragma GCC diagnostic pop

#include <cmath>
#include <cstdio>

extern "C" {
// Timestamp of the current input frame, maintained by Sys_SendKeyEvents (sys.cpp);
// the engine has no declaration for it (cl_input.c declares its own extern).
extern unsigned sys_frame_time;

// Referenced by the options menu (menu.c). Gates stick movement only - buttons
// keep working regardless, or the menus would become unnavigable.
cvar_t * in_joystick = nullptr;
} // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------
// Button -> key mapping
// ------------------------------------------------------------------------------------------------

// Each pad button sends one of two keys depending on where input is focused when
// it goes down: menus/console/message mode get UI navigation keys, gameplay gets
// JOY/AUX keys driven by the default binds below (rebindable once configs exist).
// START and SELECT behave the same in both contexts: ESCAPE toggles the menu and
// '`' is the hardcoded console toggle in keys.c.
struct ButtonMapping
{
    u16 padButton;           // PAD_* bit
    int uiKey;               // key sent while a menu/the console has focus
    int gameKey;             // key sent while playing
    const char * gameBind;   // default binding for gameKey (nullptr = leave unbound)
};

constexpr ButtonMapping kButtonMap[] = {
    { PAD_UP,       K_UPARROW,    K_AUX5,   "invuse"     },
    { PAD_DOWN,     K_DOWNARROW,  K_AUX6,   "inven"      },
    { PAD_LEFT,     K_LEFTARROW,  K_AUX7,   "invprev"    },
    { PAD_RIGHT,    K_RIGHTARROW, K_AUX8,   "invnext"    },
    { PAD_CROSS,    K_ENTER,      K_JOY1,   "+moveup"    }, // jump
    { PAD_CIRCLE,   K_ESCAPE,     K_JOY2,   "+movedown"  }, // crouch
    { PAD_SQUARE,   K_ENTER,      K_JOY3,   "+use"       },
    { PAD_TRIANGLE, K_ESCAPE,     K_JOY4,   "cmd help"   },
    { PAD_L1,       K_PGUP,       K_AUX2,   "+speed"     },
    { PAD_R1,       K_PGDN,       K_AUX1,   "+attack"    },
    { PAD_L2,       K_AUX4,       K_AUX4,   "weapprev"   },
    { PAD_R2,       K_AUX3,       K_AUX3,   "weapnext"   },
    { PAD_L3,       K_AUX9,       K_AUX9,   nullptr      },
    { PAD_R3,       K_AUX10,      K_AUX10,  "centerview" },
    { PAD_START,    K_ESCAPE,     K_ESCAPE, nullptr      },
    { PAD_SELECT,   '`',          '`',      nullptr      }
};

constexpr int kNumButtons = static_cast<int>(sizeof(kButtonMap) / sizeof(kButtonMap[0]));

// ------------------------------------------------------------------------------------------------
// Backend state
// ------------------------------------------------------------------------------------------------

// The one gamepad, polled once per frame in IN_Frame and queried by the seam below.
static ps2::input::GamePad s_gamepad;

// Edge-detection state for turning the button mask into key up/down events.
static u16 s_oldButtons = 0;

// Key delivered when each button was pressed, so the release sends the same key
// even if input focus changed while the button was held.
static int s_heldKeys[kNumButtons] = {};

// Stick tuning (cvar names and defaults follow the original win32 joystick code;
// negate a sensitivity to invert that axis).
static cvar_t * s_yawSensitivity;
static cvar_t * s_pitchSensitivity;
static cvar_t * s_forwardSensitivity;
static cvar_t * s_sideSensitivity;
static cvar_t * s_yawThreshold;
static cvar_t * s_pitchThreshold;
static cvar_t * s_forwardThreshold;
static cvar_t * s_sideThreshold;

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

void InstallDefaultBinds()
{
    for (const ButtonMapping & mapping : kButtonMap)
    {
        if (mapping.gameBind == nullptr || keybindings[mapping.gameKey] != nullptr)
        {
            continue; // Nothing to bind, or the user's config already bound this key.
        }
        // Key_SetBinding has a legacy non-const signature but copies the string;
        // go through a local buffer to keep -Wwrite-strings happy.
        char bind[64];
        std::snprintf(bind, sizeof(bind), "%s", mapping.gameBind);
        Key_SetBinding(mapping.gameKey, bind);
    }
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// IN_Init / IN_Shutdown
// ------------------------------------------------------------------------------------------------

void IN_Init()
{
    in_joystick          = Cvar_Get("in_joystick",            "1",    CVAR_ARCHIVE);
    s_yawSensitivity     = Cvar_Get("joy_yawsensitivity",     "1",    0);
    s_pitchSensitivity   = Cvar_Get("joy_pitchsensitivity",   "1",    0);
    s_forwardSensitivity = Cvar_Get("joy_forwardsensitivity", "1",    0);
    s_sideSensitivity    = Cvar_Get("joy_sidesensitivity",    "1",    0);
    s_yawThreshold       = Cvar_Get("joy_yawthreshold",       "0.15", 0);
    s_pitchThreshold     = Cvar_Get("joy_pitchthreshold",     "0.15", 0);
    s_forwardThreshold   = Cvar_Get("joy_forwardthreshold",   "0.15", 0);
    s_sideThreshold      = Cvar_Get("joy_sidethreshold",      "0.15", 0);

    if (s_gamepad.Init())
    {
        InstallDefaultBinds();
        Com_Printf("Gamepad input initialised.\n");
    }
}

void IN_Shutdown()
{
    s_gamepad.Shutdown();
}

// ------------------------------------------------------------------------------------------------
// IN_Frame - once per client frame: advance the pad state machine and poll it
// ------------------------------------------------------------------------------------------------

void IN_Frame()
{
    s_gamepad.Update();
}

// ------------------------------------------------------------------------------------------------
// IN_Commands - emit key events for button state changes
// ------------------------------------------------------------------------------------------------

void IN_Commands()
{
    const u16 buttons = s_gamepad.Buttons();
    const u16 changed = static_cast<u16>(buttons ^ s_oldButtons);
    if (changed == 0)
    {
        return;
    }
    s_oldButtons = buttons;

    const bool uiFocus = (cls.key_dest != key_game);

    for (int i = 0; i < kNumButtons; ++i)
    {
        const ButtonMapping & mapping = kButtonMap[i];
        if ((changed & mapping.padButton) == 0)
        {
            continue;
        }

        if ((buttons & mapping.padButton) != 0)
        {
            const int key = uiFocus ? mapping.uiKey : mapping.gameKey;
            s_heldKeys[i] = key;
            Key_Event(key, true, sys_frame_time);
        }
        else if (s_heldKeys[i] != 0)
        {
            Key_Event(s_heldKeys[i], false, sys_frame_time);
            s_heldKeys[i] = 0;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// IN_Move - sticks: left rotates the camera, right moves the player
// ------------------------------------------------------------------------------------------------

void IN_Move(usercmd_t * cmd)
{
    if (!s_gamepad.AnalogValid() || in_joystick->value == 0.0f)
    {
        return;
    }
    if (cls.key_dest != key_game)
    {
        return; // Sticks shouldn't drive the player while a menu/the console has focus.
    }

    const bool running = (((in_speed.state & 1) ^ static_cast<int>(cl_run->value)) != 0);
    const float speed = running ? 2.0f : 1.0f;
    const float angleSpeed = speed * cls.frametime;

    // Left stick rotates the camera. Stick right = turn right (yaw decreases),
    // stick up = look up (pitch decreases); flip a sensitivity cvar to invert.
    const float yaw = s_gamepad.LeftStickX();
    if (std::fabs(yaw) > s_yawThreshold->value)
    {
        cl.viewangles[YAW] -= yaw * s_yawSensitivity->value * angleSpeed * cl_yawspeed->value;
    }

    const float pitch = s_gamepad.LeftStickY();
    if (std::fabs(pitch) > s_pitchThreshold->value)
    {
        cl.viewangles[PITCH] += pitch * s_pitchSensitivity->value * angleSpeed * cl_pitchspeed->value;
    }

    // Right stick moves the player. CL_FinishMove clamps the pitch and packs the
    // final angles/moves after IN_Move returns.
    const float forward = s_gamepad.RightStickY();
    if (std::fabs(forward) > s_forwardThreshold->value)
    {
        cmd->forwardmove = static_cast<short>(static_cast<float>(cmd->forwardmove) -
            (forward * s_forwardSensitivity->value * speed * cl_forwardspeed->value));
    }

    const float side = s_gamepad.RightStickX();
    if (std::fabs(side) > s_sideThreshold->value)
    {
        cmd->sidemove = static_cast<short>(static_cast<float>(cmd->sidemove) +
            (side * s_sideSensitivity->value * speed * cl_sidespeed->value));
    }
}

// ------------------------------------------------------------------------------------------------
// IN_Activate
// ------------------------------------------------------------------------------------------------

void IN_Activate(qboolean active)
{
    (void)active; // No window focus on the PS2.
}

} // extern "C"
