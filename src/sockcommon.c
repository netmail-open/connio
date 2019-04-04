#include <xplip.h>
#include <xplmem.h>
#if defined(_MSC_VER)
#include <openssl/ssl.h>
#endif
#include <stdio.h>

#include "sockiop.h"
#include "conniop.h"

#define MAX_FORMATTED_SIZE 1024 * 1024 * 64

// Enable support for timeouts
#define TIMEOUTS 1

SockLibGlobals SockLib;

unsigned long SockLibInitialized = LIB_UNINITIALIZED;

static void * _malloc(size_t size)
{
	return(MemMalloc(size));
}

static void _free(void *ptr)
{
	MemFree(ptr);
}


static void * _realloc(void *ptr, size_t size)
{
	return(MemRealloc(ptr, size));
}

static void socketSSLInit( void )
{
	int count;
	char seed[129];
	char *ptr;

	/* init SSL now */
	SSL_load_error_strings();
	SSL_library_init();

	/* This is required on windows */
	CRYPTO_set_mem_functions(_malloc, _realloc, _free);

	ConnCryptoLockInit();

	memset(seed, 0, sizeof(seed));
	ptr = seed;

	for (count = 0; count < ((sizeof(seed) - 1) / 2); count++) {
		sprintf(ptr, "%02x", 0x20 + (char)(rand() % 0x5E));

		ptr += 2;
	}
	*ptr = '\0';

	RAND_seed(seed, sizeof(seed) - 1);
}


// INTERNAL caller must hold slock
int
socketGetPeerAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen )
{
	errno = 0;
	return( XplIpGetPeerName( socket->number, addr, addrLen ) );
}

// PUBLIC ENTRY
int
SocketGetPeerAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketGetPeerAddr( socket, addr, addrLen );

	if (XplIsLoopbackAddress(addr)) {
		/*
			NM-11040 If a client connects from a loopback address then return
			the address that they connected to instead of the address they
			connected from as the peer address.

			This is required because connecting to some loopback addresses is
			expected to change the behavior. For example 127.127.127.127 is used
			for testing connections as if it was not local and 127.0.0.30 is
			used by some services for connections that should not be logged.
		*/
		ret = socketGetSockAddr( socket, addr, addrLen );
	}

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// INTERNAL caller must hold slock
int
socketGetSockAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen )
{
	return( XplIpGetSockName( socket->number, addr, addrLen ) );
}

// PUBLIC ENTRY
int
SocketGetSockAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketGetSockAddr( socket, addr, addrLen );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// INTERNAL caller must hold slock
int socketClose( SOCK *socket )
{
	int ret;

	if( socket->number != SOCKET_INVALID ) {
		ret = XplIpClose( socket->number );
		socket->number = SOCKET_INVALID;
	} else {
		errno = EINVAL;
		ret = -1;
	}
	if( !ret ) {
		traceEvent( socket, TRACE_EVENT_CLOSE, NULL, 0 );
	} else {
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, errno );
	}
	return ret;
}

// PUBLIC ENTRY
int SocketClose( SOCK *socket )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketClose( socket );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

/* socketReset() is called the first time a conn is created and
   every time a conn is put back into the SOCK cache */
// INTERNAL caller must hold slock
void
socketReset( SOCK *socket )
{
	DebugAssert( socket->number == SOCKET_INVALID );
	socket->number = SOCKET_INVALID;
	DebugAssert( socket->ssl.conn == NULL );
	socket->ssl.conn = NULL;
	DebugAssert( socket->ssl.new == NULL );
	socket->ssl.new = NULL;
	DebugAssert( socket->trace == NULL );
	socket->listenerID = 0;
	sockReset( socket );
}

// PUBLIC ENTRY
void
SocketReset( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );

	socketReset( socket );

	XplLockRelease( &( socket->slock ) );
}

// INTERNAL must hold lock
int socketValid( SOCK *socket )
{
	if( socket->number != SOCKET_ERROR ) {
		return( 1 );
	}
	return( 0 );
}

// PUBLIC ENTRY
int SocketValid( SOCK *socket )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketValid( socket );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// INTERNAL must hold slock
int socketSyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn )
{
	int error;

	errno = 0;
	*blockedOn = SOCK_EVENT_TYPE_NONE;

	if( SOCKET_ERROR != ( sock->number = socCreate( FALSE ) ) ) {
		socketLogValue( sock, "Socket %d bound to this SOCK", sock->number );
		if(  !srcSaddr || !XplIpBind( sock->number, srcSaddr, srcSaddrLen ) ) {
			if( SOCKET_ERROR != XplSocketSetMode( sock->number, XPL_SOCKET_MODE_NON_BLOCKING ) ) {
				if( !XplIpConnect( sock->number, destSaddr, destSaddrLen ) ) {
					errno = 0;
				} else {
					switch (errno) {
					case EAGAIN:
#if (EWOULDBLOCK != EAGAIN)
					case EWOULDBLOCK:
#endif
#ifdef WIN32
					case EINVAL:
						/*
						  http://msdn.microsoft.com/en-us/library/windows/desktop/ms737625(v=vs.85).aspx

						  On windows a connect() call may return WSAEINVAL
						  instead of WSAEALREADY in order to be compatible
						  with some versions of winsock.
						*/
#endif
					case EALREADY:
					case EINPROGRESS:
						errno = EWOULDBLOCK;
						*blockedOn = SOCK_EVENT_TYPE_CONNECT;
						break;
					default:
						error = errno; // save errno just in case close() changes it
						traceEvent( sock, TRACE_EVENT_ERROR, NULL, errno );
						socketClose( sock );
						errno = error;
						break;
					}
				}
			}
		}
	}
	error = errno;
	errno = 0;
	return error;
}

// PUBLIC ENTRY
int SocketSyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( sock->slock ) );

	ret = socketSyncConnectStart( sock, destSaddr, destSaddrLen, srcSaddr, srcSaddrLen, blockedOn );

	XplLockRelease( &( sock->slock ) );

	return ret;
}

// INTERNAL must hold lock
void socketConnectFinish( SOCK *socket )
{
	if( SOCKET_ERROR != XplSocketSetMode( socket->number, XPL_SOCKET_MODE_DISABLE_NAGLE ) ) {
		traceEvent( socket, TRACE_EVENT_CONNECT_DONE, NULL, 0 );
	} else {
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, errno );
		DebugPrintf( "Connio: Unable to disable Nagle's Algorithm" );
	}
}

// PUBLIC ENTRY
void SocketConnectFinish( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );

	socketConnectFinish( socket );

	XplLockRelease( &( socket->slock ) );
}

// PUBLIC ENTRY
void SocketConnectAbort( SOCK *socket, int error )
{
	XplLockAcquire( &( socket->slock ) );
	traceEvent( socket, TRACE_EVENT_ERROR, NULL, error );
	socketClose( socket );
	XplLockRelease( &( socket->slock ) );
}

// PUBLIC ENTRY
void SocketDisconnectFinish( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );
	if( !socketClose( socket ) ) {
		traceEvent( socket, TRACE_EVENT_DISCONNECT_DONE, NULL, 0 );
	} else {
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, errno );
	}
	XplLockRelease( &( socket->slock ) );
}

// INTERNAL caller must hold slock
static int
disconnectWithTimeout( SOCK *socket, SocketTimeout *timeOut )
{
	int err;

	err = sockDisconnect( socket->number );

	if( ( err == EWOULDBLOCK ) || ( err == EAGAIN ) ) {

		XplLockRelease( &( socket->slock ) );

		err = sockWaitForReadable( socket->number, timeOut );

		XplLockAcquire( &( socket->slock ) );

		if( SOCKET_ERROR != err ) {
			err = 0;
		} else {
			err = errno;
			errno = 0;
		}
	}
	return err;
}

// INTERNAL caller must hold slock
static int
disconnectSSL( SOCK *socket )
{
	SocketTimeout timeout;

	SocketTimeoutSet( &timeout, 2000 );

	return disconnectWithTimeout( socket, &timeout );
}

// INTERNAL caller must hold slock
int socketDisconnect( SOCK *socket, SocketTimeout *timeOut )
{
	int err;

	traceEvent( socket, TRACE_EVENT_DISCONNECT_START, NULL, 0 );
	socketLogValue( socket, "HANGUP?: %d", SocketTimeoutGet( timeOut ) );

	err = disconnectWithTimeout( socket, timeOut );

	if( !err ) {
		socketLogValue( socket, "HUNGUP!: %d", 0 );
		traceEvent( socket, TRACE_EVENT_DISCONNECT_DONE, NULL, 0 );
		return 0;
	}
	socketLogValue( socket, "HANGUP error: %d", errno );
	traceEvent( socket, TRACE_EVENT_ERROR, NULL, errno );
	return errno;
}

// PUBLIC ENTRY
int SocketDisconnect( SOCK *socket, SocketTimeout *timeOut )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketDisconnect( socket, timeOut );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

static int
bioSockFree(BIO *a)
{
	SOCK *socket;
	if (a) {
		if (a->shutdown) {
			if (a->init) {
				socket = (SOCK *)BIO_get_callback_arg( a );
				disconnectSSL( socket );
				XplIpClose( a->num ); // SOCK is not known to the bio
			}

			a->init = 0;
			a->flags = 0;
		}

		a->num = -1;
		return(1);
	}

	return(0);
}

static int
bioSockNew(BIO *bi)
{
	bi->init=0;
	bi->num=0;
	bi->ptr=NULL;
	bi->flags=0;
	return(1);
}

static int
bioSockWrite( BIO *b, const char *in, int inl )
{
	int	ret;

	errno = 0;
    do {
        ret = XplIpSend( b->num, (void *)in, inl, 0 );
        if ( ret >= 0 ) {
			break;
        }

		switch (errno) {
			case EINTR:
				continue;

#if (EWOULDBLOCK != EAGAIN)
			case EWOULDBLOCK:
#endif
			case EAGAIN:
				BIO_set_retry_write(b);
				break;

			case ECONNRESET:
			case EPIPE:
				return(0);
			default:
				sockPrintSysError();
		}

        ret = -1;
        break;
    } while (TRUE);
	return(ret);
}

static int
bioSockRead(BIO *b, char *out, int outl)
{
    int ret;

	do {
		errno = 0;
		ret = XplIpRecv( b->num, out, outl, 0 );
		if( ret >= 0 ) {
			break;
		}
		switch (errno) {
			case EINTR:
				continue;

#if (EWOULDBLOCK != EAGAIN)
			case EWOULDBLOCK:
#endif
			case EAGAIN:
				BIO_set_retry_read(b);
				break;

			case ECONNRESET:
			case EPIPE:
				return(0);
			default:
				sockPrintSysError();
		}

		ret = -1;
		break;
	} while (TRUE);

	return( ret );
}

static long
bioSockCtrl(BIO *b, int cmd, long num, void *ptr)
{
	long ret;
	int *ip;

	switch (cmd) {
		case BIO_CTRL_RESET: {
			num = 0;
			/*	Fall through	*/
		}

		case BIO_C_FILE_SEEK: {
			ret = 0;
			break;
		}

		case BIO_C_FILE_TELL:
		case BIO_CTRL_INFO: {
			ret = 0;
			break;
		}

		case BIO_C_SET_FD: {
			bioSockFree(b);
			b->num = *((int *)ptr);
			b->shutdown = (int)num;
			b->init = 1;
			ret = 1;
			break;
		}

		case BIO_C_GET_FD: {
			if (b->init) {
				ip = (int *)ptr;
				if (ip) {
					*ip = b->num;
				}

				ret = b->num;
			} else {
				ret = -1;
			}

			break;
		}

		case BIO_CTRL_GET_CLOSE: {
			ret = b->shutdown;
			break;
		}

		case BIO_CTRL_SET_CLOSE: {
			b->shutdown = (int)num;
			ret = 1;
			break;
		}

		case BIO_CTRL_PENDING:
		case BIO_CTRL_WPENDING: {
			ret = 0;
			break;
		}

		case BIO_CTRL_DUP:
		case BIO_CTRL_FLUSH: {
			ret = 1;
			break;
		}

		default: {
			ret = 0;
			break;
		}
	}

	return(ret);
}

static BIO_METHOD nonBlockingBSDMethod =
{
	BIO_TYPE_SOCKET,
	"bsdsocket",
	bioSockWrite,
	bioSockRead,
	NULL, /* bioSockPuts, */
	NULL, /* bioSockGets, */
	bioSockCtrl,
	bioSockNew,
	bioSockFree,
	NULL
};

static BIO_METHOD *
bioNonBlocking( void )
{
	return( &nonBlockingBSDMethod );
}

static int
bioSet( SSL *s, int fd, SOCK *socket )
{
	BIO *bio;

	bio = BIO_new( bioNonBlocking() );
	if ( bio ) {
		BIO_set_fd( bio, fd, BIO_NOCLOSE );
		BIO_set_buffer_size( bio, 4096 * 4 );
		BIO_set_nbio( bio, 1 );
		SSL_set_bio( s, bio, bio );
		BIO_set_callback_arg( bio, (char *)socket );
		return(1);
	}

	SSLerr( SSL_F_SSL_SET_FD, ERR_R_BUF_LIB );

	return(0);
}

SSL *
SocketSSLAlloc( SOCK *socket, SSL_CTX *context )
{
	SSL *s;

	s = SSL_new( context );
	if( s ) {
		if( bioSet( s, socket->number, socket ) ) {
			return( s );
		}
		SSL_free( s );
	}
	errno = ENOMEM;
	return( NULL );
}

void
SocketSSLFree( SSL *s )
{
	SSL_free( s );
}

// PUBLIC ENTRY
void SocketSSLAcceptFinish( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );

	traceEvent( socket, TRACE_EVENT_SSL_ACCEPT_DONE, NULL, 0 );

	XplLockRelease( &( socket->slock ) );
}

// PUBLIC ENTRY
void SocketSSLConnectFinish( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );

	traceEvent( socket, TRACE_EVENT_SSL_CONNECT_DONE, NULL, 0 );

	XplLockRelease( &( socket->slock ) );
}

// PUBLIC ENTRY
void SocketSSLDisconnectFinish( SOCK *socket )
{
	XplLockAcquire( &( socket->slock ) );

	traceEvent( socket, TRACE_EVENT_SSL_DISCONNECT_DONE, NULL, 0 );

	XplLockRelease( &( socket->slock ) );
}

// PUBLIC ENTRY
void SocketSSLAbort( SOCK *socket, int error )
{
	XplLockAcquire( &( socket->slock ) );

	traceEvent( socket, TRACE_EVENT_ERROR, NULL, error );
	if( socket->ssl.new ) {
		SocketSSLFree( socket->ssl.new );
		socket->ssl.new = NULL;
	}
	if( socket->ssl.conn ) {
		SocketSSLFree( socket->ssl.conn );
		socket->ssl.conn = NULL;
	}

	XplLockRelease( &( socket->slock ) );
}

static int
sslErrorQueueClear( int sslError, const char *file, const int line )
{
	int threadError;
	char errstr[120];

	if( !sslError ) {
		/* The caller does not care what errors are here; they just want the queue cleaned out. */
		while ((threadError = ERR_get_error())) {
			ERR_error_string(threadError, errstr);
			DebugPrintf("CONNIO[%s:%d]: an extraneous SSL error was found in the thread error queue.\n%s (%d)\n", file, line, errstr, threadError);
		}
		return 0;
	}

	if( ( sslError != SSL_ERROR_WANT_WRITE ) && ( sslError != SSL_ERROR_WANT_READ ) ) {
		/* The error we already have is serious, just clean out the queue */
		while ((threadError = ERR_get_error())) {
			ERR_error_string(threadError, errstr);
#if defined(DEBUG_SSL_VERBOSE)
			DebugPrintf("CONNIO[%s:%d]: an additional SSL error was found in thread error queue.\n%s (%d)\n", file, line, errstr, threadError);
#endif
		}
		return 0;
	}

	/* The error we already have is not serious. Are there others that are? */
	while ((threadError = ERR_get_error())) {
		ERR_error_string(threadError, errstr);
		DebugPrintf("CONNIO[%s:%d]: a more serious SSL error was found in the thread error queue.\n%s (%d)\n", file, line, errstr, threadError);
		if( ( threadError != SSL_ERROR_WANT_WRITE ) && ( threadError != SSL_ERROR_WANT_READ ) ) {
			sslError = threadError;
		}
	}
	return( sslError );
}

static int
sslErrorGet( SSL *sslSocket, int error, SocketEventType *blockedOn, const char *file, const int line )
{
	int sslError;
#if DEBUG
	char sslErrorString[120];
#endif

	/*
		If any errors are left in the ERR_get_error() queue then the
		next call to SSL_get_error() on this thread will return
		SSL_ERROR_SSL masking the real response.

		ERR_get_error() MUST be called until it returns 0.

		WANT_READ and WANT_WRITE are not really errors.
		If there is an error in the queue other than these, we want to act on it.
	*/
	sslError = sslErrorQueueClear( SSL_get_error( sslSocket, error ), file, line );

	switch( sslError ) {
		case SSL_ERROR_WANT_WRITE:
			*blockedOn = SOCK_EVENT_TYPE_WRITABLE;
			return( EWOULDBLOCK );

		case SSL_ERROR_WANT_READ:
			*blockedOn = SOCK_EVENT_TYPE_READABLE;
			return( EWOULDBLOCK );

		case SSL_ERROR_ZERO_RETURN:
			*blockedOn = SOCK_EVENT_TYPE_NONE;
			return( ECONNCLOSEDBYPEER );

		case SSL_ERROR_NONE:
			return( EIO );
		case SSL_ERROR_SYSCALL:
		case SSL_ERROR_SSL:
		default:
			*blockedOn = SOCK_EVENT_TYPE_NONE;
#if DEBUG
			if( sslError ) {
				ERR_error_string(sslError, sslErrorString);
				XplConsolePrintf("SSL Error %d (%s)\n\tsource: %s:%d\n", sslError, sslErrorString, file, line);
			}
#endif
			sockPrintSysError();
			return( EIO );
	}
	return( ECONNIOBUG );
}

INLINE static int
sslConnect( SOCK *socket, SSL_CTX *context, SocketEventType *blockedOn, XplBool server )
{
	int sslRet;
	int err;

	if( !socket->ssl.conn ) {
		if( !socket->ssl.new ) {
			socket->ssl.new = SocketSSLAlloc( socket, context );
		}
		if( socket->ssl.new ){
			sslErrorQueueClear( 0, __FILE__, __LINE__ );
			if( server ) {
				sslRet = SSL_accept( socket->ssl.new );
			} else {
				sslRet = SSL_connect( socket->ssl.new );
			}
			if( sslRet == 1 ) {
				socket->ssl.conn = socket->ssl.new;
				socket->ssl.new = NULL;
				return( 0 );
			}
			err = sslErrorGet( socket->ssl.new, sslRet, blockedOn, __FILE__, __LINE__ );
			if( err != EWOULDBLOCK ) {
				traceEvent( socket, TRACE_EVENT_ERROR, NULL, err );
			}
			return( err );
		}
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, ECONNSSLSTART );
		return( ECONNSSLSTART );
	}
	traceEvent( socket, TRACE_EVENT_ERROR, NULL, ECONNALREADYSECURED );
	return( ECONNALREADYSECURED );
}

// PUBLIC ENTRY
int SocketSSLAccept( SOCK *socket, SSL_CTX *context, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = sslConnect( socket, context, blockedOn, TRUE );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// PUBLIC ENTRY
int SocketSSLConnect( SOCK *socket, SSL_CTX *context, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = sslConnect( socket, context, blockedOn, FALSE );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// INTERNAL must hold slock
int socketSSLDisconnect( SOCK *socket, SocketEventType *blockedOn )
{
	int sslRet;
	int err;

	if( socket->ssl.conn ) {
		sslErrorQueueClear( 0, __FILE__, __LINE__ );
		sslRet = SSL_shutdown( socket->ssl.conn );
		if( sslRet != -1 ) {
			SSL_free( socket->ssl.conn );
			socket->ssl.conn = NULL;
			return( 0 );
		}
		err = sslErrorGet( socket->ssl.conn, sslRet, blockedOn, __FILE__, __LINE__ );
		if( err != EWOULDBLOCK ) {
			traceEvent( socket, TRACE_EVENT_ERROR, NULL, err );
		}
		return( err );
	}
	return( ECONNALREADYSECURED );
}

// PUBLIC ENTRY
int SocketSSLDisconnect( SOCK *socket, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketSSLDisconnect( socket, blockedOn );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

static int
sslReceive( SOCK *socket, char *buffer, size_t length, SocketTimeout *timeOut, SocketEventType *blockedOn, int *error )
 {
	int bytes;
	int err;
	int waitErr;

	*blockedOn = SOCK_EVENT_TYPE_NONE;

	do {
		errno = 0;
		sslErrorQueueClear( 0, __FILE__, __LINE__ );
		bytes = SSL_read( socket->ssl.conn, (void *)buffer, length);
		if ( bytes > 0 ) {
			traceEvent( socket, TRACE_EVENT_RECV, buffer, bytes );
			*error = 0;
			errno = 0;
			return( bytes );
		}

		err = sslErrorGet( socket->ssl.conn, bytes, blockedOn, __FILE__, __LINE__ );
		if( err != EWOULDBLOCK ) {
			*error = err;
			traceEvent( socket, TRACE_EVENT_ERROR, NULL, err );
			errno = 0;
			return( SOCKET_ERROR );
		}

		if( !timeOut ) {
			*error = err;
			/* blockedOn initialized by socketSSLError */
			errno = 0;
			return( SOCKET_ERROR );
		}

		/* block until there is data or the timeout hits */
		if( *blockedOn == SOCK_EVENT_TYPE_READABLE ) {
			traceEvent( socket, TRACE_EVENT_READABLE, NULL, 0 );
			socketLogValue( socket, "READABLE?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForReadable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if ( waitErr != SOCKET_ERROR ) {
				socketLogValue( socket, "READABLE!: %d", 0 );
				continue;
			}
			socketLogValue( socket, "READABLE error: %d", errno );
		} else if( *blockedOn == SOCK_EVENT_TYPE_WRITABLE ) {
			traceEvent( socket, TRACE_EVENT_WRITEABLE, NULL, 0 );
			socketLogValue( socket, "WRITEABLE?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForWritable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if ( waitErr != SOCKET_ERROR ) {
				socketLogValue( socket, "WRITEABLE!: %d", 0 );
				continue;
			}
			socketLogValue( socket, "WRITEABLE error: %d", errno );
		}
		*error = errno;
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, errno );
		errno = 0;
		return(SOCKET_ERROR);
	} while (TRUE);
}

static int
sslSend( SOCK *socket, char *buffer, size_t bytes, SocketEventType *blockedOn, int *error )
{
	int ccode;
	int writeCount = 0;
	int err = 0;

	*blockedOn = SOCK_EVENT_TYPE_NONE;

	do {
		errno = 0;
		sslErrorQueueClear( 0, __FILE__, __LINE__ );
		ccode = SSL_write( socket->ssl.conn, (void *)( buffer + writeCount ), bytes - writeCount );
		if( ccode == ( bytes - writeCount ) ) {
			traceEvent( socket, TRACE_EVENT_SEND, buffer + writeCount, ccode );
			*error = 0;
			errno = 0;
			return( bytes );
		}

		if (ccode > 0) {
			traceEvent( socket, TRACE_EVENT_SEND, buffer + writeCount, ccode );
			/* short count */
			writeCount += ccode;
			continue;
		}

		err = sslErrorGet( socket->ssl.conn, ccode, blockedOn, __FILE__, __LINE__ );
		*error = err;
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, *error );
		errno = 0;
		return(SOCKET_ERROR);
	} while (TRUE);
}

INLINE static int
clearReceive( SOCK *socket, char *buffer, size_t length, SocketTimeout *timeOut, SocketEventType *blockedOn, int *error )
{
	int bytes;
	int waitErr;
	*blockedOn = SOCK_EVENT_TYPE_NONE;

	socketLogValue( socket, "Read request %d byte buffer", length );
	do {
		errno = 0;
		bytes = XplIpRecv( socket->number, buffer, length, 0);
		if( bytes > 0 ) {
			socketLogValue( socket, "Read %d bytes", bytes );
			traceEvent( socket, TRACE_EVENT_RECV, buffer, bytes );
			*error = 0;
			errno = 0;
			return( bytes );
		}


		if( bytes == 0 ) {
			socketLogValue( socket, "Read shutdown by peer %d", 777 );
			*error = ECONNCLOSEDBYPEER;
			errno = 0;
			traceEvent( socket, TRACE_EVENT_ERROR, NULL, *error );
			return( SOCKET_ERROR );
		}

		if( errno == EINTR ) {
			traceEvent( socket, TRACE_EVENT_ERROR, NULL, EINTR );
			socketLogValue( socket, "Read interupted %d", errno );
			continue;
		}

		if( errno != EWOULDBLOCK ) {
			socketLogValue( socket, "Read error: %d", errno );
			*error = errno;
			errno = 0;
			traceEvent( socket, TRACE_EVENT_ERROR, NULL, *error );
			return( SOCKET_ERROR );
		}

		*blockedOn = SOCK_EVENT_TYPE_READABLE;
		if( !timeOut ) {
			socketLogValue( socket, "Read would block: %d", errno );
			/* non-blocking */
			*error = errno;
			errno = 0;
			return( SOCKET_ERROR );
		}

		/* we need to behave like we're blocking so EWOULDBLOCK needs to be hidden */
		errno = 0;

		traceEvent( socket, TRACE_EVENT_READABLE, NULL, 0 );
		/* block until there is data or the timeout hits */
		socketLogValue( socket, "READABLE?: %d", SocketTimeoutGet( timeOut ) );

		XplLockRelease( &( socket->slock ) ); // release lock before blocking

		waitErr = sockWaitForReadable( socket->number, timeOut );

		XplLockAcquire( &( socket->slock ) );

		if ( waitErr != SOCKET_ERROR ) {
			socketLogValue( socket, "READABLE!: %d", 0 );
			continue;
		}
		socketLogValue( socket, "READABLE error: %d", errno );
		*error = errno;
		errno = 0;
		return( SOCKET_ERROR );
	} while( TRUE );
}

// INTERNAL caller must hold slock
INLINE static int
socketReceive( SOCK *socket, char *buffer, size_t length, SocketTimeout *timeOut, SocketEventType *blockedOn, int *error )
{
	if ( length ) {
		if ( socket && buffer ) {
			if( !( socket->ssl.conn ) ) {
				return( clearReceive( socket, buffer, length, timeOut, blockedOn, error ) );
			} else {
				return( sslReceive( socket, buffer, length, timeOut, blockedOn, error ) );
			}
		}
		*error = EINVAL;
		return(SOCKET_ERROR);
	}
	*error = EMSGSIZE;
	return(SOCKET_ERROR);
}

// PUBLIC ENTRY
int SocketReceive( SOCK *socket, char *buffer, size_t length, SocketTimeout *timeOut, SocketEventType *blockedOn, int *error )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketReceive( socket, buffer, length, timeOut, blockedOn, error );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

INLINE static int
clearSend( SOCK *socket, char *buffer, size_t bytes, SocketEventType *blockedOn, int *error )
{
	int ccode;
	int writeCount = 0;
#if DEBUG_SHORT_COUNT
	int shorted = 0;
#endif

	*blockedOn = SOCK_EVENT_TYPE_NONE;
	socketLogValue( socket, "Send Request: %d bytes", bytes );
	DebugAssert( bytes );  // somebody called send with no data
	do {
		errno = 0;
		ccode = XplIpSend( socket->number, buffer + writeCount, bytes - writeCount, 0);
		DebugAssert( ccode );  // send() returned 0.  This code does not expect that
		if( ccode > 0 ) {
			socketLogValue( socket, "SENT: %d", ccode );
			traceEvent( socket, TRACE_EVENT_SEND, buffer + writeCount, ccode );
			writeCount += ccode;
			if( writeCount >= bytes ) {
#if DEBUG_SHORT_COUNT
				if( shorted ) {
					printf( "%d SHORT DONE:  %d:%d:%u ERRNO on a short count is %d\n", socket->number, ccode, writeCount, bytes, errno );
				}
#endif
				*error = 0;
				errno = 0;
				return( writeCount );
			}

			/* short count */
#if DEBUG_SHORT_COUNT
printf( "%d SHORT COUNT: %d:%d:%u ERRNO on a short count is %d\n", socket->number, ccode, writeCount, bytes, errno );
shorted = 1;
#endif
            socketLogValue( socket, "SEND shorted: %d", ( bytes - writeCount ) );
			continue;
		}
		/* ccode < 0 */
		if( errno == EWOULDBLOCK ) {
			*blockedOn = SOCK_EVENT_TYPE_WRITABLE;
		}
		*error = errno;
#if DEBUG_SHORT_COUNT
printf( "%d ERRNO on a send error is %d\n", socket->number, errno );
#endif
		errno = 0;
		if( writeCount ) {
			socketLogValue( socket, "SEND partial with errno %d", *error );
			return( writeCount );
		}
		if( *error == 0 ) {
			*error = ECONNIOBUG;
		}
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, *error );
		socketLogValue( socket, "SEND error %d.", *error );
		return( SOCKET_ERROR );
	} while( TRUE );
}

// INTERNAL caller must hold slock
INLINE static int
socketSend( SOCK *socket, char *buffer, size_t bytes, SocketEventType *blockedOn, int *error )
{
	if (bytes) {
		if ( socket && buffer) {
			if ( socket->ssl.conn ) {
				return( sslSend( socket, buffer, bytes, blockedOn, error ) );
			} else {
				return( clearSend( socket, buffer, bytes, blockedOn, error ) );
			}
		}
		*error = EINVAL;
		traceEvent( socket, TRACE_EVENT_ERROR, NULL, *error );
		errno = 0;
		return(SOCKET_ERROR);
	}
	*error = 0;
	errno = 0;
	return(0);
}

// PUBLIC ENTRY
int SocketSend( SOCK *socket, char *buffer, size_t bytes, SocketEventType *blockedOn, int *error )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketSend( socket, buffer, bytes, blockedOn, error );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// INTERNAL
INLINE static int
socketWait( SOCK *socket, SocketEventType waitOn, SocketTimeout *timeOut )
{
	int err;
	int waitErr;

	switch( waitOn )
	{
		case SOCK_EVENT_TYPE_READABLE:
		{
			socketLogValue( socket, "READABLE?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForReadable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if( waitErr != SOCKET_ERROR ) {
				socketLogValue( socket, "READABLE!: %d", 0 );
				return( 0 );
			}
			socketLogValue( socket, "READABLE error: %d", errno );
			break;
		}
		case SOCK_EVENT_TYPE_WRITABLE:
		{
			socketLogValue( socket, "WRITEABLE?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForWritable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if( waitErr != SOCKET_ERROR ) {
				socketLogValue( socket, "WRITEABLE!: %d", 0 );
				return( 0 );
			}
			socketLogValue( socket, "WRITEABLE error: %d", errno );
			break;
		}
		case SOCK_EVENT_TYPE_DISCONNECT:
		{
			socketLogValue( socket, "HANGUP?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForReadable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if( waitErr != SOCKET_ERROR ) {
				socketLogValue( socket, "HUNGUP!: %d", 0 );
				return( 0 );
			}
			socketLogValue( socket, "HANGUP error: %d", errno );
			if( ECONNRESET == errno ) {
				errno = 0;
				return( 0 );
			}
			break;
		}
		case SOCK_EVENT_TYPE_CONNECT:
		{
			socketLogValue( socket, "CONNECTED?: %d", SocketTimeoutGet( timeOut ) );

			XplLockRelease( &( socket->slock ) ); // release lock before blocking

			waitErr = sockWaitForWritable( socket->number, timeOut );

			XplLockAcquire( &( socket->slock ) );

			if( SOCKET_ERROR == waitErr ) {
				socketLogValue( socket, "CONNECTED error: %d", errno );
			} else {
				/* Success */
				errno = 0;
				socketLogValue( socket, "CONNECTED!: %d", 0 );
				return( 0 );
			}
			break;
		}
		case SOCK_EVENT_TYPE_LISTEN:
		case SOCK_EVENT_TYPE_NONE:
		{
			errno = ECONNIOBUG;
			break;
		}
	}

	err = errno;
	errno = 0;
	return( err );
}

// PUBLIC ENTRY
int SocketWait( SOCK *socket, SocketEventType waitOn, SocketTimeout *timeOut )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketWait( socket, waitOn, timeOut );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

/********************************** Socket Event Code *************************************************/

static void socketEventsClean( SOCK *socket )
{
	sockEventClean( &socket->event.send );
	sockEventClean( &socket->event.receive );
	sockEventClean( &socket->event.connect );
}

// PUBLIC ENTRY
// (TO BE DEPRICATED)
void SocketEventsClean( SOCK *socket )
{
	XplLockAcquire( &socket->slock );
	socketEventsClean( socket );
	XplLockRelease( &socket->slock );
}

static void socketRelease( SOCK **sp )
{
	if( *sp ) {
		if( ( *sp )->number != SOCKET_INVALID ) {
			socketClose( *sp );
		}
		socketEventsClean( *sp );
		socketReset( *sp );
		// sp could be recycled at this point
		MemFree( *sp );
		*sp = NULL;
	}
}

void SocketRelease( SOCK **sp )
{
	if( *sp ) {
		XplLockAcquire( &( ( *sp )->slock ) );
		socketRelease( sp );
		// XplLockRelease( &( ( *sp )->slock ) ); // the lock is already gone by this point.
	}
}

// PUBLIC ENTRY
XplBool SocketIsIdle( SOCK *socket )
{
	XplBool idle;

	XplLockAcquire( &( socket->slock ) );

	if( ( socket->event.connect.cb.eventFunc == NULL ) &&
		( socket->event.receive.cb.eventFunc == NULL ) &&
		( socket->event.send.cb.eventFunc == NULL ) ) {
		idle = TRUE;
	} else {
		idle = FALSE;
	}

	XplLockRelease( &( socket->slock ) );

	return idle;
}

// INTERNAL caller must hold slock
void socketDebug( int context, SocketEvent *event, int type, char *format, ... )
{
	char	line[1024];
	va_list	args;

	switch( context )
	{
		case 0:
			strcpy( line, "SocketEventSubmit: " );
			break;
		case 1:
			strcpy( line, "SocketEventKill: " );
			break;
		case 2:
			strcpy( line, "SocketEvent: " );
			break;

		case 3:
			strcpy( line, "_SocketEventAdd: " );
			break;

		case 4:
			strcpy( line, "_SocketEventDelete: " );
			break;

		case 5:
			strcpy( line, "_PollThread: " );
			break;

		default:
			strcpy( line, "SocketEngineUnknown: " );
	}
	sprintf( line + strlen( line ), "se[%08lx](%d) ", (unsigned long)event, (event) ? event->cs->number : 0 );
	switch( type )
	{
		case SOCK_EVENT_TYPE_LISTEN:

			strcat( line, "LISTEN" );
			break;

		case SOCK_EVENT_TYPE_READABLE:
			strcat( line, "READABLE" );
			break;

		case SOCK_EVENT_TYPE_WRITABLE:
			strcat( line, "WRITABLE" );
			break;

		default:
			sprintf( line + strlen( line ), "UNKNOWN(%d)", type );
			break;
	}
	if( format )
	{
		strcat( line, " " );
		va_start( args, format );
		vsprintf( line + strlen( line ), format, args );
		va_end( args );
	}
	strcat( line, "\n" );
//	printf( "%s", line );
}

static AsyncListener *asyncListenerAlloc( AThreadEngine *threadEngine, unsigned long throttlingThreshold, ListenerEventCallback cb )
{
	AsyncListener *listener;

	listener = MemMalloc( sizeof( AsyncListener ) );
	if( listener ) {
		memset( listener, 0, sizeof( AsyncListener ) );
		listener->acceptThreadEngine = threadEngine;
		listener->throttlingThreshold = throttlingThreshold;
		listener->eventCallback = cb;
	}
	return listener;
}

static void asyncListenerRelease( AsyncListener **listener )
{
	if( listener ) {
		MemFree( *listener );
		*listener = NULL;
	}
}

int HandleSocketEvent( void *arg )
{
	SocketEvent *se;
	SOCK *cs;
	XplBool eventComplete;
	SocketEventCB cb;
	void *cbArg;
	SocketEventType type;
	int error;

	se = (SocketEvent *)arg;
	cs = se->cs;

	XplLockAcquire( &( cs->slock ) );

	/* capture important information from the socket event */
	type = se->type;
	cbArg = se->cb.arg;
	cb = se->cb.eventFunc;
	error = se->error;
	TLogWrite( "CONNIO WORKER: start working on SOCK:%p", cs );

	if( ( type == SOCK_EVENT_TYPE_LISTEN ) && listenerThrottling( se ) ) {
		listenerSpinWhileThrottling( se );
		eventComplete = sockEventListenerRequeue( cs, se );
		XplLockRelease( &( cs->slock ) );
		if( eventComplete ) {
			AThreadBegin( &( se->cb.thread ) );
		} else if( se->cs->listener->eventCallback ) {
			se->cs->listener->eventCallback( LISTENER_STARTED, 0, 0, se, cs, cs->number );
		}
	} else {
		SeUnlinkCe( se, cbArg );
		XplLockRelease( &( cs->slock ) );

		if( type == SOCK_EVENT_TYPE_LISTEN ) {
			/* a listener has failed */
			/* give child conns time to get out of their accept callbacks */
			if( cs->listener->eventCallback ) {
				cs->listener->eventCallback( LISTENER_WAITING, 0, 0, se, cs, cs->number );
			}
			for( ; ; ) {
				XplLockAcquire( &( cs->slock ) );
				if( cs->listener->inAccept < 1 ) {
					XplLockRelease( &( cs->slock ) );
					break;
				}
				XplLockRelease( &( cs->slock ) );
				XplDelay( 500 );
			}
			if( cs->listener->eventCallback ) {
				cs->listener->eventCallback( LISTENER_RETURNED, 0, 0, se, cs, cs->number );
			}
			asyncListenerRelease( &( cs->listener ) );
		}

		cb( cs, cbArg, type, error );
	}
	return 0;
}

AcceptEvent *acceptEventPop( AcceptEventCache *aeCache )
{
	ListMember *ae;
	MemberList *eventCache = &( aeCache->safeList );

	if( ( ae = eventCache->head ) )	{
		if( !( eventCache->head = ae->next ) ) {
			eventCache->tail = &eventCache->head;
		}
	}
	return ( AcceptEvent * )ae;
}

static int
acceptPickup( void *arg )
{
	AcceptEvent *acceptEvent = ( AcceptEvent * )( arg );
	int soc = acceptEvent->socket;
	void *cbArg = acceptEvent->cbArg;
	AcceptEventCB cb = acceptEvent->cb;
	SOCK *serverSp = acceptEvent->listenerSp;
	SOCK *childSp;

	/*
	  We have a copy of everything we need on the stack so we can release
	  this event structure so it can be used for another new connection
	*/
	acceptEventRelease( acceptEvent );

	childSp = socketAlloc( serverSp->listenerID );
	if( childSp ) {
		childSp->number = soc;
		cb( serverSp, &childSp, cbArg );
	} else {
		XplIpClose( soc ); // no SOCK got created. Close soc directly
	}

	XplLockAcquire( &( serverSp->slock ) );
	serverSp->listener->inAccept--;
	XplLockRelease( &( serverSp->slock ) );

	return 0;
}

// INTERNAL
AcceptEvent *acceptEventAlloc( AcceptEventCache *aeCache )
{
	AcceptEvent *acceptEvent;

	AECLock( aeCache );
	acceptEvent = acceptEventPop( aeCache );
	AECUnlock( aeCache );

	if( !acceptEvent ) {
		acceptEvent = AEMalloc();
	}
	return acceptEvent;
}

void acceptEventInit( AcceptEvent *acceptEvent, SocketEvent *listenerSe, XplSocket soc )
{
	acceptEvent->socket = soc;
	acceptEvent->listenerSp = listenerSe->cs;
	acceptEvent->cb = listenerSe->cb.acceptFunc;
	acceptEvent->cbArg = listenerSe->cb.arg;
	AThreadInit( &( acceptEvent->thread ), listenerSe->cs->listener->acceptThreadEngine, acceptPickup, acceptEvent, FALSE );
}

// releases an accept event structure back to the engine
int
acceptEventRelease( AcceptEvent *acceptEvent )
{
	ListMember *ae;
	MemberList *safeList;

	safeList = &( SockLib.eventEngine->aeCache.safeList );
	ae = ( ListMember * )acceptEvent;
	ae->next = NULL;
	XplLockAcquire( &safeList->lock );
	*( safeList->tail ) = ae;
	safeList->tail = &ae->next;
	XplLockRelease( &safeList->lock );
	return 0;
}

// INTERNAL caller must hold slock
void acceptReady( AcceptEvent *event )
{
	//DebugConnQueue( event->cs, CIO_SOCK_ACCEPT_NOTIFY, NULL, NULL, NULL );
	event->listenerSp->listener->inAccept++;
	AThreadBegin( &( event->thread ) );
}

static unsigned long getListenerUid( void )
{
	unsigned long id;
	XplLockAcquire( &( SockLib.lock ) );
	SockLib.nextListener++;
	id = SockLib.nextListener;
	XplLockRelease( &( SockLib.lock ) );
	return id;
}

SOCK *
SocketAccept( SOCK *listenerSp )
{
	int ccode;
	SOCK *childSp;

	if( ( childSp = SocketAlloc( listenerSp->listenerID ) ) ) {
		childSp->number = XplIpAccept( listenerSp->number, NULL, 0 );
		if (childSp->number != SOCKET_INVALID) {
			socketLogValue( childSp, "ip socket %d assigned to SOCK ptr", childSp->number );
			ccode = XplSocketSetMode( childSp->number, XPL_SOCKET_MODE_NON_BLOCKING );
			if (ccode != SOCKET_ERROR) {
				childSp->listenerID = listenerSp->listenerID;
				return childSp;
			}
		}
		socketRelease( &childSp );
	}
	return( NULL );
}

static int
socketListen( SOCK *sp, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, int listenerID )
{
	int ccode = 1;

	sp->number = socCreate( FALSE );
	if( sp->number != SOCKET_INVALID ) {
		socketLogValue( sp, "ip socket %d assigned to SOCK ptr", sp->number );
		XplIpSetSockOption( sp->number, SOL_SOCKET, SO_REUSEADDR, (unsigned char *)&ccode, sizeof(ccode) );
		if( XplIpBind( sp->number, srcSaddr, srcSaddrLen ) != SOCKET_ERROR ) {
			if ( XplIpListen( sp->number, backlog ) != SOCKET_ERROR ) {
				if ( XplSocketSetMode( sp->number, XPL_SOCKET_MODE_BLOCKING ) != -1 ) {
					/*
					  Why blocking? (Especially when everything else is non-blocking)
					  We could make it non-blocking and then make blocking
					  version of accept() call SocketWait(), but since the async
					  code never calls accept() unless it already knows that a
					  connection request is waiting, it will never block anyway.
					 */
					if( !XplIpGetSockName( sp->number, srcSaddr, &srcSaddrLen ) ) {
						if( listenerID ) {
							sp->listenerID = listenerID;
						} else {
							sp->listenerID = getListenerUid();
						}
						return 0;
					}
				}
			}
		}
		socketClose( sp );
	}
	return -1;
}

int
SocketListen( SOCK *sp, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, int listenerID )
{
	int err;

	XplLockAcquire( &( sp->slock ) );

	err = socketListen( sp, srcSaddr, srcSaddrLen, backlog, listenerID );

	XplLockRelease( &( sp->slock ) );

	return err;
}

/* SocketInit() is only called when a a conn is newly allocated.
   It is NOT called when a conn is put back into the conn cache */
// PUBLIC ENTRY   (TO BE DEPRICATED)
void SocketInit( SOCK *cs, char *identity )
{
	XplLockInit( &( cs->slock ) );
	XplLockAcquire( &( cs->slock ) );
	socketReset( cs );
	sockEventInit( cs, &( cs->event.receive ) );
	sockEventInit( cs, &( cs->event.send ) );
	sockEventInit( cs, &( cs->event.connect ) );
	DebugAssert( identity );
	XplLockRelease( &( cs->slock ) );
}

SOCK *SocketAlloc( int listenerId )
{
	SOCK *sp;

	sp = MemMalloc( sizeof ( SOCK ) );
	if( sp ) {
		memset( sp, 0, sizeof( SOCK ) );
		sp->number = SOCKET_INVALID;
		XplLockInit( &( sp->slock ) );
		socketReset( sp );
		sp->listenerID = listenerId;
		sockEventInit( sp, &( sp->event.receive ) );
		sockEventInit( sp, &( sp->event.send ) );
		sockEventInit( sp, &( sp->event.connect ) );
	}
	return sp;
}


/*  possible macros
#define SocketEventListenEx( cs, type, timeOut, ecb, acb, cbArg, file, line )   \
	SocketEventWaitEx( ( cs ), ( type ), ( timeOut ), NULL, 0, ( ecb ), ( acb ), ( cbArg ), ( file ), ( line ) )

#define SocketEventBeginEx( cs, type, timeOut, dst, len, ecb, cbArg, file, line )   \
	SocketEventWaitEx( ( cs ), ( type ), ( timeOut ), ( dst ), ( len ), ( ecb ), NULL, ( cbArg ), ( file ), ( line ) )

#define SocketGenEx( cs, type, timeOut, dst, len, ecb, acb, cbArg, file, line )   \
	SocketEventWaitEx( ( cs ), ( type ), ( timeOut ), ( dst ), ( len ), ( ecb ), ( acb ), ( cbArg ), ( file ), ( line ) )
*/

// PUBLIC ENTRY
int SocketEventWaitEx( SOCK *cs, SocketEventType type, SocketTimeout *timeOut, struct sockaddr *dst, socklen_t dstLen, SocketEventCB ecb, AThreadEngine *ecbThreadEngine, AcceptEventCB acb, void *acbArg, AThreadEngine *acbThreadEngine, unsigned long throttlingThreshold, ListenerEventCallback lecb, const char *file, long line )
{
	SocketEvent *se;
	XplBool eventComplete;
	uint32 traceEvent;
	int err;
	XplBool doneAlready;

	err = 0;

	DebugAssert( NULL == cs->listener );

	type = sockEventTranslate( type );

	XplLockAcquire( &( cs->slock ) );

	switch( type ) {
	case SOCK_EVENT_TYPE_READABLE:
		se = &( cs->event.receive );
		traceEvent = TRACE_EVENT_READABLE;
		break;
	case SOCK_EVENT_TYPE_WRITABLE:
		se = &( cs->event.send );
		traceEvent = TRACE_EVENT_WRITEABLE;
		break;
	case SOCK_EVENT_TYPE_LISTEN:
		se = &( cs->event.connect );
		if( SeGetFlags( se ) & FSE_LISTENER_KILLED ) {
			err = ECONNABORTED;
			break;
		}
		cs->listener = asyncListenerAlloc( acbThreadEngine, throttlingThreshold, lecb );
		if( !cs->listener ) {
			err = ENOMEM;
			break;
		}
		if( cs->listener->eventCallback ) {
			cs->listener->eventCallback( LISTENER_SUBMITTED, 0, 0, se, cs, cs->number );
		}
		traceEvent = TRACE_EVENT_READABLE;
		break;
	case SOCK_EVENT_TYPE_CONNECT:
		se = &( cs->event.connect );
		traceEvent = TRACE_EVENT_WRITEABLE;
		break;
	case SOCK_EVENT_TYPE_DISCONNECT:
		se = &( cs->event.connect );
		traceEvent = TRACE_EVENT_READABLE;
		break;
	default:
	case SOCK_EVENT_TYPE_NONE:
		DebugAssert( 0 );
		se = NULL;
		err = ECONNIOBUG;
		break;
	}

	if( err ) {
		;
	} else if( SOCKET_INVALID == cs->number ) {
		err = ENOTSOCK;
	} else if( !SeLinkCe( se, type, ecb, ecbThreadEngine, acb, acbArg, file, line ) ) {
		err = EBUSY;
	} else if( ( err = sockEventAdd( cs, se, timeOut, dst, dstLen, &doneAlready ) ) ) {
		SeUnlinkCe( se, acbArg );
	}

	if( err ) {
		traceEvent( cs, TRACE_EVENT_ERROR, NULL, errno );
		asyncListenerRelease( &( cs->listener ) );
		socketDebug( 0, se, type, "Failure" );
		eventComplete = FALSE;
	} else if( !doneAlready ) {
		/* expected case */
		traceEvent( cs, traceEvent, NULL, 0 );
		eventComplete = FALSE;
	} else {
		/* the event completed when it was submitted, we don't ever expect this
		   to happen on unix, but it will on windows
		*/
		traceEvent( cs, traceEvent, NULL, 0 );
		DebugAssert( 0 == SeGetFlags( se ) );
		asyncListenerRelease( &( cs->listener ) );
		eventComplete = TRUE;
	}

	XplLockRelease( &( cs->slock ) );

	if( eventComplete ) {
		AThreadBegin( &( se->cb.thread ) );
	}

	return err;
}

// PUBLIC ENTRY
XplBool SocketListenerSuspend( SOCK *cs )
{
	XplBool ret = FALSE;
	SocketEvent *se;
	unsigned long flags;

	XplLockAcquire( &( cs->slock ) );
	if( cs->listener ) {
		/* this is an async listener */
		se = &( cs->event.connect );
		if( se ) {
			flags = SeGetFlags( se );
			if( !( flags & ( FSE_LISTENER_KILLED | FSE_CANCELED ) ) ) {
				/* the listener has not been killed */
				SeAddFlags( se, FSE_LISTENER_SUSPENDED );
				if( cs->listener->eventCallback ) {
					cs->listener->eventCallback( LISTENER_SUSPENDED, cs->listener->throttlingThreshold, AThreadCountUpdate(), se, cs, cs->number );
				}
				ret = TRUE;
			}
		}
	}
	XplLockRelease( &( cs->slock ) );

	return ret;
}

static XplBool socketListenerResume( SOCK *cs )
{
	XplBool ret = FALSE;
	SocketEvent *se;
	unsigned long flags;

	if( cs->listener ) {
		/* this is an async listener */
		se = &( cs->event.connect );
		if( se ) {
			flags = SeGetFlags( se );
			if( flags & ( FSE_LISTENER_SUSPENDED ) ) {
				SeRemoveFlags( se, FSE_LISTENER_SUSPENDED );
				if( cs->listener->eventCallback ) {
					cs->listener->eventCallback( LISTENER_RESUMED, cs->listener->throttlingThreshold, AThreadCountUpdate(), se, cs, cs->number );
				}
				ret = TRUE;
			}
		}
	}

	return ret;
}

XplBool SocketListenerResume( SOCK *cs )
{
	XplBool ret = FALSE;

	XplLockAcquire( &( cs->slock ) );

	ret = socketListenerResume( cs );

	XplLockRelease( &( cs->slock ) );

	return ret;
}


// PUBLIC ENTRY
void SocketEventKill( SOCK *cs, SocketEventType type )
{
	XplBool eventComplete;
	SocketEvent *se;

	switch( type ) {
	case SOCK_EVENT_TYPE_READABLE:
		se = &( cs->event.receive );
		break;
	case SOCK_EVENT_TYPE_WRITABLE:
		se = &( cs->event.send );
		break;
	case SOCK_EVENT_TYPE_LISTEN:
	case SOCK_EVENT_TYPE_CONNECT:
	case SOCK_EVENT_TYPE_DISCONNECT:
		se = &( cs->event.connect );
		break;
	default:
	case SOCK_EVENT_TYPE_NONE:
		DebugAssert( 0 );
		se = NULL;
		break;
	}

	if( se ) {

		XplLockAcquire( &( se->cs->slock ) );

		/* Remove the FSE_LISTENER_SUSPENDED flag if found */
		socketListenerResume( cs );

		/* Remove socket from the engine if active */
		eventComplete = sockEventKill( se );

		XplLockRelease( &( se->cs->slock ) );

		if( eventComplete ) {
			AThreadBegin( &( se->cb.thread ) );
		}
	}
	return;
}

static void
AECShutdown( AcceptEventCache *cache )
{
	ListMember *ae;

	XplLockAcquire( &cache->safeList.lock );
	for( ; ; ) {
		if( ( ae = cache->safeList.head ) )
		{
			if( !( cache->safeList.head = ae->next ) )
			{
				cache->safeList.tail = &cache->safeList.head;
				MemFree( ae );
				break;
			}
			MemFree( ae );
			continue;
		}
		break;
	}
	XplLockRelease( &cache->safeList.lock );
}

static void
AECInit( AcceptEventCache *cache )
{
	memset( cache, 0, sizeof( AcceptEventCache ) );
	XplLockInit( &cache->safeList.lock );
	cache->safeList.head = NULL;
	cache->safeList.tail = &cache->safeList.head;
	cache->next = ACCEPT_EVENT_FAST_CACHE_SIZE;
}

// PUBLIC ENTRY
SocketEventEngine *SocketEventEngineAlloc( void )
{
	SocketEventEngine	*engine;

	if( ( engine = (SocketEventEngine *)MemMalloc( sizeof( SocketEventEngine ) ) ) )
	{
		memset( engine, 0, sizeof( SocketEventEngine ) );
		engine->state = ENGINE_RUNNING;
		engine->threadGroup = XplThreadGroupCreate( "EventEngine" );
		if( engine->threadGroup ) {
			// XplThreadGroupStackSize( engine->threadGroup, <what is the right size here? something small> );
			AECInit( &engine->aeCache );

			if( !sockEngineInit( engine ) ) {
				return engine;
			}
			XplThreadGroupDestroy( &( engine->threadGroup ) );
		}
		MemFree( engine );
	}
	return NULL;
}

// PUBLIC ENTRY
int SocketEventEngineFree( SocketEventEngine *engine )
{
	engine->state = ENGINE_SHUTDOWN;
	sockEngineShutdown( engine );
	AECShutdown( &engine->aeCache );
	XplThreadGroupDestroy( &( engine->threadGroup ) );
	MemFree( engine );
	return( 0 );
}

static XplBool socketLibInit( void )
{
	/* This happens only once when SocketLibStart is called the first time */
	memset( &SockLib, 0, sizeof( SockLibGlobals ) );
	XplLockInit( &SockLib.lock );
	XplIpInit();
	sockLibInit();
	return TRUE;
}

XplBool SocketLibStart( uint32 SockLibFlags )
{
	XplBool ret = TRUE;

	if( SockLibInitialized == LIB_UNINITIALIZED ) {
		ret = socketLibInit();
		SockLibInitialized = LIB_INITIALIZED;
	}
	if( ret ) {
		XplLockAcquire( &SockLib.lock );
		SockLib.useCount++;
		/* create resources needed by current start that have not already been created */
		if( 1 == SockLib.useCount ) {
			sockCreatorStart();
		}
		if( SockLibFlags & SOCK_LIB_FLAG_SSL ) {
			if( !SockLib.ssl.enabled ) {
				socketSSLInit();
				SockLib.ssl.enabled = TRUE;
			}
		}

		if( SockLibFlags & SOCK_LIB_FLAG_ASYNC ) {
			if( !SockLib.eventEngine ) {
				SockLib.eventEngine = SocketEventEngineAlloc();
				if( !SockLib.eventEngine ) {
					ret = FALSE;
				}
			}
		}

		XplLockRelease( &SockLib.lock );
	}

	return ret;
}

void SocketLibStop( void )
{
	XplLockAcquire( &SockLib.lock );
	SockLib.useCount--;
	if( 0 == SockLib.useCount ) {
		/* clean up all resources; threads, engines, ssl contexts etc */
		sockCreatorStop();
		if( SockLib.eventEngine ) {
			SocketEventEngineFree( SockLib.eventEngine );
			SockLib.eventEngine = NULL;
		}
		/* Stop DNS resolver and free XplInterfaceList */
		XplIpCleanup();
	}
	XplLockRelease( &SockLib.lock );
}
