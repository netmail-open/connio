cmake_minimum_required (VERSION 3.5.1)
project (CONNIO)
set(CMAKE_BUILD_TYPE Debug)

include_directories(
	include
)

include(cmake/deps.cmake)
include(cmake/platform.cmake)


include_directories(include)

add_subdirectory(src)

install(
	DIRECTORY	${CMAKE_CURRENT_SOURCE_DIR}/include/
	DESTINATION	/usr/include

	PATTERN		"*"			PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ
)
