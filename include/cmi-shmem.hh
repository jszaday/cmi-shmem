#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <atomic>
#include <cstdint>

namespace cmi {
namespace ipc {
constexpr auto nil = std::uintptr_t(0);
}
}  // namespace cmi

#define CMK_IPC_BLOCK_FIELDS \
  int src;                   \
  std::uintptr_t orig;       \
  int dst;                   \
  std::uintptr_t next;       \
  std::size_t size;

// TODO ( generate better names than src/dst )
struct CmiIpcBlock {
  // "home" rank of the block
 private:
  class blockSizeHelper_ {
    CMK_IPC_BLOCK_FIELDS;
  };

 public:
  CMK_IPC_BLOCK_FIELDS;

  CmiIpcBlock(std::size_t size_, std::uintptr_t orig_)
      : orig(orig_), next(cmi::ipc::nil), size(size_) {}

  char padding[(sizeof(blockSizeHelper_) % ALIGN_BYTES)];
};

void CmiInitIpcMetadata(char** argv, CthThread th);
void CmiIpcBlockCallback(int cond = CcdSCHEDLOOP);

bool CmiPushBlock(CmiIpcBlock*);
CmiIpcBlock* CmiPopBlock(void);

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size);
void CmiFreeBlock(CmiIpcBlock*);

// currently a no-op but may be eventually usable
// intended to "capture" blocks from remote pes
inline void CmiCacheBlock(CmiIpcBlock*) { return; }

// identifies whether a void* is the payload of a block
CmiIpcBlock* CmiIsBlock(void*);

// if (init) is true -- initializes the
// memory segment for use as a message
void* CmiBlockToMsg(CmiIpcBlock*, bool init);

// equivalent to calling above with (init = false)
inline void* CmiBlockToMsg(CmiIpcBlock* block) {
  auto res = (char*)block + sizeof(CmiIpcBlock) + sizeof(CmiChunkHeader);
  return (void*)res;
}

inline CmiIpcBlock* CmiMsgToBlock(void* msg) {
  return CmiIsBlock((char*)msg - sizeof(CmiChunkHeader));
}

#endif
