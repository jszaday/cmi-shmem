#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <atomic>
#include <cstdint>

void CmiInitIpcMetadata(char** argv);

#define CMI_HAS_XPMEM 0

// TODO ( generate better names than src/dst )
struct CmiIpcBlock {
  // "home" rank of the block
  int src;
#if CMI_HAS_XPMEM
  CmiIpcBlock* orig;
#endif
  // rank that allocated the block
  int dst;
  CmiIpcBlock* next;
  std::size_t size;
  std::atomic<bool> free;
#if CMI_HAS_XPMEM
  bool cached;
#else
  CmiIpcBlock(std::size_t size_) : next(nullptr), size(size_), free(false) {}
#endif
};

CmiIpcBlock* CmiAllocBlock(int pe, std::size_t size);

bool CmiPushBlock(CmiIpcBlock*);

CmiIpcBlock* CmiPopBlock(void);

bool CmiFreeBlock(CmiIpcBlock*);

void CmiCacheBlock(CmiIpcBlock*);

#endif
