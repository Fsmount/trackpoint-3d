#include "config.hpp"
#include "input_device.hpp"
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>

namespace {
constexpr int AXIS_MIN = -5000;
constexpr int AXIS_MAX = 5000;
constexpr unsigned int motion_axes[] = {ABS_X,  ABS_Y,  ABS_Z,
                                        ABS_RX, ABS_RY, ABS_RZ};

using MotionReport = std::array<int, ABS_RZ + 1>;

using VirtualDevice =
    std::unique_ptr<libevdev_uinput, decltype(&libevdev_uinput_destroy)>;

void check_evdev(int result, const char *operation) {
  if (result < 0)
    throw std::system_error(-result, std::generic_category(), operation);
}

using EvdevDescription = std::unique_ptr<libevdev, decltype(&libevdev_free)>;

VirtualDevice create_six_axis_mouse() {
  EvdevDescription description{libevdev_new(), libevdev_free};
  if (!description)
    throw std::bad_alloc();
  libevdev_set_name(description.get(), six_axis_mouse_name);
  libevdev_set_id_bustype(description.get(), BUS_USB);
  libevdev_set_id_vendor(description.get(), 0x046D);
  libevdev_set_id_product(description.get(), 0xC603);
  libevdev_set_id_version(description.get(), 1);
  check_evdev(
      libevdev_enable_event_code(description.get(), EV_KEY, BTN_0, nullptr),
      "enable first 3D mouse button");
  check_evdev(
      libevdev_enable_event_code(description.get(), EV_KEY, BTN_1, nullptr),
      "enable second 3D mouse button");
  const input_absinfo axis_info{.value = 0,
                                .minimum = AXIS_MIN,
                                .maximum = AXIS_MAX,
                                .fuzz = 0,
                                .flat = 0,
                                .resolution = 0};
  for (unsigned int axis : motion_axes)
    check_evdev(
        libevdev_enable_event_code(description.get(), EV_ABS, axis, &axis_info),
        "enable 3D mouse axis");
  libevdev_uinput *device = nullptr;
  check_evdev(libevdev_uinput_create_from_device(
                  description.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &device),
              "create 3D mouse");
  return VirtualDevice{device, libevdev_uinput_destroy};
}

VirtualDevice create_button_and_scroll_device(const InputDevice &trackpoint) {
  EvdevDescription description{libevdev_new(), libevdev_free};
  if (!description)
    throw std::bad_alloc();
  libevdev_set_name(description.get(), button_and_scroll_device_name);
  libevdev_set_id_bustype(description.get(), BUS_VIRTUAL);
  check_evdev(libevdev_enable_property(description.get(), INPUT_PROP_POINTER),
              "enable pointer property");
  for (unsigned int code = BTN_MOUSE; code < BTN_JOYSTICK; ++code) {
    if (trackpoint.has(EV_KEY, code))
      check_evdev(
          libevdev_enable_event_code(description.get(), EV_KEY, code, nullptr),
          "enable pointer button");
  }
  for (unsigned int code = 0; code <= REL_MAX; ++code) {
    if (trackpoint.has(EV_REL, code))
      check_evdev(
          libevdev_enable_event_code(description.get(), EV_REL, code, nullptr),
          "enable pointer axis");
  }
  libevdev_uinput *device = nullptr;
  check_evdev(libevdev_uinput_create_from_device(
                  description.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &device),
              "create forwarding pointer");
  return VirtualDevice{device, libevdev_uinput_destroy};
}

void emit_event(const VirtualDevice &device, unsigned int type,
                unsigned int code, int value) {
  check_evdev(libevdev_uinput_write_event(device.get(), type, code, value),
              "write virtual input event");
}

void emit_motion(const VirtualDevice &six_axis_mouse,
                 const MotionReport &report) {
  for (unsigned int axis : motion_axes)
    emit_event(six_axis_mouse, EV_ABS, axis, report[axis]);
  emit_event(six_axis_mouse, EV_SYN, SYN_REPORT, 0);
}

volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }

enum class MotionMode { orbit, tilt, pan };

struct Frame {
  int dx = 0;
  int dy = 0;
};

void emit_translated_motion(const VirtualDevice &six_axis_mouse, Frame frame,
                            const trackpoint_config::Setting::Gain &gain,
                            MotionMode mode) {
  constexpr int deadzone = 2;
  const double dx = std::abs(frame.dx) < deadzone ? 0 : frame.dx;
  const double dy = std::abs(frame.dy) < deadzone ? 0 : frame.dy;
  if (dx == 0 && dy == 0)
    return;
  const int x = static_cast<int>(
      std::clamp(dx * gain.value, double(AXIS_MIN), double(AXIS_MAX)));
  const int y = static_cast<int>(
      std::clamp(dy * gain.value, double(AXIS_MIN), double(AXIS_MAX)));
  MotionReport report{};
  switch (mode) {
  case MotionMode::orbit:
    report[ABS_RZ] = -x;
    report[ABS_RX] = -y;
    break;
  case MotionMode::tilt:
    report[ABS_RY] = -x;
    report[ABS_Y] = -y;
    break;
  case MotionMode::pan:
    report[ABS_X] = x;
    report[ABS_Z] = -y;
    break;
  }
  emit_motion(six_axis_mouse, report);
}

} // namespace

int main() {
  constexpr int invalid_configuration = 78;
  try {
    using trackpoint_config::SavedSettings;
    const SavedSettings saved = trackpoint_config::read_saved_settings();
    for (const SavedSettings::Diagnostic &diagnostic : saved.diagnostics) {
      if (diagnostic.kind == SavedSettings::Diagnostic::Kind::unknown_name)
        std::cerr << "Warning: " << diagnostic.message << '\n';
      else
        throw std::invalid_argument(diagnostic.message);
    }
    const trackpoint_config::Settings &settings = saved.settings;
    if (!settings.tp_event || !settings.kbd_event)
      throw std::invalid_argument(trackpoint_config::file_path.string() +
                                  ": TP_EVENT and KBD_EVENT are required");
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    try {
      InputDevice trackpoint{settings.tp_event->path};
      InputDevice keyboard{settings.kbd_event->path};
      if (trackpoint.node_id() == keyboard.node_id()) {
        std::cerr << "TrackPoint and keyboard must be different input devices; "
                     "run trackpoint-3d-configure"
                  << std::endl;
        return invalid_configuration;
      }
      if (!trackpoint.is_pointer_source() || !keyboard.is_keyboard_source() ||
          !keyboard.has(EV_KEY, settings.hotkey.code)) {
        std::cerr << "Configured devices do not support the selected pointer, "
                     "keyboard, or capture key; "
                  << "run trackpoint-3d-configure" << std::endl;
        return invalid_configuration;
      }
      VirtualDevice six_axis_mouse = create_six_axis_mouse();
      VirtualDevice button_and_scroll_device =
          create_button_and_scroll_device(trackpoint);
      if (const char *path =
              libevdev_uinput_get_devnode(button_and_scroll_device.get()))
        std::cout << "[pointer] " << path << " ("
                  << button_and_scroll_device_name << ')' << std::endl;

      bool capture_requested = false;
      bool captured = false;
      Frame frame{};

      pollfd inputs[] = {{trackpoint.file_descriptor(), POLLIN, 0},
                         {keyboard.file_descriptor(), POLLIN, 0}};
      using ReadStatus = InputDevice::ReadStatus;
      while (running) {
        while (running) {
          input_event key_event{};
          const ReadStatus key_status = keyboard.next_event(key_event);
          if (key_status == ReadStatus::drained)
            break;
          if (key_status == ReadStatus::event && key_event.type == EV_KEY &&
              key_event.code == settings.hotkey.code && key_event.value == 1)
            capture_requested = !capture_requested;
        }

        input_event event{};
        const ReadStatus status = trackpoint.next_event(event);
        if (status == ReadStatus::drained) {
          // A button can still be pressed between this check and EVIOCGRAB.
          if (running && capture_requested != captured &&
              trackpoint.observed_idle()) {
            trackpoint.set_grabbed(capture_requested);
            captured = capture_requested;
            frame = Frame{};
            if (!captured)
              emit_motion(six_axis_mouse, MotionReport{});
            std::cout << "[capture] " << (captured ? "ON" : "OFF") << std::endl;
          }
          if (running && poll(inputs, 2, 100) < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(),
                                    "wait for input");
          continue;
        }
        if (status == ReadStatus::sync)
          frame = Frame{};
        if (!captured)
          continue;

        switch (event.type) {
        case EV_REL:
          switch (event.code) {
          case REL_X:
            if (status == ReadStatus::event)
              frame.dx += event.value;
            break;
          case REL_Y:
            if (status == ReadStatus::event)
              frame.dy += event.value;
            break;
          default:
            emit_event(button_and_scroll_device, EV_REL, event.code,
                       event.value);
            break;
          }
          break;
        case EV_SYN:
          if (event.code == SYN_REPORT) {
            if (status == ReadStatus::event) {
              MotionMode mode = MotionMode::orbit;
              if (keyboard.key_down(KEY_LEFTSHIFT) ||
                  keyboard.key_down(KEY_RIGHTSHIFT))
                mode = MotionMode::tilt;
              else if (keyboard.key_down(KEY_LEFTCTRL) ||
                       keyboard.key_down(KEY_RIGHTCTRL))
                mode = MotionMode::pan;
              emit_translated_motion(six_axis_mouse, frame, settings.gain,
                                     mode);
            }
            frame = Frame{};
            emit_event(button_and_scroll_device, EV_SYN, SYN_REPORT, 0);
          }
          break;
        case EV_KEY:
          if (event.code >= BTN_MOUSE && event.code < BTN_JOYSTICK)
            emit_event(button_and_scroll_device, EV_KEY, event.code,
                       event.value);
          break;
        default:
          break;
        }
      }
      for (unsigned int code = BTN_MOUSE; code < BTN_JOYSTICK; ++code)
        if (trackpoint.has(EV_KEY, code))
          emit_event(button_and_scroll_device, EV_KEY, code, 0);
      emit_event(button_and_scroll_device, EV_SYN, SYN_REPORT, 0);
      emit_motion(six_axis_mouse, MotionReport{});

      return EXIT_SUCCESS;
    } catch (const std::exception &error) {
      std::cerr << error.what() << std::endl;
      return EXIT_FAILURE;
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return invalid_configuration;
  }
}
