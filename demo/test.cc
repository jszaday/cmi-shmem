#include <converse.h>

#include <cmi-shmem.hh>

CpvDeclare(int, handle_block);

struct test_msg_ {
  char core[CmiMsgHeaderSizeBytes];
  inline char* payload(void);
};

inline char* test_msg_::payload(void) {
  return (char*)this + sizeof(test_msg_);
}

void block_handler(void* msg) {
  auto* tmsg = (test_msg_*)msg;
  CmiPrintf("%d> got message: %s\n", CmiMyPe(), tmsg->payload());
  auto* blk = CmiMsgToBlock(msg);
  CmiAssert(blk != nullptr);
  CmiFreeBlock(blk);
}

void test_thread(void*) {
  auto pe = CmiMyPe();
  auto rank = CmiPhysicalRank(pe);
  auto node = CmiPhysicalNodeID(pe);
  auto nPes = CmiNumPesOnPhysicalNode(node);
  auto nMsgs = 16;

  for (auto imsg = 0; imsg < nMsgs; imsg++) {
    auto peer = (rank + imsg + 1) % nPes;
    if (peer == pe) {
      continue;
    }

    auto len = snprintf(NULL, 0, "(hello %d from %d!)", imsg, pe);
    auto totalSize = len + sizeof(test_msg_);
    // allocate a block message from the IPC pool
    test_msg_* msg;
    while ((msg = (test_msg_*)CmiAllocBlockMsg(peer, totalSize)) == nullptr)
      ;
    // prepare its payload/set the handler
    sprintf(msg->payload(), "(hello %d from %d!)", imsg, pe);
    CmiSetHandler(msg, CpvAccess(handle_block));
    // cache before we push to retain translation
    auto* blk = CmiMsgToBlock(msg);
    CmiCacheBlock(blk);
    // then push it onto the receiver's queue
    while (!CmiPushBlock(blk))
      ;
    // yield to allow the handler to get invoked
    unsigned int prio = 1;  // LOW PRIORITY
    CthYieldPrio(CQS_QUEUEING_IFIFO, 0, &prio);
  }

  CsdExitScheduler();
}

void test_init(int argc, char** argv) {
  // register the block handler with converse
  CpvInitialize(int, handle_block);
  CpvAccess(handle_block) = CmiRegisterHandler(block_handler);
  // create a thread to be resumed when setup completes
  auto* th = CthCreate(test_thread, nullptr, 0);
  // initialize ipc metadata
  CmiInitIpcMetadata(argv, th);
  // enable receving blocks as (converse) messages
  CmiIpcBlockCallback();
}

int main(int argc, char** argv) { ConverseInit(argc, argv, test_init, 0, 0); }
