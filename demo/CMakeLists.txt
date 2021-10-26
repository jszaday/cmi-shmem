add_executable (test.out test.cc)

target_link_libraries (test.out LINK_PUBLIC cmi-shmem)
