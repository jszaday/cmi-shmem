#ifndef CMI_SHMEM_COMMON_HH
#define CMI_SHMEM_COMMON_HH

#include <converse.h>

#include <array>
#include <cmi-shmem.hh>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#define DEBUGP(x) /* CmiPrintf x; */

constexpr std::size_t kDefaultSegmentSize = 16384;
CpvStaticDeclare(std::size_t, kSegmentSize);

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

struct ipc_metadata_;
using ipc_metadata_ptr_ = std::unique_ptr<ipc_metadata_>;
CsvStaticDeclare(ipc_metadata_ptr_, metadata_);

#if CMK_SMP
CsvStaticDeclare(CmiNodeLock, sleeper_lock);
#endif

using sleeper_map_t = std::vector<CthThread>;
CsvStaticDeclare(sleeper_map_t, sleepers);

// the data each pe shares with its peers
// contains pool of free blocks, heap, and receive queue
struct ipc_shared_ {
  std::array<std::atomic<std::uintptr_t>, kNumCutOffPoints> free;
  std::atomic<std::uintptr_t> queue;
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;

  ipc_shared_(std::uintptr_t begin, std::uintptr_t end)
      : queue(cmi::ipc::max), heap(begin), max(end) {
    for (auto& f : this->free) {
      f.store(cmi::ipc::max);
    }
  }
};

// shared data for each pe
struct ipc_metadata_ {
  // maps ranks to shared segments
  std::map<int, ipc_shared_*> shared;
  // physical node rank
  int mine;
  // base constructor
  ipc_metadata_(void) : mine(CmiMyNode()) {}
  // virtual destructor may be needed
  virtual ~ipc_metadata_() {}
};

inline static void initIpcShared_(ipc_shared_* shared) {
  auto begin = (std::uintptr_t)(sizeof(ipc_shared_) +
                                (sizeof(ipc_shared_) % ALIGN_BYTES));
  CmiAssert(begin != cmi::ipc::nil);
  auto end = begin + CpvAccess(kSegmentSize);
  new (shared) ipc_shared_(begin, end);
}

inline static ipc_shared_* makeIpcShared_(void) {
  auto* shared = (ipc_shared_*)(::operator new(sizeof(ipc_shared_) +
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
}

inline static void initSleepers_(void) {
  if (CmiMyRank() == 0) {
    CsvInitialize(sleeper_map_t, sleepers);
    CsvAccess(sleepers).resize(CmiMyNodeSize());
#if CMK_SMP
    CsvInitialize(CmiNodeLock, sleeper_lock);
    CsvAccess(sleeper_lock) = CmiCreateLock();
#endif
  }
}

inline static void putSleeper_(CthThread th) {
#if CMK_SMP
  if (CmiInCommThread()) {
    return;
  }
  CmiLock(CsvAccess(sleeper_lock));
#endif
  (CsvAccess(sleepers))[CmiMyRank()] = th;
#if CMK_SMP
  CmiAssert(!th || !CthIsMainThread(th));
  CmiUnlock(CsvAccess(sleeper_lock));
#endif
}

static void awakenSleepers_(void);

#endif
