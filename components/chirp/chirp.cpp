#include "chirp.h"
#include "esphome/core/log.h"

namespace esphome {
namespace chirp {

static const char *const TAG = "I2CSensor";

enum RegisterAddress : uint8_t {
  GET_CAPACITANCE = 0x00,  // (r)  2 bytes
  SET_ADDRESS = 0x01,      // (w)  1 byte
  GET_ADDRESS = 0x02,      // (r)  1 byte
  GET_LIGHT = 0x04,        // (r)  2 bytes
  GET_TEMPERATURE = 0x05,  // (r)  2 bytes
  GET_VERSION = 0x07,      // (r)  1 bytes
  GET_BUSY = 0x09          // (r)  1 bytes
};

static uint8_t MEASURE_LIGHT = 0x03;
static uint8_t RESET = 0x06;
static uint8_t SLEEP = 0x08;

void I2CSoilMoistureComponent::update() {
  if (!device_.started) {
    setup();
    return;
  }

  int reads = 0;
  int failures = 0;

  if (moisture_ != nullptr) {
    reads++;
    if (!read_moisture_()) {
      failures++;
      status_set_warning("Failed to read moisture.");
    }
  }

  if (temperature_ != nullptr) {
    reads++;
    if (!read_temperature_()) {
      failures++;
      status_set_warning("Failed to read temperature.");
    }
  }

  if (reads > 0 && failures == reads) {
    mark_failed_();
    return;
  }

  if (light_ != nullptr) {
    if (get_update_interval() < 3000) {
      status_set_warning("Update interval is too short for light measurements.");
      return;
    }

    if (write_register(device_.addr, &MEASURE_LIGHT, 1) != i2c::ERROR_OK) {
      status_set_warning("Failed to start light measurememnts.");
    }

    set_timeout("read_light_", 3000 - 100, [this]() {
      if (!read_light_()) {
        status_set_warning("Failed to read light.");
      }
    });
  }

  if (write_register(device_.addr, &SLEEP, 1) != i2c::ERROR_OK) {
    status_set_warning("Failed to sleep.");
  }
}

void I2CSoilMoistureComponent::setup() {
  if (!device_.failure_logged) {
    ESP_LOGD(TAG, "Initializing sensor...");
  }

  if (!write_address(device_.new_addr)) {
    mark_failed_();
    return;
  }

  if (!write_reset_()) {
    mark_failed_();
    return;
  }

  set_timeout("start", 1000, [this]() {
    uint8_t version = read_version_();

    if (version == 0) {
      mark_failed_();
      return;
    }

    if (device_.failure_logged) {
      ESP_LOGCONFIG(TAG, "Sensor recovered.");
    } else {
      ESP_LOGCONFIG(TAG, "Sensor started.");
    }
    ESP_LOGCONFIG(TAG, "Firmware Version: 0x%02X", version);

    device_.started = true;
    device_.failure_logged = false;
    status_clear_error();
    status_clear_warning();
  });
}

void I2CSoilMoistureComponent::mark_failed_() {
  device_.started = false;

  if (device_.failure_logged) {
    return;
  }

  ESP_LOGCONFIG(TAG, "Failed to initialize sensor, will keep retrying.");
  status_set_error(LOG_STR("Sensor not responding."));
  device_.failure_logged = true;
}

void I2CSoilMoistureComponent::dump_config() {
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);

  LOG_SENSOR("  ", "Moisture", moisture_);
  LOG_SENSOR("  ", "Temperature", temperature_);
  LOG_SENSOR("  ", "Light", light_);
}

bool I2CSoilMoistureComponent::write_address(uint8_t new_addr) {
  device_.addr = read_address_();
  device_.new_addr = new_addr;

  if (device_.addr == 0) {
    return false;
  }

  ESP_LOGCONFIG(TAG, "Current address: 0x%02X", device_.addr);

  if (new_addr == 0) {
    ESP_LOGCONFIG(TAG, "New address not set.");
    return true;
  }

  if (device_.addr == new_addr) {
    ESP_LOGCONFIG(TAG, "Address already set to 0x%02X", new_addr);
    return true;
  }

  if (write_register(SET_ADDRESS, &new_addr, 1) != i2c::ERROR_OK) {
    status_set_error(LOG_STR("Failed to write address."));
    return false;
  }

  // The second request is required since FW 0x26 to protect agains spurious address changes.
  if (write_register(SET_ADDRESS, &new_addr, 1) != i2c::ERROR_OK) {
    status_set_error(LOG_STR("Failed to write address."));
    return false;
  }

  device_.addr = new_addr;
  set_i2c_address(new_addr);
  status_set_error(LOG_STR("I2C address was changed. Restart is required."));

  return true;
}

bool I2CSoilMoistureComponent::read_moisture_() {
  uint8_t buffer[2];

  if (read_register(GET_CAPACITANCE, buffer, 2) != i2c::ERROR_OK) {
    return false;
  }

  uint16_t raw = (buffer[0] << 8) | buffer[1];

  ESP_LOGD(TAG, "GET_CAPACITANCE: %d (0x%02X%02X)", raw, buffer[0], buffer[1]);

  if (raw == 0xFFFF) {
    return false;
  }

  float moisture = raw;

  if (!calibration_.c_raw) {
    // Ensure the raw value is within bounds.
    if (raw < calibration_.c_Min)
      raw = calibration_.c_Min;
    else if (raw > calibration_.c_Max)
      raw = calibration_.c_Max;

    // Calculate moisture percentage.
    moisture = ((raw - calibration_.c_Min) * 100.0) / (calibration_.c_Max - calibration_.c_Min);
  }

  if (moisture_ != nullptr && device_.started) {
    moisture_->publish_state(moisture);
  }

  return true;
}

bool I2CSoilMoistureComponent::read_temperature_() {
  uint8_t buffer[2];

  if (read_register(GET_TEMPERATURE, buffer, 2) != i2c::ERROR_OK) {
    return false;
  }

  uint16_t raw = (buffer[0] << 8) | buffer[1];

  ESP_LOGD(TAG, "GET_TEMPERATURE: %d (0x%02X%02X)", raw, buffer[0], buffer[1]);

  if (temperature_ != nullptr && device_.started) {
    temperature_->publish_state(raw / 10.0f);
  }

  return true;
}

bool I2CSoilMoistureComponent::read_light_() {
  uint8_t buffer[2];

  if (read_register(GET_LIGHT, buffer, 2) != i2c::ERROR_OK) {
    status_set_error(LOG_STR("GET_LIGHT: Read failed"));
    return false;
  }

  uint16_t raw = (buffer[0] << 8) | buffer[1];

  ESP_LOGD(TAG, "GET_LIGHT: %d (0x%02X%02X)", raw, buffer[0], buffer[1]);

  if (raw == -1) {
    return false;
  }

  float light = raw;

  if (!calibration_.l_raw) {
    light = calibration_.l_coeficient * raw + calibration_.l_constant;
  }

  if (light_ != nullptr && device_.started) {
    light_->publish_state(light);
  }

  return true;
}

uint8_t I2CSoilMoistureComponent::read_version_() {
  uint8_t buffer[1];

  if (read_register(GET_VERSION, buffer, 1) != i2c::ERROR_OK) {
    return 0;
  }

  ESP_LOGD(TAG, "GET_VERSION: %d (0x%02X)", buffer[0], buffer[0]);

  return buffer[0];
}

bool I2CSoilMoistureComponent::read_busy_() {
  uint8_t buffer[1];

  if (read_register(GET_BUSY, buffer, 1) != i2c::ERROR_OK) {
    return true;
  }

  return buffer[0] == 1;
}

uint8_t I2CSoilMoistureComponent::read_address_() {
  uint8_t buffer[1];

  if (read_register(GET_ADDRESS, buffer, 1) != i2c::ERROR_OK) {
    return 0;
  }

  return buffer[0];
}

bool I2CSoilMoistureComponent::write_reset_() {
  if (write_register(device_.addr, &RESET, 1) != i2c::ERROR_OK) {
    return false;
  }

  return true;
}

}  // namespace chirp
}  // namespace esphome
