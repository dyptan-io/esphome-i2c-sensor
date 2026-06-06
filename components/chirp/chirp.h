#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace chirp {

class I2CSoilMoistureComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void set_moisture_sensor(sensor::Sensor *sensor) { moisture_sensor_ = sensor; }
  void set_temperature_sensor(sensor::Sensor *sensor) { temperature_sensor_ = sensor; }
  void set_illuminance_sensor(sensor::Sensor *sensor) { illuminance_sensor_ = sensor; }

  void set_new_address(uint8_t address) { new_address_ = address; }
  void set_moisture_calibration(uint16_t min, uint16_t max, bool raw) {
    moisture_min_ = min;
    moisture_max_ = max;
    moisture_raw_ = raw;
  }
  void set_illuminance_calibration(float coefficient, int32_t constant, bool raw) {
    illuminance_coefficient_ = coefficient;
    illuminance_constant_ = constant;
    illuminance_raw_ = raw;
  }

  void setup() override;
  void update() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  enum class ErrorCode : uint8_t {
    NONE,
    COMMUNICATION_FAILED,
    UPDATE_INTERVAL_TOO_SHORT,
  };

  bool apply_new_address_();
  void finish_setup_();
  bool read_moisture_();
  bool read_temperature_();
  bool read_illuminance_();
  bool start_light_measurement_();
  bool sleep_();

  sensor::Sensor *moisture_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *illuminance_sensor_{nullptr};

  uint16_t moisture_min_{245};
  uint16_t moisture_max_{550};
  bool moisture_raw_{false};

  float illuminance_coefficient_{-1.525f};
  int32_t illuminance_constant_{100000};
  bool illuminance_raw_{false};

  uint8_t new_address_{0};
  uint8_t firmware_version_{0};
  ErrorCode error_code_{ErrorCode::NONE};
  bool initialized_{false};
};

}  // namespace chirp
}  // namespace esphome
