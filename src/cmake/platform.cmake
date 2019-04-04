# OS specific options
if (LINUX)
	include(${CMAKE_CURRENT_LIST_DIR}/linux.cmake)
endif()

if (WINDOWS)
	include(${CMAKE_CURRENT_LIST_DIR}/windows.cmake)
endif()
