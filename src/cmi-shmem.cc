#if CMI_HAS_XPMEM
#include "cmi-xpmem.cc"
#else
#include "cmi-shm.cc"
#endif

void* CmiAllocBlockMsg(int pe, std::size_t size) {
  constexpr auto header = sizeof(CmiChunkHeader);
  auto* block = CmiAllocBlock(pe, size + header);
  if (block == nullptr) {
    return nullptr;
  } else {
    auto* res = (char*)block + header + sizeof(CmiIpcBlock);
    CmiInitMsgHeader(res, size);
    CmiAssert(((uintptr_t)res % ALIGN_BYTES) == 0);
    SIZEFIELD(res) = size;
    REFFIELDSET(res, 1);
    return (void*)res;
  }
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
