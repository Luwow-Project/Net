luwow_add_library(Luwow.NetLibrary
    NAME net
    MODES SERIAL
    SOURCES
        Library.h
        Library.cpp
        Platform.h
        Platform.cpp
        Sockets.h
        Sockets.cpp
        Events.cpp
)
target_include_directories(Luwow.NetLibrary PRIVATE ${NET_ROOT}/src)

# Socket events are signals, so include the Signal library.
target_link_libraries(Luwow.NetLibrary PRIVATE Luwow.SignalCore)

if(WIN32)
    # Winsock
    target_link_libraries(Luwow.NetLibrary PRIVATE ws2_32)
endif()
