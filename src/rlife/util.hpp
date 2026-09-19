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

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_info.h>
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
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

  const kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);

  if (kr != KERN_SUCCESS)
    return 0;

  if (count >= TASK_VM_INFO_REV3_COUNT && info.ledger_phys_footprint_peak > 0)
    return static_cast<std::uint64_t>(info.ledger_phys_footprint_peak);

  // Old-system fallback: current footprint only.
  if (count >= TASK_VM_INFO_REV1_COUNT)
      return static_cast<std::uint64_t>(info.phys_footprint);
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
