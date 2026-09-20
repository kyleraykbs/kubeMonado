// Copyright 2026, Kyle
// SPDX-License-Identifier: BSL-1.0
#include "t_constellation_tracker_internal.hpp"
#ifndef _WIN32
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
namespace xrt::tracking::constellation {
#define MAX_CLIENTS 8
#define LINE_SIZE 512
struct CalibrationClient { int fd = -1; char line[LINE_SIZE] = {}; size_t len = 0; };
struct CalibrationServer {
	ConstellationTracker *tracker = nullptr;
	int listen_fd = -1;
	std::string socket_path;
	std::vector<CalibrationClient> clients;
	std::thread thread;
	std::atomic<bool> running{false};
};
static std::string buildStatus(ConstellationTracker *tracker, std::string &out_status) {
	std::string out;
	char status[256] = "Idle";
	bool collecting = false;
	bool calibrating = false;
	size_t samples = 0;
	size_t expected = 0;
	size_t num_cameras = 0;
	for (auto &mosaic : tracker->mosaics) num_cameras += mosaic->cameras.size();
	// Per-camera target: the solve waits for the slowest camera, so the bar reflects the lagging one.
	expected = tracker->calibration_expected_per_camera;
	bool valid = !tracker->needSensorCalibration();
	std::vector<size_t> per_camera_samples;
	if (tracker->offline_sensor_calibration) {
		collecting = tracker->offline_sensor_calibration->isCollectingData();
		if (tracker->offline_sensor_calibration->numSamplesPerCameraForBestDevice(per_camera_samples) &&
		    !per_camera_samples.empty()) {
			samples = per_camera_samples[0];
			for (size_t c : per_camera_samples) {
				if (c < samples) samples = c;
			}
		}
	}
	{
		std::unique_lock<os::Mutex> lock(tracker->offline_sensor_calibration_lock);
		calibrating = tracker->running_calibration;
	}
	bool is_calibrating = collecting || calibrating;
	if (!valid && is_calibrating) {
		if (collecting) {
			// Name the cameras that still need observations so the user knows where to point the HMD.
			char lagging[128] = "";
			size_t lag_count = 0;
			size_t camera_visit = 0;
			for (auto &mosaic : tracker->mosaics) {
				for (auto &camera : mosaic->cameras) {
					size_t c = camera_visit < per_camera_samples.size() ? per_camera_samples[camera_visit] : 0;
					if (c < expected) {
						char tmp[32];
						snprintf(tmp, sizeof(tmp), "%s%zu", lag_count > 0 ? ", " : "", camera->index);
						strncat(lagging, tmp, sizeof(lagging) - strlen(lagging) - 1);
						lag_count++;
					}
					camera_visit++;
				}
			}
			if (lag_count > 0) {
				snprintf(status, sizeof(status),
				         "Move the HMD in front of camera(s) %s  [slowest %zu/%zu samples]", lagging, samples,
				         expected);
			} else {
				snprintf(status, sizeof(status), "Collecting samples %zu/%zu", samples, expected);
			}
		} else {
			snprintf(status, sizeof(status), "Running calibration solve...");
		}
	} else if (!valid && !is_calibrating) {
		snprintf(status, sizeof(status), "Waiting to collect %zu samples per camera", expected);
	} else if (valid) {
		if (tracker->calibrating_device_imus) {
			char ctrls[160] = "";
			{
				std::shared_lock devices_lock(tracker->device_lock);
				size_t shown = 0;
				for (auto &device : tracker->devices) {
					if (device->id == tracker->camera_calibration_device_id ||
					    tracker->device_imus_solved.count(device->id) != 0) {
						continue;
					}
					size_t n = tracker->offline_sensor_calibration
					               ? tracker->offline_sensor_calibration->numCameraSamplesForDevice(
					                     device->id)
					               : 0;
					char tmp[48];
					snprintf(tmp, sizeof(tmp), "%sdevice %d: %zu samples",
					         shown > 0 ? ", " : "", device->id, n);
					strncat(ctrls, tmp, sizeof(ctrls) - strlen(ctrls) - 1);
					shown++;
				}
			}
			if (ctrls[0] != '\0') {
				snprintf(status, sizeof(status),
				         "Turn on both controllers and move them in front of the cameras (%s)", ctrls);
			} else {
				snprintf(status, sizeof(status),
				         "Turn on both controllers and move them in front of the cameras");
			}
		} else {
			snprintf(status, sizeof(status), "Tracking (%zu cameras calibrated)", num_cameras);
		}
	}
	int progress = expected > 0 ? (int)(samples * 100 / expected) : 0;
	if (progress > 100) progress = 100;
	out_status = status;
	out += "STATUS ";
	out += status;
	out += "\n";
	char state[512];
	snprintf(state, sizeof(state),
	         "STATE calibrating=%d pass=0 progress=%d samples=%zu expected=%zu spread=0.000 inliers=%zu total=%zu gravity=0 agreement_mm=0.0 agreement_deg=0.0 use_all=1 refine=0 valid=%d auto=1 cameras=%zu filter=0.50 tracking_mode=0 mode=0 min_seconds=5 expected_samples=%zu collecting=%d calibrating_flag=%d\n",
	         is_calibrating ? 1 : 0, progress, samples, expected, samples, samples, valid ? 1 : 0, num_cameras, expected, collecting ? 1 : 0, calibrating ? 1 : 0);
	out += state;
	size_t camera_visit = 0;
	for (auto &mosaic : tracker->mosaics) {
		for (auto &camera : mosaic->cameras) {
			auto &pose = camera->locked_data.Txr_origin_cam;
			bool concrete = camera->locked_data.has_concrete_pose;
			size_t camera_samples = camera_visit < per_camera_samples.size() ? per_camera_samples[camera_visit] : 0;
			char line[256];
			snprintf(line, sizeof(line), "CAM %zu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f 0 %zu\n", camera->index, concrete ? 1 : 0, pose.position.x, pose.position.y, pose.position.z, pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w, camera_samples);
			out += line;
			camera_visit++;
		}
	}
	{
		std::shared_lock lock(tracker->device_lock);
		for (auto &device : tracker->devices) {
			xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
			bool has_pose = false;
			try {
				tracker->getTrackedPose(device->id, os_monotonic_get_ns(), relation);
				has_pose = (relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
			} catch (...) { has_pose = false; }
			char line[256];
			if (has_pose) {
				snprintf(line, sizeof(line), "DEV %d 1 %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", device->id, relation.pose.position.x, relation.pose.position.y, relation.pose.position.z, relation.pose.orientation.x, relation.pose.orientation.y, relation.pose.orientation.z, relation.pose.orientation.w);
			} else {
				snprintf(line, sizeof(line), "DEV %d 0 0 0 0 0 0 0 1\n", device->id);
			}
			out += line;
		}
	}
	out += "END\n";
	return out;
}
static std::string buildMlBlock(ConstellationTracker *tracker) {
	std::string out;
	char line[512];
	int64_t now = os_monotonic_get_ns();
	snprintf(line, sizeof(line), "FRAME %" PRIi64 "\n", now);
	out += line;

	// Cameras: world pose (OpenXR) + pinhole intrinsics + undistorted observations.
	for (auto &mosaic : tracker->mosaics) {
		for (auto &camera : mosaic->cameras) {
			std::unique_lock<os::Mutex> lk(camera->processing_lock);
			auto &d = camera->locked_data;
			auto &m = camera->model.calib_pinhole;
			snprintf(line, sizeof(line),
			         "CAM %zu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.4f %.4f %.4f %.4f %zu\n",
			         camera->index, d.ml_has ? 1 : 0, d.ml_world_cam.position.x, d.ml_world_cam.position.y,
			         d.ml_world_cam.position.z, d.ml_world_cam.orientation.x, d.ml_world_cam.orientation.y,
			         d.ml_world_cam.orientation.z, d.ml_world_cam.orientation.w, m.fx, m.fy, m.cx, m.cy,
			         d.ml_uv.size());
			out += line;
			for (auto &uv : d.ml_uv) {
				snprintf(line, sizeof(line), "OBS %zu %.4f %.4f\n", camera->index, uv.x, uv.y);
				out += line;
			}
		}
	}

	// Devices: LED model (unflipped back to the raw/sim frame), prior world pose from the IMU source.
	{
		std::shared_lock dlk(tracker->device_lock);
		for (auto &device : tracker->devices) {
			auto &lm = device->params.led_model;
			snprintf(line, sizeof(line), "LED %d %u\n", device->id, (unsigned)lm.led_count);
			out += line;
			for (uint16_t i = 0; i < lm.led_count; i++) {
				auto &led = lm.leds[i];
				snprintf(line, sizeof(line), "L %d %.6f %.6f %.6f %.6f %.6f %.6f\n", device->id, led.position.x,
				         -led.position.y, -led.position.z, led.normal.x, -led.normal.y, -led.normal.z);
				out += line;
			}
			xrt_space_relation pred = XRT_SPACE_RELATION_ZERO;
			if (device->params.tracking_source != nullptr) {
				t_constellation_tracker_tracking_source_get_tracked_pose(device->params.tracking_source, now, &pred);
			}
			bool valid = (pred.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
			snprintf(line, sizeof(line), "DEV %d %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", device->id,
			         valid ? 1 : 0, pred.pose.position.x, pred.pose.position.y, pred.pose.position.z,
			         pred.pose.orientation.x, pred.pose.orientation.y, pred.pose.orientation.z,
			         pred.pose.orientation.w);
			out += line;
		}
	}
	out += "ENDML\n";
	return out;
}

static void handleCommand(CalibrationServer *server, const char *line) {
	ConstellationTracker *tracker = server->tracker;
	if (strcmp(line, "START") == 0) {
		tracker->resetCalibration();
	} else if (strcmp(line, "STOP") == 0) {
		if (tracker->offline_sensor_calibration && tracker->offline_sensor_calibration->isCollectingData()) {
			tracker->offline_sensor_calibration->endCollectingData();
		}
	} else if (strcmp(line, "SAVE") == 0) {
		CT_INFO(tracker, "SAVE command received, saving room setup to %s", tracker->room_setup_file.c_str());
		tracker->saveRoomSetupCurrent();
	} else if (strcmp(line, "RESET") == 0) {
		tracker->resetCalibration();
	} else if (strncmp(line, "RECENTER", 8) == 0) {
		float eye_height = 1.6f;
		if (sscanf(line + 8, "%f", &eye_height) == 1) {}
		tracker->recenter(eye_height);
		tracker->saveRoomSetupCurrent();
	} else if (strncmp(line, "SET ", 4) == 0) {
		const char *rest = line + 4;
		char key[64] = {}; char value[64] = {};
		if (sscanf(rest, "%63s %63s", key, value) == 2) {
			if (strcmp(key, "expected") == 0 || strcmp(key, "expected_samples") == 0) {
				int v = atoi(value);
				if (v > 0) tracker->calibration_expected_per_camera = (uint32_t)v;
			} else if (strcmp(key, "filter") == 0) {
			} else if (strcmp(key, "tracking_mode") == 0 || strcmp(key, "use_all_cameras") == 0 || strcmp(key, "mode") == 0) {
			} else if (strcmp(key, "min_seconds") == 0 || strcmp(key, "refine") == 0) {
			}
		}
	} else if (strncmp(line, "SET_CAMERA", 10) == 0) {
		size_t idx; xrt_pose pose; int concrete;
		if (sscanf(line + 10, "%zu %f %f %f %f %f %f %f %d", &idx, &pose.position.x, &pose.position.y, &pose.position.z, &pose.orientation.x, &pose.orientation.y, &pose.orientation.z, &pose.orientation.w, &concrete) == 8) {
			for (auto &mosaic : tracker->mosaics) {
				for (auto &camera : mosaic->cameras) {
					if (camera->index == idx) {
						camera->locked_data.Txr_origin_cam = pose;
						camera->locked_data.has_concrete_pose = concrete != 0;
					}
				}
			}
		}
	} else if (strncmp(line, "MODE", 4) == 0) {
		const char *m = line + 4;
		while (*m == ' ') {
			m++;
		}
		bool on = strncmp(m, "ml", 2) == 0;
		tracker->ml_override_enabled.store(on);
		CT_INFO(tracker, "ML override mode: %s", on ? "on" : "off");
	} else if (strncmp(line, "MLPOSE ", 7) == 0) {
		int id = -1;
		xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		if (sscanf(line + 7, "%d %f %f %f %f %f %f %f", &id, &rel.pose.position.x, &rel.pose.position.y,
		           &rel.pose.position.z, &rel.pose.orientation.x, &rel.pose.orientation.y,
		           &rel.pose.orientation.z, &rel.pose.orientation.w) == 8) {
			std::shared_lock dlk(tracker->device_lock);
			for (auto &device : tracker->devices) {
				if (device->id == (t_constellation_device_id_t)id) {
					std::unique_lock<os::Mutex> lk(device->data_lock);
					device->locked_data.ml_relation = rel;
					device->locked_data.ml_has = true;
					device->locked_data.ml_ts = os_monotonic_get_ns();
					break;
				}
			}
		}
	}
}
static void serverThreadFunc(CalibrationServer *server) {
	while (server->running.load()) {
		std::vector<struct pollfd> pfds;
		pfds.reserve(1 + server->clients.size());
		struct pollfd listen_pfd = {};
		listen_pfd.fd = server->listen_fd;
		listen_pfd.events = POLLIN;
		pfds.push_back(listen_pfd);
		for (auto &c : server->clients) {
			if (c.fd >= 0) {
				struct pollfd pfd = {};
				pfd.fd = c.fd;
				pfd.events = POLLIN;
				pfds.push_back(pfd);
			}
		}
		int poll_ms = server->tracker->ml_override_enabled.load() ? 4 : 200;
		int ret = poll(pfds.data(), pfds.size(), poll_ms);
		if (ret < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pfds[0].revents & POLLIN) {
			int fd = accept(server->listen_fd, nullptr, nullptr);
			if (fd >= 0) {
				int flags = fcntl(fd, F_GETFL, 0);
				fcntl(fd, F_SETFL, flags | O_NONBLOCK);
				bool added = false;
				for (auto &c : server->clients) {
					if (c.fd < 0) { c.fd = fd; c.len = 0; added = true; break; }
				}
				if (!added && server->clients.size() < MAX_CLIENTS) {
					CalibrationClient client;
					client.fd = fd;
					server->clients.push_back(client);
					added = true;
				}
				if (!added) close(fd);
				else {
					std::string status;
					std::string block = buildStatus(server->tracker, status);
					send(fd, block.c_str(), block.size(), SEND_FLAGS);
				}
			}
		}
		for (size_t i = 0; i < server->clients.size(); i++) {
			auto &c = server->clients[i];
			if (c.fd < 0) continue;
			size_t pfd_idx = 1 + i;
			bool has_data = pfd_idx < pfds.size() && (pfds[pfd_idx].revents & POLLIN);
			bool hup = pfd_idx < pfds.size() && (pfds[pfd_idx].revents & (POLLHUP | POLLERR));
			if (hup && !has_data) { close(c.fd); c.fd = -1; continue; }
			if (!has_data) continue;
			char buf[LINE_SIZE];
			ssize_t n = recv(c.fd, buf, sizeof(buf) - 1, 0);
			if (n <= 0) { close(c.fd); c.fd = -1; continue; }
			for (ssize_t k = 0; k < n; k++) {
				char ch = buf[k];
				if (ch == '\n' || ch == '\r') {
					if (c.len > 0) {
						c.line[c.len] = '\0';
						handleCommand(server, c.line);
						c.len = 0;
						std::string status;
						std::string block = buildStatus(server->tracker, status);
						for (auto &other : server->clients) {
							if (other.fd >= 0) send(other.fd, block.c_str(), block.size(), SEND_FLAGS);
						}
					}
				} else if (c.len < LINE_SIZE - 1) {
					c.line[c.len++] = ch;
				}
			}
		}
		{
			static auto last_broadcast = std::chrono::steady_clock::now();
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() > 500) {
				last_broadcast = now;
				std::string status;
				std::string block = buildStatus(server->tracker, status);
				for (auto &c : server->clients) {
					if (c.fd >= 0) send(c.fd, block.c_str(), block.size(), SEND_FLAGS);
				}
			}
		}
		if (server->tracker->ml_override_enabled.load()) {
			std::string block = buildMlBlock(server->tracker);
			for (auto &c : server->clients) {
				if (c.fd >= 0) send(c.fd, block.c_str(), block.size(), SEND_FLAGS);
			}
		}
	}
}
CalibrationServer *calibration_server_create(ConstellationTracker *tracker, const char *socket_path) {
	std::string path;
	if (socket_path && socket_path[0] != '\0') path = socket_path;
	else {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (runtime && runtime[0] != '\0') path = std::string(runtime) + "/monado_calibration.sock";
		else path = "/tmp/monado_calibration.sock";
	}
	unlink(path.c_str());
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return nullptr;
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return nullptr; }
	if (listen(fd, 8) < 0) { close(fd); return nullptr; }
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	CalibrationServer *server = new CalibrationServer();
	server->tracker = tracker;
	server->listen_fd = fd;
	server->socket_path = path;
	server->clients.resize(MAX_CLIENTS);
	for (auto &c : server->clients) c.fd = -1;
	server->running.store(true);
	server->thread = std::thread(serverThreadFunc, server);
	return server;
}
void calibration_server_destroy(CalibrationServer **server_ptr) {
	if (!server_ptr || !*server_ptr) return;
	CalibrationServer *server = *server_ptr;
	server->running.store(false);
	if (server->thread.joinable()) server->thread.join();
	for (auto &c : server->clients) if (c.fd >= 0) close(c.fd);
	if (server->listen_fd >= 0) { close(server->listen_fd); unlink(server->socket_path.c_str()); }
	delete server;
	*server_ptr = nullptr;
}
} // namespace xrt::tracking::constellation
#endif
