#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

// ST25R3916 NFC reader driver (Phase 4.4).
//
// This driver talks to the ST25R3916 at I2C address 0x50 over the shared I2C
// bus (g_i2c_bus: I2C_NUM_1, SDA=GPIO12, SCL=GPIO11, 400kHz). It reuses the
// existing ESP-IDF I2C master bus and the I2cDevice op-word protocol from
// main.cpp — the 1-byte I2C "register" is an OPERATION WORD:
//     (register_or_command & 0x3F) | op,   op = 0x00 (write) | 0x40 (read)
// Space-B registers are prefixed with the 0xFB register-space-access byte.
//
// The chip has no dedicated IRQ pin wired here, so tag presence is detected by
// polling the interrupt registers (see poll_tag_present()).

namespace st25r {

// I2C device address of the ST25R3916.
constexpr uint8_t kI2cAddress = 0x50;

// Initialise the ST25R3916 on the given shared I2C bus.
//
// Runs the hardware-verified power-on sequence (ported from the M5Stack
// reference driver's begin()) and leaves the reader in NFC-A (ISO14443A)
// initiator mode with the RF field ON.
//
// Returns true if the chip was detected and configured. A missing or
// unresponsive chip returns false — this is NON-fatal by design: the caller
// must treat NFC as unavailable and simply stop polling.
bool init(i2c_master_bus_handle_t bus);

// Poll for a present target (NFC tag).
//
// Reads the chip's interrupt registers and returns whether the I_cat bit
// ("IRQ after minimum guard time expire" = a target was detected) is set.
// Reading the registers clears the latched interrupts, so this is a
// single-consumption check. Returns false when not initialised.
bool poll_tag_present();

}  // namespace st25r
