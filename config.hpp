#pragma once

#include <linux/input-event-codes.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackpoint_config {
struct Setting {
  struct TpEvent {
    std::filesystem::path path;
  };
  struct KbdEvent {
    std::filesystem::path path;
  };
  struct Gain {
    double value;

    explicit Gain(double value = 60.0);
  };
  struct Hotkey {
    unsigned int code;

    explicit Hotkey(unsigned int code = KEY_F8);
    std::string_view name() const;
  };
};

struct Settings {
  std::optional<Setting::TpEvent> tp_event;
  std::optional<Setting::KbdEvent> kbd_event;
  Setting::Gain gain{};
  Setting::Hotkey hotkey{};
};

struct SavedSettings {
  struct Diagnostic {
    enum class Kind { unknown_name, invalid_assignment };
    Kind kind;
    std::string message;
  };

  Settings settings;
  std::vector<Diagnostic> diagnostics;
};

Setting::Gain parse_gain(std::string_view text);
std::filesystem::path require_config_path(int argc, char *argv[]);

SavedSettings read_saved_settings(const std::filesystem::path &path);
void save(const std::filesystem::path &path, const Settings &settings);
} // namespace trackpoint_config
