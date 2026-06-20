#ifndef TASKS_SENSOR_HPP
#define TASKS_SENSOR_HPP

#include <stdint.h>

// Initialize ADC for sensor and diagnostic checks
void sensor_init();

// Read VSYS voltage (in Volts) and check if it is stable
float read_vsys_voltage(bool &is_stable);

// Read RP2040/RP2350 internal chip temperature (in Celsius) and check if normal
float read_internal_temp(bool &is_normal);

// Calculate and verify Flash memory integrity (returns true if healthy)
bool check_flash_integrity(uint32_t &calculated_checksum);

// Perform a quick RAM pattern check on a temporary memory block (returns true if healthy)
bool test_ram_integrity();

// Read both NTC thermistors (GP26 / GP27) and diagnose their physical status:
// Returns status for each channel via reference parameters.
void check_ntc_status_dual(float &temp_ch0, int &status_ch0, float &temp_ch1, int &status_ch1);

#endif // TASKS_SENSOR_HPP
