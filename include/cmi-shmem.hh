#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <atomic>
#include <cstdint>

void CmiInitIpcMetadata(char** argv, CthThread th);

#define CMK_IPC_BLOCK_FIELDS \
  int src;                   \
  int dst;                   \
  CmiIpcBlock* next;         \
  std::size_t size;

#if CMI_HAS_XPMEM
#define CMI_IPC_BLOCK_FT_FIELDS \
  CmiIpcBlock* orig;            \
  std::atomic<bool> free;       \
  CmiIpcBlock* cached;
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
  CmiIpcBlock(std::size_t size_)
      : next(nullptr), size(size_), orig(this), free(true), cached(nullptr) {}
#else
  CmiIpcBlock(std::size_t size_) : next(nullptr), size(size_) {}
#endif
  // TODO ( add padding to ensure alignment! )
};

void CmiIpcBlockCallback(int cond = CcdSCHEDLOOP);

CmiIpcBlock* CmiIsBlock(void*);

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size);

void* CmiAllocBlockMsg(int pe, std::size_t size);

bool CmiPushBlock(CmiIpcBlock*);

CmiIpcBlock* CmiPopBlock(void);

void CmiFreeBlock(CmiIpcBlock*);

void CmiCacheBlock(CmiIpcBlock*);

inline CmiIpcBlock* CmiMsgToBlock(void* msg) {
  return CmiIsBlock((char*)msg - sizeof(CmiChunkHeader));
}

inline void* CmiBlockToMsg(CmiIpcBlock* block) {
  auto res = (char*)block + sizeof(CmiIpcBlock) + sizeof(CmiChunkHeader);
  return (void*)res;
}

#endif
