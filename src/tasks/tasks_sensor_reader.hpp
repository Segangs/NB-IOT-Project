#ifndef TASKS_SENSOR_READER_HPP
#define TASKS_SENSOR_READER_HPP

#include <cstddef>

#include "../boot_v2/runtime_snapshot_core.hpp"

void vSensorTask(void *pvParameters);
[[nodiscard]] bool copy_sensor_quality_snapshot(
    std::size_t channel,
    boot_v2::SensorQualitySnapshotV1 &snapshot) noexcept;

#endif // TASKS_SENSOR_READER_HPP
