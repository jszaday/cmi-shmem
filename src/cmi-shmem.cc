#include <cmi-shmem.hh>

CmiIpcBlock* CmiAllocBlock(int rank, std::size_t size) {
    return nullptr;
}

bool CmiFreeBlock(CmiIpcBlock*) {
    return true;
}
