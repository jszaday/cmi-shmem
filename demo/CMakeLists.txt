add_executable (test.out test.cc)

target_link_libraries (test.out LINK_PUBLIC cmi-shmem)

if (USE_XPMEM)
    target_compile_options(test.out PRIVATE -g3 -DCMI_HAS_XPMEM=1)
    target_link_libraries(test.out PRIVATE -lxpmem "-language converse++")
else()
    target_compile_options(test.out PRIVATE -g3 -DCMI_HAS_XPMEM=0)
    target_link_libraries(test.out PRIVATE -lrt "-language converse++")
endif()
