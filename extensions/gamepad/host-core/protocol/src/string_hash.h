// SPDX-License-Identifier: GPL-3.0-only
// Derived from Sunshine d0b4a88ef727f8dabf80cc970a14739b55c885de.
// Modified for Cemu Remote GamePad, 2026-10-27; upstream copyrights retained.
#include <cstdint>
#include <limits>
#include <ctime>
#include <cmath>
#include <cstring>
#include <sstream>
/**
 * @file src/string_hash.h
 * @brief Transparent hashing for containers with owning string keys.
 */
#pragma once

// standard includes
#include <cstddef>
#include <functional>
#include <string_view>

namespace util {
  /**
   * @brief Hash strings, string views, and C strings without temporary allocations.
   */
  struct transparent_string_hash {
    using is_transparent = void;  ///< Enable heterogeneous unordered-container lookup.

    /**
     * @brief Hash all characters in a string-like key, including embedded nulls in views.
     *
     * @param value Text to hash.
     * @return Hash shared by equivalent owning and non-owning string keys.
     */
    std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view> {}(value);
    }
  };
}  // namespace util
