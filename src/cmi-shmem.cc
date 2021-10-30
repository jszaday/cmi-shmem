#if CMI_HAS_XPMEM
#include "cmi-xpmem.cc"
#else
#include "cmi-shm.cc"
#endif

CpvDeclare(std::size_t, kSegmentSize);

void* CmiBlockToMsg(CmiIpcBlock* block, bool init) {
  auto* msg = CmiBlockToMsg(block);
  if (init) {
    // NOTE ( this is identical to code in CmiAlloc )
    CmiAssert(((uintptr_t)msg % ALIGN_BYTES) == 0);
    CmiInitMsgHeader(msg, block->size);
    SIZEFIELD(msg) = block->size;
    REFFIELDSET(msg, 1);
  }
  return msg;
}

static void CmiHandleBlock_(void*, double) {
  auto* block = CmiPopBlock();
  if (block != nullptr) {
    CmiHandleMessage(CmiBlockToMsg(block));
  }
}

void CmiIpcBlockCallback(int cond) {
  CcdCallOnConditionKeep(cond, CmiHandleBlock_, nullptr);
}

CmiIpcBlock* CmiPopBlock(void) {
  auto& meta = CsvAccess(metadata_);
  auto& shared = meta->shared[meta->mine];
  return popBlock_(shared->queue, shared);
}

bool CmiPushBlock(CmiIpcBlock* block) {
  auto& meta = CsvAccess(metadata_);
  auto& shared = meta->shared[block->src];
  auto& queue = shared->queue;
  CmiAssert(block->dst == meta->mine);
  return pushBlock_(queue, block->orig, shared);
}

static std::uintptr_t allocBlock_(ipc_shared_* meta, std::size_t size);

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto myNode = CmiPhysicalNodeID(myPe);
  auto theirRank = CmiPhysicalRank(pe);
  auto theirNode = CmiPhysicalNodeID(pe);
  CmiAssert((myRank != theirRank) && (myNode == theirNode));
  auto& meta = CsvAccess(metadata_);
  auto& shared = meta->shared[theirRank];
  auto bin = whichBin_(size);
  CmiAssert(bin < kNumCutOffPoints);

  auto* block = popBlock_(shared->free[bin], shared);
  if (block == nullptr) {
    auto totalSize = kCutOffPoints[bin];
    auto offset = allocBlock_(shared, totalSize);
    if (offset == kNilOffset) {
      return nullptr;
    }
    // the block's address is relative to the share
    block = (CmiIpcBlock*)((char*)shared + offset);
    CmiAssert(((std::uintptr_t)block % alignof(CmiIpcBlock)) == 0);
    // construct the block
    new (block) CmiIpcBlock(totalSize, offset);
  }

  block->src = theirRank;
  block->dst = myRank;

  return block;
}

void CmiFreeBlock(CmiIpcBlock* block) {
  auto& meta = CsvAccess(metadata_);
  auto bin = whichBin_(block->size);
  CmiAssertMsg(bin < kNumCutOffPoints);
  auto& shared = meta->shared[block->src];
  auto& free = shared->free[bin];
  while (!pushBlock_(free, block->orig, shared))
    ;
}

CmiIpcBlock* CmiIsBlock(void* addr) {
  auto& meta = CsvAccess(metadata_);
  auto& shared = meta->shared[meta->mine];
  auto* begin = (char*)shared;
  auto* end = begin + shared->max;
  if (begin < addr && addr < end) {
    return (CmiIpcBlock*)((char*)addr - sizeof(CmiIpcBlock));
  } else {
    return nullptr;
  }
}

static std::uintptr_t allocBlock_(ipc_shared_* meta, std::size_t size) {
  auto res = meta->heap.exchange(kNilOffset, std::memory_order_acquire);
  if (res == kNilOffset) {
    return kNilOffset;
  } else {
    auto next = res + size + sizeof(CmiIpcBlock);
    auto offset = size % alignof(CmiIpcBlock);
    auto status = meta->heap.exchange(next + offset, std::memory_order_release);
    CmiAssert(status == kNilOffset);
    if (next < meta->max) {
      return res;
    } else {
      return kNilOffset;
    }
  }
}
