#pragma once

#include <string>
#include <string_view>

namespace isolate::log {

enum class Level { Quiet, Normal, Verbose, Debug };

void set_level(Level level);
Level level();

// stderr, so stdout belongs to the capsule's program.
void info(std::string_view message);
void warn(std::string_view message);
void verbose(std::string_view message);
void debug(std::string_view message);

bool stderr_is_tty();
bool stdout_is_tty();

} // namespace isolate::log
