find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate ShaderTools)

qt_standard_project_setup()

add_library(zaro_platform_qrhi src/GpuCompositor.cpp)
add_library(zaro::platform_qrhi ALIAS zaro_platform_qrhi)

target_include_directories(zaro_platform_qrhi
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(zaro_platform_qrhi
    PUBLIC  zaro::core
    PRIVATE zaro::warnings Qt6::Gui Qt6::GuiPrivate
)

# QRhi is still private API in Qt 6.11 -- Qt Quick's own renderer uses it the
# same way. The cost is that it carries no source-compatibility promise across
# Qt minor versions; see docs/adr/0007. Qt's headers also trip warnings that are
# right for our code and not for a large third-party framework.
target_compile_options(zaro_platform_qrhi PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)

# GLSL in, qsb bundles out, one shader source for Metal, Vulkan and D3D.
qt_add_shaders(zaro_platform_qrhi "zaro_shaders"
    PREFIX "/zaro/shaders"
    FILES
        shaders/composite.vert
        shaders/composite.frag
    OUTPUTS
        composite.vert.qsb
        composite.frag.qsb
)
