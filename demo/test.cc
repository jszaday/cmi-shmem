#include <converse.h>

#include <cmi-shmem.hh>

CpvDeclare(int, handle_exit);

void exit_handler(void* msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

void test_init(int argc, char** argv) {
  CmiInitIpcMetadata(argv);

  CmiPrintf("%d> ipc initialization completed.\n", CmiMyPe());

  auto pe = CmiMyPe();

  auto rank = CmiPhysicalRank(pe);
  auto node = CmiPhysicalNodeID(pe);
  auto nPes = CmiNumPesOnPhysicalNode(node);

  auto peer = (rank + 1) % nPes;

  auto len = snprintf(NULL, 0, "(hello from %d!)", pe);

  CmiIpcBlock* blk;
  while ((blk = CmiAllocBlock(peer, len)) == nullptr)
    ;

  auto* buf = ((char*)blk + sizeof(CmiIpcBlock));
  sprintf(buf, "(hello from %d!)", pe);

  CmiCacheBlock(blk);
  while (!CmiPushBlock(blk))
    ;

  while ((blk = CmiPopBlock()) == nullptr)
    ;

  CmiPrintf("%d> got message: %s\n", pe, (char*)blk + sizeof(CmiIpcBlock));

  CmiFreeBlock(blk);

  CpvInitialize(int, handle_exit);
  CpvAccess(handle_exit) = CmiRegisterHandler(exit_handler);

  auto* msg = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, CpvAccess(handle_exit));
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, (char*)msg);
}

int main(int argc, char** argv) { ConverseInit(argc, argv, test_init, 0, 0); }
