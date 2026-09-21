// ST25R3916 NFC reader driver (Phase 4.4).
//
// Talks to the ST25R3916 at I2C address 0x50 over the shared I2C bus
// (I2C_NUM_1, SDA=GPIO12, SCL=GPIO11, 400 kHz). Uses the ESP-IDF I2C master
// API directly (the I2cDevice class in main.cpp is not accessible from this
// translation unit).
//
// I2C protocol: the first byte written to the I2C address is an OPERATION WORD.
//   Space-A reg write:  [reg & 0x3F, data...]
//   Space-A reg read:   [reg & 0x3F | 0x40] then read data
//   Space-B reg write:  [0xFB, reg & 0x3F, data...]
//   Space-B reg read:   [0xFB, reg & 0x3F | 0x40] then read data
//   Direct command:     [cmd, data...]
//
// No IRQ pin is wired, so tag presence is detected by polling the
// timer-and-NFC interrupt register (0x1B) for the I_cat bit (0x02).

#include "st25r.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>

static const char* kTag = "ST25R";

namespace {

i2c_master_dev_handle_t s_dev = nullptr;
SemaphoreHandle_t       s_mtx = nullptr;

// Space-A register addresses
constexpr uint8_t REG_IO_CONFIG_1 = 0x00;
constexpr uint8_t REG_IO_CONFIG_2 = 0x01;
constexpr uint8_t REG_OP_CTRL     = 0x02;
constexpr uint8_t REG_MODE_DEF    = 0x03;
constexpr uint8_t REG_BITRATE_DEF = 0x04;
constexpr uint8_t REG_ISO14443A   = 0x05;
constexpr uint8_t REG_NFCIP_PT    = 0x08;
constexpr uint8_t REG_AUX_DEF     = 0x0A;
constexpr uint8_t REG_RECV_1      = 0x0B;
constexpr uint8_t REG_RECV_2      = 0x0C;
constexpr uint8_t REG_RECV_3      = 0x0D;
constexpr uint8_t REG_RECV_4      = 0x0E;
constexpr uint8_t REG_MASK_MAIN   = 0x16;
constexpr uint8_t REG_MASK_TIMER  = 0x17;
constexpr uint8_t REG_MASK_ERR    = 0x18;
constexpr uint8_t REG_MASK_PT     = 0x19;
constexpr uint8_t REG_MAIN_INT    = 0x1A;
constexpr uint8_t REG_TIMER_INT   = 0x1B;
constexpr uint8_t REG_ERR_INT     = 0x1C;
constexpr uint8_t REG_PT_INT      = 0x1D;
constexpr uint8_t REG_ANT_TUNE_1  = 0x26;
constexpr uint8_t REG_ANT_TUNE_2  = 0x27;
constexpr uint8_t REG_TX_DRIVER   = 0x28;
constexpr uint8_t REG_PT_MOD      = 0x29;
constexpr uint8_t REG_EFD_ACT     = 0x2A;
constexpr uint8_t REG_EFD_DEACT   = 0x2B;
constexpr uint8_t REG_AUX_DISP    = 0x31;
constexpr uint8_t REG_IC_ID       = 0x3F;

// Space-B register addresses
constexpr uint16_t REG_EMD_SUPP     = 0x0005;
constexpr uint16_t REG_CORR_1       = 0x000C;
constexpr uint16_t REG_CORR_2       = 0x000D;
constexpr uint16_t REG_OVERSHOOT_1  = 0x0030;
constexpr uint16_t REG_OVERSHOOT_2  = 0x0031;
constexpr uint16_t REG_UNDERSHOOT_1 = 0x0032;
constexpr uint16_t REG_UNDERSHOOT_2 = 0x0033;
constexpr uint16_t REG_RES_AM_MOD   = 0x002A;

// Commands
constexpr uint8_t CMD_STOP          = 0xC2;
constexpr uint8_t CMD_SET_DEFAULT   = 0xC1;
constexpr uint8_t CMD_NFC_FIELD_ON  = 0xC8;
constexpr uint8_t CMD_TEST_ACCESS   = 0xFC;
constexpr uint8_t CMD_CLEAR_FIFO    = 0xDB;
constexpr uint8_t CMD_RESET_RX_GAIN = 0xD5;
constexpr uint8_t CMD_ADJUST_REG    = 0xD6;

// Operation-word bits
constexpr uint8_t OP_READ        = 0x40;
constexpr uint8_t SPACE_B_PREFIX = 0xFB;

// Register bits
constexpr uint8_t TX_EN  = 0x08;
constexpr uint8_t RX_EN  = 0x40;
constexpr uint8_t OSC_EN = 0x80;
constexpr uint8_t OSC_OK = 0x10;  // bit in REG_AUX_DISP
constexpr uint8_t I_CAT  = 0x02;  // tag detected, in REG_TIMER_INT (0x1B)

// ---- I2C helpers -----------------------------------------------------------

bool i2c_write(const uint8_t* data, size_t len) {
    return i2c_master_transmit(s_dev, data, len, 100) == ESP_OK;
}

bool i2c_read(const uint8_t* w, size_t wlen, uint8_t* r, size_t rlen) {
    return i2c_master_transmit_receive(s_dev, w, wlen, r, rlen, 100) == ESP_OK;
}

bool write8(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {static_cast<uint8_t>(reg & 0x3F), value};
    return i2c_write(buf, 2);
}

bool read8(uint8_t reg, uint8_t& value) {
    uint8_t op = static_cast<uint8_t>((reg & 0x3F) | OP_READ);
    return i2c_read(&op, 1, &value, 1);
}

bool write8b(uint16_t reg, uint8_t value) {
    uint8_t buf[3] = {SPACE_B_PREFIX, static_cast<uint8_t>(reg & 0x3F), value};
    return i2c_write(buf, 3);
}

bool write16a(uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {
        static_cast<uint8_t>(reg & 0x3F),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return i2c_write(buf, 3);
}

bool cmd(uint8_t c) {
    return i2c_write(&c, 1);
}

bool cmd_data(uint8_t c, const uint8_t* data, size_t len) {
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) return false;
    buf[0] = c;
    std::memcpy(buf + 1, data, len);
    return i2c_write(buf, len + 1);
}

bool rmw8(uint8_t reg, uint8_t set_mask, uint8_t clear_mask) {
    uint8_t v = 0;
    if (!read8(reg, v)) return false;
    uint8_t w = static_cast<uint8_t>((v & ~clear_mask) | set_mask);
    if (w == v) return true;
    return write8(reg, w);
}

// ---- Init sequence (ported from M5Stack M5Unit-NFC, MIT) -------------------

bool do_init() {
    vTaskDelay(pdMS_TO_TICKS(50));

    // Detect chip: type must be 0x05 (ST25R3916), rev non-zero
    uint8_t id = 0;
    bool    detected = false;
    for (int i = 0; i < 5; ++i) {
        if (read8(REG_IC_ID, id)) {
            uint8_t type = static_cast<uint8_t>((id >> 3) & 0x1F);
            uint8_t rev  = static_cast<uint8_t>(id & 0x07);
            if (type == 0x05 && rev != 0) { detected = true; break; }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!detected) {
        ESP_LOGW(kTag, "not detected (id=0x%02X)", id);
        return false;
    }
    ESP_LOGI(kTag, "detected (id=0x%02X)", id);

    // Defensive reset
    cmd(CMD_STOP);
    rmw8(REG_OP_CTRL, 0x00, TX_EN | RX_EN);
    vTaskDelay(pdMS_TO_TICKS(2));

    // 1) Set to default
    if (!cmd(CMD_SET_DEFAULT)) return false;

    // 2) Protection command (prevent overheat protection)
    const uint8_t protection[2] = {0x04, 0x10};
    if (!cmd_data(CMD_TEST_ACCESS, protection, 2)) return false;

    // 3) IO configuration: I2C, 3.3 V supply
    if (!write16a(REG_IO_CONFIG_1, 0x1000 | 0x04 | 0x80)) return false;

    // TX driver: AM modulation, auto
    if (!write8(REG_TX_DRIVER, 0x00)) return false;

    // IO_CONFIG_1: MCU_CLK disabled
    rmw8(REG_IO_CONFIG_1, 0x00, 0x07);

    // Resistive AM modulation + AAT enable
    if (!write8b(REG_RES_AM_MOD, 0x80)) return false;
    rmw8(REG_IO_CONFIG_2, 0x20, 0x00);
    if (!write8b(REG_RES_AM_MOD, 0x00)) return false;

    // External field detector thresholds
    if (!write8(REG_EFD_ACT, 0x13)) return false;
    if (!write8(REG_EFD_DEACT, 0x02)) return false;

    // NFC-IP passive target: FDT + modulation + EMD suppression
    rmw8(REG_NFCIP_PT, 0x05 << 4, 0xF0);
    if (!write8(REG_PT_MOD, 0x5F)) return false;
    if (!write8b(REG_EMD_SUPP, 0x40)) return false;

    // Antenna tuning
    if (!write8(REG_ANT_TUNE_1, 0x82)) return false;
    if (!write8(REG_ANT_TUNE_2, 0x82)) return false;

    // Enable external field detector + clear FIFO
    rmw8(REG_OP_CTRL, 0x03, 0x00);
    cmd(CMD_CLEAR_FIFO);

    // 4) Mask all interrupts (except error), clear latched interrupts
    if (!write8(REG_MASK_MAIN,  0xFF)) return false;
    if (!write8(REG_MASK_TIMER, 0xFF)) return false;
    if (!write8(REG_MASK_ERR,   0x00)) return false;
    if (!write8(REG_MASK_PT,    0xFF)) return false;
    uint8_t discard;
    read8(REG_MAIN_INT, discard);
    read8(REG_TIMER_INT, discard);
    read8(REG_ERR_INT, discard);
    read8(REG_PT_INT, discard);

    // Enable oscillator and wait for it to stabilise
    uint8_t oc = 0;
    if (read8(REG_OP_CTRL, oc) && (oc & OSC_EN) == 0) {
        uint8_t mask = 0;
        if (read8(REG_MASK_MAIN, mask)) write8(REG_MASK_MAIN, static_cast<uint8_t>(mask & ~0x80));
        read8(REG_MAIN_INT, discard);
        write8(REG_OP_CTRL, static_cast<uint8_t>(oc | OSC_EN));
        bool osc_ok = false;
        for (int i = 0; i < 50; ++i) {
            uint8_t aux = 0;
            if (read8(REG_AUX_DISP, aux) && (aux & OSC_OK)) { osc_ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (read8(REG_MASK_MAIN, mask)) write8(REG_MASK_MAIN, static_cast<uint8_t>(mask | 0x80));
        if (!osc_ok) {
            ESP_LOGW(kTag, "oscillator did not stabilise");
            return false;
        }
    }

    // Adjust regulators
    if (!cmd(CMD_ADJUST_REG)) return false;
    vTaskDelay(pdMS_TO_TICKS(5));

    // 5) Configure NFC-A (ISO14443A) initiator mode
    if (!write8(REG_MODE_DEF, static_cast<uint8_t>((0x01 << 3) | 0x01))) return false;
    if (!write8(REG_BITRATE_DEF, 0x00)) return false;
    if (!write8(REG_ISO14443A, 0x00)) return false;
    rmw8(REG_AUX_DEF, 0x00, 0x04);
    if (!write8b(REG_OVERSHOOT_1, 0x40)) return false;
    if (!write8b(REG_OVERSHOOT_2, 0x03)) return false;
    if (!write8b(REG_UNDERSHOOT_1, 0x40)) return false;
    if (!write8b(REG_UNDERSHOOT_2, 0x03)) return false;
    if (!write8b(REG_CORR_1, 0x47)) return false;
    if (!write8b(REG_CORR_2, 0x00)) return false;
    if (!write8(REG_RECV_1, 0x08)) return false;
    if (!write8(REG_RECV_2, 0x2D)) return false;
    if (!write8(REG_RECV_3, 0xD8)) return false;
    if (!write8(REG_RECV_4, 0x22)) return false;
    if (!cmd(CMD_RESET_RX_GAIN)) return false;

    // Unmask the interrupts needed for polling:
    //   REG_MASK_MAIN  = 0x83  (unmask I_osc, I_cac, I_cat)
    //   REG_MASK_TIMER = 0xFD  (unmask I_nre)
    if (!write8(REG_MASK_MAIN,  0x83)) return false;
    if (!write8(REG_MASK_TIMER, 0xFD)) return false;
    if (!write8(REG_MASK_ERR,   0x00)) return false;
    if (!write8(REG_MASK_PT,    0xFF)) return false;

    // 6) Field ON
    if (!cmd(CMD_NFC_FIELD_ON)) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
    rmw8(REG_OP_CTRL, TX_EN | RX_EN, 0x00);

    ESP_LOGI(kTag, "ready: NFC-A initiator, field ON");
    return true;
}

}  // anonymous namespace

namespace st25r {

bool init(i2c_master_bus_handle_t bus) {
    if (s_dev) return true;  // already initialised
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address  = kI2cAddress;
    config.scl_speed_hz    = 400 * 1000;
    if (i2c_master_bus_add_device(bus, &config, &s_dev) != ESP_OK) {
        ESP_LOGW(kTag, "I2C add 0x%02X failed", kI2cAddress);
        return false;
    }
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = do_init();
    xSemaphoreGive(s_mtx);
    if (!ok) {
        // Leave the device on the bus (harmless); just mark as not ready.
        // The caller treats NFC as unavailable.
    }
    return ok;
}

bool poll_tag_present() {
    if (!s_dev) return false;
    uint8_t timer_int = 0;
    if (!read8(REG_TIMER_INT, timer_int)) return false;
    if ((timer_int & I_CAT) == 0) return false;
    // Clear latched interrupts so the next tag tap re-triggers
    uint8_t discard;
    read8(REG_MAIN_INT, discard);
    read8(REG_TIMER_INT, discard);
    return true;
}

}  // namespace st25r
