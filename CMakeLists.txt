project(ENetLANChatServer C)
cmake_minimum_required(VERSION 3.0)

set(ENET_LIB_CHOICE "ORIGINAL" CACHE STRING "ENET_LIB_CHOICE")
set_property(CACHE ENET_LIB_CHOICE PROPERTY STRINGS "ORIGINAL" "ZPL" "SINGLE_HEADER")

set(ENet_LIBRARY enet)
if(WIN32)
    set(ENet_LIBRARIES ${ENet_LIBRARY} ${WINDOWS_ENET_DEPENDENCIES})
else()
    set(ENet_LIBRARIES ${ENet_LIBRARY})
endif()

if(ENET_LIB_CHOICE STREQUAL "ORIGINAL")
    add_subdirectory(enet)
    add_definitions(-DENET_LIB_CHOICE=0)
elseif(ENET_LIB_CHOICE STREQUAL "ORIGINAL")
    add_subdirectory(enet_zpl)
    add_definitions(-DENET_LIB_CHOICE=1)
else()
    add_subdirectory(enet_sh)
    add_definitions(-DENET_LIB_CHOICE=2)
endif()

add_executable(server server.c common.h)
target_link_libraries(server ${ENet_LIBRARIES})

add_executable(client client.c common.h rlutil.h)
target_link_libraries(client ${ENet_LIBRARIES})
