#include <converse.h>

#include <cmi-shmem.hh>

constexpr auto nMsgs = 16;

CpvDeclare(int, send_count);
CpvDeclare(int, recv_count);
CpvDeclare(int, handle_exit);
CpvDeclare(int, handle_block);

void* null_merge_fn(int* size, void* local, void** remote, int count) {
  return local;
}

struct test_msg_ {
  char core[CmiMsgHeaderSizeBytes];
  int target;
  inline char* payload(void);
};

inline char* test_msg_::payload(void) {
  return (char*)this + sizeof(test_msg_);
}

void exit_handler(void* msg) {
  if (CmiMyPe() == 0) {
    CmiSyncBroadcastAndFree(CmiMsgHeaderSizeBytes, (char*)msg);
  } else {
    CmiFree(msg);
  }

  CsdExitScheduler();
}

void check_done(bool receiving) {
  if (receiving) {
    CpvAccess(recv_count)++;
  }
  // we're done when we've received messages for each we sent
  if (CpvAccess(recv_count) == CpvAccess(send_count)) {
    auto size = CmiMsgHeaderSizeBytes;
    auto* msg = (char*)CmiAlloc(size);
    CmiSetHandler(msg, CpvAccess(handle_exit));
    CmiReduce(msg, size, null_merge_fn);
  }
}

void block_handler(void* msg) {
  auto thisPe = CmiMyPe();
  auto* tmsg = (test_msg_*)msg;
  CmiAssert(thisPe == tmsg->target);
  CmiPrintf("%d> got message: %s\n", thisPe, tmsg->payload());
  // TODO ( determine how to support: CmiFree(msg) )
  auto* blk = CmiMsgToBlock(msg);
  if (blk)
    CmiFreeBlock(blk);
  else
    CmiFree(msg);
  // check if we're done
  check_done(true);
}

void test_thread(void*) {
  auto pe = CmiMyPe();
  auto rank = CmiPhysicalRank(pe);
  auto node = CmiPhysicalNodeID(pe);
  auto nPes = CmiNumPesOnPhysicalNode(node);
  // determine the number of iters to run for
  auto nIters = (nPes > 1) ? nMsgs : 0;
  if (nIters) {
    if (nIters < nPes) {
      nIters = nPes - 1;
    } else {
      nIters -= (nIters % (nPes - 1));
    }
  }
  CpvAccess(send_count) = nIters;
  auto nSent = 0;
  auto last = -1;
  // then send all our messages
  for (auto imsg = 0; imsg < nIters; imsg++) {
    auto peer = (rank + imsg + 1) % nPes;
    if (peer == pe) {
      nIters += 1;

      continue;
    } else {
      CmiAssert((last != peer) || (nPes == 2));
      last = peer;
      nSent++;
    }

    auto len = snprintf(NULL, 0, "(hello %d from %d!)", imsg, pe);
    auto totalSize = sizeof(test_msg_) + len + 1; // plus one for '\0'
    auto* msg = (test_msg_*)CmiAlloc(totalSize);

    sprintf(msg->payload(), "(hello %d from %d!)", imsg, pe);
    CmiSetHandler(msg, CpvAccess(handle_block));
    msg->target = peer;

    CmiIpcBlock* block = nullptr;
    try {
      block = CmiMsgToBlock((char*)msg, totalSize, CmiNodeOf(peer),
                            CmiRankOf(peer));
    } catch (std::bad_alloc) {
    }

    if (block) {
      // cache before we push to retain translation
      CmiCacheBlock(block);
      // then push it onto the receiver's queue
      while (!CmiPushBlock(block))
        ;
    } else {
      CmiSyncSendAndFree(peer, totalSize, (char*)msg);
    }
    // yield to allow the handler to get invoked
    unsigned int prio = 1;  // LOW PRIORITY
    CthYieldPrio(CQS_QUEUEING_IFIFO, 0, &prio);
  }

  if (nIters == 0) {
    CsdExitScheduler();
  } else {
    CmiAssert(nSent == CpvAccess(send_count));

    check_done(false);
  }
}

void test_init(int argc, char** argv) {
  // initialize send/receive count
  CpvInitialize(int, recv_count);
  CpvInitialize(int, send_count);
  CpvAccess(recv_count) = 0;
  CpvAccess(send_count) = nMsgs;
  // register the handlers with converse
  CpvInitialize(int, handle_block);
  CpvAccess(handle_block) = CmiRegisterHandler(block_handler);
  CpvInitialize(int, handle_exit);
  CpvAccess(handle_exit) = CmiRegisterHandler(exit_handler);
  // create a thread to be resumed when setup completes
#if CMK_SMP
  auto* th = CmiInCommThread() ? nullptr : CthCreate(test_thread, nullptr, 0);
#else
  auto* th = CthCreate(test_thread, nullptr, 0);
#endif
  // init cpu topology
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  // initialize ipc metadata
  CmiInitIpcMetadata(argv, th);
  // enable receving blocks as (converse) messages
  CmiIpcBlockCallback();
}

int main(int argc, char** argv) { ConverseInit(argc, argv, test_init, 0, 0); }
