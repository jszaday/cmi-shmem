#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmi-shmem-internal.hh>
#include <memory>

const char* kName = "cmi_shmem_meta_";
const std::size_t kDefaultSize = 16384;

using ipc_queue_ = std::atomic<CmiIpcBlock*>;

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

static ipc_metadata_* openMetadata_(const char* name, void* addr,
                                    std::size_t size) {
  auto fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
  auto init = fd >= 0;
  // if we're the first process to init the segment
  if (init) {
    // truncate it to the correct size
    auto status = ftruncate(fd, size);
    CmiAssert(status >= 0);
  } else {
    fd = shm_open(name, O_RDWR, 0666);
    CmiAssert(fd >= 0);
  }
  // map the shm segment to the specified address
  auto* res =
      (char*)mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CmiAssert(res != MAP_FAILED && res == addr);
  // various physical properties of the current PE
  auto pe = CmiMyPe();
  auto mine = CmiPhysicalNodeID(pe);
  auto nPes = CmiNumPesOnPhysicalNode(mine);
  auto first = CmiGetFirstPeOnPhysicalNode(mine);
  // determine all the offsets for the metadata
  auto metaOffset = nPes * sizeof(ipc_queue_);
  metaOffset += (metaOffset % alignof(ipc_metadata_));
  auto* meta = (ipc_metadata_*)(res + metaOffset);
  CmiAssert(((std::uintptr_t)res % alignof(ipc_queue_)) == 0);
  CmiAssert((((std::uintptr_t)meta % alignof(ipc_metadata_)) == 0));
  // if we're the node's first PE
  if (pe == first) {
    // zero the metadata's segment of memory
    std::fill(res, res + metaOffset + sizeof(ipc_metadata_), '\0');
    // initialize the metadata in place
    new (meta) ipc_metadata_(fd);
    // initialize all the queues (to empty lists)
    meta->queues = (ipc_queue_*)res;
    for (auto rank = 0; rank < nPes; rank++) {
      new (&meta->queues[rank]) ipc_queue_((CmiIpcBlock*)kTail);
    }
    // set the bounds of the heap
    auto* heap = (char*)meta + sizeof(ipc_metadata_);
    // align the start of the heap to ALIGN_BYTES
    heap += (uintptr_t)heap % ALIGN_BYTES;
    meta->max = res + size;
    meta->heap.store(heap);
    DEBUGP(("%d> pool has %ld free bytes\n", pe,
            (std::intptr_t)(meta->max - heap)));
    // initialize all the free-lists (to empty lists)
    for (auto pt = 0; pt < kNumCutOffPoints; pt++) {
      meta->free[pt].store((CmiIpcBlock*)kTail);
    }
  }

  return meta;
}

void CmiInitIpcMetadata(char** argv, CthThread th) {
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiNodeAllBarrier();

  CsvInitialize(ipc_metadata_ptr_, metadata_);
  // TODO ( figure out a better way to pick size/magic number )
  CsvAccess(metadata_).reset(
      openMetadata_(kName, (void*)0x42424000, kDefaultSize));
  CmiAssert((bool)CsvAccess(metadata_));
  // TODO ( identify which fn should be used here )
  CmiBarrier();
  // NOTE ( this has to match across all PEs on a node )
  DEBUGP(
      ("%d> meta is at address %p\n", CmiMyPe(), CsvAccess(metadata_).get()));

  // resume the callback
  if (th) CthAwaken(th);
}

static CmiIpcBlock* findBlock_(ipc_metadata_* meta, std::size_t bin) {
  return popBlock_(meta->free[bin]);
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

CmiIpcBlock* CmiIsBlock(void* addr) {
  auto* meta = CsvAccess(metadata_).get();
  auto* start = (char*)meta + sizeof(ipc_metadata_);
  auto* end = meta->max;
  if (start < addr && addr < end) {
    return (CmiIpcBlock*)((char*)addr - sizeof(CmiIpcBlock));
  } else {
    return nullptr;
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
  auto& queue = meta->queues[blk->src];
  return pushBlock_(queue, blk);
}

CmiIpcBlock* CmiPopBlock(void) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta != nullptr);
  auto& queue = meta->queues[myRank];
  return popBlock_(queue);
}

void CmiCacheBlock(CmiIpcBlock*) { return; }

void CmiFreeBlock(CmiIpcBlock* blk) {
  auto bin = whichBin_(blk->size);
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert((meta != nullptr) && (bin < kNumCutOffPoints));
  auto& head = meta->free[bin];
  while (!pushBlock_(head, blk))
    ;
}
