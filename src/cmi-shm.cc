#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmi-shmem-internal.hh>
#include <memory>

struct ipc_shm_metadata_;

CpvStaticDeclare(int, handle_callback);
CpvStaticDeclare(int, handle_node_pid);

static int sendPid_(ipc_shm_metadata_* meta);

struct pid_message_ {
  char core[CmiMsgHeaderSizeBytes];
  std::size_t key;
  pid_t pid;
};

#define CMI_SHARED_FMT "cmi_pid%lu_node%d_shared_"

// opens a shared memory segment for a given physical rank
static std::pair<int, CmiIpcShared*> openShared_(pid_t pid, int node) {
  // determine the size of the shared segment
  // (adding the size of the queues and what nots)
  auto size = CpvAccess(kSegmentSize) + sizeof(CmiIpcShared);
  // generate a name for this pe
  auto slen = snprintf(NULL, 0, CMI_SHARED_FMT, pid, node);
  auto name = new char[slen];
  sprintf(name, CMI_SHARED_FMT, pid, node);
  DEBUGP(("%d> opening share %s\n", CmiMyPe(), name));
  // try opening the share exclusively
  auto fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
  // if we succeed, we're the first accessor, so:
  if (fd >= 0) {
    // truncate it to the correct size
    auto status = ftruncate(fd, size);
    CmiAssert(status >= 0);
  } else {
    // otherwise just open it
    fd = shm_open(name, O_RDWR, 0666);
    CmiAssert(fd >= 0);
  }
  // then delete the name
  delete[] name;
  // map the segment to an address:
  auto* res = (CmiIpcShared*)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  CmiAssert(res != MAP_FAILED);
  // return the file descriptor/shared
  return std::make_pair(fd, res);
}

struct ipc_shm_metadata_ : public CmiIpcManager {
  int num_cbs_exptd, num_cbs_recvd;
  std::map<int, int> fds;
  pid_t pid;

  ipc_shm_metadata_(std::size_t key)
  : CmiIpcManager(key), num_cbs_exptd(-1), num_cbs_recvd(0) {}

  virtual ~ipc_shm_metadata_() {
    auto& size = CpvAccess(kSegmentSize);
    // for each rank/descriptor pair
    for (auto& pair : this->fds) {
      auto& proc = pair.first;
      auto& fd = pair.second;
      // unmap the memory segment
      munmap(this->shared[proc], size);
      // close the file
      close(fd);
      // unlinking the shm segment for our pe
      if (proc == this->mine) {
        auto slen =
            snprintf(NULL, 0, CMI_SHARED_FMT, this->pid, proc);
        auto name = new char[slen];
        sprintf(name, CMI_SHARED_FMT, this->pid, proc);
        shm_unlink(name);
        delete[] name;
      }
    }
  }

  static void openAllShared_(ipc_shm_metadata_* meta) {
    int* pes;
    int nPes;
    int thisNode = CmiPhysicalNodeID(CmiMyPe());
    CmiGetPesOnPhysicalNode(thisNode, &pes, &nPes);
    int nSize = CmiMyNodeSize();
    int nProcs = nPes / nSize;
    // for each rank in this physical node:
    for (auto rank = 0; rank < nProcs; rank++) {
      // open its shared segment
      auto pe = pes[rank * nSize];
      auto proc = CmiNodeOf(pe);
      auto res = openShared_(meta->pid, proc);
      // initializing it if it's ours
      if (proc == meta->mine) initIpcShared_(res.second);
      // store the retrieved data
      meta->fds[proc] = res.first;
      meta->shared[proc] = res.second;
    }
  }

  static void awakenSleepers_(ipc_shm_metadata_* meta) {
    meta->awaken_sleepers();
  }
};

// returns number of processes in node
int procBroadcastAndFree_(ipc_shm_metadata_* meta, char* msg, std::size_t size) {
  int* pes;
  int nPes;
  int thisPe = CmiMyPe();
  int thisNode = CmiPhysicalNodeID(thisPe);
  CmiGetPesOnPhysicalNode(thisNode, &pes, &nPes);
  int nSize = CmiMyNodeSize();
  int nProcs = nPes / nSize;
  CmiAssert(thisPe == pes[0]);

  meta->num_cbs_exptd = nProcs - 1;
  for (auto rank = 1; rank < nProcs; rank++) {
    auto& pe = pes[rank * nSize];
    if (rank == (nProcs - 1)) {
      CmiSyncSendAndFree(pe, size, msg);
    } else {
      CmiSyncSend(pe, size, msg);
    }
  }

  // free if we didn't send anything
  if (nProcs == 1) {
    CmiFree(msg);
  }

  return nProcs;
}

static int sendPid_(ipc_shm_metadata_* meta) {
  auto* pmsg = (pid_message_*)CmiAlloc(sizeof(pid_message_));
  CmiSetHandler(pmsg, CpvAccess(handle_node_pid));
  pmsg->key = meta->key;
  pmsg->pid = meta->pid = getpid();
  CmiAssert(meta->key > 0);
  return procBroadcastAndFree_(meta, (char*)pmsg, sizeof(pid_message_));
}

static void callbackHandler_(void* msg) {
  int mine = CmiMyPe();
  int node = CmiPhysicalNodeID(mine);
  int first = CmiGetFirstPeOnPhysicalNode(node);
  auto* pmsg = (pid_message_*)msg;
  auto* meta = (ipc_shm_metadata_*)CmiIpcManager::get_manager(pmsg->key);

  if (mine == first) {
    // if we're still expecting messages:
    if (++(meta->num_cbs_recvd) < meta->num_cbs_exptd) {
      // free this one
      CmiFree(msg);
      // and move along
      return;
    } else {
      // otherwise -- tell everyone we're ready!
      CmiPrintf("CMI> posix shm pool init'd with %luB segment.\n",
                CpvAccess(kSegmentSize));
      procBroadcastAndFree_(meta, (char*)msg, sizeof(pid_message_));
    }
  } else {
    CmiFree(msg);
  }

  ipc_shm_metadata_::openAllShared_(meta);
  ipc_shm_metadata_::awakenSleepers_(meta);
}

static void nodePidHandler_(void* msg) {
  auto* pmsg = (pid_message_*)msg;
  auto* meta = (ipc_shm_metadata_*)CmiIpcManager::get_manager(pmsg->key);
  meta->pid = pmsg->pid;

  int node = CmiPhysicalNodeID(CmiMyPe());
  int root = CmiGetFirstPeOnPhysicalNode(node);
  CmiSetHandler(msg, CpvAccess(handle_callback));
  CmiSyncSendAndFree(root, sizeof(pid_message_), (char*)msg);
}

void CmiInitializeIpc(char** argv) {
  initSegmentSize_(argv);
  CpvInitialize(int, handle_callback);
  CpvAccess(handle_callback) = CmiRegisterHandler(callbackHandler_);
  CpvInitialize(int, handle_node_pid);
  CpvAccess(handle_node_pid) = CmiRegisterHandler(nodePidHandler_);
}

CmiIpcManager *CmiIpcManager::make_manager(CthThread th) {
  auto& managers = CsvAccess(managers_);
  if (CmiMyRank() == 0) {
    auto key = managers.size() + 1;
    managers.emplace_back(new ipc_shm_metadata_(key));
  }
  CmiBarrier();

  auto& manager = managers.back();
  manager->put_sleeper(th);
  CmiNodeAllBarrier();

  auto* meta = (ipc_shm_metadata_*)manager.get();
  auto firstPe = CmiNodeFirst(CmiMyNode());
  auto procRank = CmiPhysicalRank(firstPe);
  if ((procRank == 0) && (CmiMyRank() == 0)) {
    if (sendPid_(meta) == 1) {
      ipc_shm_metadata_::openAllShared_(meta);
      meta->awaken_sleepers();
    }
  }

  return meta;
}
