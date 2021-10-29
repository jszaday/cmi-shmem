#ifndef CMI_SHMEM_COMMON_HH
#define CMI_SHMEM_COMMON_HH

#include <converse.h>

#include <array>
#include <cmi-shmem.hh>
#include <limits>

#define DEBUGP(x) /* CmiPrintf x; */

static constexpr auto kTail = std::numeric_limits<std::uintptr_t>::max();

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

const std::size_t kDefaultSegmentSize = 16384;

CpvExtern(std::size_t, kSegmentSize);

inline void initSegmentSize_(char** argv) {
  CpvInitialize(std::size_t, kSegmentSize);
  CmiInt8 value;
  auto flag =
      CmiGetArgLongDesc(argv, "+segmentsize", &value, "bytes per ipc segment");
  CpvAccess(kSegmentSize) = flag ? (std::size_t)value : kDefaultSegmentSize;
}

// TODO ( find a better way to do this )
inline std::size_t whichBin_(std::size_t size) {
  std::size_t bin;
  for (bin = 0; bin < kNumCutOffPoints; bin++) {
    if (size <= kCutOffPoints[bin]) {
      break;
    }
  }
  return bin;
}

inline static CmiIpcBlock* popBlock_(std::atomic<CmiIpcBlock*>& head) {
  auto* prev = head.exchange(nullptr, std::memory_order_acquire);
  if (prev == nullptr) {
    return nullptr;
  }
  auto nil = prev == (CmiIpcBlock*)kTail;
  auto* next = nil ? prev : prev->next;
  auto* check = head.exchange(next, std::memory_order_release);
  CmiAssert(check == nullptr);
  return nil ? nullptr : prev;
}

inline static bool pushBlock_(std::atomic<CmiIpcBlock*>& head,
                              CmiIpcBlock* block,
                              CmiIpcBlock* value = nullptr) {
  if (value == nullptr) value = block;
  CmiAssert(value != nullptr);
  auto* prev = head.exchange(nullptr, std::memory_order_acquire);
  if (prev == nullptr) {
    return false;
  }
  block->next = prev;
  auto* check = head.exchange(value, std::memory_order_release);
  CmiAssert(check == nullptr);
  return true;
}

#endif
