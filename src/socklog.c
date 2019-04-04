#include "conniop.h"

__inline static int getTID( void )
{
#ifdef LINUX
	return syscall( SYS_gettid );
#else
	return (int)XplGetThreadID();
#endif
}

void
socketLogMessage( SOCK *socket, char *msg, const char *path, const int line )
{
	int threadId;
	char buffer[ 256 ];
	char *ptr;
	char *file;
	int len;
	int space;

	if( socket ) {
		threadId = getTID();
		ptr = buffer;
		ptr += sprintf( ptr, "%d", ( int )threadId );

		*ptr = ' ';
		ptr++;

		file = strrchr( path, '/' );
		if( file ) {
			file++;
			len = strlen( file );
			while( ( ptr - buffer ) + len < 25 ) {
				*ptr = ' ';
				ptr++;
			}
			memcpy( ptr, file, len );
		} else {
			len = strlen( path );
			while( ( ptr - buffer ) + len < 25 ) {
				*ptr = ' ';
				ptr++;
			}
			memcpy( ptr, path, len );
		}
		ptr += len;
		ptr += sprintf( ptr, ":%d", line );
		while( ( ptr - buffer ) < 33 ) {
			*ptr = ' ';
			ptr++;
		}

		*ptr = ' ';
		ptr++;

		len = strlen( msg );
		space = sizeof( buffer ) - ( ptr - buffer ) - 1;
		if( len > space ) {
			len = space;
		}
		memcpy( ptr, msg, len );
		ptr += len;
		*ptr++ = '\n';
		*ptr = '\0';

		socketTraceAppend( socket, buffer, ptr - buffer );
	}
}

void
SocketLogMessage( SOCK *socket, char *msg, const char *path, const int line )
{
	if( socket ) {
		XplLockAcquire( &( socket->slock ) );

		socketLogMessage( socket, msg, path, line );

		XplLockRelease( &( socket->slock ) );
	}
}


void
socketLogValueEx( SOCK *socket, char *format, int val, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, format, val );
	socketLogMessage( socket, buffer, file, line );
	errno = errnobefore;
}

void
socketLogEventEx( SOCK *socket, SocketEvent *se, void *cbArg, char *event, char *state, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "cbArg %p se %p:%02d %s event in %s state", (void *)cbArg, (void *)se, se->type, event, state );
	socketLogMessage( socket, buffer, file, line );
	errno = errnobefore;
}

void
socketLogStateChangeEx( SOCK *socket, SocketEvent *se, void *cbArg, char *state1, char *state2, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "cbArg %p se %p:%02d changing state from %s to %s", (void *)cbArg, (void *)se, se->type, state1, state2 );
	socketLogMessage( socket, buffer, file, line );
	errno = errnobefore;
}
