#pragma once
/* ================================================================================================
 * File: pad.h
 * Brief: DualShock gamepad abstraction over libpad. The GamePad class owns the pad
 *        connection lifecycle and per-frame polling, exposing the button mask and
 *        the normalised analog sticks. The Quake input seam (input.cpp) drives a
 *        single static instance and maps its state onto key events and movement.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstdint>
#include <libpad.h> // padButtonStatus + PAD_* button bits

namespace ps2::input {

class GamePad final
{
public:
    // Brings up SIF RPC, loads the IOP pad modules and opens connector 1. Returns
    // false if the gamepad is unavailable, after which the accessors report a
    // neutral, disconnected pad forever.
    bool Init();
    void Shutdown();

    // Advances the connection state machine and, once connected, polls the pad.
    // Call exactly once per client frame before querying the state below.
    void Update();

    // Currently-pressed buttons as an active-high mask of PAD_* bits
    // (0 while disconnected).
    std::uint16_t Buttons() const { return m_buttons; }

    // True when the analog sticks carry meaningful data: a DualShock in analog
    // mode that polled successfully this frame. Digital pads and the brief
    // connect/mode-switch window report false.
    bool AnalogValid() const { return m_analogValid; }

    // Analog sticks normalised to [-1, +1] in raw hardware orientation: X grows to
    // the right, Y grows downward. Only meaningful while AnalogValid().
    float LeftStickX() const;
    float LeftStickY() const;
    float RightStickX() const;
    float RightStickY() const;

private:
    enum class Status : std::uint8_t
    {
        Unavailable,  // IOP modules or the port failed - pad permanently off
        Disconnected, // waiting for a pad to connect and stabilise
        SettingMode,  // analog (DualShock) mode requested, awaiting completion
        Ready         // connected and delivering data
    };

    static bool Connected(int state);

    Status m_status = Status::Unavailable;
    padButtonStatus m_data{};    // last good padRead() result
    std::uint16_t m_buttons = 0; // active-high pressed mask
    bool m_analogValid = false;

    // libpad DMA transfer area: 256 bytes, 64-byte aligned.
    alignas(64) char m_dmaArea[256];
};

} // namespace ps2::input
