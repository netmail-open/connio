#if !defined(_MSC_VER)
#include <openssl/ssl.h>
#endif
#include <xplip.h>
#include <xplmem.h>

#include <stdio.h>
#include <xpldir.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "sockiop.h"
#include "conniop.h"


static unsigned long getTraceId( void )
{
	unsigned long id;
	XplLockAcquire( &( SockLib.lock ) );
	SockLib.nextTrace++;
	id = SockLib.nextTrace;
	XplLockRelease( &( SockLib.lock ) );
	return id;
}

static char *tracePathCreate( char *consumer, unsigned long id, void *subject, char *buffer, unsigned int bufferSize )
{
	strprintf( buffer, bufferSize, NULL, "trace-%s/%lu.%p", consumer, id, subject );
	return( buffer );
}

static FILE *
traceFileOpen( char *consumer, unsigned long id, void *subject, char *mode )
{
	FILE *file;
	char *s, path[XPL_MAX_PATH];

	tracePathCreate( consumer, id, subject, path, sizeof( path ) );
	file = fopen( path, mode );
	if( !file ) {
								/* create superior directories */
		for( s = strchr( path, '/'); s; s = strchr( s, '/' ) ) {
			*s = '\0';
			XplMakeDir( path );
			*s++ = '/';
		}
		file = fopen( path, mode );
		if( !file ) {
			DebugPrintf("CONNIO(%s:%d): Could not create trace file %s\n", __FILE__, __LINE__, path );
		}
	}
	if( file ) {
		DebugPrintf( "CONNIO: Created trace file %s\n", path );
	}
	return( file );
}

#define MAX_FORMATTED_SIZE 1024 * 1024 * 64
static void
CreateFormattedString( char **out, int *outSize, const char *format, va_list arguments )
{
	int required;
	char *buffer = NULL;
	char *tmp;
	unsigned long bufferSize = 0;
	unsigned long nextSize = STACK_BUFFER_SIZE;

	*out = NULL;

	do {
		tmp = MemRealloc( buffer, nextSize );
		if( tmp ) {
			buffer = tmp;
			bufferSize = nextSize;
			required = XplVsnprintf( buffer, bufferSize, format, arguments );
			if( required < 0 ) {
				/*
				   we don't know how big to make the buffer
				   double it and try again
				*/
				nextSize <<= 1;
				continue;
			}
			if( required >= bufferSize ) {
				/*
				   we know how big to make the buffer
				   because clib is c99 compliant
				*/

				nextSize = required + 1;
				continue;
			}
			*out = buffer;
			*outSize = required;
			return;
		}
		if( buffer ) {
			MemFree( buffer );
		}
		errno = ENOMEM;
		return;
	} while( bufferSize < MAX_FORMATTED_SIZE );
	errno = EMSGSIZE;
	return;
}

static int traceFlush( TraceConfig *trace, char *data, size_t dataLen )
{
	int transferred = 0;
	if( trace ) {
		if( trace->iob ) {
			iobDumpToFile( trace->iob, trace->file );
			IOBInit( trace->iob );
			if( data ) {
				if( 1 == fwrite( data, dataLen, 1, trace->file ) ) {
					transferred = dataLen;
				}
			}
		} else {
			if( data ) {
				if( 1 == fwrite( data, dataLen, 1, trace->file ) ) {
					transferred = dataLen;
				}
			}
		}
		fflush( trace->file );
	}
	return transferred;
}

static int traceAppendIOB( TraceConfig *trace, char *data, size_t dataLen )
{
	int transferred;

	if( !( trace->flags & TRACE_FLUSH_WHEN_FULL ) ) {
		transferred = IOBLog( trace->iob, data, dataLen );
	} else if( IOBGetFree( trace->iob, NULL ) > dataLen ) {
		transferred = IOBWrite( trace->iob, data, dataLen );
	} else {
		transferred = traceFlush( trace, data, dataLen );
	}
	return transferred;
}


static int
vfprintIOB( TraceConfig *trace, const char *format, va_list arguments )
{
	char *out = NULL;
	int outSize;
	char stackBuffer[ STACK_BUFFER_SIZE ];
	int transferred;

	errno = 0;
	transferred = 0;

	if( trace->iob ) {
		outSize = XplVsnprintf( stackBuffer, sizeof( stackBuffer ), format, arguments );
		if( ( outSize >= 0 ) && ( outSize < sizeof( stackBuffer ) ) ) {
			/* fits in the stack buffer */
			transferred = traceAppendIOB( trace, stackBuffer, outSize );
		} else {
			/* we need to allocate a buffer */
			CreateFormattedString( &out, &outSize, format, arguments );
			if( out ) {
				transferred = traceAppendIOB( trace, out, outSize );
				MemFree( out );
			}
		}
	}
	return( transferred );
}

static int tracePrintf( TraceConfig *trace, char *format, ... )
{
	int ret = 0;
	va_list arguments;

	if( trace ) {
		va_start( arguments, format );
		if( trace->iob ) {
			ret = vfprintIOB( trace, format, arguments );
		} else {
			ret = vfprintf( trace->file, format, arguments );
		}
		if( trace->flags & TRACE_TO_CONSOLE ) {
			vprintf( format, arguments );
		}
		va_end( arguments);
	}
	return ret;
}

static void consoleEcho( char *data, int dataLen )
{
	char *str;

	str = MemMalloc( dataLen + 1 );
	if( str ) {
		memcpy( str, data, dataLen );
		str[ dataLen ] = '\0';
		printf( "%s", str );
		MemFree( str );
	}
}

static int traceWrite( void *ptr, size_t size, size_t nmemb, TraceConfig *trace)
{
	int ret = 0;

	if( trace ) {
		if( trace->iob ) {
			if( traceAppendIOB( trace, ptr, size * nmemb ) ) {
				ret = nmemb;
			}
		} else {
			ret = fwrite( ptr, size, nmemb, trace->file );
		}
		if( trace->flags & TRACE_TO_CONSOLE ) {
			consoleEcho( ptr, size * nmemb );
		}
	}

	return ret;
}

static XplBool traceLog( TraceConfig *trace, char *buff, size_t len )
{
	XplBool ret = FALSE;

	if( trace ) {
		if( trace->iob ) {
			ret = traceAppendIOB( trace, buff, len );
		} else {
			ret = fwrite( buff, len, 1, trace->file );
		}
		if( trace->flags & TRACE_TO_CONSOLE ) {
			consoleEcho( buff, len );
		}
	}
	return ret;
}

static void traceFileRemove( TraceConfig *trace )
{
	char path[XPL_MAX_PATH];

	tracePathCreate( trace->consumer, trace->id, trace->subject, path, sizeof( path ) );
	unlink( path );
}

static int traceFree( SOCK *cs )
{
	int ret = 0;
	TraceConfig *trace;

	if( cs->trace ) {
		trace = cs->trace;
		if( trace->iob ) {
			if( trace->flags & ( TRACE_FLUSH_WHEN_CLOSED | TRACE_FLUSH_WHEN_FULL ) ) {
				iobDumpToFile( trace->iob, trace->file );
			}
			IOBFree( trace->iob );
		}
		ret = fclose( trace->file );
		if( trace->flags & TRACE_DELETE_LOG ) {
			traceFileRemove( trace );
		}
		MemFree( trace );
		cs->trace = NULL;
	}
	return ret;
}

static TraceConfig *traceAlloc( SOCK *sp, uint32 flags, char *consumer, void *subject, size_t bufferSize, char *file, int line )
{
	TraceConfig *trace;

	do {
		trace = MemMalloc( sizeof( TraceConfig ) );
		if( trace ) {
			memset( trace, 0, sizeof( TraceConfig ) );
			XplTimerStart( &( trace->startTime ) );
			if( subject ) {
				trace->subject = subject;
			} else {
				trace->subject = sp;
			}
			trace->id = getTraceId();
			trace->flags = flags;
			trace->consumer = consumer;
			trace->file = traceFileOpen( trace->consumer, trace->id, trace->subject, "wt" );
			if( trace->file ) {
				if( !bufferSize  ||
					( trace->iob = IOBAllocEx( bufferSize, file, line ) ) ) {
					break;
				}
				fclose( trace->file );
			}
			MemFree( trace );
			trace = NULL;
		}
	} while( FALSE );
	return trace;
}

INLINE static XplBool
ConnTracePrintTime( TraceConfig *trace, time_t utcTime )
{
    struct tm *ltime;
	char tempBuf[80];

	tzset();
	ltime = localtime( &utcTime );
	strftime( tempBuf, 80, "%a, %d %b %Y %H:%M:%S", ltime );
	if( tracePrintf( trace, "%s", tempBuf ) > -1 ) {
        return( TRUE );
    }
    return( FALSE );
}

static void
tracePrintAddresses( SOCK *socket )
{
	struct sockaddr_in localAddr;
	int localAddrSize;
	struct sockaddr_in remoteAddr;
	int remoteAddrSize;

	remoteAddrSize = sizeof( remoteAddr );
	localAddrSize = sizeof( localAddr );

	if( socket->trace && socketValid( socket ) && (0 == socketGetPeerAddr( socket, ( struct sockaddr * )( &remoteAddr ), &remoteAddrSize ) ) ) {
		socketGetSockAddr( socket, ( struct sockaddr * )( &localAddr ), &localAddrSize );
		tracePrintf( socket->trace, "     %d.%d.%d.%d:%d <==> %d.%d.%d.%d:%d\n",
				 localAddr.sin_addr.s_net, localAddr.sin_addr.s_host, localAddr.sin_addr.s_lh, localAddr.sin_addr.s_impno, localAddr.sin_port,
				 remoteAddr.sin_addr.s_net, remoteAddr.sin_addr.s_host, remoteAddr.sin_addr.s_lh, remoteAddr.sin_addr.s_impno, remoteAddr.sin_port
		);
	}
}

static void
tracePrintBeginBanner( SOCK *socket )
{
	if( socket->trace ) {
		tracePrintf( socket->trace, "\n\n\n******************************************************************************\n" );
		if( ConnTracePrintTime( socket->trace, time( NULL ) ) ) {
			tracePrintf( socket->trace, " (%010lu.%03lu)\n", socket->trace->startTime.sec, socket->trace->startTime.usec / 1000 );
			tracePrintf( socket->trace, "     Sock:%p Trace STARTED: \n", socket );
			tracePrintAddresses( socket );
			tracePrintf( socket->trace, "******************************************************************************\n" );
		}
	}
}

static void
tracePrintEndBanner( SOCK *socket )
{
	if( socket->trace ) {
		tracePrintf( socket->trace, "\n\n\n******************************************************************************\n" );
		if( ConnTracePrintTime( socket->trace, time( NULL ) ) ) {
			tracePrintf( socket->trace, " Sock:%p Trace ENDED\n", socket );
			tracePrintf( socket->trace, "******************************************************************************\n" );
		}
	}
}

void
SocketTraceRemoveFiles( char *consumer )
{
	XplDir *dir;
	XplDir *dirEntry;
	char path[XPL_MAX_PATH];
	char *ptr;
	int bufferLeft;
	int len;

	len = strlen( "trace-" );
	len += strlen( consumer );
	if( len < sizeof( path ) ) {
		sprintf(path, "trace-%s", consumer );
		dir = XplOpenDir( path );
		if( dir ) {
			ptr = path + len;
			*ptr = '/';
			ptr++;
			bufferLeft = sizeof( path ) - ( ptr - path );
			while( ( dirEntry = XplReadDir( dir ) ) != NULL ) {
				if( dirEntry->d_name[0] != '.' ) {
					len = strlen( dirEntry->d_name );
					if( len < bufferLeft ) {
						memcpy( ptr, dirEntry->d_name, len );
						ptr[ len ] = '\0';
						unlink( path );
					}
				}
			}
			XplCloseDir( dir );
		}
	}
}


INLINE static void
socketTraceEnd( SOCK *socket )
{
    if ( socket->trace ) {
		tracePrintEndBanner( socket );
        socket->trace->useCount--;
        if( socket->trace->useCount < 1 ) {
			/* the last conn out is responsible for freeing trace */
			traceFree( socket );
        }
		socket->trace = NULL;
    }
	return;
}

// PUBLIC ENTRY
void SocketTraceEnd( SOCK *socket )
{
	XplLockAcquire( &socket->slock );

	socketTraceEnd( socket );

	XplLockRelease( &socket->slock );
}

INLINE static XplBool
socketTraceBeginEx( SOCK *socket, char *consumer, void *subject, uint32 flags, size_t bufferSize, SOCK *tracedSocket, char *file, int line )
{
	if( socket->trace ) {
		DebugPrintf("CONNIO(%s:%d): socketTraceBegin() called on a conn that is already being traced\n", file, line );
		return( FALSE );
	}
	if( !tracedSocket ) {
		/* we are just trying to trace this connection */
		socket->trace = traceAlloc( socket, flags, consumer, subject, bufferSize, file, line );
		socket->trace->useCount++;
	} else if ( tracedSocket->trace ) {
		/* we are trying to add trace information from this conn to a conn that is already being traced */
		tracedSocket->trace->useCount++;
		socket->trace = tracedSocket->trace;
	} else {
		DebugPrintf("CONNIO(%s:%d): The second conn passed to socketTraceBegin() is not being traced\n", file, line );
		return( FALSE );
	}

	tracePrintBeginBanner( socket );

	return( TRUE );
}

XplBool
SocketTraceBeginEx( SOCK *socket, char *consumer, void *subject, uint32 flags, size_t bufferSize, SOCK *tracedSocket, char *file, int line )
{
	XplBool ret;

	XplLockAcquire( &socket->slock );

	ret = socketTraceBeginEx( socket, consumer, subject, flags, bufferSize, tracedSocket, file, line );

	XplLockRelease( &socket->slock );

	return ret;
}


INLINE static uint32
socketTraceFlagsModify( SOCK *socket, uint32 addFlags, uint32 removeFlags )
{
	if( socket->trace ) {
		socket->trace->flags |= addFlags;
		socket->trace->flags &= ~removeFlags;
		return( socket->trace->flags );
	}
	return -1;
}

uint32
SocketTraceFlagsModify( SOCK *socket, uint32 addFlags, uint32 removeFlags )
{
	uint32 newFlags;

	XplLockAcquire( &socket->slock );

	newFlags = socketTraceFlagsModify( socket, addFlags, removeFlags );

	XplLockRelease( &socket->slock );

	return newFlags;
}

INLINE static long
ConnTraceFormatHeader( SOCK *socket, char *message, char direction, long len )
{
	XplTimer age;
	unsigned long msgsize;

	if(!socket->trace) {
		return 0;
	}

	XplTimerSplit( &( socket->trace->startTime ), &age );
	if( ( socket->trace->flags & TRACE_PROCESS_CPU ) == 0 ) {
		msgsize = tracePrintf( socket->trace,
					   "\n\n\n%lu %s %s  ssl: %s  age: %lu.%03lus  len: %lx (%ld)\n%c----------------------------------------------------------------------------%c\n",
					   ( unsigned long )( socket->trace->sequence++ ),
					   message,
					   ( socket->listenerID > 0 ) ? "server" : "client",
					   ( socket->ssl.conn ) ? "yes" : "no",
					   age.sec,
					   age.usec / 1000,
					   len, len,
					   direction,
					   direction );
	} else {
		msgsize = tracePrintf( socket->trace,
					   "\n\n\n%lu %s %s  ssl: %s  age: %lu.%03lus  len: %lx (%ld)\n%c----------------------------------------------------------------------------%c\n",
					   ( unsigned long )( socket->trace->sequence++ ),
					   message,
					   ( socket->listenerID > 0 ) ? "server" : "client",
					   ( socket->ssl.conn ) ? "yes" : "no",
					   age.sec,
					   age.usec / 1000,
					   len, len,
					   direction,
					   direction );
	}
    return( msgsize );
}


INLINE static long
ConnTraceFormatTrailer( SOCK *socket, char direction)
{
    return socket->trace ? (tracePrintf( socket->trace, "\n%c----------------------------------------------------------------------------%c\n", direction, direction)) : 0;
}

INLINE static long
ConnTraceSource( SOCK *socket, const char *file, const int line )
{
    return socket->trace ? (tracePrintf( socket->trace, "SOURCE: %s:%d", file, line)) : 0;
}

static void _TraceData( SOCK *socket, char *buffer, long len )
{
	int l, offset;
	char dump[80];
	char hexChart[16]={'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
/*
00000000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................\n
0         10 13 16 19 22 25 28 31 34 37 40 43 46 49 52 55  59              75
*/

	if(!socket->trace);
	else if( socket->trace->flags & TRACE_SUPRESS_DATA )
	{
		traceWrite( "( DATA OMITTED )", sizeof( "( DATA OMITTED )" ) - 1, 1, socket->trace);
	}
	else if( socket->trace->flags & TRACE_DUMP_DATA )
	{
		offset = l = 0;
		memset( dump, ' ', 75 );
		dump[75] = '\n';
		sprintf( dump, "%8.8x", offset );
		dump[8] = ' ';
		for(;len;l++,len--,buffer++)
		{
			if( l == 16 )
			{
				traceWrite( dump, 76, 1, socket->trace );
				l = 0;
				offset += 16;
				memset( dump, ' ', 75 );
				dump[75] = '\n';
				sprintf( dump, "%8.8x", offset );
				dump[8] = ' ';
			}
			dump[10+(l*3)] = hexChart[((*buffer)>>4)&0x0f];
			dump[11+(l*3)] = hexChart[(*buffer)&0x0f];
			dump[12+(l*3)] = ' ';
			if( isprint( *buffer ) )
			{
				dump[59+l] = *buffer;
			}
			else
			{
				dump[59+l] = '.';
			}
		}
		if( l )
		{
			traceWrite( dump, 76, 1, socket->trace );
		}
	}
	else
	{
		traceWrite( buffer, len, 1, socket->trace );
	}
}

void
socketTraceEvent( SOCK *socket, uint32 event, char *buffer, long len, const char *file, const int line )
{
	int errnobefore = errno;

    if( !socket->trace ) {
        return;
    }

	if( socket->trace->flags & event ) {
		switch (event) {
			case TRACE_EVENT_RECV: {
				if( ConnTraceFormatHeader( socket, "RECV", '<', len ) > -1 ) {
					_TraceData( socket, buffer, len );
					ConnTraceFormatTrailer( socket, '<' );
				}
				break;
			}
			case TRACE_EVENT_READABLE: {
				ConnTraceFormatHeader( socket, "WAIT for READABLE", '-', 0 );
				break;
			}
			case TRACE_EVENT_SEND: {
				if( ConnTraceFormatHeader( socket, "SEND", '>', len ) > -1 ) {
					_TraceData( socket, buffer, len );
					ConnTraceFormatTrailer( socket, '>' );
				}
				break;
			}
			case TRACE_EVENT_WRITEABLE: {
				ConnTraceFormatHeader( socket, "WAIT for WRITEABLE", '-', 0 );
				break;
			}
			case TRACE_EVENT_ACCEPT_DONE: {
				ConnTraceFormatHeader( socket, "ACCEPT DONE", '-', 0 );
				break;
			}
			case TRACE_EVENT_ACCEPT_START: {
				ConnTraceFormatHeader( socket, "ACCEPT START", '-', 0 );
				break;
			}
			case TRACE_EVENT_CONNECT_DONE: {
				ConnTraceFormatHeader( socket, "CONNECT DONE", '-', 0 );
				tracePrintAddresses( socket );
				break;
			}
			case TRACE_EVENT_CONNECT_START: {
				ConnTraceFormatHeader( socket, "CONNECT START", '-', 0 );
				break;
			}
			case TRACE_EVENT_DISCONNECT_DONE: {
				ConnTraceFormatHeader( socket, "DISCONNECT DONE", '-', 0 );
				break;
			}
			case TRACE_EVENT_DISCONNECT_START: {
				ConnTraceFormatHeader( socket, "DISCONNECT START", '-', 0 );
				break;
			}
			case TRACE_EVENT_READ: {
				if( ConnTraceFormatHeader( socket, "READ from conn", '-', len ) > -1 ) {
					ConnTraceSource( socket, file, line );
					ConnTraceFormatTrailer( socket, '^' );
					_TraceData( socket, buffer, len );
					ConnTraceFormatTrailer( socket, '^' );
				}
				break;
			}
			case TRACE_EVENT_WRITE: {
				if( ConnTraceFormatHeader( socket, "WRITE to conn", '-', len ) > -1 ) {
					ConnTraceSource( socket, file, line );
					ConnTraceFormatTrailer( socket, 'v' );
					_TraceData( socket, buffer, len );
					ConnTraceFormatTrailer( socket, 'v' );
				}
				break;
			}
			case TRACE_EVENT_SSL_ACCEPT_DONE: {
				ConnTraceFormatHeader( socket, "SSL ACCEPT  connection from", '-', 0 );
				break;
			}
			case TRACE_EVENT_SSL_ACCEPT_START: {
				ConnTraceFormatHeader( socket, "SSL ACCEPT (started)  connection from", '-', 0 );
				break;
			}
			case TRACE_EVENT_SSL_CONNECT_DONE: {
				ConnTraceFormatHeader( socket, "SSL CONNECT to", '-', 0 );
				break;
			}
			case TRACE_EVENT_SSL_CONNECT_START: {
				ConnTraceFormatHeader( socket, "SSL CONNECT (started) to", '-', 0 );
				break;
			}
			case TRACE_EVENT_SSL_DISCONNECT_DONE: {
				ConnTraceFormatHeader( socket, "SSL DISCONNECT DONE", '-', 0 );
				break;
			}
			case TRACE_EVENT_SSL_DISCONNECT_START: {
				ConnTraceFormatHeader( socket, "SSL DISCONNECT START", '-', 0 );
				break;
			}
			case TRACE_EVENT_ERROR: {
				if( ConnTraceFormatHeader( socket, "ERROR", '#', 0 ) > -1 ) {
					/* for this event len contains the return value of the function that failed. NOT the length of the buffer */
					if( tracePrintf( socket->trace, "%s returned %ld errno: %d", buffer, len, errno ) > -1 ) {
						ConnTraceFormatTrailer( socket, '#' );
					}
				}
				break;
			}
			case TRACE_EVENT_CLOSE: {
				ConnTraceFormatHeader( socket, "SOCKET CLOSED", '-', 0 );
				break;
			}
		}
		if( socket->trace->flags & TRACE_FLUSH_ALL )
		{
			traceFlush( socket->trace, NULL, 0 );
		}
	}
	errno = errnobefore;
    return;
}

void SocketTraceEvent( SOCK *socket, uint32 event, char *buffer, long len, const char *file, const int line )
{
	XplLockAcquire( &socket->slock );

	socketTraceEvent( socket, event, buffer, len, file, line );

	XplLockRelease( &socket->slock );
}

static void socketTraceFlush( SOCK *socket )
{
	traceFlush( socket->trace, NULL, 0 );
}

void SocketTraceFlush( SOCK *socket )
{
	XplLockAcquire( &socket->slock );

	socketTraceFlush( socket );

	XplLockRelease( &socket->slock );
}

XplBool
socketTraceAppend( SOCK *socket, char *buff, size_t len )
{
	return traceLog( socket->trace, buff, len );
}

void
socketTraceDumpToFile( SOCK *socket, FILE *file )
{
	if( socket->trace && socket->trace->iob ) {
		iobDumpToFile( socket->trace->iob, file );
		IOBInit( socket->trace->iob );
	}
}

void
SocketTraceDumpToFile( SOCK *socket, FILE *file )
{
	XplLockAcquire( &( socket->slock ) );

	socketTraceDumpToFile( socket, file );

	XplLockRelease( &( socket->slock ) );
}
