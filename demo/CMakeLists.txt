add_executable (test.out test.cc)

target_link_libraries (test.out LINK_PUBLIC cmi-shmem)
target_compile_options(test.out PRIVATE -g3)
target_link_options(test.out PRIVATE -lrt -language converse++)
