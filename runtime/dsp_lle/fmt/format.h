// Minimal <fmt/format.h> shim. The vendored DSP core only pulls fmt in for
// formatter specializations used by logging, which the runtime no-ops — so
// these are defined but never instantiated. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace fmt {
struct format_parse_context {
  const char* m_p = nullptr;
  constexpr const char* begin() const { return m_p; }
  constexpr const char* end() const { return m_p; }
  constexpr void advance_to(const char*) {}
};
template <typename T, typename Char = char>
struct formatter {
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename V, typename FormatContext>
  auto format(const V&, FormatContext& ctx) const { return ctx.out(); }
};
template <typename... A>
inline std::string format(const char*, A&&...) { return std::string(); }
}  // namespace fmt
