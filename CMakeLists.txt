SET(CMAKE_CXX_STANDARD 11)
ADD_EXECUTABLE( serverproxy main.cpp helpfunctions.cpp )
TARGET_LINK_LIBRARIES( serverproxy PUBLIC uv yaml-cpp)
TARGET_COMPILE_OPTIONS( serverproxy PRIVATE -W -Wall -Wextra -Werror )
