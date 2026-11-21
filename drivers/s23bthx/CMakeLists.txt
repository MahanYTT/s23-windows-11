cmake_minimum_required(VERSION 3.15)
project(s23bthx LANGUAGES C CXX)

# Windows Driver Kit configuration
set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
set(WDK_VERSION "10.0.26100.0")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
        set(WDK_ARCH "ARM64")
    else()
        set(WDK_ARCH "x64")
    endif()
else()
    set(WDK_ARCH "x86")
endif()

add_library(s23bthx MODULE s23bthx.c s23bthx_uart.c s23bthx_ibs.c)
set_target_properties(s23bthx PROPERTIES
    SUFFIX ".sys"
    PREFIX ""
    LINK_FLAGS "/SUBSYSTEM:NATIVE /DRIVER /ENTRY:FxDriverEntry /NODEFAULTLIB /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /MERGE:.rdata=.text"
)

target_include_directories(s23bthx PRIVATE
    "${WDK_ROOT}/Include/${WDK_VERSION}/km"
    "${WDK_ROOT}/Include/${WDK_VERSION}/km/crt"
    "${WDK_ROOT}/Include/${WDK_VERSION}/shared"
    "${WDK_ROOT}/Include/${WDK_VERSION}/wdf/kmdf/1.33"
    "../../"
)

target_compile_definitions(s23bthx PRIVATE
    WINNT=1 _WIN32_WINNT=0x0A00 WINVER=0x0A00 _ARM64_
    KMDF_VERSION_MAJOR=1 KMDF_VERSION_MINOR=33
)

target_compile_options(s23bthx PRIVATE
    /Gz /kernel /O2 /Oi /W4 /WX
)

target_link_libraries(s23bthx
    "${WDK_ROOT}/Lib/${WDK_VERSION}/km/${WDK_ARCH}/ntoskrnl.lib"
    "${WDK_ROOT}/Lib/${WDK_VERSION}/km/${WDK_ARCH}/hal.lib"
    "${WDK_ROOT}/Lib/${WDK_VERSION}/km/${WDK_ARCH}/wmilib.lib"
    "${WDK_ROOT}/Lib/${WDK_VERSION}/wdf/kmdf/${WDK_ARCH}/1.33/WdfLdr.lib"
    "${WDK_ROOT}/Lib/${WDK_VERSION}/wdf/kmdf/${WDK_ARCH}/1.33/WdfDriverEntry.lib"
)
