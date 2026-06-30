set(JKE_COMPILE_FLAGS "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    list(APPEND JKE_COMPILE_FLAGS
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor
        -O2
    )
elseif(MSVC)
    list(APPEND JKE_COMPILE_FLAGS /W4 /O2)
endif()
