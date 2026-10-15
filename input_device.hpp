#pragma once

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

inline constexpr const char *six_axis_mouse_name = "TP-3D Six-Axis Mouse";
inline constexpr const char *button_and_scroll_device_name =
    "TrackPoint-3D Pointer";

class InputDevice {
  int file_descriptor_;
  libevdev *device_ = nullptr;
  bool syncing_ = false;
  std::filesystem::path path_;

public:
  enum class ReadStatus { event, sync, drained };
  explicit InputDevice(const std::filesystem::path &path) : path_{path} {
    file_descriptor_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_descriptor_ < 0)
      throw std::system_error(errno, std::generic_category(),
                              "open " + path.string());
    const int rc = libevdev_new_from_fd(file_descriptor_, &device_);
    if (rc < 0) {
      close(file_descriptor_);
      throw std::system_error(-rc, std::generic_category(),
                              "initialize " + path.string());
    }
  }
  ~InputDevice() {
    libevdev_free(device_);
    close(file_descriptor_);
  }
  InputDevice(const InputDevice &) = delete;
  InputDevice &operator=(const InputDevice &) = delete;

  int file_descriptor() const { return file_descriptor_; }
  std::string_view name() const { return libevdev_get_name(device_); }
  bool has(unsigned int type, unsigned int code) const {
    return libevdev_has_event_code(device_, type, code);
  }
  bool is_trackpoint_3d_virtual_device() const {
    const std::string_view device_name = name();
    return device_name == six_axis_mouse_name ||
           device_name == button_and_scroll_device_name;
  }
  bool is_pointer_source() const {
    return !is_trackpoint_3d_virtual_device() && has(EV_REL, REL_X) &&
           has(EV_REL, REL_Y);
  }
  bool is_pointing_stick() const {
    return libevdev_has_property(device_, INPUT_PROP_POINTING_STICK);
  }
  bool is_keyboard_source() const {
    return !is_trackpoint_3d_virtual_device() && has(EV_KEY, KEY_A) &&
           has(EV_KEY, KEY_SPACE);
  }
  bool key_down(unsigned int key) const {
    return libevdev_get_event_value(device_, EV_KEY, key) != 0;
  }
  bool observed_idle() const {
    std::array<unsigned char, (KEY_CNT + 7) / 8> kernel_key_state{};
    if (ioctl(file_descriptor_, EVIOCGKEY(kernel_key_state.size()),
              kernel_key_state.data()) < 0)
      throw std::system_error(errno, std::generic_category(),
                              "read key state " + path_.string());
    for (unsigned int code = BTN_MOUSE; code < BTN_JOYSTICK; ++code) {
      if (key_down(code) ||
          (kernel_key_state[code / 8] & (1u << (code % 8))) != 0)
        return false;
    }
    const int pending = libevdev_has_event_pending(device_);
    if (pending < 0)
      throw std::system_error(-pending, std::generic_category(),
                              "inspect pending input " + path_.string());
    return pending == 0;
  }
  void set_grabbed(bool grabbed) {
    const int result =
        libevdev_grab(device_, grabbed ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
    if (result < 0)
      throw std::system_error(-result, std::generic_category(),
                              "change input grab " + path_.string());
  }
  dev_t node_id() const {
    struct stat node{};
    if (fstat(file_descriptor_, &node) < 0)
      throw std::system_error(errno, std::generic_category(),
                              "inspect input device " + path_.string());
    return node.st_rdev;
  }

  ReadStatus next_event(input_event &event) {
    for (;;) {
      const int rc = libevdev_next_event(device_,
                                         syncing_ ? LIBEVDEV_READ_FLAG_SYNC
                                                  : LIBEVDEV_READ_FLAG_NORMAL,
                                         &event);
      if (rc == -EAGAIN) {
        if (syncing_) {
          syncing_ = false;
          continue;
        }
        return ReadStatus::drained;
      }
      if (rc < 0)
        throw std::system_error(-rc, std::generic_category(),
                                "read " + path_.string());
      if (rc == LIBEVDEV_READ_STATUS_SYNC) {
        syncing_ = true;
        return ReadStatus::sync;
      }
      return ReadStatus::event;
    }
  }
};
