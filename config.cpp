#include "config.hpp"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace trackpoint_config {

Setting::Gain::Gain(double initial_value) : value{initial_value} {
  if (!std::isfinite(value) || value <= 0)
    throw std::invalid_argument("Gain must be positive and finite");
}

Setting::Gain parse_gain(std::string_view text) {
  double value;
  const std::from_chars_result result = std::from_chars(
      text.data(), text.data() + text.size(), value, std::chars_format::fixed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument("Gain must be a decimal number");
  return Setting::Gain{value};
}

Setting::Hotkey::Hotkey(unsigned int initial_code) : code{initial_code} {
  const char *name = libevdev_event_code_get_name(EV_KEY, code);
  if (code == 0 || !name || std::string_view{name}.substr(0, 4) != "KEY_")
    throw std::invalid_argument("Hotkey must identify a keyboard key");
}

std::string_view Setting::Hotkey::name() const {
  return libevdev_event_code_get_name(EV_KEY, code);
}

static Setting::Hotkey parse_hotkey(std::string_view text) {
  unsigned int code;
  if (text.substr(0, 4) == "KEY_") {
    const int named =
        libevdev_event_code_from_name_n(EV_KEY, text.data(), text.size());
    if (named < 0)
      throw std::invalid_argument("Unknown keyboard key name");
    code = static_cast<unsigned int>(named);
  } else {
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), code);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
      throw std::invalid_argument("Hotkey must be a key name or numeric code");
  }
  return Setting::Hotkey{code};
}

static std::optional<std::string_view> parse_name(std::string_view &text) {
  const std::string_view whitespace = " \t\r\n\f\v";
  const size_t start = text.find_first_not_of(whitespace);
  if (start == std::string_view::npos || text[start] == '#')
    return std::nullopt;
  text.remove_prefix(start);
  const size_t delimiter = text.find_first_of(" \t\r\n\f\v=");
  const size_t end =
      delimiter == std::string_view::npos ? text.size() : delimiter;
  if (end == 0)
    throw std::invalid_argument("Expected a name");
  const std::string_view name = text.substr(0, end);
  text.remove_prefix(end);
  return name;
}

static std::string_view parse_value(std::string_view remainder) {
  const std::string_view whitespace = " \t\r\n\f\v";
  const size_t separator = remainder.find_first_not_of(whitespace);
  if (separator == std::string_view::npos || remainder[separator] != '=')
    throw std::invalid_argument("Expected '='");
  size_t start = remainder.find_first_not_of(whitespace, separator + 1);
  if (start == std::string_view::npos || remainder[start] == '#')
    throw std::invalid_argument("Expected a value");
  const char opening_quote = remainder[start];
  const bool quoted = opening_quote == '\'' || opening_quote == '"';
  size_t end;
  size_t after;
  if (quoted) {
    end = remainder.find(opening_quote, start + 1);
    if (end == std::string_view::npos)
      throw std::invalid_argument("Unterminated quoted value");
    ++start;
    after = end + 1;
  } else {
    end = remainder.find_first_of(whitespace, start);
    if (end == std::string_view::npos)
      end = remainder.size();
    after = end;
  }
  const size_t trailing = remainder.find_first_not_of(whitespace, after);
  if (trailing != std::string_view::npos && remainder[trailing] != '#')
    throw std::invalid_argument("Unexpected text after value");
  if (end == start)
    throw std::invalid_argument("Expected a value");
  return remainder.substr(start, end - start);
}

static std::string serialize(const Settings &settings) {
  std::ostringstream output;
  const auto write_assignment = [&](std::string_view name,
                                    std::string_view value) {
    const char quote = value.find('"') == std::string_view::npos ? '"' : '\'';
    if (value.find(quote) != std::string_view::npos)
      throw std::invalid_argument(std::string{name} +
                                  ": Value contains both quote types");
    if (value.find('\n') != std::string_view::npos)
      throw std::invalid_argument(std::string{name} +
                                  ": Value contains a newline");
    output << name << '=' << quote << value << quote << '\n';
  };
  // Room for "0.", leading fractional zeros, and enough digits to recover the
  // original double when parsed.
  std::array<char, 2 + std::numeric_limits<double>::max_digits10 -
                       std::numeric_limits<double>::min_exponent10>
      gain_text;
  const std::to_chars_result gain_result =
      std::to_chars(gain_text.data(), gain_text.data() + gain_text.size(),
                    settings.gain.value, std::chars_format::fixed);
  if (gain_result.ec != std::errc{})
    throw std::system_error(std::make_error_code(gain_result.ec),
                            "format gain");
  if (settings.tp_event)
    write_assignment("TP_EVENT", settings.tp_event->path.string());
  if (settings.kbd_event)
    write_assignment("KBD_EVENT", settings.kbd_event->path.string());
  write_assignment("GAIN", std::string_view{gain_text.data(), gain_result.ptr});
  write_assignment("HOTKEY", std::to_string(settings.hotkey.code));
  return output.str();
}

std::filesystem::path require_config_path(int argc, char *argv[]) {
  if (argc != 3 || std::string_view{argv[1]} != "--config" || !argv[2][0])
    throw std::invalid_argument("Expected --config ABSOLUTE_PATH");
  const std::filesystem::path path{argv[2]};
  if (!path.is_absolute())
    throw std::invalid_argument("Configuration path must be absolute");
  return path;
}

static std::string read_contents(const std::filesystem::path &path) {
  const int input = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (input < 0)
    throw std::system_error(errno, std::generic_category(),
                            "open " + path.string());
  std::string contents;
  std::array<char, 4096> buffer;
  for (;;) {
    const ssize_t count = read(input, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      const int error = errno;
      close(input);
      throw std::system_error(error, std::generic_category(),
                              "read " + path.string());
    }
    if (count == 0)
      break;
    contents.append(buffer.data(), static_cast<size_t>(count));
  }
  close(input);
  return contents;
}

SavedSettings read_saved_settings(const std::filesystem::path &path) {
  const std::string contents = read_contents(path);
  SavedSettings saved;
  std::set<std::string_view> assigned;
  std::string_view unread{contents};
  for (size_t number = 1; !unread.empty(); ++number) {
    const size_t newline = unread.find('\n');
    const std::string_view line = unread.substr(0, newline);
    unread.remove_prefix(newline == std::string_view::npos ? unread.size()
                                                           : newline + 1);
    std::string context = path.string() + ":" + std::to_string(number) + ": ";
    try {
      std::string_view remainder{line};
      const std::optional<std::string_view> name = parse_name(remainder);
      if (!name)
        continue;
      context += std::string{*name} + ": ";
      const auto first_value = [&] {
        if (!assigned.insert(*name).second)
          throw std::invalid_argument("Duplicate assignment");
        return parse_value(remainder);
      };
      if (name == "TP_EVENT")
        saved.settings.tp_event =
            Setting::TpEvent{std::filesystem::path{first_value()}};
      else if (name == "KBD_EVENT")
        saved.settings.kbd_event =
            Setting::KbdEvent{std::filesystem::path{first_value()}};
      else if (name == "GAIN")
        saved.settings.gain = parse_gain(first_value());
      else if (name == "HOTKEY")
        saved.settings.hotkey = parse_hotkey(first_value());
      else
        saved.diagnostics.push_back(
            {SavedSettings::Diagnostic::Kind::unknown_name,
             context + "Unknown setting"});
    } catch (const std::invalid_argument &error) {
      saved.diagnostics.push_back(
          {SavedSettings::Diagnostic::Kind::invalid_assignment,
           context + error.what()});
    }
  }
  return saved;
}

void save(const std::filesystem::path &path, const Settings &settings) {
  const std::string contents = serialize(settings);
  std::filesystem::create_directories(path.parent_path());
  std::string temporary = path.string() + ".tmp.XXXXXX";
  int output = mkstemp(temporary.data());
  if (output < 0)
    throw std::system_error(errno, std::generic_category(),
                            "create " + temporary);
  try {
    std::string_view pending(contents);
    while (!pending.empty()) {
      const ssize_t written = write(output, pending.data(), pending.size());
      if (written < 0) {
        if (errno == EINTR)
          continue;
        throw std::system_error(errno, std::generic_category(),
                                "write " + temporary);
      }
      if (written == 0)
        throw std::system_error(EIO, std::generic_category(),
                                "write " + temporary);
      pending.remove_prefix(static_cast<size_t>(written));
    }
    if (fsync(output) < 0)
      throw std::system_error(errno, std::generic_category(),
                              "sync " + temporary);
    const int descriptor = output;
    output = -1;
    if (close(descriptor) < 0)
      throw std::system_error(errno, std::generic_category(),
                              "close " + temporary);
    std::filesystem::rename(temporary, path);
  } catch (...) {
    if (output >= 0)
      close(output);
    unlink(temporary.c_str());
    throw;
  }
  const std::filesystem::path directory_path = path.parent_path();
  const int directory =
      open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0)
    throw std::system_error(errno, std::generic_category(),
                            "open " + directory_path.string());
  const int synced = fsync(directory);
  const int error = errno;
  close(directory);
  if (synced < 0)
    throw std::system_error(error, std::generic_category(),
                            "sync " + directory_path.string());
}
} // namespace trackpoint_config
