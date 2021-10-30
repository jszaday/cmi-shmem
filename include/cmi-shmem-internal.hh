#ifndef CMI_SHMEM_COMMON_HH
#define CMI_SHMEM_COMMON_HH

#include <converse.h>

#include <array>
#include <cmi-shmem.hh>
#include <limits>
#include <map>
#include <memory>

#define DEBUGP(x) /* CmiPrintf x; */

constexpr std::uintptr_t kNilOffset = 0x0;
static constexpr auto kTail = std::numeric_limits<std::uintptr_t>::max();

constexpr std::size_t kDefaultSegmentSize = 16384;
CpvExtern(std::size_t, kSegmentSize);

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

struct ipc_metadata_;
using ipc_metadata_ptr_ = std::unique_ptr<ipc_metadata_>;
CsvStaticDeclare(ipc_metadata_ptr_, metadata_);

// the data each pe shares with its peers
// contains pool of free blocks, heap, and receive queue
struct ipc_shared_ {
  std::array<std::atomic<std::uintptr_t>, kNumCutOffPoints> free;
  std::atomic<std::uintptr_t> queue;
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;

  ipc_shared_(std::uintptr_t begin, std::uintptr_t end)
      : queue(kTail), heap(begin), max(end) {
    for (auto& f : this->free) {
      f.store(kTail);
    }
  }
};

inline static void initIpcShared_(ipc_shared_* shared) {
  auto begin = (std::uintptr_t)(sizeof(ipc_shared_) +
                                (sizeof(ipc_shared_) % ALIGN_BYTES));
  CmiAssert(begin != kNilOffset);
  auto end = begin + CpvAccess(kSegmentSize);
  new (shared) ipc_shared_(begin, end);
}

inline static ipc_shared_* makeIpcShared_(void) {
  auto* shared = (ipc_shared_*)(::operator new(sizeof(ipc_shared_) +
                                               CpvAccess(kSegmentSize)));
  initIpcShared_(shared);
  return shared;
}

// shared data for each pe
struct ipc_metadata_ {
  // maps ranks to shared segments
  std::map<int, ipc_shared_*> shared;
  // physical node rank
  int mine;
  // base constructor
  ipc_metadata_(void) : mine(CmiPhysicalRank(CmiMyPe())) {}
  // virtual destructor may be needed
  virtual ~ipc_metadata_() {}
};

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

inline static CmiIpcBlock* popBlock_(std::atomic<std::uintptr_t>& head,
                                     void* base) {
  auto prev = head.exchange(kNilOffset, std::memory_order_acquire);
  if (prev == kNilOffset) {
    return nullptr;
  } else if (prev == kTail) {
    auto check = head.exchange(prev, std::memory_order_release);
    CmiAssert(check == kNilOffset);
    return nullptr;
  } else {
    // translate the "home" PE's address into a local one
    CmiAssert(((std::uintptr_t)base % ALIGN_BYTES) == 0);
    auto* xlatd = (CmiIpcBlock*)((char*)base + prev);
    auto check = head.exchange(xlatd->next, std::memory_order_release);
    CmiAssert(check == kNilOffset);
    return xlatd;
  }
}

inline static bool pushBlock_(std::atomic<std::uintptr_t>& head,
                              std::uintptr_t value, void* base) {
  CmiAssert(value != kNilOffset);
  auto prev = head.exchange(kNilOffset, std::memory_order_acquire);
  if (prev == kNilOffset) {
    return false;
  }
  auto* block = (CmiIpcBlock*)((char*)base + value);
  block->next = prev;
  auto check = head.exchange(value, std::memory_order_release);
  CmiAssert(check == kNilOffset);
  return true;
}

#endif
