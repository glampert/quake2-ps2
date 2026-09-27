/* ================================================================================================
 * File: rumble.cpp
 * Brief: Force feedback - the IN_Rumble* hooks the client calls, the effect each gameplay
 *        event plays, and the mixer that overlaps effects onto the two motors.
 *        See rumble.h for the overview.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/hash.h"
#include "ps2/input/pad.h"
#include "ps2/input/rumble.h"

// Client code like the rest of the input backend: events are read off the local
// player's state in cl.frame, and whether to rumble at all depends on cls. The
// legacy headers redeclare a few q_common.h functions, hence the pragma.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
}
#pragma GCC diagnostic pop

#include <algorithm>
#include <cstring>

extern "C" {
// Referenced by the options menu (menu.c).
cvar_t * in_rumble = nullptr;
} // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------
// Effects
// ------------------------------------------------------------------------------------------------

// One burst of vibration. The pad has two motors: a small one that is either on or
// off - a light, high-pitched buzz - and a large one with a variable speed for the
// heavy rumble. An effect can use either or both; a zero duration leaves one out.
struct RumbleEffect
{
    const char * name; // what in_rumbledebug prints
    u8  largeSpeed;
    u16 largeMs;
    u16 smallMs;
};

// The local player's shots, keyed by the muzzle flash the server sends for each one
// (MZ_*, silenced bit stripped). The rapid-fire weapons flash every server frame,
// 100 ms apart: the machinegun's pulses are shorter than that so each shot stands
// out, while the chaingun's and hyperblaster's run longer and blend into a steady
// rumble - one that grows as the chaingun spins up to 2 and 3 shots per frame.
// Muzzle flashes not listed here aren't shots (MZ_LOGIN, MZ_RESPAWN, ...) or belong
// to the mission packs' weapons, and play nothing.
struct WeaponRumble
{
    int weapon;
    RumbleEffect effect;
};

constexpr WeaponRumble kWeaponRumbles[] = {
    //                                      large  large  small
    //                                      speed     ms     ms
    { MZ_BLASTER,      { "blaster",          0x80,    90,    80 } },
    { MZ_HYPERBLASTER, { "hyperblaster",     0x60,   120,   120 } },
    { MZ_MACHINEGUN,   { "machinegun",       0xE1,    80,    70 } },
    { MZ_CHAINGUN1,    { "chaingun x1",      0x90,   120,   120 } },
    { MZ_CHAINGUN2,    { "chaingun x2",      0xB0,   120,   120 } },
    { MZ_CHAINGUN3,    { "chaingun x3",      0xD0,   120,   120 } },
    { MZ_SHOTGUN,      { "shotgun",          0xD0,   180,   120 } },
    { MZ_SSHOTGUN,     { "super shotgun",    0xFF,   280,   180 } },
    { MZ_GRENADE,      { "grenade launcher", 0xA0,   150,    80 } },
    { MZ_ROCKET,       { "rocket launcher",  0xD0,   200,   120 } },
    { MZ_RAILGUN,      { "railgun",          0xFF,   250,   200 } },
    { MZ_BFG,          { "bfg",              0xE0,   900,   300 } }, // Flashes as the charge-up starts.
};

// Hand grenades send no muzzle flash; see CheckGrenadeThrow.
constexpr RumbleEffect kGrenadeThrowRumble = { "hand grenade", 0x90, 120, 60 };

// Sounds on the local player's item channel, which is where Touch_Item plays the
// pickup sound (g_items.c), from everyday pickups to rare finds. The channel also
// carries powerup warnings and quad damage shots, which aren't listed and play nothing.
// Keyed by the name's hash alone, with no name check on a match: the channel only
// carries the game's own couple dozen sounds.
struct ItemSoundRumble
{
    u64 soundHash; // ps2::HashStr64 of the sound's name
    RumbleEffect effect;
};

constexpr ItemSoundRumble kItemSoundRumbles[] = {
    //                                                       large large small
    //                                                       speed    ms    ms
    { ps2::HashStr64("misc/am_pkup.wav"),   { "ammo",         0x60, 100, 100 } },
    { ps2::HashStr64("misc/ar2_pkup.wav"),  { "armor shard",  0x60, 100, 100 } },
    { ps2::HashStr64("items/s_health.wav"), { "small health", 0x60, 100, 100 } },
    { ps2::HashStr64("items/n_health.wav"), { "health",       0x70, 120, 120 } },
    { ps2::HashStr64("items/l_health.wav"), { "large health", 0x80, 150, 120 } },
    { ps2::HashStr64("misc/ar1_pkup.wav"),  { "armor",        0x90, 150, 120 } },
    { ps2::HashStr64("misc/ar3_pkup.wav"),  { "power armor",  0x90, 150, 120 } },
    { ps2::HashStr64("misc/w_pkup.wav"),    { "weapon",       0xA0, 180, 120 } },
    { ps2::HashStr64("items/m_health.wav"), { "mega health",  0xC0, 300, 200 } },
    // Powerups, keys, adrenaline, the bandolier, the ammo pack and the ancient head.
    { ps2::HashStr64("items/pkup.wav"),     { "special item", 0xC0, 300, 200 } },
};

// A timed powerup coming on: quad damage, invulnerability, the environment suit or
// the rebreather. See CheckPowerups.
constexpr RumbleEffect kPowerupRumble = { "powerup on", 0xFF, 500, 400 };

// Damage taken scales from the weakest rumble at 0 up to the strongest at
// kHeavyDamage and beyond: a direct rocket hit, a railgun slug.
constexpr int kHeavyDamage = 50;
constexpr int kDamageMinSpeed = 0x70;

RumbleEffect DamageEffect(int damage)
{
    const int severity = std::min(damage, kHeavyDamage);
    RumbleEffect effect = { "damage", 0, 0, 120 };
    effect.largeSpeed = static_cast<u8>(kDamageMinSpeed + (0xFF - kDamageMinSpeed) * severity / kHeavyDamage);
    effect.largeMs = static_cast<u16>(150 + 250 * severity / kHeavyDamage);
    return effect;
}

// ------------------------------------------------------------------------------------------------
// RumbleMixer
// ------------------------------------------------------------------------------------------------

// Overlaps the effects playing onto the two motors: the small one runs while any
// effect still wants it, the large one at the highest speed any running effect asks
// for. Effects are flat pulses, so the motor values only change as a pulse starts or
// ends - which keeps the IOP calls behind GamePad::SetMotors down to a few.
class RumbleMixer final
{
public:
    void Play(const RumbleEffect & effect, u32 nowMs);
    void Stop() { *this = RumbleMixer{}; }

    bool SmallOn(u32 nowMs) const { return Running(m_small, nowMs); }
    u8 LargeSpeed(u32 nowMs) const;

private:
    struct Pulse
    {
        u32 endMs = 0;
        u8  speed = 0; // 0 = slot unused. The small motor's pulse only uses 1.
    };

    // Plenty: effects last under a second, and a server frame starts a few at most.
    static constexpr int kMaxLargePulses = 8;

    // Wrap-safe ordering of two times on the millisecond clock.
    static bool Before(u32 a, u32 b) { return static_cast<s32>(a - b) < 0; }
    static bool Running(const Pulse & pulse, u32 nowMs) { return pulse.speed != 0 && Before(nowMs, pulse.endMs); }

    Pulse m_small;
    Pulse m_large[kMaxLargePulses];
};

void RumbleMixer::Play(const RumbleEffect & effect, u32 nowMs)
{
    if (effect.smallMs != 0)
    {
        const u32 endMs = nowMs + effect.smallMs;
        if (!Running(m_small, nowMs) || Before(m_small.endMs, endMs))
        {
            m_small = { endMs, 1 };
        }
    }

    if (effect.largeSpeed != 0 && effect.largeMs != 0)
    {
        // A free slot, or else the pulse closest to its end.
        Pulse * slot = &m_large[0];
        for (Pulse & pulse : m_large)
        {
            if (!Running(pulse, nowMs))
            {
                slot = &pulse;
                break;
            }
            if (Before(pulse.endMs, slot->endMs))
            {
                slot = &pulse;
            }
        }
        *slot = { nowMs + effect.largeMs, effect.largeSpeed };
    }
}

u8 RumbleMixer::LargeSpeed(u32 nowMs) const
{
    u8 speed = 0;
    for (const Pulse & pulse : m_large)
    {
        if (Running(pulse, nowMs) && pulse.speed > speed)
        {
            speed = pulse.speed;
        }
    }
    return speed;
}

// ------------------------------------------------------------------------------------------------
// State + helpers
// ------------------------------------------------------------------------------------------------

static const cvar_t * s_rumbleDebug = nullptr;
static ps2::input::GamePad * s_pad = nullptr;
static RumbleMixer s_mixer;

Q_ALWAYS_INLINE u32 NowMs()
{
    return static_cast<u32>(Sys_Milliseconds());
}

// Whether gameplay events should rumble right now; see UpdateRumble.
bool RumbleWanted()
{
    return s_pad != nullptr &&
           in_rumble->value != 0.0f &&
           cls.state == ca_active &&
           cls.key_dest == key_game &&
           cls.disable_screen == 0.0f &&
           cl_paused->value == 0.0f &&
           !cl.attractloop;
}

void Play(const RumbleEffect & effect)
{
    if (s_rumbleDebug->value != 0.0f)
    {
        Com_Printf("Rumble: %s - large motor %d for %d ms, small motor %d ms\n",
                   effect.name, effect.largeSpeed, effect.largeMs, effect.smallMs);
    }
    s_mixer.Play(effect, NowMs());
}

// Damage taken. STAT_FLASHES is only set on the frames the player got hurt (1 = in
// health, 2 = armor absorbed some), which a health drop alone can't tell apart from
// the mega health wearing off; the drops then give the amount.
void CheckDamage(const short * stats, const short * oldStats)
{
    const int flashes = stats[STAT_FLASHES];
    if (flashes == 0)
    {
        return;
    }

    int damage = std::max(oldStats[STAT_HEALTH] - stats[STAT_HEALTH], 0);
    if ((flashes & 2) != 0)
    {
        damage += std::max(oldStats[STAT_ARMOR] - stats[STAT_ARMOR], 0);
    }
    Play(DamageEffect(damage));
}

// A timed powerup coming on. The HUD timer only shows the powerup with the highest
// priority (G_SetStats, p_hud.c), so this sees its icon appear, or its timer jump
// back up when another of the same kind stacks onto it. The icon also changes when
// that powerup runs out and uncovers one still running, but only after the timer
// counted down to 0. One switched on beneath a higher-priority powerup doesn't show
// on the timer at all, and plays nothing.
void CheckPowerups(const short * stats, const short * oldStats)
{
    const int icon = stats[STAT_TIMER_ICON];
    const int oldIcon = oldStats[STAT_TIMER_ICON];
    if (icon == 0)
    {
        return;
    }

    const bool cameOn = (icon != oldIcon) ? (oldIcon == 0 || oldStats[STAT_TIMER] > 0)
                                          : (stats[STAT_TIMER] > oldStats[STAT_TIMER]);
    if (cameOn)
    {
        Play(kPowerupRumble);
    }
}

// Hand grenades send no muzzle flash, so a throw is read off the view weapon: it
// steps off kGrenadeThrowFrame on the frame the grenade leaves the hand
// (Weapon_Grenade, p_weapon.c). A grenade held until it blows up skips that frame -
// and hurts, which rumbles as damage.
constexpr int kGrenadeThrowFrame = 12;

void CheckGrenadeThrow(const player_state_t & state, const player_state_t & oldState)
{
    if (oldState.gunframe == kGrenadeThrowFrame &&
        state.gunframe == kGrenadeThrowFrame + 1 &&
        state.gunindex == oldState.gunindex &&
        std::strcmp(cl.configstrings[CS_MODELS + state.gunindex], "models/weapons/v_handgr/tris.md2") == 0)
    {
        Play(kGrenadeThrowRumble);
    }
}

} // namespace

namespace ps2::input {

// ------------------------------------------------------------------------------------------------
// InitRumble / UpdateRumble
// ------------------------------------------------------------------------------------------------

void InitRumble(GamePad & pad)
{
    in_rumble      = Cvar_Get("in_rumble",      "1", CVAR_ARCHIVE);
    s_rumbleDebug  = Cvar_Get("in_rumbledebug", "0", 0);
    s_pad          = &pad;
}

void UpdateRumble()
{
    if (!RumbleWanted())
    {
        IN_RumbleStop(); // Drops the effects too, so none resumes later.
        return;
    }

    const u32 nowMs = NowMs();
    s_pad->SetMotors(s_mixer.SmallOn(nowMs), s_mixer.LargeSpeed(nowMs));
}

} // namespace ps2::input

extern "C" {

// ------------------------------------------------------------------------------------------------
// IN_Rumble* - the client's force feedback hooks (client/input.h)
// ------------------------------------------------------------------------------------------------

void IN_RumbleMuzzleFlash(int weapon)
{
    if (!RumbleWanted())
    {
        return;
    }
    for (const WeaponRumble & entry : kWeaponRumbles)
    {
        if (entry.weapon == weapon)
        {
            Play(entry.effect);
            return;
        }
    }
}

void IN_RumbleItemSound(const char * sound)
{
    if (!RumbleWanted())
    {
        return;
    }

    const u64 soundHash = ps2::HashStr64(sound);
    for (const ItemSoundRumble & entry : kItemSoundRumbles)
    {
        if (entry.soundHash == soundHash)
        {
            Play(entry.effect);
            return;
        }
    }
}

void IN_RumbleFrame()
{
    if (!RumbleWanted())
    {
        return;
    }

    // Events are changes from the frame before, so there has to be one. There isn't
    // on the first frame of a level or a loaded game, where nothing happened.
    const frame_t & oldFrame = cl.frames[(cl.frame.serverframe - 1) & UPDATE_MASK];
    if (!oldFrame.valid || oldFrame.serverframe != cl.frame.serverframe - 1)
    {
        return;
    }

    const player_state_t & state = cl.frame.playerstate;
    const player_state_t & oldState = oldFrame.playerstate;
    CheckDamage(state.stats, oldState.stats);
    CheckPowerups(state.stats, oldState.stats);
    CheckGrenadeThrow(state, oldState);
}

void IN_RumbleStop()
{
    s_mixer.Stop();
    if (s_pad != nullptr)
    {
        s_pad->SetMotors(false, 0);
    }
}

} // extern "C"
