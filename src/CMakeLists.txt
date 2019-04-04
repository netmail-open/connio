cmake_minimum_required (VERSION 3.5.1)
project (CONNIO)
set(CMAKE_BUILD_TYPE Debug)

include_directories(include)

include(cmake/deps.cmake)
include(cmake/platform.cmake)

message( STATUS ${HAVE_UNIX_SOCKETS})
message( STATUS ${HAVE_WIN_SOCKETS})

if(HAVE_UNIX_SOCKETS)
	set(SOCKETS_C sockunix.c)
endif()

if(HAVE_WIN_SOCKETS)
	set(SOCKETS_C sockwin.c)
endif()

add_library(connio SHARED
	connio.c
	process.c
	crypto.c
	sockcommon.c
	${SOCKETS_C}
	conniop.h
	queue.c
	threadengine.c
	throttle.c
	trace.c
	callbacks.c
	tlog.c
	connlog.c
	socklog.c
	debug.c
)

target_link_libraries(connio
	xplip${BIT}
	iobuf${BIT}
	xpldir${BIT}
	xplfile${BIT}
	xpltimer${BIT}
	wjreader${BIT}
	wjwriter${BIT}
	wjelement${BIT}
	${RT_LIBS}
	${OPENSSL_LIBS}
	${PTHREADS_LIBS}
  )


install(TARGETS connio DESTINATION /usr/lib)

