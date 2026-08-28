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

# Known limitation: only zaro-preview.app is made self-contained, by the Qt
# deployment step in app/CMakeLists.txt. The command-line tools in bin/ still
# link Qt and FFmpeg by absolute path, so they run on a machine that has the
# same dependencies the archive was built against and not otherwise. Making
# them redistributable too needs an install(RUNTIME_DEPENDENCY_SET) pass, which
# is worth doing the day the tools are something we ship at rather than build
# with.

include(GNUInstallDirs)

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
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()

# Nothing in the archive should be a test binary or a header.
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

include(CPack)
