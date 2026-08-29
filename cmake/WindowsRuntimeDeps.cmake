# The vcpkg DLLs that have to sit beside the executables in the build tree.
#
# vcpkg's toolchain does this itself: VCPKG_APPLOCAL_DEPS defaults to ON, which
# hangs a `vcpkg z-applocal` step off *every* executable, each one copying the
# DLLs that target needs next to it. Every one of our executables lands in the
# same bin/, so a parallel build runs a dozen of those steps at once, all
# copying the same avcodec.dll and avutil.dll over each other -- and one of
# them loses the file to another's open handle:
#
#   vcpkg z-applocal --target-binary=.../bin/zaro-probe.exe
#   Access is denied.
#
# The windows-* presets turn that off and this replaces it: one target, one
# process, one copy of each DLL, no two writers to race. copy_if_different so a
# rebuild does not rewrite files a running program may have mapped.
if(MSVC AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    # Debug and release builds of the same library have the same DLL name and
    # different contents, and vcpkg keeps them apart by directory.
    set(zaro_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(zaro_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    endif()

    # Globbed at configure time, so a dependency added to vcpkg.json needs a
    # reconfigure before its DLL appears here. That is the same moment vcpkg
    # installs it, so in practice the two happen together.
    file(GLOB zaro_vcpkg_dlls CONFIGURE_DEPENDS "${zaro_vcpkg_bin}/*.dll")
    if(zaro_vcpkg_dlls)
        add_custom_target(zaro_runtime_dlls ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${zaro_vcpkg_dlls} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMENT "Copying vcpkg runtime DLLs into bin/"
            VERBATIM)
    else()
        message(WARNING
            "No vcpkg DLLs found in ${zaro_vcpkg_bin}; built executables will not run "
            "from the build tree until they are beside them.")
    endif()
endif()
