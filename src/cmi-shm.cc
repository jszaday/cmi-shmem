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

CpvStaticDeclare(int, handle_node_pid);
CsvStaticDeclare(pid_t, node_pid);

static void openAllShared_(ipc_shm_metadata_*);
static void sendPid_(void);

struct pid_message_ {
  char core[CmiMsgHeaderSizeBytes];
  pid_t pid;
};

#define CMI_SHARED_FMT "cmi_pid%lu_rank%d_shared_"

// opens a shared memory segment for a given physical rank
static std::pair<int, ipc_shared_*> openShared_(int rank) {
  // get the size from the cpv
  auto& size = CpvAccess(kSegmentSize);
  // generate a name for this pe
  auto slen = snprintf(NULL, 0, CMI_SHARED_FMT, CsvAccess(node_pid), rank);
  auto name = new char[slen];
  sprintf(name, CMI_SHARED_FMT, CsvAccess(node_pid), rank);
  CmiPrintf("opening share %s\n", name);
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
  auto* res = (ipc_shared_*)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  CmiAssert(res != MAP_FAILED);
  // return the file descriptor/shared
  return std::make_pair(fd, res);
}

struct ipc_shm_metadata_ : public ipc_metadata_ {
  std::map<int, int> fds;

  ipc_shm_metadata_(void) {
    if (this->mine == 0) {
      sendPid_();
      openAllShared_(this);
    }
  }

  virtual ~ipc_shm_metadata_() {
    auto& size = CpvAccess(kSegmentSize);
    // for each rank/descriptor pair
    for (auto& pair : this->fds) {
      auto& rank = pair.first;
      auto& fd = pair.second;
      // unmap the memory segment
      munmap(this->shared[rank], size);
      // close the file
      close(fd);
      // unlinking the shm segment for our pe
      if (rank == this->mine) {
        auto slen =
            snprintf(NULL, 0, CMI_SHARED_FMT, CsvAccess(node_pid), rank);
        auto name = new char[slen];
        sprintf(name, CMI_SHARED_FMT, CsvAccess(node_pid), rank);
        shm_unlink(name);
        delete[] name;
      }
    }
  }
};

static void openAllShared_(ipc_shm_metadata_* meta) {
  int node = CmiPhysicalNodeID(CmiMyPe());
  int nPes = CmiNumPesOnPhysicalNode(node);
  int nProcs = nPes / CmiMyNodeSize();
  // for each rank in this physical node:
  for (auto rank = 0; rank < nProcs; rank++) {
    // open its shared segment
    auto res = openShared_(rank);
    // initializing it if it's ours
    if (rank == meta->mine) initIpcShared_(res.second);
    // store the retrieved data
    meta->fds[rank] = res.first;
    meta->shared[rank] = res.second;
  }
}

static void sendPid_(void) {
  CsvInitialize(pid_t, node_pid);
  CsvAccess(node_pid) = getpid();

  auto* pmsg = (pid_message_*)CmiAlloc(sizeof(pid_message_));
  CmiSetHandler(pmsg, CpvAccess(handle_node_pid));
  pmsg->pid = CsvAccess(node_pid);

  int nPes;
  int* pes;
  int node = CmiPhysicalNodeID(CmiMyPe());
  CmiGetPesOnPhysicalNode(node, &pes, &nPes);
  int nSize = CmiMyNodeSize();
  int nProcs = nPes / nSize;

  for (auto rank = 1; rank < nProcs; rank++) {
    auto& pe = pes[rank * nSize];
    if (rank == (nProcs - 1)) {
      CmiSyncSendAndFree(pe, sizeof(pid_message_), (char*)pmsg);
    } else {
      CmiSyncSend(pe, sizeof(pid_message_), (char*)pmsg);
    }
  }
}

static void nodePidHandler_(void* msg) {
  auto* meta = (ipc_shm_metadata_*)CsvAccess(metadata_).get();
  auto* pmsg = (pid_message_*)msg;
  CsvInitialize(pid_t, node_pid);
  CsvAccess(node_pid) = pmsg->pid;
  openAllShared_(meta);
  CmiFree(msg);
}

void CmiInitIpcMetadata(char** argv, CthThread th) {
  initSegmentSize_(argv);
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiNodeAllBarrier();

  CpvInitialize(int, handle_node_pid);
  CpvAccess(handle_node_pid) = CmiRegisterHandler(nodePidHandler_);

  if (CmiMyRank() == 0) {
    CsvInitialize(ipc_metadata_ptr_, metadata_);
    CsvAccess(metadata_).reset(new ipc_shm_metadata_);
  }

  if (CmiMyPe() == 0) {
    CmiPrintf("CMI> posix shm pool init'd with %luB segment.\n",
              CpvAccess(kSegmentSize));
  }

  // TODO ( identify which fn should be used here )
  // ( basically phyical node barrier vs. node barrier )
  CmiBarrier();

  // resume the callback
  if (th) CthAwaken(th);
}
