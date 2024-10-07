# CMakeList.txt : CMake project for relaxed_concurrent_fifo, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (relaxed_concurrent_fifo "main.cpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET relaxed_concurrent_fifo PROPERTY CXX_STANDARD 20)
endif()

if(MSVC)
  target_compile_options(relaxed_concurrent_fifo PRIVATE /W3 /WX)
else()
  target_compile_options(relaxed_concurrent_fifo PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# TODO: Add tests and install targets if needed.
