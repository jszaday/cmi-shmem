add_library(cmi-shmem "cmi-shmem.cc")

target_include_directories(
    cmi-shmem PUBLIC "${CMAKE_BINARY_DIR}/include"
)

if (USE_XPMEM)
    target_compile_options(cmi-shmem PRIVATE -g3 -DCMI_HAS_XPMEM=1)
else()
    target_compile_options(cmi-shmem PRIVATE -g3)
endif()

target_include_directories (cmi-shmem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

