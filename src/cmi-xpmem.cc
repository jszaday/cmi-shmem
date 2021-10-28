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

  ipc_shared_(void) : queue((CmiIpcBlock*)kTail) {
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

struct ipc_metadata_ {
  // maps ranks to segments
  std::map<int, xpmem_segid_t> segments;
  // maps segments to xpmem apids
  std::map<xpmem_segid_t, xpmem_apid_t> instances;
  // maps ranks to shared segments
  std::map<int, ipc_shared_*> shared;
  // maps ranks to (cached) blocks
  // TODO ( the cache is _NOT_ SMP safe )
  // TODO ( take sizes into consideration )
  std::map<int, CmiIpcBlock*> cache;
  // our physical rank
  int mine;
  // number of physical peers
  int nPeers;
  // initialization lock
  CthThread init;

  ipc_metadata_(void) : mine(CmiPhysicalRank(CmiMyPe())) {
    // initialize our ipc shared region
    this->shared[mine] = new ipc_shared_();
  }

  void put_segment(int rank, const xpmem_segid_t& segid) {
    auto ins = this->segments.emplace(rank, segid);
    CmiAssert(ins.second);
  }

  CmiIpcBlock* get_cache(int rank) const {
    auto search = this->cache.find(rank);
    if (search == std::end(this->cache)) {
      return (CmiIpcBlock*)kTail;
    } else {
      return search->second;
    }
  }

  CmiIpcBlock* seek_cached(int rank, std::size_t size) {
    auto* block = this->get_cache(rank);
    while (block != (CmiIpcBlock*)kTail) {
      if (block->size >= size && block->free.load(std::memory_order_relaxed)) {
        break;
      } else {
        block = block->cached;
      }
    }
    return (block == (CmiIpcBlock*)kTail) ? nullptr : block;
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

inline static bool acquireBlock_(ipc_metadata_* meta, CmiIpcBlock* block) {
  block->dst = meta->mine;
  return block->free.exchange(false, std::memory_order_relaxed);
}

// TODO ( fold this into the extant pop block mechanism )
CmiIpcBlock* allocTranslateBlock_(ipc_metadata_* meta, int rank,
                                  std::size_t size) {
  auto bin = whichBin_(size);
  CmiAssert(bin < kNumCutOffPoints);
  auto& free = meta->shared[rank]->free[bin];
  auto* head = free.exchange(nullptr, std::memory_order_acquire);
  if (head == nullptr) {
    return nullptr;
  } else if (head == (CmiIpcBlock*)kTail) {
    auto* check = free.exchange(head, std::memory_order_release);
    CmiAssert(check == nullptr);
    return nullptr;
  } else {
    // translate the "home" PE's address into a local one
    auto* xlatd = (CmiIpcBlock*)translateAddr_(meta, rank, head,
                                               size + sizeof(CmiIpcBlock));
    auto* next = xlatd->next;
    auto* check = free.exchange(next, std::memory_order_release);
    CmiAssert(check == nullptr);
    return xlatd;
  }
}

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size) {
  auto myPe = CmiMyPe();
  auto myRank = CmiPhysicalRank(myPe);
  auto myNode = CmiPhysicalNodeID(myPe);
  auto theirRank = CmiPhysicalRank(pe);
  auto theirNode = CmiPhysicalNodeID(pe);
  CmiAssert((myRank != theirRank) && (myNode == theirNode));
  auto* meta = CsvAccess(metadata_).get();
  auto* block = meta->seek_cached(theirRank, size);
  if (block == nullptr) {
    // allocate a block from the rank
    block = allocTranslateBlock_(meta, theirRank, size);
    // then acquire it if we succeed
    if (block != nullptr) {
      auto acq = acquireBlock_(meta, block);
      CmiAssert(acq);
    }
  } else {
    // sanity check (we should get our own block)
    CmiAssert(block->dst == meta->mine);
    // set the block as in-use again
    auto free = block->free.exchange(false, std::memory_order_relaxed);
    CmiAssert(free);
  }
  return block;
}

// TODO ( implement this function! )
// (OpenSHMEM might've had a mechanism for determining addr range?)
CmiIpcBlock* CmiIsBlock(void* addr) {
  return (CmiIpcBlock*)((char*)addr - sizeof(CmiIpcBlock));
}

void CmiCacheBlock(CmiIpcBlock* block) {
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta->mine == block->dst);
  // if the block hasn't been cached already:
  if (block->cached == nullptr) {
    // add it to the cache linked list:
    block->cached = meta->get_cache(block->src);
    meta->cache[block->src] = block;
    DEBUGP(("%d> cached block (%p) from %d.\n", meta->mine, block->orig,
            block->src));
  }
}

void CmiFreeBlock(CmiIpcBlock* block) {
  auto* meta = CsvAccess(metadata_).get();
  auto ours = block->src == meta->mine;
  if (ours) {
    auto used = block->free.exchange(true, std::memory_order_relaxed);
    CmiAssertMsg(!used, "double free?");
    if (block->cached == nullptr) {
      auto bin = whichBin_(block->size);
      auto& free = meta->shared[meta->mine]->free[bin];
      pushBlock_(free, block);
    }
  } else {
    // TODO ( implement this codepath! )
    CmiAbort("allocator-side free'ing unsupported\n");
  }
}

CmiIpcBlock* CmiPopBlock(void) {
  auto* meta = CsvAccess(metadata_).get();
  auto& queue = meta->shared[meta->mine]->queue;
  return popBlock_(queue);
}

bool CmiPushBlock(CmiIpcBlock* block) {
  auto* meta = CsvAccess(metadata_).get();
  CmiAssert(meta && (block->src != meta->mine));
  auto& queue = meta->shared[block->src]->queue;
  // the value we want to save in the "home"
  // queue is the block's original address
  return pushBlock_(queue, block, block->orig);
}

CmiIpcBlock* CmiNewBlock(std::size_t size) {
  // TODO ( ensure this is correctly aligned! )
  // TODO ( cmiassert(is_bin(size)) )
  auto& meta = CsvAccess(metadata_);
  auto* raw = ::operator new(size + sizeof(CmiIpcBlock));
  CmiAssert((std::uintptr_t)raw % alignof(CmiIpcBlock) == 0);
  auto* block = new (raw) CmiIpcBlock(size);
  block->src = meta->mine;
  return block;
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
      CmiPrintf("CMI> xpmem ipc pool ready.\n");
    }
    CthAwaken(meta->init);
  }
}

void CmiInitIpcMetadata(char** argv, CthThread th) {
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiNodeAllBarrier();

  CsvInitialize(ipc_metadata_ptr_, metadata_);
  auto& meta = CsvAccess(metadata_);
  meta.reset(new ipc_metadata_());
  meta->init = th;

  // TODO ( determine how to seed pool! )
  // NOTE ( if the demo hangs -- init size may be wrong )
  auto nInitial = 16;
  auto initialSize = 128;
  auto bin = whichBin_(initialSize);
  auto* shared = meta->shared[meta->mine];
  for (auto i = 0; i < nInitial; i++) {
    auto* block = CmiNewBlock(initialSize);
    pushBlock_(shared->free[bin], block);
  }

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
    imsg->shared = shared;
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
