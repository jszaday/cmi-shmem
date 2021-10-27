#if CMI_HAS_XPMEM
#include "cmi-xpmem.cc"
#else
#include "cmi-shm.cc"
#endif
