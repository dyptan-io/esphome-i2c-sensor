#include "chirp.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace chirp {

static const char *const TAG = "chirp";

// Register and command opcodes — see https://github.com/Miceuz/i2c-moisture-sensor#protocol
static const uint8_t REG_GET_CAPACITANCE = 0x00;   // r, 2 bytes
static const uint8_t REG_SET_ADDRESS = 0x01;       // w, 1 byte
static const uint8_t REG_GET_ADDRESS = 0x02;       // r, 1 byte
static const uint8_t CMD_MEASURE_LIGHT = 0x03;     // w, 0 bytes
static const uint8_t REG_GET_LIGHT = 0x04;         // r, 2 bytes
static const uint8_t REG_GET_TEMPERATURE = 0x05;   // r, 2 bytes
static const uint8_t CMD_RESET = 0x06;             // w, 0 bytes
static const uint8_t REG_GET_VERSION = 0x07;       // r, 1 byte
static const uint8_t CMD_SLEEP = 0x08;             // w, 0 bytes

// Time the sensor needs after RESET before it answers register reads.
static const uint32_t RESET_SETTLE_MS = 1000;
// Integration time for a light measurement; GET_LIGHT returns 0xFFFF if read sooner.
static const uint32_t LIGHT_INTEGRATION_MS = 3000;
// Sentinel value returned by the sensor for an unavailable reading.
static const uint16_t INVALID_READING = 0xFFFF;
// Delay between writing a register address (STOP) and reading the result (START).
// The AVR firmware can't service a repeated-start read in time, so transactions
// must be split with a brief wait — see Miceuz/i2c-moisture-sensor protocol notes.
static const uint32_t REGISTER_READ_DELAY_MS = 20;

void I2CSoilMoistureComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Chirp sensor...");

  if (this->illuminance_sensor_ != nullptr && this->get_update_interval() < LIGHT_INTEGRATION_MS) {
    this->error_code_ = ErrorCode::UPDATE_INTERVAL_TOO_SHORT;
    this->mark_failed();
    return;
  }

  if (this->new_address_ != 0 && !this->apply_new_address_()) {
    this->error_code_ = ErrorCode::COMMUNICATION_FAILED;
    this->mark_failed();
    return;
  }

  if (this->write(&CMD_RESET, 1) != i2c::ERROR_OK) {
    this->error_code_ = ErrorCode::COMMUNICATION_FAILED;
    this->mark_failed();
    return;
  }

  this->set_timeout("init", RESET_SETTLE_MS, [this]() { this->finish_setup_(); });
}

void I2CSoilMoistureComponent::finish_setup_() {
  if (this->read_register(REG_GET_VERSION, &this->firmware_version_, 1) != i2c::ERROR_OK ||
      this->firmware_version_ == 0) {
    this->error_code_ = ErrorCode::COMMUNICATION_FAILED;
    this->mark_failed();
    return;
  }

  this->initialized_ = true;
  ESP_LOGCONFIG(TAG, "Chirp sensor ready (firmware 0x%02X).", this->firmware_version_);
}

void I2CSoilMoistureComponent::update() {
  if (!this->initialized_) {
    return;
  }

  bool ok = true;
  if (this->moisture_sensor_ != nullptr) {
    ok &= this->read_moisture_();
  }
  if (this->temperature_sensor_ != nullptr) {
    ok &= this->read_temperature_();
  }

  if (this->illuminance_sensor_ == nullptr) {
    if (!ok) {
      this->status_set_warning();
    } else {
      this->status_clear_warning();
    }
    this->sleep_();
    return;
  }

  if (!this->start_light_measurement_()) {
    this->status_set_warning();
    return;
  }

  // SLEEP must wait until after the light read — putting the sensor to sleep
  // while a light measurement is in flight aborts it and GET_LIGHT returns 0xFFFF.
  this->set_timeout("light", LIGHT_INTEGRATION_MS, [this, ok]() {
    bool all_ok = ok && this->read_illuminance_();
    if (all_ok) {
      this->status_clear_warning();
    } else {
      this->status_set_warning();
    }
    this->sleep_();
  });
}

void I2CSoilMoistureComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Chirp I2C Soil Moisture Sensor:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);

  switch (this->error_code_) {
    case ErrorCode::COMMUNICATION_FAILED:
      ESP_LOGE(TAG, "  Communication with sensor failed.");
      break;
    case ErrorCode::UPDATE_INTERVAL_TOO_SHORT:
      ESP_LOGE(TAG, "  update_interval must be at least %ums when illuminance is configured.",
               LIGHT_INTEGRATION_MS);
      break;
    case ErrorCode::NONE:
      ESP_LOGCONFIG(TAG, "  Firmware version: 0x%02X", this->firmware_version_);
      break;
  }

  LOG_SENSOR("  ", "Moisture", this->moisture_sensor_);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
  LOG_SENSOR("  ", "Illuminance", this->illuminance_sensor_);
}

bool I2CSoilMoistureComponent::apply_new_address_() {
  uint8_t current = 0;
  if (this->read_register(REG_GET_ADDRESS, &current, 1) != i2c::ERROR_OK) {
    return false;
  }

  if (current == this->new_address_) {
    return true;
  }

  ESP_LOGCONFIG(TAG, "Changing I2C address from 0x%02X to 0x%02X.", current, this->new_address_);

  // From firmware 0x26 the address write must be repeated to guard against spurious changes.
  if (this->write_register(REG_SET_ADDRESS, &this->new_address_, 1) != i2c::ERROR_OK ||
      this->write_register(REG_SET_ADDRESS, &this->new_address_, 1) != i2c::ERROR_OK) {
    return false;
  }

  this->set_i2c_address(this->new_address_);
  ESP_LOGW(TAG, "I2C address changed to 0x%02X — restart required.", this->new_address_);
  return true;
}

bool I2CSoilMoistureComponent::start_light_measurement_() {
  return this->write(&CMD_MEASURE_LIGHT, 1) == i2c::ERROR_OK;
}

bool I2CSoilMoistureComponent::sleep_() {
  return this->write(&CMD_SLEEP, 1) == i2c::ERROR_OK;
}

bool I2CSoilMoistureComponent::read_moisture_() {
  uint8_t buffer[2];
  if (this->read_register(REG_GET_CAPACITANCE, buffer, 2) != i2c::ERROR_OK) {
    return false;
  }

  uint16_t raw = encode_uint16(buffer[0], buffer[1]);
  ESP_LOGD(TAG, "Capacitance raw: %u", raw);

  if (raw == INVALID_READING) {
    return false;
  }

  float value = raw;
  if (!this->moisture_raw_) {
    uint16_t clamped = clamp(raw, this->moisture_min_, this->moisture_max_);
    value = (clamped - this->moisture_min_) * 100.0f / (this->moisture_max_ - this->moisture_min_);
  }
  this->moisture_sensor_->publish_state(value);
  return true;
}

bool I2CSoilMoistureComponent::read_temperature_() {
  uint8_t buffer[2];
  if (this->read_register(REG_GET_TEMPERATURE, buffer, 2) != i2c::ERROR_OK) {
    return false;
  }

  // Sensor reports a signed 16-bit value in tenths of degrees Celsius.
  int16_t raw = static_cast<int16_t>(encode_uint16(buffer[0], buffer[1]));
  ESP_LOGD(TAG, "Temperature raw: %d", raw);

  this->temperature_sensor_->publish_state(raw / 10.0f);
  return true;
}

bool I2CSoilMoistureComponent::read_illuminance_() {
  // The light register is the one read whose timing the AVR firmware can't
  // service via repeated-START; it must be a separate write+read with a
  // brief gap, otherwise GET_LIGHT returns 0xFFFF. Copy the register into
  // a RAM-backed local — some ESP I2C drivers can't DMA from .rodata
  // (where file-scope `static const` lands).
  uint8_t reg = REG_GET_LIGHT;
  auto write_err = this->write(&reg, 1);
  if (write_err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Light read: write(reg) failed, err=%d", write_err);
    return false;
  }
  delay(REGISTER_READ_DELAY_MS);
  uint8_t buffer[2];
  auto read_err = this->read(buffer, 2);
  if (read_err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Light read: read(2) failed, err=%d", read_err);
    return false;
  }

  uint16_t raw = encode_uint16(buffer[0], buffer[1]);
  ESP_LOGD(TAG, "Light raw: %u", raw);

  if (raw == INVALID_READING) {
    return false;
  }

  float value = raw;
  if (!this->illuminance_raw_) {
    value = this->illuminance_coefficient_ * raw + this->illuminance_constant_;
  }
  this->illuminance_sensor_->publish_state(value);
  return true;
}

}  // namespace chirp
}  // namespace esphome
