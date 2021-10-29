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
