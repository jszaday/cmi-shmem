#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <cstdint>

struct CmiIpcBlock {

};

CmiIpcBlock* CmiAllocBlock(int rank, std::size_t size);
bool CmiFreeBlock(CmiIpcBlock*);

#endif
