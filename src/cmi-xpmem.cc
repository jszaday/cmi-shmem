#include <cmi-shmem-internal.hh>
#include <map>
#include <memory>

extern "C" {
#include <xpmem.h>
}

// "borrowed" from VADER
// (https://github.com/open-mpi/ompi/tree/386ba164557bb8115131921041757be94a989646/opal/mca/smsc/xpmem)
#define OPAL_DOWN_ALIGN(x, a, t) ((x) & ~(((t)(a)-1)))
#define OPAL_DOWN_ALIGN_PTR(x, a, t) \
  ((t)OPAL_DOWN_ALIGN((uintptr_t)x, a, uintptr_t))
#define OPAL_ALIGN(x, a, t) (((x) + ((t)(a)-1)) & ~(((t)(a)-1)))
#define OPAL_ALIGN_PTR(x, a, t) ((t)OPAL_ALIGN((uintptr_t)x, a, uintptr_t))
#define OPAL_ALIGN_PAD_AMOUNT(x, s) \
  ((~((uintptr_t)(x)) + 1) & ((uintptr_t)(s)-1))

struct ipc_shared_ {
  std::array<std::atomic<CmiIpcBlock*>, kNumCutOffPoints> free;
  std::atomic<CmiIpcBlock*> queue;
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;

  ipc_shared_(std::uintptr_t begin, std::uintptr_t end)
  : queue((CmiIpcBlock*)kTail), heap(begin), max(end) {
    for (auto& f : this->free) {
      f.store((CmiIpcBlock*)kTail);
    }
  }
};

struct init_msg_ {
  char core[CmiMsgHeaderSizeBytes];
  int from;
  xpmem_segid_t segid;
  ipc_shared_* shared;
};

inline static ipc_shared_* makeIpcShared_(void) {
  auto& size = CpvAccess(kSegmentSize);
  constexpr auto shsize = sizeof(ipc_shared_);
  auto* shared = (ipc_shared_*)(::operator new(shsize + size));
  auto begin = (std::uintptr_t)(shsize + (shsize % ALIGN_BYTES));
  CmiAssert(begin != 0x0);
  auto end = begin + size;
  new (shared) ipc_shared_(begin, end);
  return shared;
}

struct ipc_metadata_ {
  // maps ranks to segments
  std::map<int, xpmem_segid_t> segments;
  // maps segments to xpmem apids
  std::map<xpmem_segid_t, xpmem_apid_t> instances;
  // maps ranks to shared segments
  std::map<int, ipc_shared_*> shared;
  // our physical rank
  int mine;
  // number of physical peers
  int nPeers;
  // initialization lock
  CthThread init;

  ipc_metadata_(void) : mine(CmiPhysicalRank(CmiMyPe())) {
    // initialize our ipc shared region
    this->shared[mine] = makeIpcShared_();
  }

  void put_segment(int rank, const xpmem_segid_t& segid) {
    auto ins = this->segments.emplace(rank, segid);
    CmiAssert(ins.second);
  }

  xpmem_segid_t get_segment(int rank) {
    auto search = this->segments.find(rank);
    if (search == std::end(this->segments)) {
      if (mine == rank) {
        auto segid =
            xpmem_make(0, XPMEM_MAXADDR_SIZE, XPMEM_PERMIT_MODE, (void*)0666);
        this->put_segment(mine, segid);
        return segid;
      } else {
        return -1;
      }
    } else {
      return search->second;
    }
  }

  xpmem_apid_t get_instance(int rank) {
    auto segid = this->get_segment(rank);
    if (segid >= 0) {
      auto search = this->instances.find(segid);
      if (search == std::end(this->instances)) {
        auto apid = xpmem_get(segid, XPMEM_RDWR, XPMEM_PERMIT_MODE, NULL);
        CmiAssertMsg(apid >= 0, "invalid segid?");
        auto ins = this->instances.emplace(segid, apid);
        CmiAssert(ins.second);
        search = ins.first;
      }
      return search->second;
    } else {
      return -1;
    }
  }
};

using ipc_metadata_ptr_ = std::unique_ptr<ipc_metadata_>;
CsvStaticDeclare(ipc_metadata_ptr_, metadata_);

// TODO ( fold this into the extant pop block mechanism )
CmiIpcBlock* popTranslateBlock_(std::atomic<CmiIpcBlock*>& free, char* base) {
  auto* head = free.exchange(nullptr, std::memory_order_acquire);
  if (head == nullptr) {
    return nullptr;
  } else if (head == (CmiIpcBlock*)kTail) {
    auto* check = free.exchange(head, std::memory_order_release);
    CmiAssert(check == nullptr);
    return nullptr;
  } else {
    // translate the "home" PE's address into a local one
    auto* xlatd = (CmiIpcBlock*)(base + (std::uintptr_t)head);
    auto* check = free.exchange(xlatd->next, std::memory_order_release);
    CmiAssert(check == nullptr);
    return xlatd;
  }
}

static std::uintptr_t allocBlock_(ipc_shared_* meta, std::size_t size) {
  constexpr auto nil = 0x0;
  auto res = meta->heap.exchange(nil, std::memory_order_acquire);
  if (res == nil) {
    return nil;
  } else {
    auto next = res + size + sizeof(CmiIpcBlock);
    auto offset = size % alignof(CmiIpcBlock);
    auto status = meta->heap.exchange(next + offset, std::memory_order_release);
    CmiAssert(status == nil);
    if (next < meta->max) {
      return res;
    } else {
      return nil;
    }
  }
}

void* translateAddr_(ipc_metadata_* meta, int rank, void* remote_ptr,
                     const std::size_t& size) {
  // TODO ( add support for SMP mode )
  auto mine = meta->mine;
  if (mine == rank) {
    return remote_ptr;
  } else {
    auto apid = meta->get_instance(rank);
    CmiAssert(apid >= 0);
    // this magic was borrowed from VADER
    uintptr_t attach_align = 1 << 23;
    auto base = OPAL_DOWN_ALIGN_PTR(remote_ptr, attach_align, uintptr_t);
    auto bound =
        OPAL_ALIGN_PTR(remote_ptr + size - 1, attach_align, uintptr_t) + 1;

    using offset_type = decltype(xpmem_addr::offset);
    xpmem_addr addr{.apid = apid, .offset = (offset_type)base};
    auto* ctx = xpmem_attach(addr, bound - base, NULL);
    CmiAssert(ctx != (void*)-1);

    return (void*)((uintptr_t)ctx +
                   (ptrdiff_t)((uintptr_t)remote_ptr - (uintptr_t)base));
  }
}

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
  CmiAssert(((std::uintptr_t)shared % ALIGN_BYTES) == 0);

  auto* block = popTranslateBlock_(shared->free[bin], (char*)shared);
  if (block == nullptr) {
    auto totalSize = kCutOffPoints[bin];
    auto offset = allocBlock_(shared, totalSize);
    if (offset == 0x0) {
      return nullptr;
    }
    // the block's address is relative to the share
    block = (CmiIpcBlock*)((char*)shared + offset);
    CmiAssert(((std::uintptr_t)block % alignof(CmiIpcBlock)) == 0);
    // construct the block
    new (block) CmiIpcBlock(totalSize, offset);
    block->src = theirRank;
    block->dst = myRank;
  }

  return block;
}

// TODO ( implement this function! )
// (OpenSHMEM might've had a mechanism for determining addr range?)
CmiIpcBlock* CmiIsBlock(void* addr) {
  return (CmiIpcBlock*)((char*)addr - sizeof(CmiIpcBlock));
}

void CmiCacheBlock(CmiIpcBlock* block) {
  return;
}

void CmiFreeBlock(CmiIpcBlock* block) {
  auto& meta = CsvAccess(metadata_);
  auto ours = block->src == meta->mine;
  if (ours) {
    auto bin = whichBin_(block->size);
    auto& free = meta->shared[meta->mine]->free[bin];
    pushBlock_(free, block, (CmiIpcBlock*)block->orig);
  } else {
    // TODO ( implement this codepath! )
    CmiAbort("allocator-side free'ing unsupported\n");
  }
}

CmiIpcBlock* CmiPopBlock(void) {
  auto& meta = CsvAccess(metadata_);
  auto* shared = meta->shared[meta->mine];
  return popTranslateBlock_(shared->queue, (char*)shared);
}

bool CmiPushBlock(CmiIpcBlock* block) {
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta && (block->src != meta->mine));
  auto& queue = meta->shared[block->src]->queue;
  // the value we want to save in the "home"
  // queue is the block's original address
  return pushBlock_(queue, block, (CmiIpcBlock*)block->orig);
}

static void handleInitialize_(void* msg) {
  auto* imsg = (init_msg_*)msg;
  auto& meta = CsvAccess(metadata_);
  // extract the segment id and shared region
  // from the msg (registering it in our metadata)
  meta->put_segment(imsg->from, imsg->segid);
  meta->shared[imsg->from] = (ipc_shared_*)translateAddr_(
      meta.get(), imsg->from, imsg->shared, sizeof(ipc_shared_));
  // then free the message
  CmiFree(imsg);
  // if we received messages from all our peers:
  if ((meta->nPeers == meta->shared.size()) && meta->init) {
    // resume the sleeping thread
    if (CmiMyPe() == 0) {
      CmiPrintf("CMI> xpmem pool init'd with %luB segment.\n",
                CpvAccess(kSegmentSize));
    }
    CthAwaken(meta->init);
  }
}

void CmiInitIpcMetadata(char** argv, CthThread th) {
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  initSegmentSize_(argv);
  CmiNodeAllBarrier();

  CsvInitialize(ipc_metadata_ptr_, metadata_);
  auto& meta = CsvAccess(metadata_);
  meta.reset(new ipc_metadata_());
  meta->init = th;

  int* pes;
  int nPes;
  auto thisPe = CmiMyPe();
  auto thisNode = CmiPhysicalNodeID(CmiMyPe());
  CmiGetPesOnPhysicalNode(thisNode, &pes, &nPes);
  meta->nPeers = nPes;

  if (nPes > 1) {
    auto initHdl = CmiRegisterHandler(handleInitialize_);
    auto* imsg = (init_msg_*)CmiAlloc(sizeof(init_msg_));
    CmiSetHandler(imsg, initHdl);
    imsg->from = meta->mine;
    imsg->segid = meta->get_segment(meta->mine);
    imsg->shared = meta->shared[meta->mine];
    // send messages to all the pes on this node
    for (auto i = 0; i < nPes; i++) {
      auto& pe = pes[i];
      auto last = i == (nPes - 1);
      if (pe == thisPe) {
        if (last) {
          CmiFree(imsg);
        }
        continue;
      } else if (last) {
        // free'ing with the last send
        CmiSyncSendAndFree(pe, sizeof(init_msg_), (char*)imsg);
      } else {
        // then sending (without free) otherwise
        CmiSyncSend(pe, sizeof(init_msg_), (char*)imsg);
      }
    }
  } else {
    // single pe -- wake up the sleeping thread
    CthAwaken(th);
  }
}
