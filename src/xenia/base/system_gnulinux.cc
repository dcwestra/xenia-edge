/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <alloca.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

// Forward-declared instead of pulling in <SDL3/SDL.h> — xenia-base must not
// drag SDL onto its include path, and only the dlsym() cast needs the type.
// Flags and signature are stable across SDL2/3.
namespace {
using SDL_ShowSimpleMessageBox_fn = int (*)(uint32_t flags, const char* title,
                                            const char* message, void* window);
constexpr uint32_t kSDL_MessageBoxError = 0x00000010;
constexpr uint32_t kSDL_MessageBoxWarning = 0x00000020;
constexpr uint32_t kSDL_MessageBoxInformation = 0x00000040;
}  // namespace

extern char** environ;

namespace xe {

// Hands |target| to xdg-open as its own argument, with no shell to interpret
// it, so a path holding $, ` or " opens as named, and returns without waiting.
// system() ran it through sh and blocked the caller until xdg-open exited.
static void XdgOpen(const std::string& target) {
  char program[] = "xdg-open";
  std::string argument = target;
  char* argv[] = {program, argument.data(), nullptr};
  pid_t pid;
  int result = posix_spawnp(&pid, program, nullptr, nullptr, argv, environ);
  if (result != 0) {
    XELOGE("Failed to run xdg-open for {}: {}", target, std::strerror(result));
    return;
  }
  // Reap it off the calling (often UI) thread so it doesn't linger as a zombie.
  std::thread([pid]() {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
}

void LaunchWebBrowser(const std::string_view url) { XdgOpen(std::string(url)); }

void LaunchFileExplorer(const std::filesystem::path& path) {
  XdgOpen(path.string());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  void* libsdl = dlopen("libSDL3.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl) {
    libsdl = dlopen("libSDL3.so", RTLD_LAZY | RTLD_LOCAL);
  }
  assert_not_null(libsdl);
  if (libsdl) {
    auto pSDL_ShowSimpleMessageBox =
        reinterpret_cast<SDL_ShowSimpleMessageBox_fn>(
            dlsym(libsdl, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      uint32_t flags;
      const char* title;
      char* message_copy = reinterpret_cast<char*>(alloca(message.size() + 1));
      std::memcpy(message_copy, message.data(), message.size());
      message_copy[message.size()] = '\0';

      switch (type) {
        default:
        case SimpleMessageBoxType::Help:
          title = "Xenia Help";
          flags = kSDL_MessageBoxInformation;
          break;
        case SimpleMessageBoxType::Warning:
          title = "Xenia Warning";
          flags = kSDL_MessageBoxWarning;
          break;
        case SimpleMessageBoxType::Error:
          title = "Xenia Error";
          flags = kSDL_MessageBoxError;
          break;
      }
      pSDL_ShowSimpleMessageBox(flags, title, message_copy, NULL);
    }
    dlclose(libsdl);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }
}  // namespace xe
