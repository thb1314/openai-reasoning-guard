cmake_minimum_required(VERSION 3.16)

project(net_tunnel_cpp_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NET_TUNNEL_BUILD_TESTS "Build QtTest-based tests" ON)

set(NET_TUNNEL_QT_SDK_ROOT "/mnt/data/qt-2080ti-sync/qt5-openssl" CACHE PATH "Qt 5 SDK root")
set(NET_TUNNEL_QT_PREFIX_ROOT "/mnt/data/qt-2080ti-sync/qt-5.9.6-linux-gcc" CACHE PATH "Fallback Qt 5 prefix root")

if(EXISTS "${NET_TUNNEL_QT_SDK_ROOT}/bin/moc")
    set(_qt_root "${NET_TUNNEL_QT_SDK_ROOT}")
elseif(EXISTS "${NET_TUNNEL_QT_PREFIX_ROOT}/bin/moc")
    set(_qt_root "${NET_TUNNEL_QT_PREFIX_ROOT}")
elseif(WIN32 AND EXISTS "${NET_TUNNEL_QT_SDK_ROOT}/bin/moc.exe")
    set(_qt_root "${NET_TUNNEL_QT_SDK_ROOT}")
elseif(WIN32 AND EXISTS "${NET_TUNNEL_QT_PREFIX_ROOT}/bin/moc.exe")
    set(_qt_root "${NET_TUNNEL_QT_PREFIX_ROOT}")
else()
    message(FATAL_ERROR "No usable Qt SDK found. Set NET_TUNNEL_QT_SDK_ROOT.")
endif()

set(_qt_include_dir "${_qt_root}/include")
set(_qt_lib_dir "${_qt_root}/lib")
set(CMAKE_BUILD_RPATH "${_qt_lib_dir}")
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../qt/lib;$ORIGIN")
endif()

list(PREPEND CMAKE_PREFIX_PATH
    "${_qt_root}"
    "${_qt_root}/lib/cmake"
)

set(_qt_components Core Network Gui Widgets)
if(NET_TUNNEL_BUILD_TESTS)
    list(APPEND _qt_components Test)
endif()

find_package(Qt5 COMPONENTS ${_qt_components} QUIET CONFIG)

if(Qt5_FOUND)
    message(STATUS "Using Qt CMake package from ${_qt_root}")
else()
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "Qt CMake package was not found under ${_qt_root}. Non-Linux builds require a Qt SDK with lib/cmake/Qt5.")
    endif()

    set(_qt_mkspec_dir "${_qt_root}/mkspecs/linux-g++")

    set(_qt_required_paths
        "${_qt_include_dir}/QtCore"
        "${_qt_include_dir}/QtGui"
        "${_qt_include_dir}/QtWidgets"
        "${_qt_include_dir}/QtNetwork"
        "${_qt_lib_dir}/libQt5Core.so.5"
        "${_qt_lib_dir}/libQt5Gui.so.5"
        "${_qt_lib_dir}/libQt5Widgets.so.5"
        "${_qt_lib_dir}/libQt5Network.so.5"
        "${_qt_root}/bin/moc"
        "${_qt_root}/bin/uic"
        "${_qt_root}/bin/rcc"
    )
    if(NET_TUNNEL_BUILD_TESTS)
        list(APPEND _qt_required_paths
            "${_qt_include_dir}/QtTest"
            "${_qt_lib_dir}/libQt5Test.so.5"
        )
    endif()

    foreach(path IN LISTS _qt_required_paths)
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Qt artifact missing: ${path}")
        endif()
    endforeach()

    set(CMAKE_AUTOMOC_EXECUTABLE "${_qt_root}/bin/moc")
    set(CMAKE_AUTOUIC_EXECUTABLE "${_qt_root}/bin/uic")
    set(CMAKE_AUTORCC_EXECUTABLE "${_qt_root}/bin/rcc")

    set(_qt_common_includes
        "${_qt_include_dir}"
        "${_qt_mkspec_dir}"
    )

    function(add_qt5_imported_library module)
        string(TOUPPER "${module}" module_upper)
        set(target_name "Qt5::${module}")
        set(lib_name "libQt5${module}.so.5")

        if(TARGET "${target_name}")
            return()
        endif()

        add_library("${target_name}" SHARED IMPORTED GLOBAL)
        set_target_properties("${target_name}" PROPERTIES
            IMPORTED_LOCATION "${_qt_lib_dir}/${lib_name}"
            INTERFACE_QT_MAJOR_VERSION 5
            INTERFACE_INCLUDE_DIRECTORIES "${_qt_common_includes};${_qt_include_dir}/Qt${module}"
            INTERFACE_COMPILE_DEFINITIONS "QT_${module_upper}_LIB"
        )
    endfunction()

    add_qt5_imported_library(Core)
    add_qt5_imported_library(Network)
    add_qt5_imported_library(Gui)
    add_qt5_imported_library(Widgets)
    if(NET_TUNNEL_BUILD_TESTS)
        add_qt5_imported_library(Test)
    endif()

    set_property(TARGET Qt5::Network APPEND PROPERTY INTERFACE_LINK_LIBRARIES Qt5::Core)
    set_property(TARGET Qt5::Gui APPEND PROPERTY INTERFACE_LINK_LIBRARIES Qt5::Core)
    set_property(TARGET Qt5::Widgets APPEND PROPERTY INTERFACE_LINK_LIBRARIES "Qt5::Gui;Qt5::Core")
    if(NET_TUNNEL_BUILD_TESTS)
        set_property(TARGET Qt5::Test APPEND PROPERTY INTERFACE_LINK_LIBRARIES Qt5::Core)
    endif()
endif()

function(net_tunnel_add_private_compile_options target_name)
    if(NOT MSVC)
        target_compile_options("${target_name}" PRIVATE -fPIC)
    endif()
endfunction()

add_library(net-tunnel-core STATIC
    src/core/app_config.h
    src/core/app_config.cpp
    src/core/http_proxy_server.h
    src/core/http_proxy_server.cpp
    src/core/json_utils.h
    src/core/json_utils.cpp
)
target_include_directories(net-tunnel-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(net-tunnel-core PUBLIC
    Qt5::Core
    Qt5::Network
)

set(NET_TUNNEL_WINDOWS_ICON_RESOURCE)
if(WIN32)
    enable_language(RC)
    set(NET_TUNNEL_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/openai-reasoning-guard.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/openai-reasoning-guard.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/openai-reasoning-guard.rc"
        @ONLY
    )
    set(NET_TUNNEL_WINDOWS_ICON_RESOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/openai-reasoning-guard.rc"
    )
endif()

add_executable(net-tunnel-cli
    src/cli/main.cpp
    ${NET_TUNNEL_WINDOWS_ICON_RESOURCE}
)
net_tunnel_add_private_compile_options(net-tunnel-cli)
target_link_libraries(net-tunnel-cli PRIVATE
    net-tunnel-core
    Qt5::Core
    Qt5::Network
)

add_executable(net-tunnel-gui
    src/gui/main.cpp
    src/gui/main_window.h
    src/gui/main_window.cpp
    assets/app_resources.qrc
    ${NET_TUNNEL_WINDOWS_ICON_RESOURCE}
    third_party/qui/form/quiwidget.h
    third_party/qui/form/quiwidget.cpp
    third_party/qui/qui_resources.qrc
)
net_tunnel_add_private_compile_options(net-tunnel-gui)
target_include_directories(net-tunnel-gui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qui/form"
)
target_link_libraries(net-tunnel-gui PRIVATE
    net-tunnel-core
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
)
if(WIN32)
    set_target_properties(net-tunnel-gui PROPERTIES WIN32_EXECUTABLE TRUE)
    if(TARGET Qt5::WinMain)
        target_link_libraries(net-tunnel-gui PRIVATE Qt5::WinMain)
    endif()
endif()

if(NET_TUNNEL_BUILD_TESTS)
    if(NOT TARGET Qt5::Test)
        message(FATAL_ERROR "QtTest target missing. Provide a Qt SDK with QtTest or set NET_TUNNEL_BUILD_TESTS=OFF.")
    endif()

    enable_testing()

    add_executable(http-proxy-server-test
        tests/http_proxy_server_test.cpp
    )
    net_tunnel_add_private_compile_options(http-proxy-server-test)
    target_link_libraries(http-proxy-server-test PRIVATE
        net-tunnel-core
        Qt5::Core
        Qt5::Network
        Qt5::Test
    )
    add_test(NAME http-proxy-server-test COMMAND http-proxy-server-test)
endif()

install(TARGETS net-tunnel-cli net-tunnel-gui RUNTIME DESTINATION bin)
