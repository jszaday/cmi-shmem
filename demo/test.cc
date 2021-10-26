#include <cmi-shmem.hh>

int main(void) {
    auto* blk = CmiAllocBlock(1, 2);
    return !CmiFreeBlock(blk);
}
