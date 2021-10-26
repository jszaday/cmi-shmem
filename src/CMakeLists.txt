add_library(cmi-shmem "cmi-shmem.cc")

target_include_directories(
    cmi-shmem PUBLIC "${CMAKE_BINARY_DIR}/include"
)

target_include_directories (cmi-shmem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
