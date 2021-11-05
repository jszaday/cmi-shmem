#ifndef CMI_SHMEM_HH
#define CMI_SHMEM_HH

#include <atomic>
#include <cstdint>
#include <limits>

namespace cmi {
namespace ipc {
// recommended cutoff for block sizes
CpvExtern(std::size_t, kRecommendedCutoff);
// used to represent an empty linked list
constexpr auto nil = std::uintptr_t(0);
// used to represent the tail of a linked list
constexpr auto max = std::numeric_limits<std::uintptr_t>::max();
// used to indicate a message bound for a node
constexpr auto nodeDatagram = std::numeric_limits<CmiUInt2>::max();
// default number of attempts to alloc before timing out
constexpr auto defaultTimeout = 4;
// manager instance number
using api_key_t = std::size_t;
}  // namespace ipc
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

struct CmiIpcManager {
 protected:
  int mine;

  CmiIpcManager(void) : mine(CmiMyNode()) {}

 public:
  void deallocate(CmiIpcBlock*);
  CmiIpcBlock* allocate(int node, std::size_t size);

  bool enqueue(CmiIpcBlock*);
  CmiIpcBlock* dequeue(void);

  CmiIpcBlock* is_block(void*);

  inline CmiIpcBlock* to_block(char* msg) {
    return this->is_block(BLKSTART(msg));
  }

  inline CmiIpcBlock* to_block(char* msg, std::size_t len, int node,
                               int rank = cmi::ipc::nodeDatagram,
                               int timeout = cmi::ipc::defaultTimeout);


  static CmiIpcManager* make_manager(CthThread th);
};

void CmiInitIpc(char** argv);

CmiIpcManager* CmiGetIpcManager(cmi::ipc::api_key_t);

void CmiIpcBlockCallback(int cond = CcdSCHEDLOOP);

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

// deliver a block as a message
void CmiDeliverBlockMsg(CmiIpcBlock*);

inline const std::size_t& CmiRecommendedBlockCutoff(void) {
  using namespace cmi::ipc;
  return CpvAccess(kRecommendedCutoff);
}

#endif
