// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace host::test_env {
inline bool agent_copy() {
  // Keep the account lock for automation launched with an older configuration.
  for (const char* flag : {"MELEE_AGENT_COPY", "YOUGAME_AGENT_COPY"}) {
    const char* value = std::getenv(flag);
    if (value && std::strcmp(value, "1") == 0) return true;
  }
  const char* legacy_label = std::getenv("YOUGAME_TEST_LABEL");
  return legacy_label && *legacy_label;
}
inline const char* getenv(const char* name) { return std::getenv(name); }
} // namespace host::test_env
