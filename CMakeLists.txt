project(ENetLANChatServer C)
cmake_minimum_required(VERSION 3.0)

option(USE_ENET_ZPL OFF)

set(ENet_LIBRARY enet)
if(WIN32)
    set(WINDOWS_ENET_DEPENDENCIES "ws2_32;winmm")
    set(ENet_LIBRARIES ${ENet_LIBRARY} ${WINDOWS_ENET_DEPENDENCIES})
else()
    set(ENet_LIBRARIES ${ENet_LIBRARY})
endif()

if(USE_ENET_ZPL)
    add_subdirectory(enet_zpl)
    add_definitions(-DUSE_ENET_ZPL)
else()
    add_subdirectory(enet)
endif()

add_executable(server server.c common.h)
target_link_libraries(server ${ENet_LIBRARIES})

add_executable(client client.c common.h rlutil.h)
target_link_libraries(client ${ENet_LIBRARIES})
