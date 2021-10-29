#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <atomic>
#include <cstdint>

#define CMK_IPC_BLOCK_FIELDS \
  int src;                   \
  int dst;                   \
  CmiIpcBlock* next;         \
  std::size_t size;

#if CMI_HAS_XPMEM
#define CMI_IPC_BLOCK_FT_FIELDS \
  std::uintptr_t orig;
#else
#define CMI_IPC_BLOCK_FT_FIELDS
#endif

// TODO ( generate better names than src/dst )
struct CmiIpcBlock {
  // "home" rank of the block
 private:
  class blockSizeHelper_ {
    CMK_IPC_BLOCK_FIELDS;
    CMI_IPC_BLOCK_FT_FIELDS;
  };

 public:
  CMK_IPC_BLOCK_FIELDS;
  CMI_IPC_BLOCK_FT_FIELDS;

  char align[(sizeof(blockSizeHelper_) % ALIGN_BYTES)];

#if CMI_HAS_XPMEM
  CmiIpcBlock(std::size_t size_, std::uintptr_t orig_)
      : next(nullptr), size(size_), orig(orig_) {}
#else
  CmiIpcBlock(std::size_t size_) : next(nullptr), size(size_) {}
#endif
  // TODO ( add padding to ensure alignment! )
};

void CmiInitIpcMetadata(char** argv, CthThread th);
void CmiIpcBlockCallback(int cond = CcdSCHEDLOOP);

bool CmiPushBlock(CmiIpcBlock*);
CmiIpcBlock* CmiPopBlock(void);

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size);
void CmiFreeBlock(CmiIpcBlock*);

void CmiCacheBlock(CmiIpcBlock*);

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
