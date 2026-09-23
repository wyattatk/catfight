# Unix specific settings (this includes macOS and emscripten)

if(NOT UNIX)
    return()
endif()

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_unix.c)

if(EMSCRIPTEN)
    list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_passive.c)
else()
    list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_tty.c)
endif()

# catfight: child processes with pipes, for cl_steam.c. Client only -- a
# dedicated server has no helper to run. A web build has no process model at
# all, so it gets the backend that politely refuses.
if(EMSCRIPTEN)
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_process_null.c)
else()
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_process_unix.c)
endif()

if(USE_HTTP)
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/client/cl_http_curl.c)
endif()

list(APPEND COMMON_LIBRARIES
    ${CMAKE_DL_LIBS}    # Dynamic loader
    m                   # Math library
)
