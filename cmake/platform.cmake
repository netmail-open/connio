######### OS Detection ##############
if (CMAKE_SYSTEM_NAME MATCHES Linux)
	set (LINUX						1)
	set (UNIX						1)

	find_program(LSB_RELEASE_EXEC lsb_release)
	execute_process(COMMAND ${LSB_RELEASE_EXEC} -is
	  OUTPUT_VARIABLE LINUX_DISTRIBUTION
	  OUTPUT_STRIP_TRAILING_WHITESPACE)
	message( "Creating make files for " ${CMAKE_SYSTEM_NAME} " distribution " ${LINUX_DISTRIBUTION} )
endif (CMAKE_SYSTEM_NAME MATCHES Linux)

if (CMAKE_SYSTEM_NAME MATCHES Windows)
	set (WIN32						1)
	set (WINDOWS					1)
	set (_WIN32						)
	set (_CRT_SECURE_NO_DEPRECATE	1)

	macro(get_WIN32_WINNT version)
	  if (WIN32 AND CMAKE_SYSTEM_VERSION)
		set(ver ${CMAKE_SYSTEM_VERSION})
		string(REPLACE "." "" ver ${ver})
		string(REGEX REPLACE "([0-9])" "0\\1" ver ${ver})

		set(${version} "0x${ver}")
	  endif()
	endmacro()

	get_WIN32_WINNT( WINDOWS_VERSION )

	message( "Creating make files for " ${CMAKE_SYSTEM_NAME} " distribution " ${WINDOWS_VERSION} )
endif (CMAKE_SYSTEM_NAME MATCHES Windows)


# OS specific options
if (LINUX)
	include(${CMAKE_CURRENT_LIST_DIR}/linux.cmake)
endif()

if (WINDOWS)
	include(${CMAKE_CURRENT_LIST_DIR}/windows.cmake)
endif()
