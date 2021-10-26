add_executable (cmi-shmem-test test.cc)

target_link_libraries (cmi-shmem-test LINK_PUBLIC cmi-shmem)
