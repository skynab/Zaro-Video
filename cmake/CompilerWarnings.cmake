# A single interface target carrying the project's warning policy.
# Link it PRIVATE into every Zaro target; never into anything we export.
add_library(zaro_warnings INTERFACE)
add_library(zaro::warnings ALIAS zaro_warnings)

if(MSVC)
    target_compile_options(zaro_warnings INTERFACE
        /W4 /permissive- /w14242 /w14263 /w14265 /w14287 /w14296
        /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640
        /w14826 /w14905 /w14906 /w14928)
    if(ZARO_WARNINGS_AS_ERRORS)
        target_compile_options(zaro_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(zaro_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough)
    if(ZARO_WARNINGS_AS_ERRORS)
        target_compile_options(zaro_warnings INTERFACE -Werror)
    endif()
endif()

# Sanitizers are a debug-only opt-in. ASan and UBSan together catch the two
# classes of bug that hurt most in a media pipeline: buffer overruns in frame
# handling and signed overflow in time arithmetic.
if(ZARO_SANITIZE AND NOT MSVC)
    target_compile_options(zaro_warnings INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(zaro_warnings INTERFACE
        -fsanitize=address,undefined)
endif()
