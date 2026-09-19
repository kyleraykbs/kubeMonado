#!/usr/bin/env python3
# Copyright 2026, Kyle
# SPDX-License-Identifier: BSL-1.0
"""GTK4 status and room setup application for the Monado constellation tracker.

It talks to the control socket that monado-service exposes at
$XDG_RUNTIME_DIR/monado_calibration.sock.
"""

import os
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Gio  # noqa: E402


DEFAULT_SOCKET = os.path.join(
    os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "monado_calibration.sock"
)
SOCKET_PATH = os.environ.get("MONADO_CALIBRATION_SOCKET", DEFAULT_SOCKET)

TRACKING_MODES = ["Fuse all cameras", "One camera (cross fade)"]


class Camera:
    def __init__(self, index):
        self.index = index
        self.concrete = False
        self.position = (0.0, 0.0, 0.0)
        self.orientation = (0.0, 0.0, 0.0, 1.0)
        self.solves = 0
        self.samples = 0


class Device:
    def __init__(self, index):
        self.index = index
        self.valid = False
        self.position = (0.0, 0.0, 0.0)

    @property
    def name(self):
        return f"Device {self.index}"


class CalibrationWindow(Gtk.ApplicationWindow):
    def __init__(self, application):
        super().__init__(application=application, title="Monado Room Setup")
        self.set_default_size(960, 900)

        self.connection = None
        self.output = None
        self.stream = None

        self.status_text = "Not connected"
        self.state = {}
        self.cameras = []
        self.devices = []
        self.updating = False

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_margin_top(16)
        box.set_margin_bottom(16)
        box.set_margin_start(16)
        box.set_margin_end(16)

        scroller = Gtk.ScrolledWindow()
        scroller.set_child(box)
        scroller.set_vexpand(True)
        self.set_child(scroller)

        self.build_controls(box)
        self.build_settings(box)
        self.build_plot(box)
        self.build_status(box)

        self.connect_socket()

    # -------------------------------------------------------------- controls

    def build_controls(self, box):
        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        box.append(buttons)

        self.start_button = Gtk.Button(label="Start Calibration")
        self.start_button.connect("clicked", lambda _b: self.send("START"))
        buttons.append(self.start_button)

        self.stop_button = Gtk.Button(label="Stop")
        self.stop_button.connect("clicked", lambda _b: self.send("STOP"))
        buttons.append(self.stop_button)

        self.save_button = Gtk.Button(label="Save")
        self.save_button.connect("clicked", lambda _b: self.send("SAVE"))
        buttons.append(self.save_button)

        self.reset_button = Gtk.Button(label="Reset")
        self.reset_button.connect("clicked", lambda _b: self.send("RESET"))
        buttons.append(self.reset_button)

        self.recenter_button = Gtk.Button(label="Recenter Player")
        self.recenter_button.connect("clicked", self.on_recenter)
        buttons.append(self.recenter_button)

        buttons.append(Gtk.Label(label="Eye height (m)", xalign=0))
        self.eye_height_spin = Gtk.SpinButton.new_with_range(1.0, 2.2, 0.05)
        self.eye_height_spin.set_value(1.6)
        self.eye_height_spin.set_digits(2)
        buttons.append(self.eye_height_spin)

    def build_settings(self, box):
        settings = Gtk.Grid(column_spacing=24, row_spacing=6)
        box.append(settings)

        settings.attach(Gtk.Label(label="Samples required", xalign=0), 0, 0, 1, 1)
        self.expected_spin = Gtk.SpinButton.new_with_range(10, 5000, 10)
        self.expected_spin.set_value(100)
        self.expected_spin.connect(
            "value-changed",
            lambda s: self.send(f"SET expected {int(s.get_value())}"),
        )
        settings.attach(self.expected_spin, 1, 0, 1, 1)

        self.refine_switch = Gtk.Switch()
        self.refine_switch.set_active(True)
        self.refine_switch.connect("state-set", self.on_refine_changed)
        settings.attach(Gtk.Label(label="Refinement pass", xalign=0), 0, 1, 1, 1)
        settings.attach(self.refine_switch, 1, 1, 1, 1)

        self.mode_dropdown = Gtk.DropDown.new_from_strings(TRACKING_MODES)
        self.mode_dropdown.set_selected(0)
        self.mode_dropdown.connect("notify::selected", self.on_mode_changed)
        settings.attach(Gtk.Label(label="Tracking mode", xalign=0), 0, 2, 1, 1)
        settings.attach(self.mode_dropdown, 1, 2, 1, 1)

        settings.attach(Gtk.Label(label="Minimum seconds", xalign=0), 0, 3, 1, 1)
        self.min_seconds_spin = Gtk.SpinButton.new_with_range(0, 60, 1)
        self.min_seconds_spin.set_value(5)
        self.min_seconds_spin.connect(
            "value-changed",
            lambda s: self.send(f"SET min_seconds {int(s.get_value())}"),
        )
        settings.attach(self.min_seconds_spin, 1, 3, 1, 1)

        settings.attach(Gtk.Label(label="Filter strength", xalign=0), 0, 4, 1, 1)
        self.filter_spin = Gtk.SpinButton.new_with_range(0.0, 1.0, 0.05)
        self.filter_spin.set_value(0.5)
        self.filter_spin.set_digits(2)
        self.filter_spin.connect(
            "value-changed",
            lambda s: self.send(f"SET filter {s.get_value():.2f}"),
        )
        settings.attach(self.filter_spin, 1, 4, 1, 1)

    def build_plot(self, box):
        top_down_label = Gtk.Label(label="Top down view (X/Z, metres)", xalign=0)
        top_down_label.add_css_class("title-5")
        box.append(top_down_label)

        self.drawing_area = Gtk.DrawingArea()
        self.drawing_area.set_content_height(340)
        self.drawing_area.set_draw_func(self.draw_top_down)
        frame = Gtk.Frame()
        frame.set_child(self.drawing_area)
        box.append(frame)

    def build_status(self, box):
        status_label = Gtk.Label(label="Status", xalign=0)
        status_label.add_css_class("title-5")
        box.append(status_label)

        self.status_label = Gtk.Label(label="Not connected", xalign=0)
        box.append(self.status_label)

        self.progress = Gtk.ProgressBar()
        self.progress.set_show_text(True)
        box.append(self.progress)

        grid = Gtk.Grid(column_spacing=24, row_spacing=6)
        box.append(grid)

        self.state_labels = {}
        fields = [
            ("cameras", "Cameras"),
            ("pass", "Pass"),
            ("samples", "Pair samples"),
            ("expected", "Samples required"),
            ("spread", "Spread (m)"),
            ("agreement_mm", "Camera agreement (mm)"),
            ("agreement_deg", "Camera agreement (deg)"),
            ("gravity", "Gravity samples"),
        ]
        for row, (key, label) in enumerate(fields):
            name = Gtk.Label(label=label, xalign=0)
            value = Gtk.Label(label="-", xalign=0)
            value.add_css_class("monospace")
            grid.attach(name, 0, row, 1, 1)
            grid.attach(value, 1, row, 1, 1)
            self.state_labels[key] = value

        cameras_label = Gtk.Label(label="Cameras", xalign=0)
        cameras_label.add_css_class("title-5")
        box.append(cameras_label)

        self.camera_list = Gtk.ListBox()
        self.camera_list.set_selection_mode(Gtk.SelectionMode.NONE)
        box.append(self.camera_list)

        devices_label = Gtk.Label(label="Devices", xalign=0)
        devices_label.add_css_class("title-5")
        box.append(devices_label)

        self.device_list = Gtk.ListBox()
        self.device_list.set_selection_mode(Gtk.SelectionMode.NONE)
        box.append(self.device_list)

    # --------------------------------------------------------------- signals

    def on_refine_changed(self, _switch, state):
        self.send(f"SET refine {1 if state else 0}")
        return False

    def on_mode_changed(self, dropdown, _param):
        if self.updating:
            return
        mode = "average" if dropdown.get_selected() == 0 else "single"
        self.send(f"SET tracking_mode {mode}")

    def on_recenter(self, _button):
        eye_height = self.eye_height_spin.get_value()
        self.send(f"RECENTER {eye_height:.2f}")
        self.send("SAVE")

    # ---------------------------------------------------------------- socket

    def connect_socket(self):
        try:
            address = Gio.UnixSocketAddress.new(SOCKET_PATH)
            client = Gio.SocketClient()
            self.connection = client.connect(address)
            self.output = self.connection.get_output_stream()
            self.stream = Gio.DataInputStream.new(self.connection.get_input_stream())
            self.read_line()
        except GLib.Error:
            self.connection = None
            self.status_label.set_text(f"Waiting for monado at {SOCKET_PATH}")
            GLib.timeout_add_seconds(1, self.retry_connect)

    def retry_connect(self):
        if self.connection is not None:
            return GLib.SOURCE_REMOVE
        self.connect_socket()
        return GLib.SOURCE_REMOVE

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish(result)
        except GLib.Error:
            line = None

        if line is None:
            self.connection = None
            self.status_label.set_text(f"Disconnected, waiting for monado at {SOCKET_PATH}")
            GLib.timeout_add_seconds(1, self.retry_connect)
            return

        self.handle_line(line.decode("utf-8", "replace"))
        self.read_line()

    def send(self, command):
        if self.output is None:
            return
        try:
            self.output.write_all((command + "\n").encode())
        except GLib.Error:
            self.connection = None

    # --------------------------------------------------------------- parsing

    def handle_line(self, line):
        if line.startswith("STATUS "):
            self.status_text = line[len("STATUS "):]
        elif line.startswith("STATE "):
            self.state = {}
            for token in line[len("STATE "):].split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    self.state[key] = value
            self.cameras = []
            self.devices = []
        elif line.startswith("CAM "):
            parts = line.split()
            if len(parts) >= 12:
                camera = Camera(int(parts[1]))
                camera.concrete = parts[2] == "1"
                camera.position = tuple(float(v) for v in parts[3:6])
                camera.orientation = tuple(float(v) for v in parts[6:10])
                camera.solves = int(parts[10])
                camera.samples = int(parts[11])
                self.cameras.append(camera)
        elif line.startswith("DEV "):
            parts = line.split()
            if len(parts) >= 10:
                device = Device(int(parts[1]))
                device.valid = parts[2] == "1"
                device.position = tuple(float(v) for v in parts[3:6])
                self.devices.append(device)
        elif line == "END":
            self.update_ui()

    # ------------------------------------------------------------------- ui

    def update_ui(self):
        self.status_label.set_text(self.status_text)

        progress = 0.0
        try:
            progress = float(self.state.get("progress", "0")) / 100.0
        except ValueError:
            pass
        self.progress.set_fraction(max(0.0, min(1.0, progress)))
        self.progress.set_text(f"{progress * 100.0:.0f}%")

        for key, label in self.state_labels.items():
            value = self.state.get(key, "-")
            if key in ("spread", "agreement_mm", "agreement_deg") and value != "-":
                try:
                    value = f"{float(value):.3f}"
                except ValueError:
                    pass
            label.set_text(value)

        calibrating = self.state.get("calibrating", "0") == "1"
        self.start_button.set_sensitive(not calibrating)
        self.stop_button.set_sensitive(calibrating)

        try:
            self.updating = True
            self.refine_switch.set_active(self.state.get("refine", "1") == "1")
            self.mode_dropdown.set_selected(0 if self.state.get("mode", "0") == "0" else 1)
            expected = int(self.state.get("expected", "100"))
            if self.expected_spin.get_value_as_int() != expected:
                self.expected_spin.set_value(expected)

            min_seconds = int(self.state.get("min_seconds", "5"))
            if self.min_seconds_spin.get_value_as_int() != min_seconds:
                self.min_seconds_spin.set_value(min_seconds)

            filter_strength = float(self.state.get("filter", "0.5"))
            if abs(self.filter_spin.get_value() - filter_strength) > 0.001:
                self.filter_spin.set_value(filter_strength)
        except ValueError:
            pass
        finally:
            self.updating = False

        self.update_list(self.camera_list, [self.camera_row(c) for c in self.cameras])
        self.update_list(self.device_list, [self.device_row(d) for d in self.devices])

        self.drawing_area.queue_draw()

    def update_list(self, listbox, rows):
        child = listbox.get_first_child()
        while child is not None:
            next_child = child.get_next_sibling()
            listbox.remove(child)
            child = next_child

        for row in rows:
            listbox.append(row)

    def camera_row(self, camera):
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        row.set_margin_top(4)
        row.set_margin_bottom(4)

        name = Gtk.Label(label=f"Camera {camera.index}", xalign=0)
        name.set_size_request(90, -1)
        row.append(name)

        concrete = Gtk.Label(label="calibrated" if camera.concrete else "not calibrated", xalign=0)
        concrete.set_size_request(120, -1)
        row.append(concrete)

        pose = Gtk.Label(
            label=(
                f"pos {camera.position[0]:+.3f} {camera.position[1]:+.3f} {camera.position[2]:+.3f}  "
                f"quat {camera.orientation[0]:+.3f} {camera.orientation[1]:+.3f} "
                f"{camera.orientation[2]:+.3f} {camera.orientation[3]:+.3f}"
            ),
            xalign=0,
        )
        pose.add_css_class("monospace")
        row.append(pose)

        stats = Gtk.Label(label=f"solves {camera.solves}  samples {camera.samples}", xalign=1)
        stats.set_hexpand(True)
        stats.add_css_class("monospace")
        row.append(stats)

        return row

    def device_row(self, device):
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        row.set_margin_top(4)
        row.set_margin_bottom(4)

        row.append(Gtk.Label(label=device.name, xalign=0, width_chars=12))

        status = Gtk.Label(label="tracked" if device.valid else "not tracked", xalign=0)
        status.set_size_request(100, -1)
        row.append(status)

        pose = Gtk.Label(
            label=f"pos {device.position[0]:+.3f} {device.position[1]:+.3f} {device.position[2]:+.3f}",
            xalign=0,
        )
        pose.add_css_class("monospace")
        row.append(pose)

        return row

    # --------------------------------------------------------------- drawing

    def draw_top_down(self, area, context, width, height):
        context.set_source_rgb(0.12, 0.12, 0.14)
        context.paint()

        points = [(c.position[0], c.position[2]) for c in self.cameras]
        points += [(d.position[0], d.position[2]) for d in self.devices if d.valid]

        if not points:
            return

        min_x = min(p[0] for p in points)
        max_x = max(p[0] for p in points)
        min_y = min(p[1] for p in points)
        max_y = max(p[1] for p in points)

        span = max(max_x - min_x, max_y - min_y, 1.0) * 1.4
        center_x = (min_x + max_x) / 2.0
        center_y = (min_y + max_y) / 2.0

        scale = min(width, height) / span

        def to_screen(x, y):
            return (
                width / 2.0 + (x - center_x) * scale,
                height / 2.0 - (y - center_y) * scale,
            )

        # Grid every metre.
        context.set_line_width(1.0)
        context.set_source_rgb(0.22, 0.22, 0.26)
        start = int(center_x - span / 2.0) - 1
        end = int(center_x + span / 2.0) + 2
        for i in range(start, end):
            x1, y1 = to_screen(i, center_y - span)
            x2, y2 = to_screen(i, center_y + span)
            context.move_to(x1, y1)
            context.line_to(x2, y2)
            context.stroke()

        start = int(center_y - span / 2.0) - 1
        end = int(center_y + span / 2.0) + 2
        for i in range(start, end):
            x1, y1 = to_screen(center_x - span, i)
            x2, y2 = to_screen(center_x + span, i)
            context.move_to(x1, y1)
            context.line_to(x2, y2)
            context.stroke()

        # Cameras as blue dots, devices as green dots.
        for camera in self.cameras:
            x, y = to_screen(camera.position[0], camera.position[2])
            context.set_source_rgb(0.35, 0.66, 1.0)
            context.arc(x, y, 9.0, 0.0, 6.2832)
            context.fill()

            context.set_source_rgb(0.9, 0.9, 0.95)
            context.move_to(x + 12, y + 5)
            context.show_text(f"C{camera.index}")

        for device in self.devices:
            if not device.valid:
                continue
            x, y = to_screen(device.position[0], device.position[2])
            context.set_source_rgb(0.4, 0.9, 0.45)
            context.arc(x, y, 6.0, 0.0, 6.2832)
            context.fill()


class CalibrationApp(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="org.monado.RoomSetup")
        self.window = None

    def do_activate(self):
        if self.window is None:
            self.window = CalibrationWindow(self)
        self.window.present()


def main():
    app = CalibrationApp()
    return app.run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
