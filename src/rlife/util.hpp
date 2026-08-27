#pragma once

#include <cstdint>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <psapi.h>
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#ifdef __linux__
#include <sys/time.h>
#endif

namespace rlife::llsss {

inline std::uint64_t getMaxRSS() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
  }
#elif defined(__APPLE__)
  rusage usage{};
  if(getrusage(RUSAGE_SELF, &usage) == 0) {
    // macOS reports ru_maxrss in bytes.
    return static_cast<std::uint64_t>(usage.ru_maxrss);
  }
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  rusage usage{};
  if(getrusage(RUSAGE_SELF, &usage) == 0) {
    // Linux/BSD report ru_maxrss in KiB.
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
  }
#endif

  return 0;
}

inline std::string integer_format(std::uint64_t n) {
  if(n < 10 * 1024)
    return std::to_string(n);
  if(n < 10 * 1024 * 1024)
    return std::to_string(n >> 10) + "K";
  if(n < (10ll << 30))
    return std::to_string(n >> 20) + "M";
  return std::to_string(n >> 30) + "G";
}

} // namespace rlife::llsss
