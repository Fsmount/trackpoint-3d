#include "config.hpp"
#include "input_device.hpp"

#include <poll.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using trackpoint_config::Setting;

struct Device {
  fs::path path;
  std::string name;
  dev_t node_id;
};
struct Devices {
  std::vector<Device> pointers, pointing_sticks, keyboards;
};
struct Cancelled {};

static std::string ask(const std::string &prompt) {
  std::cout << prompt << std::flush;
  std::string answer;
  if (!std::getline(std::cin, answer)) {
    if (std::cin.bad())
      throw std::runtime_error("Failed to read configuration input.");
    throw Cancelled{};
  }
  const size_t first = answer.find_first_not_of(" \t\r\n");
  answer = first == std::string::npos
               ? std::string{}
               : answer.substr(first,
                               answer.find_last_not_of(" \t\r\n") - first + 1);
  if (answer == "q" || answer == "quit")
    throw Cancelled{};
  return answer;
}

static void confirm_save(const fs::path &path) {
  const std::string answer = ask("\nSave to " + path.string() + "? [y/N]: ");
  if (answer != "y" && answer != "Y" && answer != "yes")
    throw Cancelled{};
}

static Devices discover() {
  Devices devices;
  std::set<dev_t> seen;
  for (const char *directory :
       {"/dev/input/by-id", "/dev/input/by-path", "/dev/input"}) {
    std::error_code error;
    const fs::directory_iterator entries(directory, error);
    if (error == std::errc::no_such_file_or_directory)
      continue;
    if (error)
      throw fs::filesystem_error("list input devices", directory, error);
    std::set<fs::path> paths;
    for (const fs::directory_entry &entry : entries)
      paths.insert(entry.path());
    for (const fs::path &path : paths) {
      try {
        InputDevice input{path};
        const dev_t node_id = input.node_id();
        if (!seen.insert(node_id).second)
          continue;
        const bool pointer = input.is_pointer_source();
        const bool keyboard = input.is_keyboard_source();
        if (!pointer && !keyboard)
          continue;
        const Device candidate{path, std::string{input.name()}, node_id};
        if (pointer) {
          devices.pointers.push_back(candidate);
          if (input.is_pointing_stick())
            devices.pointing_sticks.push_back(candidate);
        }
        if (keyboard)
          devices.keyboards.push_back(candidate);
      } catch (const std::system_error &) {
        // Skip inaccessible or disconnected candidates.
      }
    }
  }
  return devices;
}

template <typename DeviceSetting>
static const Device &choose(const std::vector<Device> &choices,
                            const char *kind,
                            const std::optional<DeviceSetting> &saved) {
  if (choices.empty())
    throw std::runtime_error(std::string{"No readable "} + kind +
                             " devices found in /dev/input.");
  std::cout << '\n' << kind << " devices:\n";
  struct stat current{};
  const bool have_current = saved && stat(saved->path.c_str(), &current) == 0;
  size_t default_index = 1;
  for (size_t i = 0; i < choices.size(); ++i) {
    const bool selected = have_current && choices[i].node_id == current.st_rdev;
    if (selected)
      default_index = i + 1;
    std::cout << "  " << i + 1 << ") " << choices[i].name << " ("
              << choices[i].path.string() << ')'
              << (selected ? " [current]" : "") << '\n';
  }
  if (choices.size() == 1) {
    std::cout << "Selected " << choices.front().name << ".\n";
    return choices.front();
  }
  const std::string prompt = std::string{"Choose "} + kind + " [" +
                             std::to_string(default_index) + "]: ";
  for (;;) {
    const std::string answer = ask(prompt);
    if (answer.empty())
      return choices[default_index - 1];
    size_t index = 0;
    const std::from_chars_result result =
        std::from_chars(answer.data(), answer.data() + answer.size(), index);
    if (result.ec == std::errc{} &&
        result.ptr == answer.data() + answer.size() && index > 0 &&
        index <= choices.size())
      return choices[index - 1];
    std::cout << "Enter a number from 1 to " << choices.size() << ".\n";
  }
}

static void wait_for_keyboard(const InputDevice &keyboard) {
  pollfd descriptor{keyboard.file_descriptor(), POLLIN, 0};
  int result;
  do {
    result = poll(&descriptor, 1, -1);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    throw std::system_error(errno, std::generic_category(), "wait for hotkey");
  if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
    throw std::runtime_error("Keyboard disconnected while choosing hotkey.");
}

static void wait_until_keys_released(InputDevice &keyboard) {
  for (;;) {
    input_event event{};
    while (keyboard.next_event(event) != InputDevice::ReadStatus::drained) {
    }
    bool released = true;
    for (unsigned int code = 0; code < KEY_CNT && released; ++code)
      released = !keyboard.key_down(code);
    if (released)
      return;
    wait_for_keyboard(keyboard);
  }
}

static unsigned int wait_for_key_press(InputDevice &keyboard) {
  for (;;) {
    input_event event{};
    InputDevice::ReadStatus status;
    while ((status = keyboard.next_event(event)) !=
           InputDevice::ReadStatus::drained) {
      if (status == InputDevice::ReadStatus::event && event.type == EV_KEY &&
          event.value == 1)
        return event.code;
    }
    wait_for_keyboard(keyboard);
  }
}

static Setting::Hotkey ask_hotkey(const fs::path &path,
                                  const Setting::Hotkey &current) {
  InputDevice keyboard{path};
  wait_until_keys_released(keyboard);
  keyboard.set_grabbed(true);
  std::cout << "Capture hotkey [" << current.name() << "]: " << std::flush;
  const Setting::Hotkey hotkey = [&] {
    for (;;) {
      const unsigned int code = wait_for_key_press(keyboard);
      if (code == KEY_ENTER || code == KEY_KPENTER)
        return current;
      try {
        return Setting::Hotkey{code};
      } catch (const std::invalid_argument &) {
      }
    }
  }();
  wait_until_keys_released(keyboard);
  std::cout << hotkey.name() << '\n';
  return hotkey;
}

static Setting::Gain ask_gain(const Setting::Gain &current) {
  std::ostringstream prompt;
  prompt << "Gain [" << current.value << "]: ";
  for (;;) {
    const std::string answer = ask(prompt.str());
    if (answer.empty())
      return current;
    try {
      return trackpoint_config::parse_gain(answer);
    } catch (const std::invalid_argument &error) {
      std::cout << error.what() << '\n';
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    std::cout << "Usage: trackpoint-3d-configure --config ABSOLUTE_PATH\n"
                 "Interactively configure the TrackPoint-3D daemon.\n"
                 "Requires input-device access and permission to write the "
                 "configuration file.\n"
                 "Run from a terminal. Enter q to cancel.\n";
    return 0;
  }
  try {
    const fs::path config_path =
        trackpoint_config::require_config_path(argc, argv);
    const trackpoint_config::Settings initial_settings = [&] {
      try {
        const trackpoint_config::SavedSettings saved =
            trackpoint_config::read_saved_settings(config_path);
        for (const trackpoint_config::SavedSettings::Diagnostic &diagnostic :
             saved.diagnostics)
          std::cout << diagnostic.message << '\n';
        return saved.settings;
      } catch (const std::system_error &error) {
        if (error.code() != std::errc::no_such_file_or_directory)
          throw;
      }
      return trackpoint_config::Settings{};
    }();
    std::cout << "Configure the trackpoint-3d service. Enter q to cancel.\n";
    Devices devices = discover();
    const Device &pointer =
        choose(devices.pointing_sticks.empty() ? devices.pointers
                                               : devices.pointing_sticks,
               "Pointer", initial_settings.tp_event);
    const Setting::TpEvent tp_event{pointer.path};
    devices.keyboards.erase(
        std::remove_if(devices.keyboards.begin(), devices.keyboards.end(),
                       [&](const Device &device) {
                         return device.node_id == pointer.node_id;
                       }),
        devices.keyboards.end());
    const Device &keyboard =
        choose(devices.keyboards, "Keyboard", initial_settings.kbd_event);
    const Setting::KbdEvent kbd_event{keyboard.path};
    std::cout << "\nGain controls stick sensitivity.\n";
    const Setting::Gain gain = ask_gain(initial_settings.gain);
    std::cout << "\nThe capture hotkey toggles capture.\n";
    const Setting::Hotkey hotkey =
        ask_hotkey(kbd_event.path, initial_settings.hotkey);
    const trackpoint_config::Settings settings{tp_event, kbd_event, gain,
                                               hotkey};
    std::cout << "\nPointer: " << pointer.name << " (" << tp_event.path.string()
              << ")\n"
              << "Keyboard: " << keyboard.name << " ("
              << kbd_event.path.string() << ")\n"
              << "Gain: " << gain.value << "\nCapture hotkey: " << hotkey.name()
              << '\n';
    confirm_save(config_path);
    trackpoint_config::save(config_path, settings);
    std::cout << "Saved to " << config_path.string()
              << ". Start the daemon with the same --config path.\n";
    return 0;
  } catch (const Cancelled &) {
    std::cout << "Configuration cancelled; nothing saved.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "trackpoint-3d-configure: " << error.what() << '\n';
    return 1;
  }
}
