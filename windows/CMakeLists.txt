cmake_minimum_required(VERSION 3.16)
project(px68k_windows C CXX)

# Toolchain: MSYS2 + MinGW-w64 only (see ../x68k_mcp.md 1.2). Configure with
#   cmake -S windows -B windows/build -G "MinGW Makefiles"
# from an MSYSTEM=MINGW64 shell.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CORE_DIR)
    set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../px68k-libretro)
endif()
get_filename_component(CORE_DIR ${CORE_DIR} ABSOLUTE)

# No IMPORTED_TARGET: that form makes CMake pass pkg-config's *exact resolved
# file path* (e.g. .../libSDL2.dll.a) straight to the linker, which bypasses
# -static below entirely (a fixed path isn't subject to the linker's .a-vs-
# .dll.a search preference). Plain variables give bare "-lSDL2"/"-lz", so
# -static can actually pick the true static archive over the import lib.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(ZLIB_PC REQUIRED zlib)

# Source list mirrors px68k-libretro/Makefile.common exactly (C68K backend,
# the project's only supported backend; see x68k_mcp.md 0.1/3.1). The core
# itself is compiled unmodified.
set(CORE_C_SOURCES
    ${CORE_DIR}/m68000/m68000.c
    ${CORE_DIR}/m68000/c68k/c68k.c
    ${CORE_DIR}/m68000/c68k/c68kexec.c

    ${CORE_DIR}/x68k/adpcm.c
    ${CORE_DIR}/x68k/bg.c
    ${CORE_DIR}/x68k/crtc.c
    ${CORE_DIR}/x68k/dmac.c
    ${CORE_DIR}/x68k/fdc.c
    ${CORE_DIR}/x68k/fdd.c
    ${CORE_DIR}/x68k/disk_d88.c
    ${CORE_DIR}/x68k/disk_dim.c
    ${CORE_DIR}/x68k/disk_xdf.c
    ${CORE_DIR}/x68k/gvram.c
    ${CORE_DIR}/x68k/ioc.c
    ${CORE_DIR}/x68k/irqh.c
    ${CORE_DIR}/x68k/mem_wrap.c
    ${CORE_DIR}/x68k/mercury.c
    ${CORE_DIR}/x68k/mfp.c
    ${CORE_DIR}/x68k/palette.c
    ${CORE_DIR}/x68k/midi.c
    ${CORE_DIR}/x68k/pia.c
    ${CORE_DIR}/x68k/rtc.c
    ${CORE_DIR}/x68k/sasi.c
    ${CORE_DIR}/x68k/scc.c
    ${CORE_DIR}/x68k/scsi.c
    ${CORE_DIR}/x68k/sram.c
    ${CORE_DIR}/x68k/sysport.c
    ${CORE_DIR}/x68k/tvram.c

    ${CORE_DIR}/libretro/joystick.c
    ${CORE_DIR}/libretro/keyboard.c
    ${CORE_DIR}/libretro/mouse.c
    ${CORE_DIR}/libretro/prop.c
    ${CORE_DIR}/libretro/timer.c
    ${CORE_DIR}/libretro/dswin.c
    ${CORE_DIR}/libretro/windraw.c
    ${CORE_DIR}/libretro/winui.c
    ${CORE_DIR}/libretro/dosio.c
    ${CORE_DIR}/libretro/fake.c
    ${CORE_DIR}/libretro/peace.c
    ${CORE_DIR}/libretro/state.c
    ${CORE_DIR}/libretro.c

    ${CORE_DIR}/libretro-common/compat/compat_posix_string.c
    ${CORE_DIR}/libretro-common/compat/compat_strcasestr.c
    ${CORE_DIR}/libretro-common/compat/compat_strl.c
    ${CORE_DIR}/libretro-common/compat/compat_snprintf.c
    ${CORE_DIR}/libretro-common/compat/fopen_utf8.c
    ${CORE_DIR}/libretro-common/encodings/encoding_utf.c
    ${CORE_DIR}/libretro-common/file/file_path.c
    ${CORE_DIR}/libretro-common/file/file_path_io.c
    ${CORE_DIR}/libretro-common/file/retro_dirent.c
    ${CORE_DIR}/libretro-common/streams/file_stream.c
    ${CORE_DIR}/libretro-common/streams/file_stream_transforms.c
    ${CORE_DIR}/libretro-common/string/stdstring.c
    ${CORE_DIR}/libretro-common/time/rtime.c
    ${CORE_DIR}/libretro-common/vfs/vfs_implementation.c
)

set(CORE_CXX_SOURCES
    ${CORE_DIR}/fmgen/fmg_wrap.cpp
    ${CORE_DIR}/fmgen/fmgen.cpp
    ${CORE_DIR}/fmgen/fmtimer.cpp
    ${CORE_DIR}/fmgen/opm.cpp
    ${CORE_DIR}/fmgen/opna.cpp
    ${CORE_DIR}/fmgen/psg.cpp
)

set(FRONTEND_SOURCES
    main.c
    arcl_control.c
    arcl_l0.c
    arcl_l1.c
    arcl_l2.c
    arcl_l3.c
    arcl_l4.c
    arcl_opm.c
    arcl_watchpoint.c
    base64.c
    frontend_core.c
    m68k_disasm.c
    mcp_json.c
    mcp_server.c
    mutex_lock.c
    png_write.c
    wav_write.c
)

add_executable(px68k
    ${CORE_C_SOURCES}
    ${CORE_CXX_SOURCES}
    ${FRONTEND_SOURCES}
)

target_include_directories(px68k PRIVATE
    ${CORE_DIR}
    ${CORE_DIR}/libretro-common/include
    ${CORE_DIR}/libretro
    ${CORE_DIR}/x68k
    ${CORE_DIR}/fmgen
    ${CORE_DIR}/m68000
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${ZLIB_PC_INCLUDE_DIRS}
)
target_link_directories(px68k PRIVATE ${SDL2_LIBRARY_DIRS} ${ZLIB_PC_LIBRARY_DIRS})

# Matches px68k-libretro/Makefile.libretro's `platform=win` definitions
# (C68K backend, VFS glue, Mercury sound board disabled on Windows).
# ARCL_WATCHPOINT enables the minimal-diff hook points added to
# x68k/mem_wrap.c for the L2 debug layer (x68k_mcp.md 0.1.1); undefining it
# reverts mem_wrap.c to byte-identical upstream behavior.
target_compile_definitions(px68k PRIVATE
    __LIBRETRO__
    NO_MERCURY
    HAVE_C68K
    C68K_NO_JUMP_TABLE
    USE_LIBRETRO_VFS
    PX68K_VERSION="0.15+"
    GIT_VERSION=""
    ARCL_WATCHPOINT
    ARCL_OPM_SHADOW
)

# fmgen is old-style, loosely-typed C++; Makefile.libretro compiles it with
# -fpermissive too.
set_source_files_properties(${CORE_CXX_SOURCES} PROPERTIES COMPILE_FLAGS "-fpermissive")

# Everything statically linked (see -static below, which makes the linker
# prefer these .a archives over their .dll.a/.dll counterparts): SDL2 and
# zlib's extra transitive Win32 dependencies (imm32/ole32/oleaut32/version/
# uuid/setupapi/dinput8) come from `pkg-config --static --libs sdl2`, minus
# -lSDL2main/-mwindows (this project keeps a plain console-subsystem main()
# for --mcp's stdio JSON-RPC; SDL2main's WinMain would break that).
target_link_libraries(px68k PRIVATE
    SDL2 z
    winmm imm32 ole32 oleaut32 version uuid setupapi dinput8
)

if(MINGW)
    # -static: single self-contained px68k.exe (no SDL2.dll/zlib1.dll/
    # libwinpthread-1.dll alongside it - x68k_mcp.md notes this as the
    # "standalone exe" deployment option). Only Windows' own system DLLs
    # (kernel32/user32/gdi32/msvcrt/...) remain as dependencies.
    target_link_options(px68k PRIVATE -static)
endif()
