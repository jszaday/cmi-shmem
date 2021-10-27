#include <converse.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmi-shmem.hh>
#include <limits>
#include <memory>

#define DEBUGP(x) /** CmiPrintf x; */

const char* kName = "cmi_shmem_meta_";
const std::size_t kDefaultSize = 16384;

static constexpr auto kTail = std::numeric_limits<std::uintptr_t>::max();

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

struct ipc_queue_ {
  const int node;
  std::atomic<CmiIpcBlock*> head;
  ipc_queue_(int node_) : node(node_), head((CmiIpcBlock*)kTail) {}
};

struct ipc_metadata_ {
  std::array<std::atomic<CmiIpcBlock*>, kNumCutOffPoints> free;
  std::atomic<char*> heap;
  ipc_queue_* queues;
  char* max;
  int node;
  int fd;

  ipc_metadata_(int fd_) : node(CmiPhysicalRank(CmiMyPe())), fd(fd_) {}
};

struct ipc_metadata_deleter_ {
  inline void operator()(ipc_metadata_* meta) {
    if (meta->node == CmiPhysicalRank(CmiMyPe())) {
      auto fd = meta->fd;
      auto status = munmap((void*)meta->queues, kDefaultSize);
      status = status && close(fd);
      shm_unlink(kName);
    }
  }
};

using ipc_metadata_ptr_ = std::unique_ptr<ipc_metadata_, ipc_metadata_deleter_>;
CsvStaticDeclare(ipc_metadata_ptr_, metadata_);

static ipc_metadata_* open_metadata_(const char* name, void* addr,
                                     std::size_t size) {
  auto fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
  auto init = fd >= 0;

  if (init) {
    auto status = ftruncate(fd, size);
    CmiAssert(status >= 0);
  } else {
    fd = shm_open(name, O_RDWR, 0666);
    CmiAssert(fd >= 0);
  }

  auto* res =
      (char*)mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CmiAssert(res != MAP_FAILED && res == addr);

  auto mine = CmiPhysicalNodeID(CmiMyPe());
  auto nPes = CmiNumPesOnPhysicalNode(mine);
  auto metaOffset = nPes * sizeof(ipc_queue_);
  metaOffset += (metaOffset % alignof(ipc_metadata_));
  auto* meta = (ipc_metadata_*)(res + metaOffset);
  CmiAssert(((std::uintptr_t)res % alignof(ipc_queue_)) == 0);
  CmiAssert((((std::uintptr_t)meta % alignof(ipc_metadata_)) == 0));

  if (init) {
    new (meta) ipc_metadata_(fd);

    meta->queues = (ipc_queue_*)res;
    for (auto rank = 0; rank < nPes; rank++) {
      new (&meta->queues[rank]) ipc_queue_(rank);
    }

    auto* heap = (char*)meta + sizeof(ipc_metadata_);
    meta->max = res + size;
    meta->heap.store(heap);

    DEBUGP(("%d> pool has %ld free bytes\n", CmiMyPe(),
            (std::intptr_t)(meta->max - heap)));

    for (auto pt = 0; pt < kNumCutOffPoints; pt++) {
      meta->free[pt].store((CmiIpcBlock*)kTail);
    }
  }

  return meta;
}

void CmiInitIpcMetadata(char** argv) {
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiNodeAllBarrier();

  CsvInitialize(ipc_metadata_ptr_, metadata_);
  // TODO ( figure out a better way to pick size/magic number )
  CsvAccess(metadata_).reset(
      open_metadata_(kName, (void*)0x42424000, kDefaultSize));
  CmiAssert((bool)CsvAccess(metadata_));

  CmiNodeAllBarrier();

  // NOTE ( this has to match across all PEs on a node )
  DEBUGP(
      ("%d> meta is at address %p\n", CmiMyPe(), CsvAccess(metadata_).get()));
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

static CmiIpcBlock* findBlock_(ipc_metadata_* meta, std::size_t bin) {
  auto& head = meta->free[bin];
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

static CmiIpcBlock* allocBlock_(ipc_metadata_* meta, std::size_t size) {
  auto* res = meta->heap.exchange(nullptr, std::memory_order_acquire);
  if (res == nullptr) {
    return nullptr;
  } else {
    res += (std::uintptr_t)res % alignof(CmiIpcBlock);
    auto* next = res + size + sizeof(CmiIpcBlock);
    auto* status = meta->heap.exchange(next, std::memory_order_release);
    CmiAssert(status == nullptr);
    if (next < meta->max) {
      return (CmiIpcBlock*)res;
    } else {
      return nullptr;
    }
  }
}

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t reqd) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto myNode = CmiPhysicalNodeID(myPe);
  auto theirRank = CmiPhysicalRank(pe);
  auto theirNode = CmiPhysicalNodeID(pe);
  CmiAssert((myRank != theirRank) && (myNode == theirNode));

  auto bin = whichBin_(reqd);
  if (bin >= kNumCutOffPoints) {
    return nullptr;
  }
  auto size = kCutOffPoints[bin];

  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta != nullptr);
  auto* block = findBlock_(meta, bin);

  if (block == nullptr) {
    block = allocBlock_(meta, size);
    if (block == nullptr) {
      return nullptr;
    }
    new (block) CmiIpcBlock(size);
  }

  block->src = theirRank;
  block->dst = myRank;

  return block;
}

bool CmiPushBlock(CmiIpcBlock* blk) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert((meta != nullptr) && (myRank == blk->dst));
  auto* queue = &(meta->queues[blk->src]);
  auto* prev = queue->head.exchange(nullptr, std::memory_order_acquire);
  if (prev == nullptr) {
    return false;
  }
  blk->next = prev;
  auto* check = queue->head.exchange(blk, std::memory_order_release);
  CmiAssert(check == nullptr);
  return true;
}

CmiIpcBlock* CmiPopBlock(void) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta != nullptr);
  auto* queue = &(meta->queues[myRank]);
  auto* prev = queue->head.exchange(nullptr, std::memory_order_acquire);
  if (prev == nullptr) {
    return nullptr;
  }
  auto nil = prev == (CmiIpcBlock*)kTail;
  auto* next = nil ? prev : prev->next;
  auto* check = queue->head.exchange(next, std::memory_order_release);
  CmiAssert(check == nullptr);
  return nil ? nullptr : prev;
}

void CmiCacheBlock(CmiIpcBlock*) { return; }

void CmiFreeBlock(CmiIpcBlock* blk) {
  auto bin = whichBin_(blk->size);
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert((meta != nullptr) && (bin < kNumCutOffPoints));
  auto& head = meta->free[bin];
  CmiIpcBlock* prev;
  while ((prev = head.exchange(nullptr, std::memory_order_acquire)) == nullptr)
    ;
  blk->next = prev;
  auto* check = head.exchange(blk, std::memory_order_release);
  CmiAssert(check == nullptr);
}
