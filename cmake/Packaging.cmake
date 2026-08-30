# What a release archive contains, and how it is built.
#
# Included from the top-level CMakeLists after every target exists. The install
# rules themselves live next to the targets they install -- app/ and tools/ --
# because that is where the Qt deployment step has to run from; this file only
# carries the parts that are about the archive as a whole.
#
#   cmake --build   --preset release
#   cpack           --preset release
#
# produces build/release/CutReel-<version>-<platform>.{tar.gz,zip}.

# Windows produces two things: the ZIP, for anybody who wants the files, and an
# MSI built by WiX, for anybody who wants it installed. Everything in bin/ is
# self-contained there -- Qt from windeployqt, FFmpeg and SDL from the vcpkg
# DLLs installed in cmake/WindowsRuntimeDeps.cmake, and the MSVC runtime from
# InstallRequiredSystemLibraries below.
#
# Linux produces two as well: the tarball, self-contained, unpack it anywhere;
# and a .deb that puts the same tree under /usr. Everything both carry is in
# lib/cutreel, found through the relative RPATH set in tools/ and app/.
#
# Known limitation: macOS. Only zaro-preview.app is made self-contained there,
# so the command-line tools in bin/ still expect a machine carrying the same
# Homebrew dependencies the archive was built against. The same treatment Linux
# gets below would work, with @loader_path in place of $ORIGIN.

include(GNUInstallDirs)

# The Visual C++ runtime. A machine that has never had Visual Studio on it does
# not carry vcruntime140.dll, and every executable in bin/ needs it. The UCRT
# is deliberately left out: it has been part of Windows since 10, and shipping
# a copy is forty more files for a machine that already has them.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endif()

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/CutReel"
    COMPONENT runtime)

set(CPACK_PACKAGE_NAME "CutReel")
set(CPACK_PACKAGE_VENDOR "CutReel")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "CutReel")
set(CPACK_VERBATIM_VARIABLES ON)

# One directory inside the archive rather than a bare spill of bin/ and share/
# into whatever the user unpacked it in.
set(CPACK_PACKAGE_FILE_NAME
    "CutReel-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

if(WIN32)
    set(CPACK_GENERATOR ZIP WIX)

    # --- The MSI ---------------------------------------------------------------
    # Stable for the life of the product, and the one value here that must never
    # change: WiX uses it to recognise an installed older CutReel and replace it.
    # A new GUID turns every future upgrade into a second copy installed beside
    # the first, with two Start Menu entries and no way back.
    set(CPACK_WIX_UPGRADE_GUID "0FC6F111-2EC6-47EE-955D-32DCD93A32D2")

    # One machine, one copy, in Program Files. Stated rather than left to the
    # default, which is still the "NONE" CPack 3.28 and older used: no
    # InstallScope at all, so ALLUSERS is never set and every shortcut and
    # registry key the package carries is per-user data sitting in a
    # per-machine install. WiX's validator refuses to build that -- ICE57 on
    # the desktop shortcut below, whose HKMU keypath cannot resolve to a hive
    # when nothing has said which context the install runs in -- and ICE90
    # says the same thing about the Start Menu entry CPack writes itself.
    #
    # perMachine settles it: the installer asks for elevation, DesktopFolder
    # and ProgramMenuFolder are the all-users ones, and HKMU is HKLM.
    #
    # Safe to state now and not later: an install made without an
    # InstallScope cannot be cleanly upgraded by one that has it, and no MSI
    # has ever been released -- this is the error that stopped every one of
    # them from being built.
    set(CPACK_WIX_INSTALL_SCOPE "perMachine")

    # Program Files\CutReel, one Start Menu entry, pointing at the app rather
    # than at one of the seven command-line tools beside it.
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "CutReel")
    set(CPACK_WIX_ROOT_FEATURE_TITLE "CutReel")
    set(CPACK_PACKAGE_EXECUTABLES "zaro-preview" "CutReel")

    # A desktop shortcut and the .zaro file association, neither of which CPack
    # can express as a variable. See the file for what it does and why it is
    # written against the two fragment ids CPack documents as stable.
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix-patch.xml")

    # WiX reads a licence as .txt or .rtf and ours is extensionless, so it is
    # copied to a name the installer's licence page will accept. COPYONLY: the
    # text of a licence is not something to run through a substitution pass.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                   "${CMAKE_BINARY_DIR}/License.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/License.txt")
elseif(APPLE)
    set(CPACK_GENERATOR TGZ)
else()
    set(CPACK_GENERATOR TGZ DEB)

    # --- The .deb --------------------------------------------------------------
    # Lowercase, because a Debian package name has to be. The rest of CPack's
    # naming is left to the generator, which spells it cutreel_0.7.0_amd64.deb.
    set(CPACK_DEBIAN_PACKAGE_NAME "cutreel")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR}")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/skynab/Zaro-Video")

    # Three, and only three. The package carries its own Qt, FFmpeg and SDL, so
    # the only things it needs from the distribution are the ones no program can
    # bring with it. Naming more would be naming them wrongly: what the desktop
    # stack is called moves between releases -- libasound2 became libasound2t64
    # in 24.04 -- and a Depends line that names a package apt cannot find is an
    # install that fails on a machine that had everything it needed.
    #
    # Deliberately not CPACK_DEBIAN_PACKAGE_SHLIBDEPS: dpkg-shlibdeps resolves
    # every library a binary links, ours included, and cannot name a package for
    # the copy of Qt in lib/cutreel because no package provides it.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libstdc++6, libgcc-s1")
endif()

# --- What the Linux packages carry ---------------------------------------------
if(UNIX AND NOT APPLE)
    # Everything the installed executables and plugins link, resolved the way
    # the loader resolves it, minus the list below.
    #
    # The list is the libraries that belong to the machine rather than to us:
    # the C and C++ runtimes, the graphics and windowing stack, the sound
    # servers, and the handful of system services Qt talks to. Bundling any of
    # them is how a program stops working on the next machine -- a copied libGL
    # cannot talk to a driver it was not built beside, and a copied libz that
    # loads before the system's changes what every other library in the process
    # sees. Everything else -- Qt, FFmpeg, SDL, and the codec libraries FFmpeg
    # pulls in -- is ours to ship, because no distribution promises the version
    # we built against.
    #
    # Matched on the file name, so it applies whether a library was found in
    # /lib, /usr/lib or the Qt the build used.
    set(zaro_system_libraries
        "ld-linux.*" "libc\\.so.*" "libm\\.so.*" "libdl\\.so.*" "libpthread\\.so.*"
        "librt\\.so.*" "libresolv\\.so.*" "libutil\\.so.*"
        "libgcc_s\\.so.*" "libstdc\\+\\+\\.so.*"
        # Graphics: the driver's, never ours.
        "libGL.*\\.so.*" "libEGL.*\\.so.*" "libOpenGL\\.so.*" "libGLdispatch\\.so.*"
        "libGLU\\.so.*" "libdrm\\.so.*" "libgbm\\.so.*" "libvulkan\\.so.*"
        # Windowing.
        "libX11.*\\.so.*" "libXext\\.so.*" "libXrender\\.so.*" "libXi\\.so.*"
        "libXrandr\\.so.*" "libXcursor\\.so.*" "libXfixes\\.so.*" "libXau\\.so.*"
        "libXdmcp\\.so.*" "libxcb.*\\.so.*" "libwayland.*\\.so.*" "libxkbcommon.*\\.so.*"
        # Fonts: fontconfig reads the machine's configuration and freetype is
        # built against it, so the pair has to come from the same machine.
        "libfontconfig\\.so.*" "libfreetype\\.so.*"
        # Sound.
        "libasound\\.so.*" "libpulse.*\\.so.*" "libjack.*\\.so.*" "libpipewire.*\\.so.*"
        # System services and the libraries everything already has.
        "libglib-2\\.0\\.so.*" "libgobject-2\\.0\\.so.*" "libgio-2\\.0\\.so.*"
        "libgmodule-2\\.0\\.so.*" "libgthread-2\\.0\\.so.*"
        "libdbus-1\\.so.*" "libsystemd\\.so.*" "libudev\\.so.*" "libselinux\\.so.*"
        "libz\\.so.*" "libcrypto\\.so.*" "libssl\\.so.*")

    install(RUNTIME_DEPENDENCY_SET zaro_runtime_deps
        PRE_EXCLUDE_REGEXES  ${zaro_system_libraries}
        POST_EXCLUDE_REGEXES ${zaro_system_libraries}
        DESTINATION "${ZARO_PRIVATE_LIBDIR}"
        COMPONENT runtime)
endif()

# Nothing in the archive should be a test binary or a header.
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

# There is one component and it is the product, so the installer's feature tree
# says "CutReel" and offers no choice, rather than presenting a checkbox called
# "runtime" that nothing works without.
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "CutReel")
set(CPACK_COMPONENT_RUNTIME_REQUIRED ON)

# Read once per generator; see the file for what has to differ between them.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackOptions.cmake")

include(CPack)
