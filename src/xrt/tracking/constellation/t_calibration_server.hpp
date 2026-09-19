// Copyright 2026, Kyle
// SPDX-License-Identifier: BSL-1.0
#pragma once
namespace xrt::tracking::constellation {
struct ConstellationTracker;
struct CalibrationServer;
CalibrationServer *calibration_server_create(ConstellationTracker *tracker, const char *socket_path);
void calibration_server_destroy(CalibrationServer **server);
}
