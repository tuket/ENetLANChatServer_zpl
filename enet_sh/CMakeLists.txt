project(enet)

set(SRCS enet.h enet.c)

add_library(enet ${SRCS})
target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
source_group("" FILES ${SRCS})