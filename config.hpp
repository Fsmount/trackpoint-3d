#pragma once

#include <linux/input-event-codes.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackpoint_config {
inline const std::filesystem::path file_path{
    "/etc/trackpoint-3d/trackpoint-3d.env"};

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

SavedSettings read_saved_settings();
void save(const Settings &settings);
} // namespace trackpoint_config
