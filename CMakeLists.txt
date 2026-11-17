cmake_minimum_required(VERSION 3.25)
project(portal-mux VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies come from the OS package manager (distro, MSYS2, MacPorts/Homebrew); see docs/BOOTSTRAP.md §6.
find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Multimedia Network WebSockets) # Network: Wake-on-LAN, Proxmox; WebSockets: its VNC consoles
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus) # keyboard layout from systemd-localed
    find_package(KF6WindowSystem 6 CONFIG REQUIRED) # blur behind the sidebar (KWin's blur protocol)
    find_package(Qt6 REQUIRED COMPONENTS WaylandClient GuiPrivate) # "Grab all possible keys" on Wayland
endif()
find_package(Qt6Keychain CONFIG REQUIRED)
find_package(FreeRDP 3 CONFIG REQUIRED)
find_package(FreeRDP-Client 3 CONFIG REQUIRED)
find_package(WinPR 3 CONFIG REQUIRED)
find_package(LibVNCServer CONFIG REQUIRED)
find_package(libssh CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)
pkg_check_modules(SPICE REQUIRED IMPORTED_TARGET spice-client-glib-2.0)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale) # video/audio decoding (scrcpy, Moonlight)
find_package(OpenSSL 3 REQUIRED) # GameStream pairing and HTTPS
find_package(SDL3 3.2 CONFIG REQUIRED COMPONENTS SDL3) # game controllers
# Moonlight's protocol library, vendored (not packaged by OSes); static in rdbackends.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/moonlight-common-c EXCLUDE_FROM_ALL)
qt_standard_project_setup()

# Protocol-independent core; no Qt, so it tests headless.
add_library(rdcore STATIC
    src/core/Framebuffer.cpp
    src/core/Properties.cpp
    src/core/TestPatternSession.cpp
)
target_include_directories(rdcore PUBLIC src)

add_library(rdbackends STATIC
    src/rdp/RdpSession.cpp
    src/vnc/VncSession.cpp
    src/ssh/SshConfig.cpp
    src/ssh/SshConnection.cpp
    src/ssh/SshTerminalSession.cpp
    src/ssh/SshTunnel.cpp
    src/ssh/SftpClient.cpp
    src/spice/SpiceDisplaySession.cpp
    src/media/AudioDecoder.cpp
    src/media/VideoDecoder.cpp
    src/android/Adb.cpp
    src/android/ScrcpySession.cpp
    src/moonlight/GameStream.cpp
    src/moonlight/MoonlightSession.cpp
    src/local/LocalTerminalSession.cpp
    src/ssh/SshForwards.cpp
    src/stream/StreamTerminalSession.cpp
    src/stream/Telnet.cpp
)
# scrcpy's device server, pushed to Android devices; the client must ask for exactly its version.
set(SCRCPY_SERVER_VERSION 5.0.1)
set(PORTAL_MUX_SCRCPY_SERVER "${PROJECT_SOURCE_DIR}/third_party/scrcpy/scrcpy-server-v${SCRCPY_SERVER_VERSION}"
    CACHE FILEPATH "scrcpy-server file of version SCRCPY_SERVER_VERSION")
if(PORTAL_MUX_SCRCPY_SERVER STREQUAL "${PROJECT_SOURCE_DIR}/third_party/scrcpy/scrcpy-server-v${SCRCPY_SERVER_VERSION}")
    file(SHA256 "${PORTAL_MUX_SCRCPY_SERVER}" scrcpy_server_sha256)
    if(NOT scrcpy_server_sha256 STREQUAL "764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536")
        message(FATAL_ERROR "${PORTAL_MUX_SCRCPY_SERVER} does not match the scrcpy ${SCRCPY_SERVER_VERSION} release")
    endif()
endif()
configure_file("${PORTAL_MUX_SCRCPY_SERVER}" "${CMAKE_BINARY_DIR}/scrcpy-server" COPYONLY)
target_compile_definitions(rdbackends PRIVATE SCRCPY_SERVER_VERSION="${SCRCPY_SERVER_VERSION}")
if(UNIX AND NOT APPLE)
    target_link_libraries(rdbackends PRIVATE util) # forkpty (glibc before 2.34 keeps it in libutil)
endif()
target_link_libraries(rdbackends PUBLIC rdcore freerdp freerdp-client winpr LibVNCServer::vncclient ssh PkgConfig::SPICE
    PkgConfig::FFMPEG moonlight-common-c OpenSSL::SSL OpenSSL::Crypto)

qt_add_executable(portal-mux WIN32 MACOSX_BUNDLE
    src/app/main.cpp
    src/app/About.cpp
    src/app/ActivityBar.cpp
    src/app/Addresses.cpp
    src/app/Icons.cpp
    src/app/AudioPlayer.cpp
    src/app/ConnectionDialog.cpp
    src/app/ConnectionStore.cpp
    src/app/Gamepads.cpp
    src/app/GuestRow.cpp
    src/app/Keymap.cpp
    src/app/KeyboardGrab.cpp
    src/app/KrdcImport.cpp
    src/app/LockKeys.cpp
    src/app/MainWindow.cpp
    src/app/MonitorWindow.cpp
    src/app/PanelTree.cpp
    src/app/Prompts.cpp
    src/app/Proxmox.cpp
    src/app/ProxmoxClient.cpp
    src/app/ProxmoxTerminal.cpp
    src/app/ProxmoxView.cpp
    src/app/RemoteView.cpp
    src/app/SftpView.cpp
    src/app/WebSocketTunnel.cpp
    src/app/WindowEffects.cpp
    src/terminal/TerminalView.cpp
)
file(GLOB app_icons RELATIVE "${PROJECT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/third_party/icons/material-icon-theme/*.svg" "${PROJECT_SOURCE_DIR}/third_party/icons/tabler/*.svg"
     "${PROJECT_SOURCE_DIR}/third_party/icons/lucide/*.svg" "${PROJECT_SOURCE_DIR}/third_party/icons/brands/*.svg")
qt_add_resources(portal-mux icons PREFIX "/" BASE third_party FILES ${app_icons})
set(window_icons)
foreach(size 16 22 24 28 32 48 64 96 128 256)
    list(APPEND window_icons data/icons/hicolor/${size}x${size}/apps/portal-mux.png)
endforeach()
qt_add_resources(portal-mux window_icon PREFIX "/" BASE data/icons FILES ${window_icons}) # :/hicolor/<size>/apps/portal-mux.png
if(WIN32)
    target_sources(portal-mux PRIVATE data/icons/portal-mux.rc)
endif()
if(UNIX AND NOT APPLE)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    if(NOT WAYLAND_PROTOCOLS_DIR)
        message(FATAL_ERROR "wayland-protocols not found (pkg-config)")
    endif()
    qt6_generate_wayland_protocol_client_sources(portal-mux FILES
        "${WAYLAND_PROTOCOLS_DIR}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml")
    target_compile_definitions(portal-mux PRIVATE PORTAL_MUX_WAYLAND)
    target_link_libraries(portal-mux PRIVATE Qt6::WaylandClient Qt6::GuiPrivate)
endif()
if(UNIX AND NOT APPLE)
    pkg_check_modules(LIBVIRT IMPORTED_TARGET libvirt) # optional: running local VMs in the connection list
    if(LIBVIRT_FOUND)
        target_sources(portal-mux PRIVATE src/app/DomainXml.cpp src/app/LibvirtConsole.cpp src/app/LibvirtVms.cpp src/app/VmConsole.cpp src/app/VmView.cpp)
        target_compile_definitions(portal-mux PRIVATE PORTAL_MUX_LIBVIRT)
        target_link_libraries(portal-mux PRIVATE PkgConfig::LIBVIRT)
    else()
        message(STATUS "libvirt not found: no \"Virtual machines\" group")
    endif()
endif()
if(APPLE)
    enable_language(OBJCXX)
    target_sources(portal-mux PRIVATE src/app/WindowEffects_mac.mm data/icons/portal-mux.icns)
    set_source_files_properties(data/icons/portal-mux.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(portal-mux PROPERTIES MACOSX_BUNDLE_ICON_FILE portal-mux.icns)
    target_link_libraries(portal-mux PRIVATE "-framework AppKit")
endif()
target_compile_definitions(portal-mux PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
target_link_libraries(portal-mux PRIVATE
    rdbackends
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
    Qt6::WebSockets
    Qt6Keychain::Qt6Keychain
    $<TARGET_NAME_IF_EXISTS:Qt6::DBus>
    $<TARGET_NAME_IF_EXISTS:KF6::WindowSystem>
    PkgConfig::VTERM
    SDL3::SDL3
)

if(MSVC)
    foreach(t rdcore rdbackends portal-mux)
        target_compile_options(${t} PRIVATE /W4)
    endforeach()
else()
    foreach(t rdcore rdbackends portal-mux)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS portal-mux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
install(FILES "${CMAKE_BINARY_DIR}/scrcpy-server" DESTINATION ${CMAKE_INSTALL_DATADIR}/portal-mux)
if(UNIX AND NOT APPLE) # rdp://, vnc://, spice:// links and .rdp/.vnc files open in Portal Mux
    install(FILES data/portal-mux.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/portal-mux-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME portal-mux.xml)
    install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
