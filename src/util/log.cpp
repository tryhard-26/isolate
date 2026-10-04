#include "util/log.h"

#include <unistd.h>

#include <cstdio>
#include <string>

namespace isolate::log {
namespace {

Level g_level = Level::Normal;

bool color_enabled() {
    static const bool enabled = [] {
        if (const char* no = std::getenv("NO_COLOR"); no && *no) return false;
        return ::isatty(STDERR_FILENO) == 1;
    }();
    return enabled;
}

void emit(const char* color, std::string_view tag, std::string_view message) {
    std::string line;
    line += "isolate: ";
    if (!tag.empty()) {
        if (color_enabled()) line += color;
        line += tag;
        if (color_enabled()) line += "\033[0m";
        line += ": ";
    }
    line += message;
    line += '\n';
    std::fputs(line.c_str(), stderr);
}

} // namespace

void set_level(Level level) {
    g_level = level;
}

Level level() {
    return g_level;
}

void info(std::string_view message) {
    if (g_level == Level::Quiet) return;
    emit("", {}, message);
}

void warn(std::string_view message) {
    if (g_level == Level::Quiet) return;
    emit("\033[33m", "warning", message);
}

void verbose(std::string_view message) {
    if (g_level < Level::Verbose) return;
    emit("\033[36m", "info", message);
}

void debug(std::string_view message) {
    if (g_level < Level::Debug) return;
    emit("\033[90m", "debug", message);
}

bool stderr_is_tty() {
    return ::isatty(STDERR_FILENO) == 1;
}

bool stdout_is_tty() {
    return ::isatty(STDOUT_FILENO) == 1;
}

} // namespace isolate::log
