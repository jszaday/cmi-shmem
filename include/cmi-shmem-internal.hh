#ifndef CMI_SHMEM_COMMON_HH
#define CMI_SHMEM_COMMON_HH

#include <converse.h>

#include <array>
#include <cmi-shmem.hh>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#define DEBUGP(x) CmiPrintf x;

namespace cmi {
namespace ipc {
CpvDeclare(std::size_t, kRecommendedCutoff);
}
}

CpvStaticDeclare(std::size_t, kSegmentSize);
constexpr std::size_t kDefaultSegmentSize = 8*1024*1024;

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

using ipc_managers_ = std::vector<std::unique_ptr<CmiIpcManager>>;
CsvStaticDeclare(ipc_managers_, managers_);

// the data each pe shares with its peers
// contains pool of free blocks, heap, and receive queue
struct CmiIpcShared {
  std::array<std::atomic<std::uintptr_t>, kNumCutOffPoints> free;
  std::atomic<std::uintptr_t> queue;
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;

  CmiIpcShared(std::uintptr_t begin, std::uintptr_t end)
      : queue(cmi::ipc::max), heap(begin), max(end) {
    for (auto& f : this->free) {
      f.store(cmi::ipc::max);
    }
  }
};

inline std::size_t whichBin_(std::size_t size);

inline static void initIpcShared_(CmiIpcShared* shared) {
  auto begin = (std::uintptr_t)(sizeof(CmiIpcShared) +
                               (sizeof(CmiIpcShared) % ALIGN_BYTES));
  CmiAssert(begin != cmi::ipc::nil);
  auto end = begin + CpvAccess(kSegmentSize);
  CmiPrintf("init'ing shared on %d\n", CmiMyPe());
  new (shared) CmiIpcShared(begin, end);
}

inline static CmiIpcShared* makeIpcShared_(void) {
  auto* shared = (CmiIpcShared*)(::operator new(sizeof(CmiIpcShared) +
                                                CpvAccess(kSegmentSize)));
  initIpcShared_(shared);
  return shared;
}

inline void initSegmentSize_(char** argv) {
  CpvInitialize(std::size_t, kSegmentSize);
  CmiInt8 value;
  auto flag =
      CmiGetArgLongDesc(argv, "+segmentsize", &value, "bytes per ipc segment");
  CpvAccess(kSegmentSize) = flag ? (std::size_t)value : kDefaultSegmentSize;
  CmiEnforceMsg(CpvAccess(kSegmentSize), "segment size must be non-zero!");
  using namespace cmi::ipc;
  CpvInitialize(std::size_t, kRecommendedCutoff);
  if (CmiGetArgLongDesc(argv, "+ipccutoff", &value, "enforce cutoff for ipc blocks")) {
    auto bin = whichBin_((std::size_t)value);  
    CmiEnforceMsg(bin < kNumCutOffPoints, "ipc cutoff out of range!");
    CpvAccess(kRecommendedCutoff) = kCutOffPoints[bin];
  } else {
    auto max = CpvAccess(kSegmentSize) / kNumCutOffPoints;
    auto bin = (std::intptr_t)whichBin_(max) - 1;
    CpvAccess(kRecommendedCutoff) = kCutOffPoints[(bin >= 0) ? bin : 0];
  }
}

#endif
