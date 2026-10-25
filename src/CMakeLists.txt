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
        Http.h
        Http.cpp
)
target_include_directories(Luwow.NetLibrary PRIVATE ${NET_ROOT}/src)

# Socket events are signals, so include the Signal library.
target_link_libraries(Luwow.NetLibrary PRIVATE Luwow.SignalCore)

if(WIN32)
    # Winsock, and WinHTTP for net.request
    target_link_libraries(Luwow.NetLibrary PRIVATE ws2_32 winhttp)
elseif(APPLE)
    # NSURLSession for net.request
    target_sources(Luwow.NetLibrary PRIVATE Http.mm)
    target_link_libraries(Luwow.NetLibrary PRIVATE "-framework Foundation")
    # The receiver and its session are released by ARC, as gui's Objective-C++ is
    target_compile_options(Luwow.NetLibrary PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
    # libcurl for net.request, the HTTP client Linux distributions ship (libcurl4-openssl-dev to build)
    find_package(CURL REQUIRED)
    target_link_libraries(Luwow.NetLibrary PRIVATE CURL::libcurl)
endif()
