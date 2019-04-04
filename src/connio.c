#if !defined(_MSC_VER)
#include <openssl/ssl.h>
#endif
#include <xplip.h>
#include <xplmem.h>
#include<xplproc.h>
#include <xplfile.h>
#include <stdio.h>

#include "conniop.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

ConnIOGlobals ConnIO;

ConnectionPriv *ConnListPush( ConnList *list, ConnectionPriv *conn );

typedef enum {
	WITH_DATA,
	EMPTY_OK
} SendBufferOptions;

static void bufferedSendDataFree( ConnectionPriv *conn )
{
	if( conn->sendBuffer ) {
		IOBFree( conn->sendBuffer );
		conn->sendBuffer = NULL;
	}
}

static void bufferedSendDataKeep( ConnectionPriv *conn, IOBuffer *sendBuffer )
{
	DebugAssert( conn->sendBuffer == NULL );
	DebugAssert( sendBuffer );

	if( IOBGetUsed( sendBuffer, NULL ) > 0 ) {
		conn->sendBuffer = sendBuffer;
	} else {
		IOBFree( sendBuffer );
	}
}

static IOBuffer *bufferedSendDataGet( ConnectionPriv *conn, SendBufferOptions options )
{
	IOBuffer *iob;

	if( conn->sendBuffer ) {
		iob = conn->sendBuffer;
		conn->sendBuffer = NULL;
		if( IOBGetUsed( iob, NULL ) || ( options == EMPTY_OK ) ) {
			return iob;
		}
		IOBFree( iob );
		return NULL;
	}

	if( options == EMPTY_OK ) {
		return IOBAlloc( CONN_IOB_SIZE );
	}
	return NULL;
}

INLINE static XplBool connIsIdle( ConnectionPriv *conn )
{
	if( ( conn->activeEvent.connect == NULL ) && ( conn->activeEvent.receive == NULL ) && ( conn->activeEvent.send == NULL ) ) {
		return TRUE;
	}
	return FALSE;
}

#ifdef DEBUG
#define ConnIsIdle( c ) connIsIdle( c )
#else
#define ConnIsIdle( c ) TRUE
#endif

/* ConnFreeCB()

   ConnFreeCB() releases any resources referenced by a conn
   before the conn is destroyed.
*/
static int
ConnFreeCB(void *buffer, void *data)
{
	int success;
	ConnHandlePriv *h = (ConnHandlePriv *)data;
	ConnectionPriv *c = (ConnectionPriv *)buffer;

	if (h->connCache.cb.free) {
		success = h->connCache.cb.free(c, h->connCache.cb.data);
	} else {
		success = 1;
	}

	LogConnDisposition( c, "Destroyed", __FILE__, __LINE__ );

	EventLockAcquire( c );
	ConnChangeState( c, CONN_STATE_FREE );
	EventLockRelease( c );
	bufferedSendDataFree( c );
	IOBFree( c->receiveBuffer );
	c->receiveBuffer = NULL;

	XplMutexDestroy( c->mutex );

	return( success );
}

/* ConnectionInit()

   ConnectionInit() exists so that a conn can be initialized exactly the same way when it is first created and
   when it is returned to the cache.

   Anything that needs allocated or that is expensive to initialize is set using arguments.
   Everything else is wiped and any non-zero values are set.
*/
static void
ConnectionInit( ConnectionPriv *c )
{
	/* clear everything up to the mutex */
	memset(c, 0, offsetof( ConnectionPriv, mutex ) );

	memcpy(&(c->timeout.receive), &(c->connHandle->timeOut.receive), sizeof(SocketTimeout));
	memcpy(&(c->timeout.send), &(c->connHandle->timeOut.send), sizeof(SocketTimeout));
	memcpy(&(c->timeout.connect),&(c->connHandle->timeOut.connect), sizeof(SocketTimeout));
	memcpy(&(c->timeout.disconnect),&(c->connHandle->timeOut.disconnect), sizeof(SocketTimeout));

	bufferedSendDataFree( c );
	IOBInit( c->receiveBuffer );

	c->marked = 0;

	EventLockAcquire( c );
	ConnChangeState( c, CONN_STATE_CACHED );
	EventLockRelease( c );
}

/* ConnAllocCB()

   ConnAllocCB() is used to initialize a conn when it is first created.
*/
static int
ConnAllocCB(void *buffer, void *data)
{
	ConnectionPriv *c = (ConnectionPriv *)buffer;
	ConnHandlePriv *h = (ConnHandlePriv *)data;

    /* thing that only need initialized once event if connection is reused */

	memset( c, 0, sizeof( ConnectionPriv ) );

	XplMutexInit( c->mutex );

	c->connHandle = h;

	c->receiveBuffer = IOBAlloc( CONN_IOB_SIZE );

	if( c->receiveBuffer )
	{
		ConnectionInit( c );

		LogConnDisposition( c, "Created", __FILE__, __LINE__ );
		/* give the consumer a shot at the conn */
		if( !( h->connCache.cb.alloc ) || h->connCache.cb.alloc(c, h->connCache.cb.data) ) {
			return( 1 );
		}
	}
	if( c->receiveBuffer )
	{
		IOBFree( c->receiveBuffer );
		c->receiveBuffer = NULL;
	}

	XplMutexDestroy( c->mutex );
	return( 0 );
}


INLINE static XplBool
notShuttingHandleDown( ConnHandlePriv *handle )
{
	if ( !handle->shuttingDown ) {
		return( TRUE );
	}

	errno = ECONNHANDLESHUTDOWN;
	return( FALSE );
}


INLINE static XplBool
notShuttingDown( ConnectionPriv *conn )
{
	return notShuttingHandleDown( conn->connHandle );
}

/* asyncEnabledHandle()

   asyncEnabledHandle() is used by the public ConnA functions to check that the conn
   event engine has been enabled.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static XplBool
asyncEnabledHandle( ConnHandlePriv *handle )
{
	if ( handle->flags.fixed & CONN_HANDLE_ASYNC_ENABLED ) {
		return( TRUE );
	}
	errno = ECONNIOINIT;
	return( FALSE );
}

/* asyncEnabled()

   asyncEnabled() is used by the public ConnA functions to check that the conn
   event engine has been enabled.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static XplBool
asyncEnabled( ConnectionPriv *conn )
{
	return( asyncEnabledHandle( conn->connHandle ) );
}

/* allocWorked()

   allocWorked() is used by the public Conn and ConnA functions to check that an
   allocated pointer is non-null.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static void *
allocWorked( void *ptr )
{
	if ( ptr ) {
		return( ptr );
	}
	errno = ENOMEM;
	return( NULL );
}

/* isAsyncListener()

   isAsyncListener() is used by the public Conn and ConnA functions to check that an
   argument is non-null.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static int
isAsyncListener( ConnectionPriv *conn )
{
	if ( conn->state == CONN_STATE_ASYNCHRONOUS_LISTENER ) {
		return( 1 );
	}
	errno = EINVAL;
	return( 0 ) ;
}


/* notNULL()

   notNULL() is used by the public Conn and ConnA functions to check that an
   argument is non-null.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static int
notNull( void *ptr )
{
	if ( ptr ) {
		return( 1 );
	}
	errno = EINVAL;
	return( 0 ) ;
}

/* newListenersAllowed()

   newListenersAllowed() is used by the public Conn and ConnA functions to check that a
   new listener can be started.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static int
newListenersAllowed( ConnHandle *pubHandle )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;

	if( !handle->listener.preventNew ) {
		return( 1 );
	}
	errno = ESHUTDOWN;
	return( 0 ) ;
}

/* isPos()

   isPos() is used by the public Conn and ConnA functions to check that an
   argument is a positive number.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static int
isPos( int num )
{
	if ( num > 0 ) {
		return( 1 );
	}
	errno = EINVAL;
	return( 0 ) ;
}

/* notNegative()

   notNegative() is used by the public Conn and ConnA functions to check that an
   argument is not zero.  This is done in its own function so errno can
   be set in the failure case.
*/
INLINE static int
notNegative(int val)
{
	if ( val >= 0 ) {
		return( 1 );
	}
	errno = EINVAL;
	return( 0 ) ;
}

#ifdef DEBUG
static int
_validFileAndLine(const char *file, const int line)
{
	if ( NULL != file && '\0' != *file && 0 < line) {
		return( 1 );
	}
	errno = EINVAL;
	return( 0 ) ;
}
#define validFileAndLine(_file,_line) _validFileAndLine(_file,_line)
#else
#define validFileAndLine(_file,_line) (1)
#endif
/* validConnIOArgs()

   validConnIOArgs() is used by the public Conn and ConnA read/write functions to validate
   arguments.  This is done in its own function so errno can be set in the failure case.

   zero is returned in the case where the arguments are valid (errno is not set) but there is
   nothing to do because the datalength is 0
*/

static int
validConnIOArgs(ConnectionPriv *conn, void *dest, int length )
{
	if (conn) {
		if (length) {
			if (dest) {
				return( 1);
			}
			errno = EINVAL;
			return( -1 );
		}
		return( 0 );
	}
	errno = EINVAL;
	return( -1 );
}

/* validConnIOArgs2()

   validConnIOArgs() is used by the public Conn and ConnA read/write functions to validate
   arguments.  This is done in its own function so errno can be set in the failure case.

   THE DIFFERENCE BETWEEN THIS AND THE non-2 VERSION IS ONLY THAT THE length
   PARAMETER IS ALLOWED TO BE ZERO. AND THE NEGATIVE length SETS errno

*/
static int
validConnIOArgs2(ConnectionPriv *conn, void *dest, int length )
{
	if (conn) {
		if (length >= 0) {
			if (dest) {
				return( 1);
			}
			errno = EINVAL;
			return( -1 );
		}
	}
	errno = EINVAL;
	return( -1 );
}

static void
ConnEventSetTimeout( ConnEvent *connEvent, int timeout, SocketTimeout *connTimeOut )
{
	if( timeout > 0 ) {
		SocketTimeoutSet( &( connEvent->timeOut ), timeout );
	} else {
		switch( timeout ) {
		case CONN_TIMEOUT_IMMEDIATE:
			SocketTimeoutSet( &( connEvent->timeOut ), 0 );
			break;
		case CONN_TIMEOUT_DEFAULT:
			memcpy( &( connEvent->timeOut ), connTimeOut, sizeof( SocketTimeout ) );
			break;
		default:
			DebugAssert( 0 ); // Invalid timeout
			memcpy( &( connEvent->timeOut ), connTimeOut, sizeof( SocketTimeout ) );
			break;
		}
	}
}

static XplBool
connKillEx( ConnectionPriv *conn, char *file, int line )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	switch( conn->state ) {
		case CONN_STATE_CACHED:
		case CONN_STATE_FREE:
		{
			EventLockRelease( conn );   /***** UNLOCK *****/

			DebugPrintErr( "CONNIO(%s:%d): ConnKill() called with a conn %p that has already been freed!!!\n", __FILE__, __LINE__, conn );
			return FALSE;
		}
		case CONN_STATE_IDLE:
		case CONN_STATE_SYNCHRONOUS:
		{
			if( conn->sp->number != -1 ) {
				SocketClose( conn->sp );
			}

			EventLockRelease( conn );   /***** UNLOCK *****/

			return TRUE;
		}
		case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
		case CONN_STATE_ASYNCHRONOUS_ENGINE:
		case CONN_STATE_ASYNCHRONOUS_CLIENT:
		{
			EventQueueAbort( conn, NULL, CONN_EVENT_KILLED, ECANCELED, file, line );

			EventLockRelease( conn );   /***** UNLOCK *****/

			return TRUE;
		}
		case CONN_STATE_SYNCHRONOUS_LISTENER:
		case CONN_STATE_ASYNCHRONOUS_LISTENER:
		{
			EventLockRelease( conn );   /***** UNLOCK *****/
			/* the connio consumer must shutdown listeners explicitly with the Listener APIs */
			return FALSE;
		}
	}
	DebugPrintErr( "CONNIO(%s:%d): Unhandled case in ConnKill()\n", __FILE__, __LINE__ );
	DebugAssert( 0 );

	EventLockRelease( conn );   /***** UNLOCK *****/

	return FALSE;
}


/*! \fn		 static int CloseHandleConnections(ConnHandlePriv *handle)
	\brief	  A brief explanation.

				A detailed explanation.
	\param[in]  buffer
	\param[in]  data
	\return	 On success ...
	\retval	 <count>
	\todo	   Complete the CloseHandleConnections interface.
	\todo	   Complete the documentation for the CloseHandleConnections interface.
 */
static int
CloseHandleConnections(ConnHandlePriv *handle)
{
	ConnectionPriv *conn;

	errno = 0;

	XplMutexLock(handle->mutex);

	conn = handle->inUse.head;
	while( conn ) {
		connKillEx( conn, __FILE__, __LINE__ );
		conn = conn->list.next;
	}

	XplMutexUnlock(handle->mutex);

	return(0);
}

#ifdef DEBUG
static XplBool
PrintTimeF( FILE *fp, time_t utcTime )
{
    struct tm *ltime;
	char tempBuf[80];

	tzset();
	ltime = localtime( &utcTime );
	strftime( tempBuf, 80, "%a, %d %b %Y %H:%M:%S", ltime );
	if( fprintf( fp, "%s", tempBuf ) > -1 ) {
        return( TRUE );
    }
    return( FALSE );
}


static int
connLeakReport( ConnHandlePriv *handle )
{
	char path[ STACK_BUFFER_SIZE ];
	FILE *reportFile;
	ConnectionPriv *conn;
	int sequence;
	const char *file;
	unsigned long line;

	strprintf( path, sizeof( path ), NULL, "ConnLeakReport_%s.txt", handle->identity );
	reportFile = fopen( path, "ab" );
	if( reportFile ) {

		fprintf( reportFile, "Connection Leak Report for \"%s\" Process\n started: ", handle->identity );
		PrintTimeF( reportFile, handle->startTime );
		fprintf( reportFile, "\n ended:   " );
		PrintTimeF( reportFile, time( NULL ) );

		XplMutexLock(handle->mutex);
		fprintf( reportFile, "\n\n %d conns outstanding\n\n", handle->inUse.count );
		conn = handle->inUse.head;
		sequence = 1;
		while( conn ) {
			MemGetOwner( conn, &file, &line );
			fprintf( reportFile, "%d: %s:%lu\n\n", sequence, file, line );
			fprintf( reportFile, "Usage Log: **********START************\n" );
			SocketTraceDumpToFile( conn->sp, reportFile );
			fprintf( reportFile, "           ***********END*************\n\n" );
			conn = conn->list.next;
			sequence++;
		}
		XplMutexUnlock(handle->mutex);
		fprintf( reportFile, "\n\n" );
		fclose( reportFile );
		DebugPrintErr( "Connio: connection leak report created: %s\n", path );
		return( 0 );
	}
	return( -1 );
}
#define ConnLeakReport( h )		connLeakReport( h )

#else

#define ConnLeakReport( h )

#endif

EXPORT uint32 ConnMarkEx( Connection *c, uint32 mark, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)c;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnMark", file, line );
		conn->marked |= mark;
		return conn->marked;
	}
	return -1;
}

EXPORT uint32 ConnUnmarkEx( Connection *c, uint32 mark, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)c;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnUnmark", file, line );
		conn->marked &= ~mark;
		return conn->marked;
	}
	return -1;
}

static XplBool isChildOfListener( ConnectionPriv *conn, ConnListenerListPriv *cllist )
{
	WJElement last;
	int lID;

	if( conn->sp->listenerID ) {
		for (last = NULL; lID = _WJENumber(cllist->list, "[]", WJE_GET, &last, 0); ) {
			if( conn->sp->listenerID == lID ) {
				return TRUE;
			}
		}
	}
	return FALSE;
}

XplBool connChild( ConnListenerListPriv *cllist, ConnectionPriv *conn )
{
	int lID;
	XplBool ret;
	WJElement last;

	ret = FALSE;

	switch( conn->state ) {
	case CONN_STATE_IDLE:
	case CONN_STATE_SYNCHRONOUS:
	case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
	case CONN_STATE_ASYNCHRONOUS_ENGINE:
	case CONN_STATE_ASYNCHRONOUS_CLIENT:
		if( conn->sp->listenerID ) {
			/* this connection is the child of a listener */
			for (last = NULL; lID = _WJENumber(cllist->list, "[]", WJE_GET, &last, 0); ) {
				if( conn->sp->listenerID == lID ) {
					ret = TRUE;
					break;
				}
			}
		}
		break;
	case CONN_STATE_SYNCHRONOUS_LISTENER:
	case CONN_STATE_ASYNCHRONOUS_LISTENER:
		/* this is a listener, not a child of a listener */
		break;
	case CONN_STATE_CACHED:
	case CONN_STATE_FREE:
		DebugAssert( 0 );  // A connection that has been freed should not be in this list (rodney)
		break;
	}
	return ret;
}

static int translateShutdownTimeout( int timeout )
{
	if( timeout > 0 ) {
		if( timeout > 1000 ) {
			return timeout / 1000;
		}
		return 1;
	}
	switch( timeout ) {
	case CONN_TIMEOUT_IMMEDIATE:
		return 0;
	case CONN_TIMEOUT_INFINITE:
		return -1;
	case CONN_TIMEOUT_DEFAULT:
		return 10;
	default:
		DebugAssert( 0 ); // invalid timeout
		return 10;
	}
}

static void synchronousListenerClose( ConnectionPriv *conn )
{
	if( conn->sp->number != SOCKET_INVALID ) {
		SocketClose( conn->sp );
	}
	EventLockAcquire( conn );
	ConnChangeState( conn, CONN_STATE_IDLE );
	EventLockRelease( conn );
}

static void asynchronousListenerShutdown( ConnectionPriv *conn )
{
	if( conn->activeEvent.connect && ( conn->state == CONN_STATE_ASYNCHRONOUS_LISTENER ) ) {
		LogConnEventKilled( conn, conn->activeEvent.connect );
		SocketEventKill( conn->sp, SOCK_EVENT_TYPE_CONNECT );
	}
}

static XplBool
ConnHandleWaitForChildrenToExit( ConnListenerListPriv *cllist, int timeout, int *waited, int *count )
{
	ConnectionPriv *conn;
	int i = 0;
	int connectionWaitCount;

	timeout = translateShutdownTimeout( timeout );
	for(;;) {
		XplMutexLock(cllist->handle->mutex);
		conn = cllist->handle->inUse.head;
		connectionWaitCount = 0;
		while( conn ) {
			if( connChild( cllist, conn ) ) {
				connectionWaitCount++;
			}
			conn = conn->list.next;
		}
		XplMutexUnlock(cllist->handle->mutex);

		if( connectionWaitCount && ( ( timeout < 0 ) || ( i < timeout ) ) ) {
			i++;
			if( ( timeout < 0 ) && ( ( i % 20 ) == 0 ) ) {
				DebugUnreleasedConns( cllist->handle, cllist, i );
			}
			XplDelay( 1000 );
			continue;
		}

		if( waited ) {
			*waited = i;
		}

		if( !connectionWaitCount ) {
			/* awesome! all the children are gone */
			return TRUE;
		}

		/* the timeout has expired and there are still children */
		if( count ) {
			*count = connectionWaitCount;
		}
		break;
	}
	return( FALSE );

}

EXPORT XplBool
ConnHandleWaitForConnectionsToExit( ConnHandle *pubHandle, int timeout, int *waited )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	int i = 0;

	errno = 0;

	if( notNull( handle ) ) {
		timeout = translateShutdownTimeout( timeout );
		for(;;) {
			XplMutexLock(handle->mutex);

			if( !( handle->inUse.head ) && (0 == XplSafeRead( handle->inCleanup ) ) ) {
				XplMutexUnlock(handle->mutex);
				if( waited ) {
					*waited = i;
				}
				return( TRUE );
			}
			XplMutexUnlock(handle->mutex);
			if( ( timeout > -1 ) && ( i >= timeout ) ) {
				break;
			}
			i++;
			if( ( timeout < 0 ) && ( ( i % 20 ) == 0 ) ) {
				DebugUnreleasedConns( handle, NULL, i );
			}
			XplDelay( 1000 );
		}
	}
	return( FALSE );
}
EXPORT void
ConnHandleConnectionsAbortEx( ConnHandle *pubHandle, ConnListenerList *clList, const char *file, const int line )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerListPriv *cllist = (ConnListenerListPriv *)clList;
	ConnectionPriv *conn;

	errno = 0;

	if( notNull( handle ) ) {
		XplMutexLock(handle->mutex);
		conn = handle->inUse.head;
		while ( conn ) {
			EventLockAcquire( conn );   /***** LOCK *****/
			switch( conn->state ) {
			case CONN_STATE_CACHED:
			case CONN_STATE_FREE:
				{
					DebugPrintErr("CONNIO(%s:%d): %s: freed conns found in the 'in use' list!!!!\n", __FILE__, __LINE__, handle->identity );
					break;
				}
			case CONN_STATE_IDLE:
			case CONN_STATE_SYNCHRONOUS:
				{
					if( cllist && !isChildOfListener( conn, cllist ) ) {
						break;
					}

					if( conn->sp->number != -1 ) {
						SocketDisconnect( conn->sp, &( handle->timeOut.shutdown ) );
						SocketClose( conn->sp );
						DebugPrintErr( "[%p] closed when idle\n", conn->sp );
					}
					break;
				}
			case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
				{
					/* this event is done, let the completion callback happen */
					DebugPrintErr( "[%p] left alone when done\n", conn );
					break;
				}
			case CONN_STATE_ASYNCHRONOUS_ENGINE:
			case CONN_STATE_ASYNCHRONOUS_CLIENT:
				{
					if( cllist && !isChildOfListener( conn, cllist ) ) {
						break;
					}

					EventQueueAbort( conn, NULL, CONN_EVENT_KILLED, ESHUTDOWN, file, line );
					DebugPrintErr( "[%p] aborted in engine\n", conn );

					break;
				}
			case CONN_STATE_SYNCHRONOUS_LISTENER:
				/*
				  The connio consumer is responsible for shutting down listeners explicitly with the Listener APIs.
				  This conn should be gone before this code ever runs, but at least we can stop the listener socket.
				*/
				synchronousListenerClose( conn);
				break;
			case CONN_STATE_ASYNCHRONOUS_LISTENER:
				{
					/*
					  The connio consumer is responsible for shutting down listeners explicitly with the Listener APIs.
					  This conn should be gone before this code ever runs, but at least we can remove the listener socket from the async engine.
					*/
					asynchronousListenerShutdown( conn );
					break;
				}
			}
			EventLockRelease( conn );   /***** UNLOCK *****/
			conn = conn->list.next;
		}

		XplMutexUnlock(handle->mutex);
	}
}

EXPORT void
ConnHandleConnectionsCloseEx( ConnHandle *pubHandle, ConnListenerList *clList, const char *file, const int line )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerListPriv *cllist = (ConnListenerListPriv *)clList;
	ConnectionPriv *conn;

	errno = 0;

	if( notNull( handle ) ) {
		XplMutexLock(handle->mutex);
		conn = handle->inUse.head;
		while ( conn ) {
			if( !conn->shutdownCallback || !conn->shutdownCallback( ( Connection *)conn ) ) {
				EventLockAcquire( conn );   /***** LOCK *****/
				switch( conn->state ) {
				case CONN_STATE_CACHED:
				case CONN_STATE_FREE:
					{
						DebugPrintErr("CONNIO(%s:%d): %s: freed conns found in the 'in use' list!!!!\n", __FILE__, __LINE__, handle->identity );
						break;
					}
				case CONN_STATE_IDLE:
				case CONN_STATE_SYNCHRONOUS:
				case CONN_STATE_ASYNCHRONOUS_ENGINE:
				case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
				case CONN_STATE_ASYNCHRONOUS_CLIENT:
					{
						if( cllist && !isChildOfListener( conn, cllist ) ) {
							break;
						}
						if( -1 != conn->sp->number )
							{
								SocketDisconnect( conn->sp, &( handle->timeOut.shutdown ) );
								SocketClose( conn->sp );
							}
						break;
					}
				case CONN_STATE_SYNCHRONOUS_LISTENER:
					/*
					  The connio consumer is responsible for shutting down listeners explicitly with the Listener APIs.
					  This conn should be gone before this code ever runs, but at least we can stop the listener socket.
					*/
					synchronousListenerClose( conn);
					break;
				case CONN_STATE_ASYNCHRONOUS_LISTENER:
					{
						/*
						  The connio consumer is responsible for shutting down listeners explicitly with the Listener APIs.
						  This conn should be gone before this code ever runs, but at least we can remove the listener socket from the async engine.
						*/
						asynchronousListenerShutdown( conn );
						break;
					}
				}
				EventLockRelease( conn );   /***** UNLOCK *****/
			}
			conn = conn->list.next;
		}

		XplMutexUnlock(handle->mutex);
	}
}

static WJElement ConnGetConfig(void)
{
	FILE		*f;
	WJElement	config	= NULL;

	XplMutexLock(ConnIO.mutex);

	if (ConnIO.config) {
		config = WJECopyDocument(NULL, ConnIO.config, NULL, NULL);

		XplMutexUnlock(ConnIO.mutex);
		return(config);
	}


	if ((f = fopen(SYSCONF_DIR "/connio.json", "rb"))) {
		if ((ConnIO.config = WJEReadFILE(f))) {
			config = WJECopyDocument(NULL, ConnIO.config, NULL, NULL);
		}
		fclose(f);
	}

	XplMutexUnlock(ConnIO.mutex);

	if (!config) {
		config = WJEObject(NULL, NULL, WJE_SET);
	}

	return(config);
}

static void ConnReleaseConfig(void)
{
	XplMutexLock(ConnIO.mutex);
	if (ConnIO.config) {
		WJECloseDocument(ConnIO.config);
		ConnIO.config = NULL;
	}
	XplMutexUnlock(ConnIO.mutex);
}

EXPORT int
ConnHandleShutdownEx( ConnHandle *pubHandle, int errorTimeout, int closeTimeout, const char *file, const int line )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	int waited;

	errno = 0;

	if( notNull( handle ) ) {
		ConnAListenerWaitForShutdownEx( pubHandle, file, line );

		if( ConnHandleWaitForConnectionsToExit( pubHandle, errorTimeout, &waited ) ) {
			handle->shuttingDown = TRUE;
			return 0;
		}
		DebugPrintErr( "ConnHandleShutdown(): %s has %d conn%s outstanding after %d seconds.\n",
					   handle->identity, handle->inUse.count, (handle->inUse.count > 1) ? "s" : "", translateShutdownTimeout( errorTimeout ) );
		/* prevent new events */
		handle->shuttingDown = TRUE;

		ConnHandleConnectionsAbortEx( pubHandle, NULL, file, line );

		if( ConnHandleWaitForConnectionsToExit( pubHandle, closeTimeout, &waited ) ) {
			DebugPrintErr( "ConnHandleShutdown(): SUCCESS! %s released all its conns in %d seconds\n", handle->identity, waited );
			return 0;
		}
		DebugPrintErr( "ConnHandleShutdown(): %s has not released %d conn%s in the %d seconds after new events were prohibited, queued events were canceled, and active events were killed!!\n",
					   handle->identity, handle->inUse.count, (handle->inUse.count > 1) ? "s" : "", translateShutdownTimeout( closeTimeout ) );

		ConnLeakReport( handle );
		ConnHandleConnectionsCloseEx( pubHandle, NULL, file, line );

		ConnHandleWaitForConnectionsToExit( pubHandle, CONN_TIMEOUT_INFINITE, &waited );
		DebugPrintErr( "ConnHandleShutdown(): SUCCESS! %s released all its conns in %d seconds\n", handle->identity, waited );

		ConnReleaseConfig();
		return 0;
	}
	return -1;
}

EXPORT int ConnChildrenShutdownEx( ConnListenerList *clList, int errorTimeout, int closeTimeout, const char *file, const int line )
{
	ConnListenerListPriv *cllist = (ConnListenerListPriv *)clList;
	int waited;
	int count;

	errno = 0;
	if( ConnHandleWaitForChildrenToExit( cllist, errorTimeout, &waited, &count ) ) {
		return( 0 );
	}
	DebugPrintErr( "ConnChildrenShutdown(): %s has %d conn%s outstanding after %d seconds.\n",
				 cllist->handle->identity, count, (count > 1) ? "s" : "", errorTimeout );

	ConnHandleConnectionsAbortEx( &cllist->handle->pub, clList, file, line );

	if( ConnHandleWaitForChildrenToExit( cllist, closeTimeout, &waited, &count ) ) {
		DebugPrintErr( "ConnChildrenShutdown(): SUCCESS! %s released all its conns in %d seconds\n", cllist->handle->identity, waited );
		return( 0 );
	}
	DebugPrintErr( "ConnChildrenShutdown(): %s has not released %d conn%s in the %d seconds after new events were prohibited, queued events were canceled, and active events were killed!!\n",
				 cllist->handle->identity, count, (count > 1) ? "s" : "", closeTimeout );

	ConnHandleConnectionsCloseEx( &cllist->handle->pub, clList, file, line );

	ConnHandleWaitForChildrenToExit( cllist, CONN_TIMEOUT_INFINITE, &waited, &count );
	DebugPrintErr( "ConnChildrenShutdown(): SUCCESS! %s released all its conns in %d seconds\n", cllist->handle->identity, waited );
	return( 0 );
}

EXPORT void
ConnHandleStats( ConnHandle *pubHandle, ConnStats *stats )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	MemStatistics connPoolStats;

	errno = 0;

	if( notNull( handle ) && notNull( stats ) ) {
		MemPrivatePoolStatistics( handle->connCache.cache, &connPoolStats );

		stats->thread.count 				= AThreadCountUpdate();
		stats->thread.peak.count			= AThreadPeakCount();
		stats->thread.peak.time				= AThreadPeakTime();

		stats->conns.min 					= connPoolStats.totalAlloc.min;
		stats->conns.max 					= connPoolStats.totalAlloc.max;

		XplMutexLock( handle->mutex );
		if( handle->inUse.count < connPoolStats.totalAlloc.count ) {
			stats->conns.total 			 	= connPoolStats.totalAlloc.count;
			stats->conns.idle				= connPoolStats.totalAlloc.count  - handle->inUse.count;
		} else {
			stats->conns.total 			 	= handle->inUse.count;
			stats->conns.idle				= 0;
		}
		stats->conns.peak					= handle->inUse.peak;
		XplMutexUnlock( handle->mutex );
	}
}

static void
ConnHandleRemember( ConnHandlePriv *handle )
{
	XplMutexLock(ConnIO.mutex);

	handle->previous = ConnIO.handles.tail;
	if (ConnIO.handles.tail) {
		ConnIO.handles.tail->next = handle;
	} else {
		ConnIO.handles.head = handle;
	}

	ConnIO.handles.tail = handle;
	ConnIO.handles.count++;

	if( !ConnIO.threadEngine.internal &&
		handle->threadEngine.internal &&
		( !handle->config || !handle->config->threadEngine.internal )
	  ) {
		ConnIO.threadEngine.internal = handle->threadEngine.internal;
	}

	if( !ConnIO.threadEngine.defaultConsumer &&
		handle->threadEngine.defaultConsumer &&
		( !handle->config || !handle->config->threadEngine.defaultConsumer )
	  ) {
		ConnIO.threadEngine.defaultConsumer = handle->threadEngine.defaultConsumer;
	}

	XplMutexUnlock(ConnIO.mutex);
}

static void
ConnHandleForget( ConnHandlePriv *handle )
{
	XplMutexLock(ConnIO.mutex);

	if (handle->previous != NULL) {
		handle->previous->next = handle->next;
	} else {
		ConnIO.handles.head = handle->next;
	}

	if (handle->next != NULL) {
		handle->next->previous = handle->previous;
	} else {
		ConnIO.handles.tail = handle->previous;
	}

	ConnIO.handles.count--;
	if( ConnIO.handles.count < 1 ) {
		if( ConnIO.threadEngine.internal ) {
			AThreadEngineDestroy( ConnIO.threadEngine.internal );
			ConnIO.threadEngine.internal = NULL;
		}
		if( ConnIO.threadEngine.defaultConsumer ) {
			AThreadEngineDestroy( ConnIO.threadEngine.defaultConsumer );
			ConnIO.threadEngine.defaultConsumer = NULL;
		}
	}

	XplMutexUnlock(ConnIO.mutex);
}


/*! \fn		 int ConnHandleFree(ConnHandle *pubHandle)
	\brief	  A brief explanation.

				A detailed explanation.
	\param[in]  handle
	\return	 On success ...
	\retval	 <count>
	\retval	 <EINVAL>
	\todo	   Complete the ConnHandleFree interface.
	\todo	   Complete the documentation for the ConnHandleFree interface.
 */
EXPORT int
ConnHandleFree(ConnHandle *pubHandle)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	int ccode;

	errno = 0;

	if( notNull( handle ) ) {
		ConnHandleForget( handle );

		ccode = CloseHandleConnections(handle);

		SocketLibStop();

		MemPrivatePoolFree( handle->connCache.cache );
		handle->connCache.cache = NULL;

		if (handle->pub.sslv23.client) {
			SSL_CTX_free(handle->pub.sslv23.client);
		}

		if (handle->pub.sslv23.server) {
			SSL_CTX_free(handle->pub.sslv23.server);
		}

		if (handle->pub.tls.client) {
			SSL_CTX_free(handle->pub.tls.client);
		}

		if (handle->pub.tls.server) {
			SSL_CTX_free(handle->pub.tls.server);
		}

		MemFree( handle->identity );
		MemFree( handle );
	} else {
		ccode = SOCKET_ERROR;
	}

	return(ccode);
}

EXPORT void ConnHandlePreventNewListeners( ConnHandle *pubHandle )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;

	errno = 0;

	if( notNull( handle ) ) {
		handle->listener.preventNew = TRUE;
	}
}

EXPORT uint32
ConnHandleFlagsModify( ConnHandle *pubHandle, uint32 addFlags, uint32 removeFlags )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	uint32 newFlags;

	errno = 0;
	if( notNull( pubHandle ) ) {
		handle->flags.dynamic |= addFlags;
		handle->flags.dynamic &= ~removeFlags;
		newFlags = handle->flags.dynamic;
	} else {
		newFlags = -1;
	}
	return newFlags;
}

EXPORT uint32
ConnHandleStaticFlags( ConnHandle *pubHandle )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	uint32 curFlags;

	errno = 0;

	if( notNull( handle ) ) {
		curFlags = handle->flags.fixed;
	} else {
		curFlags = -1;
	}

	return curFlags;
}

static XplBool
connCacheInit( ConnCache *connCache, ConnHandlePriv *handle, char *identity, uint32 flags, ConnHandleConfig *config )
{
	unsigned long minimum;
	unsigned long maximum;
	char *ptr;
	char name[256];
	size_t length;

	if (config) {
		if (config->connCache.minimum == 0 &&
			config->connCache.maximum == 0) {
			// defaults requested.
			minimum = CONN_DEFAULT_POOL_MINIMUM;
			maximum = CONN_DEFAULT_POOL_MAXIMUM;
		}
		else {
			// validate the sanity of config contents
			if (config->connCache.minimum < 1 ||
				config->connCache.maximum < config->connCache.minimum) {
				errno = EINVAL;
				return( FALSE );
			}
			minimum = config->connCache.minimum;
			maximum = config->connCache.maximum;
		}
		// All of these fields are optional and start out NULL.
		// (see memset inside ConnHandleAllocEx)
		connCache->cb.data = config->connCache.cb.data;
		connCache->cb.free = config->connCache.cb.free;
		connCache->cb.alloc = config->connCache.cb.alloc;
	} else {
		minimum = CONN_DEFAULT_POOL_MINIMUM;
		maximum = CONN_DEFAULT_POOL_MAXIMUM;
	}

	length = strlen(identity);
	if( length >= ( sizeof( name ) - 16 ) ) {
		errno = EMSGSIZE;
		return( FALSE );
	}

	memcpy( name, identity, length );
	ptr = name + length;
	*ptr++ = ' ';
	memcpy( ptr, " Connection", 12);

	connCache->cache = MMPoolAlloc( name,
									sizeof( ConnectionPriv ),
									minimum, 			/* minimum determines how many conns there are in a slab.  i.e. when the first conn is created, this many will be created */
									ConnAllocCB,
									NULL,
									ConnFreeCB,
									handle,
									__FILE__, __LINE__ );
	if( connCache->cache ) {
		return( TRUE );
	}
	return( FALSE );
}

static void
connTimeoutInit( ConnHandlePriv *handle, ConnHandleConfig *config )
{
	SocketTimeoutSet(&(handle->timeOut.accept),
		config && config->timeOut.connect ? config->timeOut.connect : CONN_DEFAULT_TIMEOUT_CONNECT);
	SocketTimeoutSet(&(handle->timeOut.connect),
		config && config->timeOut.connect ? config->timeOut.connect : CONN_DEFAULT_TIMEOUT_CONNECT);
	SocketTimeoutSet(&(handle->timeOut.disconnect),
		config && config->timeOut.disconnect ? config->timeOut.disconnect : CONN_DEFAULT_TIMEOUT_DISCONNECT);
	SocketTimeoutSet(&(handle->timeOut.receive),
		config && config->timeOut.receive ? config->timeOut.receive : CONN_DEFAULT_TIMEOUT_READ);
	SocketTimeoutSet(&(handle->timeOut.send),
		config && config->timeOut.send    ? config->timeOut.send	: CONN_DEFAULT_TIMEOUT_WRITE);
	SocketTimeoutSet(&(handle->timeOut.shutdown),
		config && config->timeOut.shutdown? config->timeOut.shutdown: CONN_DEFAULT_SHUTDOWN_TIMEOUT);
}

static char *
CopyIdentity( char *identity )
{
	unsigned long idLen;
	char *idStart;
	char *ptr;
	char *idCopy;

	if( identity ) {
		/* if identity is a path, just keep the file */
		ptr = strrchr( identity, '/' );
		if( !ptr ) {
			ptr = strrchr( identity, '\\' );
		}
		if( ptr ) {
			idStart = ptr + 1;
		} else {
			idStart = identity;
		}
		/* if identity is a c file, remove the .c */
		ptr = strrchr( idStart, '.' );
		if( ptr && !strcmp( ptr, ".c" ) ) {
			idLen = ptr - idStart;
		} else {
			idLen = strlen( idStart );
		}
		if( idLen ) {
			idCopy = MemMalloc( idLen + 1 );
			if( idCopy ) {
				memcpy( idCopy, idStart, idLen );
				idCopy[ idLen ] = '\0';
				return( idCopy );
			}
		}
	}
	return( NULL );
}

static SSL_CTX *
CreateClientContext(XplBool tls, char *identity)
{
	SSL_CTX *ret;
	STACK_OF(SSL_COMP)* comp_methods;
	WJElement	config	= ConnGetConfig();

	/* allocate a client ssl context */

	/*
		Use SSLv23 method, which will support all protocols but disable specific
		ones via options below.
	*/
	if ( ret = SSL_CTX_new( SSLv23_client_method() ) ) {
		SSL_CTX_set_mode(ret, SSL_MODE_AUTO_RETRY);
		SSL_CTX_set_options(ret, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
		SSL_CTX_set_options(ret, SSL_OP_NO_SSLv2);
		SSL_CTX_set_options(ret, SSL_OP_NO_SSLv3);
		// SSL_CTX_set_options(ret, SSL_OP_NO_TLSv1);

		SSL_CTX_set_cipher_list(ret, WJEString(config, "cipher-list", WJE_GET, RECOMMENDED_CIPHER_LIST));

		if ((WJEBool(config, "cipher-server-order", WJE_NEW, TRUE))) {
			SSL_CTX_set_options(ret, SSL_OP_CIPHER_SERVER_PREFERENCE);
		}
		SSL_CTX_set_ecdh_auto(ret, 1);

		comp_methods = SSL_COMP_get_compression_methods();
		if (comp_methods){
#ifdef SSL_OP_NO_COMPRESSION
			SSL_CTX_set_options(ret, SSL_OP_NO_COMPRESSION);
#else
			sk_SSL_COMP_zero(comp_methods);
#endif
		}
	}
	else {
		int sslError;
		while ( ( sslError = ERR_get_error ( ) ) ) {
			char errstr[120];
			ERR_error_string( sslError, errstr );
			DebugPrintErr( "CONNIO[%s:%d]: %s error %s (%d).\n", __FILE__, __LINE__, tls ? "TLS" : "SSL", errstr, sslError );
		}

		printf( "CONNIO(%s:%d): %s failed to start because Connection I/O management library could not initialize %s\r\n", __FILE__, __LINE__, identity, tls ? "TLS" : "SSL" );
		errno = EPERM;
	}

	if (config) {
		WJECloseDocument(config);
	}

	return ret;
}

static void
ConnListInit( ConnList *list )
{
	memset( list, 0, sizeof( ConnList ) );
	XplMutexInit( list->mutex );
}

static void
connListAppend( ConnList *list, ConnectionPriv *conn )
{
	list->count++;
	if( list->count > list->peak ) {
		list->peak = list->count;
	}
	conn->list.previous = list->tail;
	if (list->tail) {
		list->tail->list.next = conn;
	} else {
		list->head = conn;
	}
	list->tail = conn;
}

static ConnectionPriv *
connListRemoveMature( ConnList *list, ConnectionPriv *conn )
{
	ConnectionPriv *mature;

	if( list->count > list->limit ) {
		mature = list->head;
		list->head = mature->list.next;
		if( !list->head ) {
			list->tail = NULL;
		}
		mature->list.next = mature->list.previous = NULL;
	} else {
		mature = NULL;
	}
	return mature;
}

static void
ConnListAppend( ConnList *list, ConnectionPriv *conn )
{
	DebugAssert( !conn->list.next && !conn->list.previous ); // This appears to already belong to a list (rodney)
	conn->list.next = NULL;

	XplMutexLock( list->mutex );

	connListAppend( list, conn );

	XplMutexUnlock( list->mutex );

	return;
}

static void
ConnListRemove( ConnList *list, ConnectionPriv *conn )
{
	XplMutexLock( list->mutex );

	if ( conn->list.previous != NULL ) {
		conn->list.previous->list.next = conn->list.next;
	} else {
		list->head = conn->list.next;
	}

	if ( conn->list.next != NULL ) {
		conn->list.next->list.previous = conn->list.previous;
	} else {
		list->tail = conn->list.previous;
	}
	list->count--;
	XplMutexUnlock( list->mutex );

	conn->list.next = conn->list.previous = NULL;
}

ConnectionPriv *
ConnListPush( ConnList *list, ConnectionPriv *conn )
{
	ConnectionPriv *mature;

	conn->list.next = NULL;

	XplMutexLock( list->mutex );

	connListAppend( list, conn );
	mature = connListRemoveMature( list, conn );

	XplMutexUnlock( list->mutex );

	return mature;
}

static int internalThreadEngineSet( ConnHandlePriv *handle, ConnHandleConfig *config )
{
	int err;
	void *threadEngineContext;

	if( config && config->threadEngine.internal ) {
		handle->threadEngine.internal = config->threadEngine.internal;
		err = 0;
	} else if( ConnIO.threadEngine.internal ) {
		handle->threadEngine.internal = ConnIO.threadEngine.internal;
		err = 0;
	} else if( !( threadEngineContext = AThreadDefaultCreateContext( handle->identity, 0 ) ) ) {
		err = -1;
	} else if( !( handle->threadEngine.internal = AThreadEngineCreate( AThreadDefaultThreadInit, AThreadDefaultThreadCount, threadEngineContext, AThreadDefaultFreeContext ) ) ) {
		AThreadDefaultFreeContext( threadEngineContext );
		err = -1;
	} else {
		err = 0;
	}
	return err;
}

static void internalThreadEngineClear( ConnHandlePriv *handle, ConnHandleConfig *config )
{
	if( !handle->threadEngine.internal ) {
		/* nothing to clear */
		;
	} else if( config && config->threadEngine.internal &&
			   (  config->threadEngine.internal == handle->threadEngine.internal ) ) {
		/* we did not create this one, it belongs to the consumer */
		handle->threadEngine.internal = NULL;
	} else if( ConnIO.threadEngine.internal &&
			   ( ConnIO.threadEngine.internal == handle->threadEngine.internal ) ) {
		/* we did not create this one, it belongs to the Global */
		handle->threadEngine.internal = NULL;
	} else {
		AThreadEngineDestroy( handle->threadEngine.internal );
		handle->threadEngine.internal = NULL;
	}
}

static int defaultConsumerThreadEngineSet( ConnHandlePriv *handle, ConnHandleConfig *config )
{
	int err;
	void *threadEngineContext;

	if( config && config->threadEngine.defaultConsumer ) {
		handle->threadEngine.defaultConsumer = config->threadEngine.defaultConsumer;
		err = 0;
	} else if( ConnIO.threadEngine.defaultConsumer ) {
		handle->threadEngine.defaultConsumer = ConnIO.threadEngine.defaultConsumer;
		err = 0;
	} else if( !( threadEngineContext = AThreadDefaultCreateContext( handle->identity, 0 ) ) ) {
		err = -1;
	} else if( !( handle->threadEngine.defaultConsumer = AThreadEngineCreate( AThreadDefaultThreadInit, AThreadDefaultThreadCount, threadEngineContext, AThreadDefaultFreeContext ) ) ) {
		AThreadDefaultFreeContext( threadEngineContext );
		err = -1;
	} else {
		err = 0;
	}
	return err;
}

static void defaultConsumerThreadEngineClear( ConnHandlePriv *handle, ConnHandleConfig *config )
{
	if( !handle->threadEngine.defaultConsumer ) {
		/* nothing to clear */
		;
	} else if( config && config->threadEngine.defaultConsumer &&
			   (  config->threadEngine.defaultConsumer == handle->threadEngine.defaultConsumer ) ) {
		/* we did not create this one, it belongs to the consumer */
		handle->threadEngine.defaultConsumer = NULL;
	} else if( ConnIO.threadEngine.defaultConsumer &&
			   ( ConnIO.threadEngine.defaultConsumer == handle->threadEngine.defaultConsumer ) ) {
		/* we did not create this one, it belongs to the Global */
		handle->threadEngine.defaultConsumer = NULL;
	} else {
		AThreadEngineDestroy( handle->threadEngine.defaultConsumer );
		handle->threadEngine.defaultConsumer = NULL;
	}
}

EXPORT ConnHandle *
ConnHandleAllocEx(char *identity, uint32 flags, ConnHandleConfig *config, const char *file, const int line)
{
	ConnHandlePriv *handle;
	int count;

	errno = 0;

	// ===================================
	// Parameter Checks, in order of parameters, left to right
	// ===================================
	if( !identity ) {
		errno = EINVAL;
		return( NULL );
	}
	if (flags & ~(	CONN_HANDLE_SSL_CLIENT_ENABLED |
					CONN_HANDLE_SSL_SERVER_ENABLED |
					CONN_HANDLE_ASYNC_ENABLED)) {
		errno = EINVAL;
		return( NULL );
	}
	// Note that the config parameter is optional and tested before it is used in connCacheInit().
	if (!validFileAndLine(file,line)){
		return( NULL );
	}

	/* make sure ConnIO global is initialized */
	if (ConnIO.state == CONNIO_STATE_LOADED) {
		/* init ConnIO global */
		ConnIO.state = CONNIO_STATE_INITIALIZING;

		XplMutexInit(ConnIO.mutex);

		ConnIO.handles.head = NULL;
		ConnIO.handles.tail = NULL;

		ConnIO.threadEngine.internal = NULL;
		ConnIO.threadEngine.defaultConsumer = NULL;

		XplGetHighResolutionTime(count);
		srand(count);
		ConnIO.state = CONNIO_STATE_RUNNING;
	} else {
		/* wait while another thread gets ConnIO started */
		for (count = 0; (ConnIO.state < CONNIO_STATE_RUNNING) && (count < 90); count++) {
			XplDelay(333);
		}
		if( ConnIO.state < CONNIO_STATE_RUNNING ) {
			/* it is probably not going to happen if it hasn't happened already */
			return( NULL );
		}
	}

	if( !SocketLibStart( ( ( flags & CONN_HANDLE_SSL_ENABLED ) ? SOCK_LIB_FLAG_SSL: 0 ) |
						 ( ( flags & CONN_HANDLE_ASYNC_ENABLED ) ? SOCK_LIB_FLAG_ASYNC: 0 ) ) ) {
		XplConsolePrintf(" CONNIO(%s:%d): %s failed to start because Connection I/O management library could not initialize the socket library\r\n", __FILE__, __LINE__, identity );
		return( NULL );
	}

	handle = (ConnHandlePriv *)MemMalloc(sizeof(ConnHandlePriv));

	if( !handle ) {
		XplConsolePrintf(" CONNIO(%s:%d): %s failed to start because Connection I/O management library could not get memory\r\n", __FILE__, __LINE__, identity );
		return( NULL );
	}

	MemUpdateOwner( handle, file, line );
	memset(handle, 0, sizeof(ConnHandlePriv));
	handle->config = config;

	handle->identity = CopyIdentity( identity );
	if( !handle->identity ) {
		XplConsolePrintf(" CONNIO(%s:%d): %s failed to start because Connection I/O management library could not get memory\r\n", __FILE__, __LINE__, identity );
		MemFree( handle );
		return( NULL );
	}

	if( flags & CONN_HANDLE_SSL_CLIENT_ENABLED ) {
		/* Attempt to clear any previous SSL errors before loading the new context */
		int sslError;

		while ( (sslError = ERR_get_error( ) ) ) {
			char errstr[120];
			ERR_error_string( sslError, errstr );
			DebugPrintErr( "CONNIO[%s:%d]: SSL error %s (%d).\n", __FILE__, __LINE__, errstr, sslError );
		}

		if( !( handle->pub.sslv23.client = CreateClientContext( FALSE, handle->identity ) ) ||
			!( handle->pub.tls.client = CreateClientContext( TRUE, handle->identity ) ) ) {
			if (handle->pub.sslv23.client) { /* in case barfed in TLS context creation */
				SSL_CTX_free(handle->pub.sslv23.client);
			}

			MemFree( handle->identity );
			MemFree( handle );
			return( NULL );
		}
	}

	/*
	  Note: the server ssl context does not get created until
	  ConnLoadServerCertificate() is called on the handle
	*/

	if( flags & CONN_HANDLE_ASYNC_ENABLED ) {
		if( internalThreadEngineSet( handle, config ) ||
			defaultConsumerThreadEngineSet( handle, config ) ) {

			defaultConsumerThreadEngineClear( handle, config );
			internalThreadEngineClear( handle, config );

			XplConsolePrintf(" CONNIO(%s:%d): %s failed to start because Connection I/O management library could not initialize the asynchronous thread engine\r\n", __FILE__, __LINE__, handle->identity );
			errno = EPERM;
			if( handle->pub.sslv23.client ) {
				SSL_CTX_free( handle->pub.sslv23.client );
			}
			if( handle->pub.tls.client ) {
				SSL_CTX_free( handle->pub.tls.client );
			}
			MemFree( handle->identity );
			MemFree( handle );
			return( NULL );
		}
	}

	/*
	  Note: the connCacheInit triggers the allocation of the
	  first Connection structure and copies the timeout
	  values at that time.
	*/
	connTimeoutInit( handle, config );
	ConnListInit( &( handle->inUse ) );
	XplSafeInit( handle->inCleanup, 0 );
	XplMutexInit(handle->mutex);
	handle->flags.fixed = flags;

	/*
	  So we have completed init of the handle structure
	  above in order to prevent the use of other uninitialized
	  fields within the paths invoked by connCacheInit(). MK 8/11/08
	*/
	if( !connCacheInit( &( handle->connCache ), handle, handle->identity, flags, config ) ) {
		// Either 1) we couldn't alloc the cache
		// or 2) the identity was too long
		// or 3) the config struct improperly populated.
		if( handle->pub.sslv23.client ) {
			SSL_CTX_free( handle->pub.sslv23.client );
		}
		if( handle->pub.tls.client ) {
			SSL_CTX_free( handle->pub.tls.client );
		}

		XplMutexLock( ConnIO.mutex );
		XplMutexUnlock( ConnIO.mutex );

		MemFree( handle->identity );
		MemFree( handle );
		return( NULL );
	}


	SocketTraceRemoveFiles( handle->identity );
	ConnHandleRemember( handle );
	return (ConnHandle *)( handle );
}

static SSL_CTX * CreateServerContext(XplBool tls, char *identity, const char *publicKeyPath, const char *privateKeyPath)
{
	SSL_CTX		*ret;
	int			r;
	WJElement	config	= ConnGetConfig();

	errno = 0;

	/*
		Use SSLv23 method, which will support all protocols but disable specific
		ones via options below.
	*/
	if ( ret = SSL_CTX_new( SSLv23_server_method() ) ) {
		char		pubpath[XPL_MAX_PATH];
		char		prvpath[XPL_MAX_PATH];

		if (publicKeyPath && !XplExpandEnv(pubpath, publicKeyPath, sizeof(pubpath))) {
			publicKeyPath = (const char *) pubpath;
		}

		if (privateKeyPath && !XplExpandEnv(prvpath, privateKeyPath, sizeof(prvpath))) {
			privateKeyPath = (const char *) prvpath;
		}

		SSL_CTX_set_mode(ret, SSL_MODE_AUTO_RETRY);
		SSL_CTX_set_options(ret, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
		SSL_CTX_set_options(ret, SSL_OP_NO_SSLv2);
		SSL_CTX_set_options(ret, SSL_OP_NO_SSLv3);
		// SSL_CTX_set_options(ret, SSL_OP_NO_TLSv1);

		SSL_CTX_set_cipher_list(ret, WJEString(config, "cipher-list", WJE_GET, RECOMMENDED_CIPHER_LIST));

		if ((WJEBool(config, "cipher-server-order", WJE_NEW, TRUE))) {
			SSL_CTX_set_options(ret, SSL_OP_CIPHER_SERVER_PREFERENCE);
		}
		SSL_CTX_set_ecdh_auto(ret, 1);

		if (!privateKeyPath || !stricmp(publicKeyPath, privateKeyPath)) {
			/*
				If the consumer is attempting to load a private key out of the
				same file as the public key then chain loading will not work
				properly.
			*/
			r = SSL_CTX_use_certificate_file(ret, publicKeyPath, SSL_FILETYPE_PEM);
		} else {
			r = SSL_CTX_use_certificate_chain_file(ret, publicKeyPath);
		}

		if ( ( 1 != r ) ||
			 ( 1 != SSL_CTX_use_PrivateKey_file( ret, privateKeyPath, SSL_FILETYPE_PEM ) ) ||
			 ( 1 != SSL_CTX_check_private_key( ret ) ) ) {
			SSL_CTX_free( ret );
			ret = NULL;
		}
	} else {
		int		sslError;
		char	errstr[120];

		while ( ( sslError = ERR_get_error ( ) ) ) {
			ERR_error_string( sslError, errstr );
			XplConsolePrintf(" CONNIO(%s:%d): %s Could not initialize %s; %s\r\n", __FILE__, __LINE__, identity, tls ? "TLS" : "SSL", errstr );
		}
	}

	if (config) {
		WJECloseDocument(config);
	}

	return ret;
}


EXPORT XplBool ConnLoadServerCertificate(ConnHandle *pubHandle, const char *publicKeyPath, const char *privateKeyPath)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	char pubpath[XPL_MAX_PATH];
	char prvpath[XPL_MAX_PATH];

	errno = 0;
	if( notNull( handle ) ) {
		if( handle->pub.sslv23.server ) {
			SSL_CTX_free( handle->pub.sslv23.server );
			handle->pub.sslv23.server = NULL;
		}

		if( handle->pub.tls.server ) {
			SSL_CTX_free( handle->pub.tls.server );
			handle->pub.tls.server = NULL;
		}

		if( notShuttingHandleDown( handle ) && ( handle->flags.fixed & CONN_HANDLE_SSL_SERVER_ENABLED ) && publicKeyPath ) {
			if( publicKeyPath && !XplExpandEnv( pubpath, publicKeyPath, sizeof( pubpath ) ) ) {
				publicKeyPath = (const char *) pubpath;
			}

			if( privateKeyPath && !XplExpandEnv( prvpath, privateKeyPath, sizeof( prvpath ) ) ) {
				privateKeyPath = (const char *) prvpath;
			}

			if( ( handle->pub.sslv23.server = CreateServerContext( FALSE, handle->identity, publicKeyPath, privateKeyPath ) ) &&
				( handle->pub.tls.server = CreateServerContext( TRUE, handle->identity, publicKeyPath, privateKeyPath ) ) ) {
				return TRUE;
			}
			if (handle->pub.sslv23.server) {
				/* in case barfed in TLS context creation */
				SSL_CTX_free( handle->pub.sslv23.server );
			}
		}
	}
	return FALSE;
}

static ConnEvent *
connEventAlloc( ConnectionPriv *conn, const char *file, const int line )
{
	ConnEvent *connEvent;

	if ( ( connEvent = MemMalloc( sizeof( ConnEvent ) ) ) ) {
		MemUpdateOwner( connEvent, file, line );
		LogConnEventAlloc( conn, connEvent );
	}
	return (ConnEvent *)allocWorked( connEvent );
}

void
ConnEventFreeResources( ConnEvent *ce )
{
	/* check for allocated data on the event and free it */
	if( ce->type == CONN_EVENT_CONNECT ) {
		if ( ce->mode == CONN_EVENT_MODE_ASYNCHRONOUS ) {
 			ConnectAddressesFreeCopy( ce );
 		}
	} else if( ce->type == CONN_EVENT_WRITE_FROM_CLIENT_BUFFER ) {
		if( ce->args.buffer.start ) {
			ce->args.buffer.freeCB( ce->args.buffer.start );
			ce->args.buffer.start = NULL;
		}
	} else if( ce->eventClass == CONN_EVENT_CLASS_SEND ) {
		if( ce->ioBuffer ) {
			IOBFree( ce->ioBuffer );
			ce->ioBuffer = NULL;
		}
 	}
}

void
ConnEventFree( ConnectionPriv *conn, ConnEvent *ce )
{
	ConnEventFreeResources( ce );
	LogConnEventFree( conn, ce );
	MemFree( ce );
}

static XplBool
connIsStillConnected( ConnectionPriv *conn )
{
	if ( conn->sp->number != SOCKET_INVALID ) {
		/* in the tradditional world of IP, the presence of a socket does not
		   necessarily mean that socket is connected, but in Connio, a conn
		   is not created unless a connection already exists.
		*/

		return( TRUE );
	}
	errno = ENOTCONN;
	return( FALSE );
}

static XplBool
connIsSecured( ConnectionPriv *conn )
{
	if ( conn->sp->ssl.conn ) {
		return( TRUE );
	}
	errno = EALREADY;
	return( FALSE );
}

static XplBool
connIsNotSecured( ConnectionPriv *conn )
{
	if ( !( conn->sp->ssl.conn ) ) {
		return( TRUE );
	}
	errno = EALREADY;
	return( FALSE );
}


INLINE static void
_ConnEventInit( ConnEvent *connEvent, ConnectionPriv *conn )
{
	memset( connEvent, 0, sizeof( ConnEvent ) );
	connEvent->conn = conn;
	connEvent->socket = conn->sp;
}

INLINE static void
ConnEventAssign( ConnEvent *ce, void *clientData, int length, int timeout, ConnEventType eventType, ConnEventClass eventClass, ConnEventMode mode, IOBuffer *iob, const char *file, const int line )
{
	ConnectionPriv *conn = ce->conn;

	/* initialized everytime to 0 */
	ce->error = 0;
	ce->status = 0;
	ce->transferred = 0;
	ce->next = NULL;
	memset( &( ce->args ), 0, sizeof( ClientArgs ) );

	ce->inserted = 0;

	/* initialized everytime to a value */
	ce->type = eventType;
	ce->eventClass = eventClass;
	ce->source.file = (char *)file;
	ce->source.line = (int)line;

	ce->mode = mode;


	switch( eventType ) {
		case CONN_EVENT_READ:
		case CONN_EVENT_READ_COUNT:
		case CONN_EVENT_READ_UNTIL_PATTERN: {
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.receive ) );

			DebugAssert( iob && ( iob == conn->receiveBuffer ) );
			ce->ioBuffer = iob;

			ce->clientBuffer.read = ce->clientBuffer.write = ce->args.buffer.start = (char *)clientData;
			ce->clientBuffer.remaining = ce->args.buffer.size = length;

			break;
		}
		case CONN_EVENT_READ_TO_CB: {
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.receive ) );

			DebugAssert( iob && ( iob == conn->receiveBuffer ) );
			ce->ioBuffer = iob;

			ce->args.cb.count = length;

			break;
		}
		case CONN_EVENT_READ_TO_CONN:		{
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.receive ) );

			DebugAssert( iob && ( iob == conn->receiveBuffer ) );
			ce->ioBuffer = iob;

			ce->args.conn.handle = (ConnectionPriv *)clientData;
			ce->args.conn.count = length;

			break;
		}
		case CONN_EVENT_WRITE_FROM_CB: {
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.send ) );

			DebugAssert( iob );
			ce->ioBuffer = iob;

			ce->args.cb.count = length;

			break;
		}
		case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER: {
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.send ) );

			DebugAssert( clientData );

			ce->ioBuffer = NULL;
			ce->clientBuffer.read = ce->clientBuffer.write = ce->args.buffer.start = (char *)clientData;
			ce->clientBuffer.remaining = ce->args.buffer.size = length;

			break;
		}
		case CONN_EVENT_FLUSH: {
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.send ) );

			DebugAssert( iob );
			ce->ioBuffer = iob;

			break;
		}
		case CONN_EVENT_CONNECT: {
			/* the addresses are set up by ConnectAddressCopy() when ConnEventInit returns */

			DebugAssert( !iob );
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.connect ) );

			break;
		}
		case CONN_EVENT_SSL_ACCEPT:
		case CONN_EVENT_TLS_ACCEPT:
		case CONN_EVENT_SSL_CONNECT:
		case CONN_EVENT_TLS_CONNECT:
		case CONN_EVENT_ACCEPT: {
			DebugAssert( !iob );
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.connect ) );
			break;
		}
		case CONN_EVENT_DISCONNECT:
		case CONN_EVENT_SSL_DISCONNECT: {
			DebugAssert( !iob );
			ConnEventSetTimeout( ce, timeout, &( conn->timeout.disconnect ) );
		}
	}
}

static XplBool
SynchronousConnectEventInsert( ConnectionPriv *conn, ConnEvent *connEvent )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( conn->state == CONN_STATE_IDLE ) {
		if( ( conn->activeEvent.connect == NULL ) && ( conn->activeEvent.receive == NULL ) && ( conn->activeEvent.send == NULL ) ) {
			ConnChangeState( conn, CONN_STATE_SYNCHRONOUS );
			ConnEventConnectInProgressSet( conn, connEvent );

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EBUSY;

		EventLockRelease( conn );   /***** UNLOCK *****/

		DebugAssert( 0 );
		return( FALSE );
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

static XplBool
SynchronousConnectEventRemove( ConnectionPriv *conn, ConnEvent *connectEvent )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( conn->state == CONN_STATE_SYNCHRONOUS ) {
		if( conn->activeEvent.connect == connectEvent) {
			ConnEventConnectInProgressClear( conn, connectEvent );
			ConnChangeState( conn, CONN_STATE_IDLE );

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EINVAL;

		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

static XplBool
SynchronousReceiveEventInsert( ConnectionPriv *conn, ConnEvent *receiveEvent, IOBuffer **bufferedSendData )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( ( conn->state == CONN_STATE_IDLE ) || ( conn->state == CONN_STATE_SYNCHRONOUS ) ) {
		if( ( conn->activeEvent.connect == NULL ) && ( conn->activeEvent.receive == NULL ) ) {
			ConnChangeState( conn, CONN_STATE_SYNCHRONOUS );
			/* check for buffered data and do implicit flush */
			*bufferedSendData = bufferedSendDataGet( conn, WITH_DATA );
			if( *bufferedSendData ) {
				DebugAssert( conn->activeEvent.send == NULL );
				/*
				   there is data to flush - we need to send and receive
				   put connEvent in the connect in progress slot to
				   prevent other reads and writes
				*/
				ConnEventConnectInProgressSet( conn, receiveEvent );
			} else {
				ConnEventReceiveInProgressSet( conn, receiveEvent );
			}

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EBUSY;

		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

static XplBool
SynchronousReceiveEventRemove( ConnectionPriv *conn, ConnEvent *connEvent, XplBool haveConnectLock )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( conn->state == CONN_STATE_SYNCHRONOUS ) {
		if( haveConnectLock && ( conn->activeEvent.connect == connEvent) ) {
			ConnEventConnectInProgressClear( conn, connEvent );
			ConnChangeState( conn, CONN_STATE_IDLE );

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		if( !haveConnectLock && ( conn->activeEvent.receive == connEvent) ) {
			ConnEventReceiveInProgressClear( conn, connEvent );
			ConnChangeState( conn, CONN_STATE_IDLE );

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EINVAL;

		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

static XplBool
SynchronousSendEventInsert( ConnectionPriv *conn, ConnEvent *sendEvent )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( ( conn->state == CONN_STATE_IDLE ) || ( conn->state == CONN_STATE_SYNCHRONOUS ) ) {
		if( ( conn->activeEvent.connect == NULL ) && ( conn->activeEvent.send == NULL ) ) {
			ConnChangeState( conn, CONN_STATE_SYNCHRONOUS );
			ConnEventSendInProgressSet( conn, sendEvent);

			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EBUSY;

		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

static XplBool
SynchronousSendEventRemove( ConnectionPriv *conn, ConnEvent *sendEvent )
{
	EventLockAcquire( conn );   /***** LOCK *****/

	if( conn->state == CONN_STATE_SYNCHRONOUS ) {
		if( conn->activeEvent.send == sendEvent) {
			ConnEventSendInProgressClear( conn, sendEvent );
			if( conn->activeEvent.receive == NULL ) {
				ConnChangeState( conn, CONN_STATE_IDLE );
			}
			EventLockRelease( conn );   /***** UNLOCK *****/

			return( TRUE );
		}
		errno = EINVAL;
		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}
	EventLockRelease( conn );   /***** UNLOCK *****/

	errno = ECONNIOMODE;
	return( FALSE);
}

/* the caller of this function must guarantee that conn->writeBuffer exists and has data */
static int
connFlushEx( ConnectionPriv *conn, IOBuffer *writeBuffer, int timeout, const char *file, const int line )
{
	ConnEvent connEvent;

	DebugAssert( writeBuffer && ( IOBGetUsed( writeBuffer, NULL) > 0 ) );  // there is nothing to flush!! the calling function messed up
	_ConnEventInit( &connEvent, conn );
	ConnEventAssign( &connEvent, NULL, 0, timeout, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, writeBuffer, file, line );
	if( SynchronousSendEventInsert( conn, &connEvent ) ) {
		ConnEventProcess( &( connEvent ) );
		SynchronousSendEventRemove( conn, &connEvent );
		errno = connEvent.error;
		return connEvent.transferred;
	}
	IOBFree( writeBuffer );
	return 0;
}

/* The Implicit functions can only be called when the consumer hold the SynchronousConnectEvent */
/* A pointer to a stack ConnEvent should be passed in */
static void
ImplicitFlushOnConnectEvent( ConnEvent *connEvent, ConnectionPriv *conn, const char *file, const int line )
{
	IOBuffer *iob;

	iob = bufferedSendDataGet( conn, WITH_DATA );
	if( iob ) {
		ConnEventAssign( connEvent, NULL, 0, -1, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, iob, file, line );
		/*
		   because SynchronousConnectEventInsert() worked,
		   this thread owns the conn and can insert receive and send events manually
		*/
		ConnEventProcess( connEvent );
	}
}

static void
ImplicitSSLDisconnectOnConnectEvent( ConnEvent *connEvent, ConnectionPriv *conn, const char *file, const int line )
{
	if( conn->sp->ssl.conn ) {
		/* Shutdown SSL before disconnecting */
		ConnEventAssign( connEvent, NULL, 0, -1, CONN_EVENT_SSL_DISCONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
		ConnEventProcess( connEvent );
	}
}

static void
CheckNotSamePort( struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen )
{
	if(destSaddr && srcSaddr && (destSaddr->sa_family == AF_INET) && (srcSaddr->sa_family == AF_INET)) {
		struct sockaddr_in *dsa4 = (struct sockaddr_in *) destSaddr;
		struct sockaddr_in *ssa4 = (struct sockaddr_in *) srcSaddr;
		if((dsa4->sin_addr.s_addr == ssa4->sin_addr.s_addr) && ssa4->sin_port && (ssa4->sin_port == dsa4->sin_port)) {
			DebugAssert(0);
		}
	}
}

static void
ConnectAddressesCopy( struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, ConnEvent *ce )
{
	CheckNotSamePort(destSaddr, destSaddrLen, srcSaddr, srcSaddrLen);
	ce->args.addr.src = srcSaddr;
	ce->args.addr.srcSize = srcSaddrLen;
	ce->args.addr.dstSize = destSaddrLen;
	ce->args.addr.dst = destSaddr;
}

static XplBool
ConnectAddressesDuplicate( struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, ConnEvent *ce )
{
	CheckNotSamePort(destSaddr, destSaddrLen, srcSaddr, srcSaddrLen);
	ce->args.addr.src = NULL;
	ce->args.addr.srcSize = 0;
	ce->args.addr.dstSize = 0;
	ce->args.addr.dst = MemMalloc( destSaddrLen );
	if( ce->args.addr.dst ) {
		memcpy( ce->args.addr.dst, destSaddr, destSaddrLen );
		ce->args.addr.dstSize = destSaddrLen;
		if( !srcSaddr ) {
			return( TRUE );
		}
		ce->args.addr.src = MemMalloc( srcSaddrLen );
		if( ce->args.addr.src ) {
			memcpy( ce->args.addr.src, srcSaddr, srcSaddrLen );
			ce->args.addr.srcSize = srcSaddrLen;
			return( TRUE );
		}
	}
	return( FALSE );
}

void
ConnectAddressesFreeCopy( ConnEvent *ce )
{
	ce->args.addr.srcSize = 0;
	ce->args.addr.dstSize = 0;
	if( ce->args.addr.dst ) {
		MemFree( ce->args.addr.dst );
		ce->args.addr.dst = NULL;
	}
	if( ce->args.addr.src ) {
		MemFree( ce->args.addr.src );
		ce->args.addr.src = NULL;
	}
}

static void ConnectionClean( ConnectionPriv *conn )
{
	if (conn->pub.client.free) {
		conn->pub.client.free( ( Connection * )conn );
	}
}

/* the stategy with ConnRecycle() is to:
   - make a copy of what is worth saving
   - reinitialize the conn with the saved data
   this way the conn is always restored to a pristine state
*/
static void
ConnRecycle( ConnectionPriv *conn )
{
    ConnectionClean( conn );
	ConnectionInit( conn );
	MemPrivatePoolReturnEntry( conn );
	return;
}

ConnectionPriv *
_ConnAllocEx(ConnHandlePriv *handle, SOCK *SP, const char *file, const int line)
{
	ConnectionPriv *conn;
	SOCK *sp = NULL;

	errno = 0;
	if ( notNull( handle ) ) {
		if( SP || allocWorked( sp = SocketAlloc( 0 ) ) ) {
			conn = (ConnectionPriv *)MemPrivatePoolGetEntry(handle->connCache.cache);
			if ( allocWorked( conn ) ) {
				conn->sp = SP ? SP: sp;
				LogConnBindSocket( conn );
				LogConnDisposition( conn, "Allocated", file, line );
				MemUpdateOwner(conn, file, line);  /* make the memory count against the consumer instead of the library */
				conn->pub.created.file = file;
				conn->pub.created.line = line;
				conn->connHandle = handle;

				ConnChangeState( conn, CONN_STATE_IDLE );
				ConnListAppend( &(handle->inUse), conn );
				return conn;
			}
			SocketRelease( &sp );
		}
	}
	return NULL;
}

INLINE static XplBool
connFreeEx( ConnectionPriv *conn, const char *file, const int line )
{
	ConnEvent connEvent;
	int saveErrno = errno;
	XplBool restoreErrno = 0 != errno;
	XplBool returnValue = FALSE;
	ConnHandlePriv *handle;

	if( !notNull( conn ) ) {
		goto fn_exit;
	}

	LogConnDisposition( conn, "Released", file, line );
	DebugAssert( ConnIsIdle( conn ) && SocketIsIdle( conn->sp ) );

	if( ( conn->state == CONN_STATE_FREE ) || ( conn->state == CONN_STATE_CACHED ) ) {
		DebugPrintErr("CONNIO(%s:%d): ConnFree() was called on a conn that has already been freed\n", file, line );
		DebugAssert( 0 );
		goto fn_exit;
	}

	if( conn->state != CONN_STATE_ASYNCHRONOUS_LISTENER && conn->state != CONN_STATE_SYNCHRONOUS_LISTENER ) {
		_ConnEventInit( &connEvent, conn );
		if( !SynchronousConnectEventInsert( conn, &connEvent ) ) {
			DebugPrintErr("CONNIO(%s:%d): ConnFree() was called on a conn being used by another thread.\n", file, line );
			DebugAssert( 0 );
			goto fn_exit;
		}

		if( connIsStillConnected( conn ) ) {
			ImplicitFlushOnConnectEvent( &connEvent, conn, file, line );
			ImplicitSSLDisconnectOnConnectEvent( &connEvent, conn, file, line );
			SocketDisconnect( conn->sp, &( conn->timeout.disconnect ) );
			SocketClose( conn->sp );
		}
		else {
			// compensate for side effect of connIsStillConnected().
			// it set errno to ENOTCONN and returned a FALSE
			errno = 0;
		}
	}

	if( conn->sp->ssl.conn || conn->sp->ssl.new ) {
		/* Listeners and closed connections may still have an SSL pointer */
		SocketSSLAbort( conn->sp, ESHUTDOWN );
	}
	SocketTraceEnd( conn->sp );
	SocketRelease( &( conn->sp ) );
	handle = conn->connHandle;
	/*
	  If another thread is in ConnHandleShutdown() and this function is freeing
	  the last conn, the other thread could free the conn handle before this
	  function is finished calling ConnRecycle().  To prevent this race
	  condition, handle->inCleanup is incremented until ConnRecycle() is done.
	  ConnHandleWaitForConnectionsToExit() will block if there are conns in the
	  inUse list or threads inCleanup.
	 */
	XplSafeIncrement( handle->inCleanup );
	ConnListRemove( &( conn->connHandle->inUse ), conn );

	if( conn->state != CONN_STATE_ASYNCHRONOUS_LISTENER && conn->state != CONN_STATE_SYNCHRONOUS_LISTENER ) {
		SynchronousConnectEventRemove( conn, &connEvent );
	}

	ConnRecycle( conn );
	XplSafeDecrement( handle->inCleanup );
	returnValue = TRUE;
fn_exit:;
	if (restoreErrno) {
		errno = saveErrno;
	}
	return( returnValue );
}

EXPORT XplBool
ConnFreeEx( Connection *Conn, const char *file, const int line )
{
	XplBool ret = FALSE;

	errno = 0;
	if( notNull( Conn ) ) {
		ConnectionPriv *conn = (ConnectionPriv *)Conn;
		RememberAPICalls( conn, "ConnFree", file, line );
		ret = connFreeEx( conn, file, line );
	}
	return ret;
}

EXPORT XplBool
ConnReleaseEx( Connection **Conn, const char *file, const int line )
{
	XplBool ret = FALSE;

	errno = 0;
	if( notNull( Conn ) && notNull( *Conn ) ) {
		ret = ConnFreeEx( *Conn, file, line );
		*Conn = NULL;
	}
	return ret;
}

EXPORT int
ConnSetShutdownCallbackEx( Connection *Conn, ConnShutdownCB callback, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnSetShutdownCallback", file, line );
		conn->shutdownCallback = callback;
		errno = 0;
		return 0;
	}
	return -1;
}

EXPORT void
ConnANoopEx( Connection *Conn, const char *file, const int line )
{
	XplBool ret;
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	if( notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnANoop", file, line );
		ret = EventEnqueue( conn, NULL, file, line );
	}
}

EXPORT XplBool
ConnAQueuedEventsEx( Connection *Conn, const char *file, const int line )
{
	XplBool ret = FALSE;
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	if( notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnAQueuedEvents", file, line );
		ret = EventsQueued( conn, file, line );
	}
	return ret;
}

EXPORT XplBool
ConnASubmitEx( Connection *Conn, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	if( notNull( conn ) && notNull( successCB ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnASubmit", file, line );
		if( EventsSubmit( conn, successCB, failureCB, client, cbThreadEngine, file, line ) ) {
			return TRUE;
		}
		DebugAssert( 0 );  // a bad argument was passed or async is not enabled in the handle
	}
	return FALSE;
}

EXPORT Connection *
ConnListenEx(ConnHandle *pubHandle, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnectionPriv *conn;

	errno = 0;
	if( notNull( handle ) ) {
		conn = _ConnAllocEx( handle, NULL, file, line );
		if( conn ) {
			RememberAPICalls( conn, "ConnListen", file, line );
			if( notShuttingHandleDown( handle ) ) {
				if( 0 == SocketListen( conn->sp, srcSaddr, srcSaddrLen, backlog, 0 ) ) {
					EventLockAcquire( conn );
					ConnChangeState( conn, CONN_STATE_SYNCHRONOUS_LISTENER );
					EventLockRelease( conn );
					return ( Connection * )conn;
				}
			}
			connFreeEx( conn, file, line );
		}
	}
	return NULL;
}

EXPORT XplBool
ConnCloseListenerEx(Connection *Conn, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool ret = FALSE;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnCloseListener", file, line );
		if( conn->state == CONN_STATE_SYNCHRONOUS_LISTENER ) {
			synchronousListenerClose( conn );
			ret = TRUE;
		} else {
			DebugPrintErr( "CONNIO(%s:%d): ConnCloseListener() called on a conn that is not synchronous listener\n", file, line );
		}
	}
	return ret;
}

/* ConnAListenSuspend()

   This function causes an active asynchronous listener to stop pulling
   new connections from the backlog.  If the number of connections exceeds the
   backlog before the listener is resumed, the OS will drop new connections.
 */
EXPORT XplBool ConnAListenSuspendEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool success = FALSE;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnAListenSuspend", file, line );
		success = SocketListenerSuspend( conn->sp );
	}
	return success;
}

/* ConnAListenResume()

   This function causes a suspended asynchronous listener to start pulling
   new connections from the backlog.
 */
EXPORT XplBool ConnAListenResumeEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool success = FALSE;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnAListenResume", file, line );
		success = SocketListenerResume( conn->sp );
	}
	return success;
}

EXPORT Connection *
ConnAListenEx(ConnHandle *pubHandle, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, int listenerID, ConnAAcceptCB acb, void * acbContext, ConnAListenCB lcb, AThreadEngine *threadEngine, unsigned long throttlingThreshold, ListenerEventCallback lecb, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnEvent *connEvent;
	ConnectionPriv *conn;
	AThreadEngine *thrEngine;

	errno = 0;
	if( notNull( handle ) && notShuttingHandleDown( handle ) && newListenersAllowed( pubHandle ) ) {
		if( asyncEnabledHandle( handle ) ) {
			if( ( conn = _ConnAllocEx( handle, NULL, file, line ) ) ) {
				if( 0 == SocketListen( conn->sp, srcSaddr, srcSaddrLen, backlog, listenerID ) ) {
					conn->async.cb.accept = acb;
					conn->async.cb.acceptContext = acbContext;
					conn->async.cb.listen = lcb;
					/* create and add ConnEvent */
					if( ( connEvent = connEventAlloc( conn, file, line ) ) ) {
						_ConnEventInit( connEvent, conn );
						ConnEventAssign( connEvent, NULL, 0, -1, CONN_EVENT_ACCEPT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );

						EventLockAcquire( conn );   /***** LOCK *****/

						ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_LISTENER );
						ConnEventConnectInProgressSet( conn, connEvent );

						EventLockRelease( conn );   /***** UNLOCK *****/

						thrEngine = threadEngine ? threadEngine: handle->threadEngine.defaultConsumer;

						if( 0 == SocketEventWaitEx( conn->sp, SOCK_EVENT_TYPE_LISTEN, &connEvent->timeOut, NULL, 0, AsyncListenerCallback, thrEngine, AcceptCallback, connEvent, thrEngine, throttlingThreshold, lecb, file, line ) ) {
							LogSocketEventStarted( conn, connEvent, SOCK_EVENT_TYPE_LISTEN );
							return ( Connection * )conn;
						}
						LogSocketEventFailedToStart( conn, connEvent, SOCK_EVENT_TYPE_LISTEN );

						EventLockAcquire( conn );   /***** LOCK *****/

						ConnEventConnectInProgressClear( conn, connEvent );
						ConnChangeState( conn, CONN_STATE_IDLE );

						EventLockRelease( conn );   /***** UNLOCK *****/

						ConnEventFree( conn, connEvent );
					}
				}
				connFreeEx( conn, file, line );
			}
		}
	}
	return NULL;
}

static void ConnListenerFree( ConnHandlePriv *handle, ConnListenerPriv *listener )
{
	/* Remove the ConnListener from the linked list on the handle */
	if (listener->previous) {
		listener->previous->next = listener->next;
	} else {
		handle->listener.head = listener->next;
	}

	if (listener->next) {
		listener->next->previous = listener->previous;
	} else {
		handle->listener.tail = listener->previous;
	}

	listener->next		= NULL;
	listener->previous	= NULL;
	listener->handle	= NULL;

	if (listener->sa) {
		MemFree(listener->sa);
	}

	MemFree(listener);
}

/*
	ConnAListenerCallback is called whenever an error occurs on an asynchronous listener.
	it is responsible for restarting a listener if needed or cleaning up the listener
	and the conn.
*/
static void
ConnAListenerCallback(struct Connection *Conn)
{
	ConnectionPriv *conn = ( ConnectionPriv *)Conn;
	if (conn) {
		ConnListenerPriv	*l;
		ConnHandlePriv		*h;

		l = conn->pub.client.data;
		conn->pub.client.data = NULL;

		h = conn->connHandle;
		connFreeEx( conn, __FILE__, __LINE__ );
		if( l && h ) {
			XplMutexLock( h->mutex );
			DebugAssert( l->pub.conn );  // A listener came out of the engine without a conn (rap)
			DebugAssert( ( ConnectionPriv *)l->pub.conn == conn );  // A listener came out of the engine with the wrong conn (rap)
			l->pub.conn = NULL;
			if( !(h->shuttingDown || h->listener.preventNew) && !( CONN_LISTENER_STOPPED & l->state ) ) {
				/*
				  We have a listener, that does not appear to have been
				  stopped intentionally, which implies that some other error
				  has occurred.  Attempt to restart it.
				*/
				XplMutexUnlock(h->mutex);
				ConnAStartListenerEx( NULL, 0, (ConnListener *)l, __FILE__, __LINE__ );
				XplMutexLock( h->mutex );
			}
			XplMutexUnlock(h->mutex);
		}
	}
}

EXPORT void ConnAListenerShutdownEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = ( ConnectionPriv *)Conn;
	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnAListenerShutdown", file, line );
		asynchronousListenerShutdown( conn );
	}
}

EXPORT int ConnAListenerConfigure( ConnListener *Listener, unsigned long threshold, ListenerEventCallback cb )
{
	ConnListenerPriv *listener	= (ConnListenerPriv *)Listener;

	errno = 0;
	if( notNull( listener ) ) {
		if( listener->state == CONN_LISTENER_FRESH ) {
			listener->throttlingThreshold = threshold;
			listener->eventCallback = cb;
			return 0;
		}
		errno = EBUSY;  /* needs to be called before the listener is started */
	}
	return -1;
}


EXPORT ConnListener *
ConnAAddListenerEx(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, XplBool openFirewall, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener	= NULL;

	errno = 0;
	if (!notNull(handle) || !notNull(addr) || !notNull(acb) ||
		!asyncEnabledHandle(handle) || !notShuttingHandleDown( handle ) ||
		!newListenersAllowed( pubHandle )
	) {
		return(NULL);
	}

	/* Has this already been added? */
	for (listener = handle->listener.head; listener; listener = listener->next) {
		if (addrlen == listener->salen && !memcmp(addr, listener->sa, addrlen) &&
				backlog == listener->backlog && acb == listener->callback &&
				name == listener->name
		) {
			/* Remove the expired flag */
			listener->state &= ~CONN_LISTENER_EXPIRED;

			return(ConnListener *)listener;
		}
	}

	listener = MemMalloc(sizeof(ConnListenerPriv));
	if (allocWorked(listener)) {
		memset(listener, 0, sizeof(ConnListenerPriv));

		listener->sa = MemMalloc(addrlen);
		if (allocWorked(listener->sa)) {
			MemUpdateOwner(listener, file, line);
			MemUpdateOwner(listener->sa, file, line);

			memcpy(listener->sa, addr, addrlen);
			listener->salen		= addrlen;

			listener->name		= (char *) name;
			listener->throttlingThreshold = 0;
			listener->eventCallback = NULL;
			listener->cbThreadEngine = cbThreadEngine;
			listener->handle	= handle;
			listener->backlog	= backlog;
			listener->callback	= acb;
			listener->acceptContext = acbContext;
			listener->next		= NULL;
			listener->state		= CONN_LISTENER_FRESH;

			XplMutexLock(handle->mutex);

			handle->listener.last++;
			listener->id = handle->listener.last;
			listener->previous	= handle->listener.tail;

			if (handle->listener.tail) {
				handle->listener.tail->next = listener;
			} else {
				handle->listener.head = listener;
			}
			handle->listener.tail = listener;

			XplMutexUnlock(handle->mutex);
		} else {
			MemFree(listener);
			listener = NULL;
		}
	}

	return(ConnListener *)listener;
}

//EXPORT ConnListener *
//ConnAAddPublicListener(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, const char *file, const int line)
//{
//	return  _ConnAAddListenerEx(pubHandle, name, addr, addrlen, backlog, acb, acbContext, cbThreadEngine, TRUE, file, line);
//}

//EXPORT ConnListener *
//ConnAAddListenerEx(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, const char *file, const int line)
//{
//	return  _ConnAAddListenerEx(pubHandle, name, addr, addrlen, backlog, acb, acbContext, cbThreadEngine, FALSE, file, line);
//}

EXPORT ConnListener *
ConnAAddIndependentListenerEx(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, const char *file, const int line)
{
	ConnListener	*l;
	// hans TODO: check if these could be public too

	errno = 0;
	l = NULL;
	if( newListenersAllowed( pubHandle ) ) {
		if ((l = ConnAAddListenerEx(pubHandle, name, addr, addrlen, backlog, acb, acbContext, cbThreadEngine, FALSE, file, line))) {
			((ConnListenerPriv *)l)->state |= CONN_LISTENER_INDEPENDENT;
		}
	}
	return(l);
}


/* Remove listener without locking the mutex.  The caller must lock it first. */
static int _ConnARemoveListenerEx(ConnHandlePriv *handle, ConnListenerPriv *listener)
{
	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle) || !listener) {
		return(-1);
	}

	listener->state		= CONN_LISTENER_STOPPED;

	if( listener->pub.conn ) {
		/* Kick the listener out of the async engine.  */
		ConnAListenerShutdown( listener->pub.conn );
		/*	The callback  will:
			- close the listening socket and
			- remove the listener from the linked list.
		*/
		return( 0 );
	}

	/* There is no conn associated with this listener, so it can be cleaned up immediately */
	ConnListenerFree( handle, listener );
	return(0);
}

EXPORT void ConnListenerListDestroy( ConnListenerList **clList )
{
	ConnListenerListPriv **cllist = (ConnListenerListPriv **)clList;

	errno = 0;

	if( notNull( cllist ) && notNull( *cllist ) ) {
		if( ( *cllist )->list ) {
			WJECloseDocument( ( *cllist )->list );
		}
		MemFree( *cllist );
		*cllist = NULL;
	}
}

static ConnListenerListPriv *connListenerListCreate( ConnHandlePriv *handle )
{
	ConnListenerListPriv *clist;

	clist = MemMallocWait( sizeof( ConnListenerListPriv ) );
	memset( clist, 0, sizeof( ConnListenerListPriv ) );
	clist->list = WJEArray(NULL, NULL, WJE_NEW);
	clist->handle = handle;
	return clist;
}

static void connListenerListAppend( ConnListenerListPriv *cllist, int id )
{
	WJENumber( cllist->list, "[$]", WJE_NEW, id );
}

EXPORT int ConnARemoveListenerEx(ConnHandle *pubHandle, int which, ConnListener *Listener, ConnListenerList **clList, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerListPriv **cllist = (ConnListenerListPriv **)clList;
	ConnListenerPriv	*listener = (ConnListenerPriv *)Listener;
	int					c;
	ConnListenerPriv	*l;
	ConnListenerPriv	*n;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(-1);
	}

	if (cllist) {
		*cllist = connListenerListCreate(handle);
	}
	c = 0;
	XplMutexLock(handle->mutex);

	for (l = handle->listener.head; l; l = n) {
		n = l->next;

		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (which && !(l->state & which))			continue;
		if (listener && l != listener)				continue;

		if (!_ConnARemoveListenerEx(handle, l)) {
			if( cllist && *cllist ) {
				connListenerListAppend( *cllist, l->id );
			}
			c++;
		}
	}

	XplMutexUnlock(handle->mutex);
	return(c);
}

EXPORT int ConnAStartListenerEx(ConnHandle *pubHandle, int which, ConnListener *Listener, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int				c = 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) ||
		!asyncEnabledHandle(handle) ||
		!notShuttingHandleDown( handle ) ||
		!newListenersAllowed( pubHandle )
		) {
		return(FALSE);
	}


	if (which == 0) {
		which = CONN_LISTENER_FRESH | CONN_LISTENER_ACTIVE |
				CONN_LISTENER_FAILED | CONN_LISTENER_STOPPED;
	}

	XplMutexLock(handle->mutex);

	/* Start all listeners that match which */
	for (l = listener ? listener : handle->listener.head; l; l = listener ? NULL : l->next) {
		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (l->pub.conn || !(l->state & which))						continue;

		XplMutexUnlock(handle->mutex);
		l->pub.conn = ConnAListenEx(pubHandle, l->sa, l->salen, l->backlog, l->id, l->callback, l->acceptContext, ConnAListenerCallback, l->cbThreadEngine, l->throttlingThreshold, l->eventCallback, file, line);
		XplMutexLock(handle->mutex);

		if (l->name) {
			char				msgstr[256];

			if (AF_INET == l->sa->sa_family &&
				0 == ((struct sockaddr_in *)l->sa)->sin_addr.s_addr
			) {
				strprintf(msgstr, sizeof(msgstr), NULL, "port %d",
					ntohs(((struct sockaddr_in *)l->sa)->sin_port));
			} else {
				XplIPAddrString(l->sa, msgstr, sizeof(msgstr));
			}

			if (l->pub.conn) {
				printf("\rListening for connections on %s\n", msgstr);
			} else if (!(l->state & CONN_LISTENER_FAILED)) {
				printf("\rFailed to start listening on %s\n", msgstr);
			}
			fflush(stdout);		/* force this message immediately */
		}

		if (l->pub.conn) {
			l->state = (CONN_LISTENER_ACTIVE | (l->state & CONN_LISTENER_INDEPENDENT));
			l->pub.conn->client.data = l;

			c++;
		} else {
			l->state = (CONN_LISTENER_FAILED | (l->state & CONN_LISTENER_INDEPENDENT));
		}
	}

	XplMutexUnlock(handle->mutex);

	return(c);
}

EXPORT int ConnAStopListenerEx(ConnHandle *pubHandle, int which, ConnListener *Listener, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int c = 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(0);
	}

	XplMutexLock(handle->mutex);

	for (l = handle->listener.head; l; l = l->next) {
		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (which && !(l->state & which))			continue;
		if (listener && l != listener)				continue;

		l->state = (CONN_LISTENER_STOPPED | (l->state & CONN_LISTENER_INDEPENDENT));
		if (l->pub.conn) {
			ConnAListenerShutdown( l->pub.conn );
		}
		c++;
	}

	XplMutexUnlock(handle->mutex);

	return(c);
}

EXPORT int ConnASuspendListenerEx(ConnHandle *pubHandle, ConnListener *Listener, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int c = 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(0);
	}

	XplMutexLock(handle->mutex);

	for (l = handle->listener.head; l; l = l->next) {
		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (!(l->state & CONN_LISTENER_ACTIVE))			continue;
		if (listener && l != listener)				continue;

		l->state = (CONN_LISTENER_SUSPENDED | (l->state & CONN_LISTENER_INDEPENDENT));
		if (l->pub.conn) {
			ConnAListenSuspend( l->pub.conn );
		}
		c++;
	}

	XplMutexUnlock(handle->mutex);

	return(c);
}

EXPORT int ConnAResumeListenerEx(ConnHandle *pubHandle, ConnListener *Listener, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int c = 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(0);
	}

	XplMutexLock(handle->mutex);

	for (l = handle->listener.head; l; l = l->next) {
		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (!(l->state & CONN_LISTENER_SUSPENDED))			continue;
		if (listener && l != listener)				continue;

		l->state = (CONN_LISTENER_ACTIVE | (l->state & CONN_LISTENER_INDEPENDENT));
		if (l->pub.conn) {
			ConnAListenResume( l->pub.conn );
		}
		c++;
	}

	XplMutexUnlock(handle->mutex);

	return(c);
}

EXPORT int ConnAExpireListenerEx(ConnHandle *pubHandle, int which, ConnListener *Listener, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int c	= 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(0);
	}

	for (l = handle->listener.head; l; l = l->next) {
		if (!listener && (l->state & CONN_LISTENER_INDEPENDENT))	continue;
		if (l->state & CONN_LISTENER_EXPIRED)		continue;
		if (listener && l != listener)				continue;

		l->state |= CONN_LISTENER_EXPIRED;
		c++;
	}

	return(c);
}

EXPORT int ConnARestartListenerEx(ConnListener *Listener, struct sockaddr *addr, socklen_t addrlen, const char *file, const int line)
{
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	Connection		*c;
	struct sockaddr	*sa;
	socklen_t		salen;

	errno = 0;

	if (!notNull(listener) ||
		!notNull(addr) ||
		!asyncEnabledHandle(listener->handle) ||
		!notShuttingHandleDown( listener->handle ) ||
		!newListenersAllowed( (ConnHandle *)listener->handle )
		) {
		return(0);
	}

	if (addrlen == listener->salen && !memcmp(addr, listener->sa, addrlen)) {
		/* This listener is already connected */
		return(listener->pub.conn || ConnAStartListenerEx(NULL, 0, (ConnListener *)listener, __FILE__, __LINE__));
	}

	/* backup the current values */
	c 		= listener->pub.conn;
	sa		= listener->sa;
	salen	= listener->salen;

	listener->state	= CONN_LISTENER_FRESH;
	listener->pub.conn	= NULL;

	/* Copy the new values into place */
	listener->sa = MemMalloc(addrlen);
	if (allocWorked(listener->sa)) {
		MemUpdateOwner(listener->sa, file, line);

		memcpy(listener->sa, addr, addrlen);
		listener->salen = addrlen;

		if (ConnAStartListenerEx(NULL, 0, (ConnListener *)listener, __FILE__, __LINE__)) {
			/* It worked, clean up the old values */
			MemFree(sa);

			/* ConnAListenerCallback() will free the conn */
			c->client.data = NULL;
			ConnAListenerShutdown(c);

			/* Remove the expired flag */
			listener->state &= ~CONN_LISTENER_EXPIRED;
			return(1);
		}

		MemFree(listener->sa);
	}

	/* Restore the original values */
	listener->pub.conn	= c;
	listener->sa	= sa;
	listener->salen	= salen;

	return(0);
}

EXPORT int ConnAListenerStatusEx(ConnHandle *pubHandle, int which, ConnListener *Listener, XplBool shuttingdown, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnListenerPriv *listener = (ConnListenerPriv *)Listener;
	ConnListenerPriv *l;
	int				c = 0;

	errno = 0;

	if (!handle && listener) {
		handle = listener->handle;
	}

	if (!notNull(handle) || !asyncEnabledHandle(handle)) {
		return(0);
	}

	XplMutexLock(handle->mutex);
	for (l = handle->listener.head; l; l = l->next) {
		if (!listener && (CONN_LISTENER_INDEPENDENT & l->state))	continue;
		if (which && !(l->state & which))			continue;
		if (listener && l != listener)				continue;

		if( shuttingdown && !l->pub.conn ) {
			ConnListenerFree( handle, l );
			continue;
		}
		c++;
	}
	XplMutexUnlock(handle->mutex);
	return(c);
}

EXPORT void ConnAListenerWaitForShutdownEx( ConnHandle *pubHandle, const char *file, const int line )
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;

	errno = 0;

	if( notNull( handle ) && asyncEnabledHandle( handle ) ) {
		unsigned int count;

		count = 0;
		for( ; ConnAListenerStatusEx( pubHandle, 0, NULL, TRUE, file, line ); ) {
			XplDelay( 1000 );
			count++;
			if( count % 10 ) {
				DebugPrintErr( "The '%s' conn handle has waited %d seconds for its listeners to exit.\n", handle->identity, count );
			}
		}
	}
}


/*! \fn		 int ConnAcceptEx(Connection *server, Connection **conn, char *file, int line)
	\brief	  A brief explanation.

				A detailed explanation.
u	\param[in]  server
	\param[in]  flags
	\param[out] conn
	\param[in]  file
	\param[in]  line
	\return	 On success ...
	\retval	 <0>
	\retval	 <ENOSYS>
	\todo	   Complete the documentation for the ConnAcceptEx interface.
 */
EXPORT Connection *
ConnAcceptEx(Connection *Server, char *file, int line)
{
	ConnectionPriv *server = ( ConnectionPriv *)Server;
	ConnectionPriv *child;

	errno = 0;

	if( notNull( server ) ) {
		RememberAPICalls( server, "ConnAccept", file, line );
		if( notShuttingHandleDown( server->connHandle ) ) {
			if( ( child = _ConnAllocEx( server->connHandle, NULL, file, line) ) ) {
				RememberAPICalls( child, "ConnAccept", file, line );
				if( ( child->sp = SocketAccept( server->sp ) ) ) {
					return ( Connection * )child;
				}
				connFreeEx( child, file, line );
			}
		}
	}
	return NULL;
}


EXPORT XplBool
ConnKillEx( Connection *Conn, char *file, int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool result = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnCancel", file, line );
		return( connKillEx( conn, file, line ) );
	}

	return( result );
}

EXPORT XplBool
ConnDisconnectEx( Connection *Conn, int timeout, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	XplBool result = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnDisconnect", file, line );
		_ConnEventInit( &connEvent, conn );
		if( SynchronousConnectEventInsert( conn, &connEvent ) ) {
			if( connIsStillConnected( conn ) && connIsNotSecured( conn ) ) {
				ImplicitFlushOnConnectEvent( &connEvent, conn, file, line );
				ConnEventAssign( &connEvent, NULL, 0, timeout, CONN_EVENT_DISCONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
				if( ConnEventProcess( &( connEvent ) ) == CONN_EVENT_COMPLETE ) {
					result = TRUE;
				}
			}
			SynchronousConnectEventRemove( conn, &connEvent );
		} else {
			DebugAssert( 0 );  // this attempt failed because this conn is actively being used;
			/*
			   If this function is being called by a thread trying to get this conn to error out,
			   it should be using ConnCancel().  If this thread thinks it has a legal right to this
			   conn, there is a bug in the application.
			*/
		}
	}

	return( result );
}

EXPORT void
ConnADisconnectEx( Connection *Conn, int timeout, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnADisconnect", file, line );
		connEvent = connEventAlloc( conn, file, line );
		if( connEvent ) {
			_ConnEventInit( connEvent, conn );
			ConnEventAssign( connEvent, NULL, 0, timeout, CONN_EVENT_DISCONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
			if( EventEnqueue( conn, connEvent, file, line ) ) {
				return;
			}
			ConnEventFree( conn, connEvent );
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT Connection *
ConnConnectEx(ConnHandle *pubHandle, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int timeout, ConnCreateCB cccb, void *cccbArg, char *file, int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnEvent connEvent;
	ConnectionPriv *conn;
	errno = 0;

	if( notNull( handle ) && ( conn = _ConnAllocEx( handle, NULL, file, line) ) ) {
		if( cccb ) {
			cccb( ( Connection * )conn, cccbArg );
		}
		RememberAPICalls( conn, "ConnConnect", file, line );
		if( notShuttingHandleDown( handle ) ) {
			if( validConnIOArgs( conn, destSaddr, destSaddrLen ) > 0  && validFileAndLine(file,line)) {
				_ConnEventInit( &connEvent, conn );
				if( SynchronousConnectEventInsert( conn, &connEvent ) ) {
					/*
					  ConnectAddressesCopy must follow ConnEventInit because the memset
					  in ConnEventInit will wipe out the addresses copied there by ConnectAddressesCopy

					*/
					ConnEventAssign( &connEvent, NULL, 0, timeout, CONN_EVENT_CONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
					ConnectAddressesCopy( destSaddr, destSaddrLen, srcSaddr, srcSaddrLen, &connEvent );
					if( ConnEventProcess( &( connEvent ) ) == CONN_EVENT_COMPLETE ) {
						SynchronousConnectEventRemove( conn, &connEvent );
						return( ( Connection * )conn );
					}
					SynchronousConnectEventRemove( conn, &connEvent );
				}
			}
		}
		connFreeEx( conn, file, line );
	}
	return NULL;
}

EXPORT Connection *
ConnAConnectEx(ConnHandle *pubHandle, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int timeout, ConnCreateCB cccb, void *cccbArg, const char *file, const int line)
{
	ConnHandlePriv *handle = (ConnHandlePriv *)pubHandle;
	ConnEvent *connEvent;
	ConnectionPriv *conn;

	errno = 0;
	if( notNull( handle ) && asyncEnabledHandle( handle ) && notShuttingHandleDown( handle ) ) {
		if( ( conn = _ConnAllocEx( handle, NULL, file, line) ) ) {
			if( cccb ) {
				cccb( ( Connection * )conn, cccbArg );
			}
			RememberAPICalls( conn, "ConnAConnect", file, line );
			if( validConnIOArgs( conn, destSaddr, destSaddrLen ) > 0  && validFileAndLine(file,line)) {
				if( ( connEvent = connEventAlloc( conn, file, line ) ) ) {
					/*
					  ConnectAddressesDuplicate must follow ConnEventInit because the memset
					  in ConnEventInit will wipe out the addresses copied there by ConnectAddressesDuplicate

					  Because the event is asynchronous the addresses must be duplicated because the stack
					  arguments will not be around when the address is needed

					*/
					_ConnEventInit( connEvent, conn );
					ConnEventAssign( connEvent, NULL, 0, timeout, CONN_EVENT_CONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
					if( ConnectAddressesDuplicate( destSaddr, destSaddrLen, srcSaddr, srcSaddrLen, connEvent ) ) {
						/* just queue the connect event */
						if( EventEnqueue( conn, connEvent, file, line ) ) {
							return ( Connection *)conn;
						}
						ConnectAddressesFreeCopy( connEvent );
					}
					ConnEventFree( conn, connEvent );
				}
			}
			connFreeEx( conn, file, line );
		}
	}
	return NULL;
}

EXPORT XplBool
ConnSSLConnectEx(Connection *Conn, int timeout, XplBool tls, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	XplBool result = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnSSLConnect", file, line );
		if( notShuttingDown( conn ) ) {
			_ConnEventInit( &connEvent, conn );
			if( SynchronousConnectEventInsert( conn, &connEvent ) ) {
				if( connIsStillConnected( conn ) && connIsNotSecured( conn ) ) {
					ImplicitFlushOnConnectEvent( &connEvent, conn, file, line );
					ConnEventAssign( &connEvent, NULL, 0, timeout, tls ? CONN_EVENT_TLS_CONNECT : CONN_EVENT_SSL_CONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
					if( ConnEventProcess( &( connEvent ) ) == CONN_EVENT_COMPLETE ) {
						result = TRUE;
					}
				}
				SynchronousConnectEventRemove( conn, &connEvent );
			}
		}
	}

	return( result );
}

EXPORT void
ConnASSLConnectEx(Connection *Conn, int timeout, XplBool tls, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn ) && notShuttingDown( conn ) ) {
		RememberAPICalls( conn, "ConnASSLConnect", file, line );
		connEvent = connEventAlloc( conn, file, line );
		if( connEvent ) {
			_ConnEventInit( connEvent, conn );
			ConnEventAssign( connEvent, NULL, 0, timeout, tls ? CONN_EVENT_TLS_CONNECT : CONN_EVENT_SSL_CONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
			if( EventEnqueue( conn, connEvent, file, line ) ) {
				return;
			}
			ConnEventFree( conn, connEvent );
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT XplBool
ConnSSLAcceptEx(Connection *Conn, int timeout, XplBool tls, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	XplBool result = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnSSLAccept", file, line );
		if( notShuttingDown( conn ) ) {
			_ConnEventInit( &connEvent, conn );
			if( SynchronousConnectEventInsert( conn, &connEvent ) ) {
				if( connIsStillConnected( conn ) && connIsNotSecured( conn ) ) {
					ImplicitFlushOnConnectEvent( &connEvent, conn, file, line );
					ConnEventAssign( &connEvent, NULL, 0, timeout, tls ? CONN_EVENT_TLS_ACCEPT : CONN_EVENT_SSL_ACCEPT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
					if( ConnEventProcess( &( connEvent ) ) == CONN_EVENT_COMPLETE ) {
						result = TRUE;
					}
				}
				SynchronousConnectEventRemove( conn, &connEvent );
			}
		}
	}

	return( result );
}

EXPORT void
ConnASSLAcceptEx(Connection *Conn, int timeout, XplBool tls, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnASSLAccept", file, line );
		if( notShuttingDown( conn ) ) {
			connEvent = connEventAlloc( conn, file, line );
			if( connEvent ) {
				_ConnEventInit( connEvent, conn );
				ConnEventAssign( connEvent, NULL, 0, timeout, tls ? CONN_EVENT_TLS_ACCEPT : CONN_EVENT_SSL_ACCEPT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
				if( EventEnqueue( conn, connEvent, file, line ) ) {
					return;
				}
				ConnEventFree( conn, connEvent );
			}
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT XplBool
ConnSSLDisconnectEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	XplBool result = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnSSLDisconnect", file, line );
		_ConnEventInit( &connEvent, conn );
		if( SynchronousConnectEventInsert( conn, &connEvent ) ) {
			if( connIsSecured( conn ) ) {
				ImplicitFlushOnConnectEvent( &connEvent, conn, file, line );
				ConnEventAssign( &connEvent, conn, 0, -1, CONN_EVENT_SSL_DISCONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_SYNCHRONOUS, NULL, file, line );
				if( ConnEventProcess( &( connEvent ) ) == CONN_EVENT_COMPLETE ) {
					result = TRUE;
				}
				errno = connEvent.error;
			}
			SynchronousConnectEventRemove( conn, &connEvent );
		}
	}

	return( result );
}

INLINE static ConnEvent *
sslDisconnectEventCreate( ConnectionPriv *conn, int timeout, const char *file, const int line )
{
	ConnEvent *ce;

	ce = connEventAlloc( conn, file, line );
	if( ce ) {
		_ConnEventInit( ce, conn );
		ConnEventAssign( ce, NULL, 0, timeout, CONN_EVENT_SSL_DISCONNECT, CONN_EVENT_CLASS_CONNECT, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
		return( ce );
	}
	return( NULL );
}

ConnEvent *
SSLDisconnectEventCreate( ConnectionPriv *conn, int timeout, const char *file, const int line )
{
	return( sslDisconnectEventCreate( conn, timeout, file, line ) );;
}


EXPORT void
ConnASSLDisconnectEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnASSLDisconnect", file, line );
		connEvent = sslDisconnectEventCreate( conn, -1, file, line );
		if( connEvent ) {
			if( EventEnqueue( conn, connEvent, file, line ) ) {
				return;
			}
			ConnEventFree( conn, connEvent );
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT XplBool
ConnSyncReadyEx(Connection *Conn, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool success = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnSyncReady", file, line );
		if( conn->state == CONN_STATE_IDLE ) {
			success = TRUE;
		}
	}
	return success;
}

EXPORT XplBool
ConnIsSecureEx(Connection *Conn, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool success = FALSE;

	errno = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnIsSecure", file, line );
		if ( conn->sp->ssl.conn ) {
			success = TRUE;
		}
	}
	return success;
}

INLINE static int
ConnReadToCondition(ConnectionPriv *conn, char *api, char *buff, int buffSize, ConnEventType type, char *pattern, ConnEventMode mode, uint32 dataMode, int timeout, const char *file, const int line)
{
	ConnEvent connEvent;
	IOBuffer *bufferedSendData;
	XplBool flushHappened;

	if( validFileAndLine(file,line) && notNull( conn )  ) {
		RememberAPICalls( conn, api, file, line );
		if( !errno ) {
			if( ( type != CONN_EVENT_READ_UNTIL_PATTERN ) || notNegative( sizeof( connEvent.args.buffer.pattern ) - ( strlen( pattern ) + 1 ) ) ) {
				if( notShuttingDown( conn ) ) {
					_ConnEventInit( &connEvent, conn );
					if( SynchronousReceiveEventInsert( conn, &connEvent, &bufferedSendData ) ) {
						if( bufferedSendData ) {
							flushHappened = TRUE;
							ConnEventAssign( &connEvent, NULL, 0, timeout, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, bufferedSendData, file, line );
							ConnEventProcess( &( connEvent ) );
						} else {
							flushHappened = FALSE;
						}
						ConnEventAssign( &connEvent, buff, buffSize, timeout, type, CONN_EVENT_CLASS_RECEIVE, mode, conn->receiveBuffer, file, line );
						/* these variable must be set after ConnEventInt so they do not get clobbered by the memset */
						connEvent.dataMode = dataMode;
						if( type == CONN_EVENT_READ_UNTIL_PATTERN ) {
							strcpy( connEvent.args.buffer.pattern, pattern );
						}
						ConnEventProcess( &( connEvent ) );
						SynchronousReceiveEventRemove( conn, &connEvent, flushHappened );
						errno = connEvent.error;
						return( connEvent.transferred );
					}
				}
			}
		}
	}
	return( 0 );
}

EXPORT int
ConnReadEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnRead",
								 buff, buffSize,
								 CONN_EVENT_READ, NULL,
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE,
								 timeout, file, line ) );
}

EXPORT int
ConnReadCountEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadCount",
								 buff, buffSize,
								 CONN_EVENT_READ_COUNT, NULL,
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE,
								 timeout, file, line ) );
}

EXPORT int
ConnReadLineEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadLine",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, "\n",
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_INCLUDE_PATTERN | CONN_READ_TERMINATE,
								 timeout, file, line ) );
}

EXPORT int
ConnReadCRLFEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadCRLF",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, SPECIAL_PATTERN_CRLF,
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_TERMINATE,
								 timeout, file, line ) );
}

EXPORT int
ConnReadGetSEx(Connection *Conn, char *buff, int buffSize, XplBool includeLF, int timeout, const char *file, const int line)
{
	uint32 mode;

	if( includeLF ) {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE | CONN_READ_INCLUDE_PATTERN;
	} else {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE;
	}
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnGetS",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, "\n",
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 mode,
								 timeout, file, line ) );
}

EXPORT int
ConnReadAnswerEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadAnswer",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, "\n",
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_TERMINATE,
								 timeout, file, line ) );
}

EXPORT int
ConnReadUntilPatternEx(Connection *Conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadUntilPattern",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, pattern,
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_COPY | CONN_READ_DELETE,
								 timeout, file, line ) );
}

EXPORT int
ConnEatCountEx(Connection *Conn, int count, int timeout, const char *file, const int line)
{
	errno = 0;
	isPos( count );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnReadCount",
								 NULL, 0,
								 CONN_EVENT_READ_COUNT, NULL,
								 CONN_EVENT_MODE_SYNCHRONOUS,
								 CONN_READ_DELETE,
								 timeout, file, line ) );
}

EXPORT int
ConnPeekEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnPeek",
								 buff, buffSize,
								 CONN_EVENT_READ, NULL,
								 CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
								 CONN_READ_COPY,
								 timeout, file, line ) );
}

EXPORT int
ConnPeekCountEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnPeekCount",
								 buff, buffSize,
								 CONN_EVENT_READ_COUNT, NULL,
								 CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
								 CONN_READ_COPY,
								 timeout, file, line ) );
}

EXPORT int
ConnPeekLineEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnPeekLine",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, "\n",
								 CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
								 CONN_READ_COPY | CONN_READ_INCLUDE_PATTERN | CONN_READ_TERMINATE,
								 timeout, file, line ) );
}

EXPORT int
ConnPeekAnswerEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnPeekAnswer",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, "\n",
								 CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
								 CONN_READ_COPY | CONN_READ_TERMINATE,
								 timeout, file, line ) );
}

EXPORT int
ConnPeekUntilPatternEx(Connection *Conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	return( ConnReadToCondition( (ConnectionPriv *)Conn, "ConnPeekUntilPattern",
								 buff, buffSize,
								 CONN_EVENT_READ_UNTIL_PATTERN, pattern,
								 CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
								 CONN_READ_COPY,
								 timeout, file, line ) );
}

INLINE static void
ConnAReadToCondition(ConnectionPriv *conn, char *api, char *buff, int buffSize, ConnEventType type, char *pattern, uint32 dataMode, int timeout, const char *file, const int line )
{
	ConnEvent *connEvent;

	if( validFileAndLine(file,line) && notNull( conn ) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, api, file, line );
		if( !errno ) {
			if( ( type != CONN_EVENT_READ_UNTIL_PATTERN ) || notNegative( sizeof( connEvent->args.buffer.pattern ) - ( strlen( pattern ) + 1 ) ) ) {
				if( notShuttingDown( conn ) ) {
					connEvent = connEventAlloc( conn, file, line );
					if( connEvent ) {
						_ConnEventInit( connEvent, conn );
						ConnEventAssign( connEvent, buff, buffSize, timeout, type, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_ASYNCHRONOUS, conn->receiveBuffer, file, line );
						connEvent->dataMode = dataMode;
						if( type == CONN_EVENT_READ_UNTIL_PATTERN ) {
							strcpy( connEvent->args.buffer.pattern, pattern );
						}
						if( EventEnqueue( conn, connEvent, file, line ) ) {
							return;
						}
						ConnEventFree( conn, connEvent );
					}
				}
			}
		}
		EventEnqueueFailure( conn, file, line );
	} else {
		DebugAssert( 0 );
	}
	return;
}



EXPORT void
ConnAReadEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnARead",
						  buff, buffSize,
						  CONN_EVENT_READ, NULL,
						  CONN_READ_COPY | CONN_READ_DELETE,
						  timeout, file, line );
}

EXPORT void
ConnAReadCountEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAReadCount",
						  buff, buffSize,
						  CONN_EVENT_READ_COUNT, NULL,
						  CONN_READ_COPY | CONN_READ_DELETE,
						  timeout, file, line );
}

EXPORT void
ConnAReadLineEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAReadLine",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_INCLUDE_PATTERN | CONN_READ_TERMINATE,
						  timeout, file, line );
}

EXPORT void
ConnAReadCRLFEx(Connection *Conn, char *buff, int buffSize, XplBool includeLF, int timeout, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line)
{
	ConnectionPriv *conn = ( ConnectionPriv *)Conn;
	uint32 mode;

	if( includeLF ) {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE | CONN_READ_INCLUDE_PATTERN;
	} else {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE;
	}

	DebugAssert( ( conn->state == CONN_STATE_IDLE ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) );  // the caller of this function is not in state where this function can be called.

	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( conn, "ConnAReadCRLF",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, SPECIAL_PATTERN_CRLF,
						  mode,
						  timeout, file, line );
	EventsSubmit( conn, successCB, failureCB, client, cbThreadEngine, file, line );
}

EXPORT void
ConnAGetSEx(Connection *Conn, char *buff, int buffSize, XplBool includeLF, int timeout, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	uint32 mode;

	if( includeLF ) {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE | CONN_READ_INCLUDE_PATTERN;
	} else {
		mode = CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_PARTIAL_OK | CONN_READ_TERMINATE;
	}

	DebugAssert( ( conn->state == CONN_STATE_IDLE ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) );  // the caller of this function is not in state where this function can be called.

	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAGetS",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  mode,
						  timeout, file, line );
	EventsSubmit( conn, successCB, failureCB, client, cbThreadEngine, file, line );
}

EXPORT void
ConnAReadAnswerEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAReadAnswer",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  CONN_READ_COPY | CONN_READ_DELETE | CONN_READ_TERMINATE,
						  timeout, file, line );
}

EXPORT void
ConnAReadUntilPatternEx(Connection *Conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAReadUntilPattern",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, pattern,
						  CONN_READ_COPY | CONN_READ_DELETE,
						  timeout, file, line );
}

EXPORT void
ConnAEatCountEx(Connection *Conn, int count, int timeout, const char *file, const int line)
{
	errno = 0;
	isPos( count );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAReadCount",
						  NULL, count,
						  CONN_EVENT_READ_COUNT, NULL,
						  CONN_READ_DELETE,
						  timeout, file, line );
}


EXPORT void
ConnAPeekEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAPeek",
						  buff, buffSize,
						  CONN_EVENT_READ, NULL,
						  CONN_READ_COPY,
						  timeout, file, line );
}

EXPORT void
ConnAPeekCountEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAPeekCount",
						  buff, buffSize,
						  CONN_EVENT_READ_COUNT, NULL,
						  CONN_READ_COPY,
						  timeout, file, line );
}

EXPORT void
ConnAPeekLineEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAPeekLine",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  CONN_READ_COPY | CONN_READ_TERMINATE,
						  timeout, file, line );
}

EXPORT void
ConnAPeekAnswerEx(Connection *Conn, char *buff, int buffSize, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAPeekAnswer",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  CONN_READ_COPY | CONN_READ_INCLUDE_PATTERN | CONN_READ_TERMINATE,
						  timeout, file, line );
}

EXPORT void
ConnAPeekUntilPatternEx(Connection *Conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line)
{
	errno = 0;
	notNull( buff );
	isPos( buffSize );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnAPeekUntilPattern",
						  buff, buffSize,
						  CONN_EVENT_READ_UNTIL_PATTERN, pattern,
						  CONN_READ_COPY,
						  timeout, file, line );
}

EXPORT void
ConnATestReadEx(Connection *Conn, int timeout, const char *file, const int line)
{
	errno = 0;
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnATest",
						  NULL, 0,
						  CONN_EVENT_READ, NULL,
						  0,
						  timeout, file, line );
}

EXPORT void
ConnATestReadCountEx(Connection *Conn, int count, int timeout, const char *file, const int line)
{
	errno = 0;
	isPos( count );
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnATestCount",
						  NULL, count,
						  CONN_EVENT_READ_COUNT, NULL,
						  0,
						  timeout, file, line );
}

EXPORT void
ConnATestReadLineEx(Connection *Conn, int timeout, const char *file, const int line)
{
	errno = 0;
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnATestLine",
						  NULL, 0,
						  CONN_EVENT_READ_UNTIL_PATTERN, "\n",
						  0,
						  timeout, file, line );
}

EXPORT void
ConnATestReadCRLFEx(Connection *Conn, int timeout, const char *file, const int line)
{
	errno = 0;
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnATestCRLF",
						  NULL, 0,
						  CONN_EVENT_READ_UNTIL_PATTERN, SPECIAL_PATTERN_CRLF,
						  0,
						  timeout, file, line );
}

EXPORT void
ConnATestReadUntilPatternEx(Connection *Conn, char *pattern, int timeout, const char *file, const int line)
{
	errno = 0;
	ConnAReadToCondition( ( ConnectionPriv *)Conn, "ConnATestUntilPattern",
						  NULL, 0,
						  CONN_EVENT_READ_UNTIL_PATTERN, pattern,
						  0,
						  timeout, file, line );
}

EXPORT int
ConnReadToCBEx(Connection *Conn, ConnIOCB cb, void *cbArg, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	IOBuffer *bufferedSendData;
	XplBool flushHappened;

	errno = 0;

	/* a zero count means: keep calling callback until callback returns zero */
	if( ( validConnIOArgs2( conn, cb, count ) > 0 ) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnReadToCB", file, line );
		if( notShuttingDown( conn ) ) {
			_ConnEventInit( &connEvent, conn );
			if( SynchronousReceiveEventInsert( conn, &connEvent, &bufferedSendData  ) ) {
				if( bufferedSendData ) {
					flushHappened = TRUE;
					ConnEventAssign( &connEvent, NULL, 0, timeout, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, bufferedSendData, file, line );
					ConnEventProcess( &( connEvent ) );
				} else {
					flushHappened = FALSE;
				}
				ConnEventAssign( &connEvent, NULL, count, timeout, CONN_EVENT_READ_TO_CB, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_SYNCHRONOUS, conn->receiveBuffer, file, line );
				connEvent.args.cb.func = cb;
				connEvent.args.cb.arg = cbArg;
				ConnEventProcess( &( connEvent ) );
				SynchronousReceiveEventRemove( conn, &connEvent, flushHappened );
				errno = connEvent.error;
				return( connEvent.transferred );
			}
		}
	}
	return( 0 );
}

EXPORT void
ConnAReadToCBEx(Connection *Conn, ConnIOCB cb, void *cbArg, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	/* a zero count means: keep calling callback until callback returns zero */
	if( ( validConnIOArgs2( conn, cb, count ) > 0 ) && asyncEnabled( conn ) && validFileAndLine(file,line) ) {
		RememberAPICalls( conn, "ConnAReadToCB", file, line );
		if( notShuttingDown( conn ) ) {
			connEvent = connEventAlloc( conn, file, line );
			if( connEvent ) {
				_ConnEventInit( connEvent, conn );
				ConnEventAssign( connEvent, NULL, count, timeout, CONN_EVENT_READ_TO_CB, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_ASYNCHRONOUS, conn->receiveBuffer, file, line );
				connEvent->args.cb.func = cb;
				connEvent->args.cb.arg = cbArg;
				if( EventEnqueue( conn, connEvent, file, line ) ) {
					return;
				}
				ConnEventFree( conn, connEvent );
			}
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

/*! \fn		 int ConnReadToConn(Connection *src, Connection *dest, int count)
	\brief	  A brief explanation.

				A detailed explanation.
	\param[in]  src
	\param[in]  dest
	\param[in]  count
	\return	 On success ...
	\retval	 <count>
	\retval	 <ENOSYS>
	\todo	   Complete the ConnReadToConn interface.
	\todo	   Complete the documentation for the ConnReadToConn interface.
 */
EXPORT int
ConnReadToConnEx(Connection *Conn, Connection *dest, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	IOBuffer *bufferedSendData;

	errno = 0;

	if( (validConnIOArgs( conn, dest, count ) > 0) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnReadToConn", file, line );
		if( notShuttingDown( conn ) ) {
			_ConnEventInit( &connEvent, conn );
			if( SynchronousReceiveEventInsert( conn, &connEvent, &bufferedSendData ) ) {
				if( bufferedSendData ) {
					ConnEventAssign( &connEvent, dest, count, timeout, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, bufferedSendData, file, line );
					ConnEventProcess( &( connEvent ) );
					ConnEventAssign( &connEvent, dest, count, timeout, CONN_EVENT_READ_TO_CONN, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_SYNCHRONOUS, conn->receiveBuffer, file, line );
					ConnEventProcess( &( connEvent ) );
					SynchronousReceiveEventRemove( conn, &connEvent, TRUE );
				} else {
					ConnEventAssign( &connEvent, dest, count, timeout, CONN_EVENT_READ_TO_CONN, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_SYNCHRONOUS, conn->receiveBuffer, file, line );
					ConnEventProcess( &( connEvent ) );
					SynchronousReceiveEventRemove( conn, &connEvent, FALSE );
				}
				errno = connEvent.error;
				return( connEvent.transferred );
			}
		}
	}

	return( 0 );
}

EXPORT void
ConnAReadToConnEx(Connection *Conn, Connection *dest, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;

	errno = 0;

	if( ( validConnIOArgs( conn, dest, count ) > 0 ) && validFileAndLine(file,line) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnAReadToConn", file, line );
		if( notShuttingDown( conn ) ) {
			connEvent = connEventAlloc( conn, file, line );
			if( connEvent ) {
				_ConnEventInit( connEvent, conn );
				ConnEventAssign( connEvent, dest, count, timeout, CONN_EVENT_READ_TO_CONN, CONN_EVENT_CLASS_RECEIVE, CONN_EVENT_MODE_ASYNCHRONOUS, conn->receiveBuffer, file, line );
				if( EventEnqueue( conn, connEvent, file, line ) ) {
					return;
				}
				ConnEventFree( conn, connEvent );
			}
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT int
ConnFlushEx(Connection *Conn, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int transferred;
	IOBuffer *iob;

	errno = 0;
	transferred = 0;

	if( notNull( conn ) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnFlush", file, line );
		iob = bufferedSendDataGet( conn, WITH_DATA );
		if( iob ) {
			transferred = connFlushEx( conn, iob, timeout, file, line );
		}
	}

	return( transferred );
}

/* the caller of this function must guarantee that writeBuffer is non-NULL and has data */
INLINE static ConnEvent *
flushAEventCreate( ConnectionPriv *conn, IOBuffer *writeBuffer, int timeout, const char *file, const int line )
{
	ConnEvent *ce;

	DebugAssert( writeBuffer && ( IOBGetUsed( writeBuffer, NULL ) > 0 ) ); // no data to flush - the calling function messed up
	ce = connEventAlloc( conn, file, line );
	if( ce ) {
		_ConnEventInit( ce, conn );
		ConnEventAssign( ce, NULL, 0, timeout, CONN_EVENT_FLUSH, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_ASYNCHRONOUS, writeBuffer, file, line );
		return( ce );
	}
	IOBFree( writeBuffer );
	return( NULL );
}

XplBool FlushAEventNeededEx( ConnectionPriv *conn, ConnEvent *connEvent, ConnEvent **flushEvent, const char *file, const int line )
{
	IOBuffer *writeBuffer;

	if( connEvent->eventClass == CONN_EVENT_CLASS_SEND ) {
		/*
		   writes are still coming in -
		   no need to do an implicit flush yet
		*/
		return( FALSE );
	}

	/*
	  the current event is a 'read' or 'connect'.
	  in either case, any buffered write data
	  needs flushed
	*/

	writeBuffer = bufferedSendDataGet( conn, WITH_DATA );
	if( !writeBuffer ) {
		return( FALSE );
	}
	/*
	  there have been writes that were not followed by a flush
	  insert a flush event
	*/
	*flushEvent = flushAEventCreate( conn, writeBuffer, -1, file, line );

	return( TRUE );
}

/* the caller of this function must guarantee that writeBuffer is non-NULLs and has data */
static void
connAFlushEx( ConnectionPriv *conn, IOBuffer *writeBuffer, int timeout, const char *file, const int line )
{
	ConnEvent *connEvent;

	connEvent = flushAEventCreate( conn, writeBuffer, timeout, file, line );
	if( connEvent ) {
		if( EventEnqueue( conn, connEvent, file, line ) ) {
			return;
		}
		ConnEventFree( conn, connEvent );
	}
	EventEnqueueFailure( conn, file, line );
}

EXPORT void
ConnAFlushEx(Connection *Conn, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	IOBuffer *iob;
	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn )  && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnAFlush", file, line );
		iob = bufferedSendDataGet( conn, WITH_DATA );
		if( iob ) {
			connAFlushEx( conn, iob, timeout, file, line );
		}
	}
	return;
}

static void startWriter( ConnectionPriv *conn )
{
#ifdef DEBUG
	XplMutexLock( conn->mutex );
	DebugAssert( 0 == conn->writers ); // The consumer of connio currently has more than one thread writing to this conn.  Consider a mutex in the consumer.
	conn->writers++;
	XplMutexUnlock( conn->mutex );
#endif
}

static void endWriter( ConnectionPriv *conn )
{
#ifdef DEBUG
	XplMutexLock( conn->mutex );
	DebugAssert( 1 == conn->writers );
	conn->writers--;
	XplMutexUnlock( conn->mutex );
#endif
}

static int
writeClientData( ConnectionPriv *conn, char *data, int length, XplBool async, int timeout, const char *file, const int line )
{
	int transferred;
	int remaining;
	int bufferSpace;
	char *buffer;
	IOBuffer *writeBuffer;

	startWriter( conn );
	if( async ) {
		if( conn->state == CONN_STATE_IDLE ) {
			ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_CLIENT );
		}
		DebugAssert( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT );
	}
	transferred = 0;
	if( length > 0 ) {
		TraceApiEvent( conn->sp, TRACE_EVENT_WRITE, data, length, file, line );
		for( ; ; ) {
			writeBuffer = bufferedSendDataGet( conn, EMPTY_OK );
			if( !writeBuffer  ) {
				errno = ENOMEM;
				if( async ) {
					EventEnqueueFailure( conn, file, line );
				}
				endWriter( conn );
				return transferred;
			}
			while( ( bufferSpace = IOBGetFree( writeBuffer, &buffer ) ) > 0 ) {
				remaining = length - transferred;
				if( bufferSpace > remaining ) {
					IOBWrite( writeBuffer, data + transferred, remaining );
					transferred += remaining;
					bufferedSendDataKeep( conn, writeBuffer );
					endWriter( conn );
					return transferred;
				}
				IOBWrite( writeBuffer, data + transferred, bufferSpace );
				transferred += bufferSpace;
			}
			/* the buffer is full - flush it */
			if( async ) {
				connAFlushEx( conn, writeBuffer, timeout, file, line );
			} else {
				connFlushEx( conn, writeBuffer, timeout, file, line );
			}
			if( errno ) {
				endWriter( conn );
				return transferred;
			}
		}
	}
	endWriter( conn );
	return 0;
}

int connWriteEx( ConnectionPriv *conn, char *data, int length, int timeout, const char *file, const int line)
{
	int transferred;

	if( notShuttingDown( conn ) ) {
		transferred = writeClientData( conn, data, length, FALSE, timeout, file, line );
	} else {
		transferred = 0;
	}
	return transferred;
}

/*! \fn		 int ConnWrite(Connection *Conn, const char *buffer, int length)
	\brief	  A brief explanation.

				A detailed explanation.
	\param[in]  conn
	\param[in]  buffer
	\param[in]  length
	\return	 On success ...
	\retval	 <count>
	\retval	 <ENOSYS>
	\todo	   Complete the ConnWrite interface.
	\todo	   Complete the documentation for the ConnWrite interface.
 */

EXPORT int
ConnWriteEx(Connection *Conn, char *data, int length, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int transferred;

	errno = 0;
	transferred = 0;

	if( (validConnIOArgs( conn, data, length ) > 0) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnWrite", file, line );
		if( notShuttingDown( conn ) ) {
			transferred = writeClientData( conn, data, length, FALSE, timeout, file, line );
		}
	}
	return( transferred );
}

EXPORT int
ConnAWriteBacklogEx(Connection *Conn, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int count;

	errno = 0;

	if( notNull( conn ) && asyncEnabled( conn )  && validFileAndLine( file,line ) ) {
		RememberAPICalls( conn, "ConnAWriteBacklog", file, line );
		count = EventQueueCountFlushes( conn );
	} else {
		count = 0;
	}
	return count;
}

EXPORT void
ConnAWriteEx(Connection *Conn, char *data, int length, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	errno = 0;

	if( ( validConnIOArgs( conn, data, length ) > 0 ) && validFileAndLine(file,line) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnAWrite", file, line );
		writeClientData( conn, data, length, TRUE, timeout, file, line );
	}
	return;
}

EXPORT void
ConnAWriteFromBufferEx(Connection *Conn, char *data, int length, ConnAFreeCB freeCB, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
 	ConnEvent *connEvent;

	errno = 0;

 	if( notNull( freeCB ) && ( validConnIOArgs( conn, data, length ) > 0 ) && validFileAndLine(file,line) && asyncEnabled( conn ) ) {
 		RememberAPICalls( conn, "ConnAWriteFromBuffer", file, line );
 		if( length < CONN_IOB_SIZE ) {
 			writeClientData( conn, data, length, TRUE, timeout, file, line );
 			freeCB( data );
 		} else {
 			/* if we have any buffered data, flush it */
 			IOBuffer *writeBuffer;

 			writeBuffer = bufferedSendDataGet( conn, WITH_DATA );
 			if( writeBuffer ) {
 				connAFlushEx( conn, writeBuffer, timeout, file, line );
 			}
			TraceApiEvent( conn->sp, TRACE_EVENT_WRITE, data, length, file, line );
 			/* Create and queue an event for the client's buffer */
 			if( ( connEvent = connEventAlloc( conn, file, line ) ) ) {
 				_ConnEventInit( connEvent, conn );
 				ConnEventAssign( connEvent, data, length, timeout, CONN_EVENT_WRITE_FROM_CLIENT_BUFFER, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_ASYNCHRONOUS, NULL, file, line );
 				/* The setting of connEvent->args.buffer.cb must follow ConnEventInit because the memset in ConnEventInit */
 				connEvent->args.buffer.freeCB = freeCB;
 				if( EventEnqueue( conn, connEvent, file, line ) ) {
 					return;
 				}
 				ConnEventFree( conn, connEvent );
 			} else {
 				freeCB( data );
 			}
 			EventQueueAbort( conn, NULL, CONN_EVENT_FAILED, ENOMEM, file, line );
 		}
 	}
 	return;
}

EXPORT int
ConnWriteStringEx(Connection *Conn, char *string, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int transferred;

	errno = 0;
	transferred = 0;

	if( (validConnIOArgs( conn, string, 1 ) > 0) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnWriteString", file, line );
		if( notShuttingDown( conn ) ) {
			transferred = writeClientData( conn, string, strlen( string ), FALSE, timeout, file, line );
		}
	}

	return( transferred );
}

EXPORT void
ConnAWriteStringEx(Connection *Conn, char *string, XplBool freeBuff, ConnAFreeCB freeCB, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	errno = 0;

	if( ( validConnIOArgs( conn, string, 1 ) > 0 ) && validFileAndLine(file,line) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnAWriteString", file, line );
		writeClientData( conn, string, strlen( string ), TRUE, timeout, file, line );
		if( freeBuff ) {
			if( freeCB ) {
				freeCB( string );
			} else {
				MemFree( string );
			}
		}
	}
	return;
}


#define MAX_CONN_WRITE_FORMATTED_SIZE 1024 * 1024 * 64

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
	} while( bufferSize < MAX_CONN_WRITE_FORMATTED_SIZE );
	errno = EMSGSIZE;
	return;
}

EXPORT int
ConnWriteVEx( Connection *Conn, const char *format, va_list arguments, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	char *out = NULL;
	int outSize;
	char stackBuffer[ STACK_BUFFER_SIZE ];
	int transferred;

	errno = 0;
	transferred = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnWriteV", file, line );
		if( notShuttingDown( conn ) ) {
			outSize = XplVsnprintf( stackBuffer, sizeof( stackBuffer ), format, arguments );
			if( ( outSize >= 0 ) && ( outSize < sizeof( stackBuffer ) ) ) {
				/* fits in the stack buffer */
				transferred = writeClientData( conn, stackBuffer, outSize, FALSE, timeout, file, line );
			} else {
				/* we need to allocate a buffer */
				CreateFormattedString( &out, &outSize, format, arguments );
				if( out ) {
					transferred = writeClientData( conn, out, outSize, FALSE, timeout, file, line );
					MemFree( out );
				}
			}
		}
	}
	return( transferred );
}


EXPORT int
ConnWriteF( Connection *Conn, char *format, ...)
{
	int ret;
	va_list arguments;

	errno = 0;

	va_start( arguments, format );
	ret = ConnWriteVEx( Conn, format, arguments, -1, "na", 0 );
	va_end( arguments);
	return( ret );
}


EXPORT void
ConnAWriteVEx( Connection *Conn, char *format, va_list arguments, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	char *out;
	int outSize;
	char stackBuffer[ STACK_BUFFER_SIZE ];

	errno = 0;
	out = NULL;

	if( ( validConnIOArgs( conn, format, 1 ) > 0 ) && validFileAndLine(file,line) && asyncEnabled( conn ) ) {
		RememberAPICalls( conn, "ConnAWriteV", file, line );

		outSize = XplVsnprintf( stackBuffer, sizeof( stackBuffer ), format, arguments );
		if( ( outSize >= 0 ) && ( outSize < sizeof( stackBuffer ) ) ) {
			/* fits in the stack buffer */
			writeClientData( conn, stackBuffer, outSize, TRUE, timeout, file, line );
		} else {
			/* we need to allocate a buffer */
			CreateFormattedString( &out, &outSize, format, arguments );
			if( out ) {
				writeClientData( conn, out, outSize, TRUE, timeout, file, line );
				MemFree( out );
			}
		}
	}
	return;
}

#if 0
EXPORT void
ConnAWriteF( Connection *Conn, char *format, ...)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	va_list arguments;

	errno = 0;

	va_start( arguments, format );
	ConnAWriteVEx( conn, format, arguments, -1, __FILE__, __LINE__ );
	va_end( arguments);
	return;
}
#endif

EXPORT void
ConnAWriteFEx( Connection *Conn, const char *file, const int line, char *format, ...)
{
	va_list arguments;

	errno = 0;

	va_start( arguments, format );
	ConnAWriteVEx( Conn, format, arguments, -1, file, line );
	va_end( arguments);
	return;
}

EXPORT int  ConnWriteFromCBEx(Connection *Conn, ConnIOCB cb, void * cbArg, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent connEvent;
	IOBuffer *writeBuffer;

	errno = 0;

	/* a zero count means: keep calling callback until callback returns zero */
	if( ( validConnIOArgs2( conn, cb, count ) > 0 ) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnWriteFromCB", file, line );
		if( notShuttingDown( conn ) ) {
			writeBuffer = bufferedSendDataGet( conn, EMPTY_OK );
			if( !writeBuffer ) {
				errno = ENOMEM;
				return 0;
			}

			_ConnEventInit( &connEvent, conn );
			if( SynchronousSendEventInsert( conn, &connEvent  ) ) {
				ConnEventAssign( &connEvent, NULL, count, timeout, CONN_EVENT_WRITE_FROM_CB, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_SYNCHRONOUS, writeBuffer, file, line );
				connEvent.args.cb.func = cb;
				connEvent.args.cb.arg = cbArg;
				ConnEventProcess( &( connEvent ) );
				SynchronousSendEventRemove( conn, &connEvent );
				errno = connEvent.error;
				return( connEvent.transferred );
			}
			bufferedSendDataKeep( conn, writeBuffer );
		}
	}

	return( 0 );
}

EXPORT void ConnAWriteFromCBEx(Connection *Conn, ConnIOCB cb, void * cbArg, int count, int timeout, const char *file, const int line)
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	ConnEvent *connEvent;
	IOBuffer *writeBuffer;

	errno = 0;

	/* a zero count means: keep calling callback until callback returns zero */
	if( ( validConnIOArgs2( conn, cb, count ) > 0 ) && asyncEnabled( conn ) && validFileAndLine(file,line)) {
		RememberAPICalls( conn, "ConnAWriteFromCB", file, line );

		connEvent = connEventAlloc( conn, file, line );
		if( connEvent ) {
			_ConnEventInit( connEvent, conn );
			writeBuffer = bufferedSendDataGet( conn, EMPTY_OK );
			if( writeBuffer ) {
				ConnEventAssign( connEvent, NULL, count, timeout, CONN_EVENT_WRITE_FROM_CB, CONN_EVENT_CLASS_SEND, CONN_EVENT_MODE_ASYNCHRONOUS, writeBuffer, file, line );
				connEvent->args.cb.func = cb;
				connEvent->args.cb.arg = cbArg;
				if( EventEnqueue( conn, connEvent, file, line ) ) {
					return;
				}
			}
			ConnEventFree( conn, connEvent );
		}
		EventEnqueueFailure( conn, file, line );
	}
	return;
}

EXPORT int
ConnGetPeerAddrEx( Connection *Conn, struct sockaddr *addr, socklen_t *addrLen, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int err = EINVAL;

	errno = 0;

	if( notNull( conn ) && notNull( addr ) ) {
		RememberAPICalls( conn, "ConnGetPeerAddr", file, line );
		err = SocketGetPeerAddr( conn->sp, addr, addrLen );
	}
	return err;
}

EXPORT int
ConnGetSockAddrEx( Connection *Conn, struct sockaddr *addr, socklen_t *addrLen, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	int err = EINVAL;

	errno = 0;

	if( notNull( conn ) && notNull( addr ) ) {
		RememberAPICalls( conn, "ConnGetSockAddr", file, line );
		err = SocketGetSockAddr( conn->sp, addr, addrLen );
	}
	return err;
}

EXPORT XplBool ConnTraceBeginEx( Connection *Conn, uint32 flags, size_t buffSize, Connection *tracedConn, char *file, int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	XplBool success = FALSE;

	errno = 0;

	if( notNull( conn ) && notShuttingDown( conn ) ) {
		RememberAPICalls( conn, "ConnTraceBegin", file, line );
		success = SocketTraceBeginEx( conn->sp, conn->connHandle->identity, conn, flags, buffSize, tracedConn ? ( ( ConnectionPriv * )tracedConn )->sp : NULL, file, line );
	}
	return success;
}

EXPORT void ConnTraceEndEx( Connection *Conn, const char *file, const int line )
{
	errno = 0;

	if( notNull( Conn ) ) {
		ConnectionPriv *conn = (ConnectionPriv *)Conn;
		RememberAPICalls( conn, "ConnTraceEnd", file, line );
		SocketTraceEnd( conn->sp );
	}
}

EXPORT uint32
ConnTraceFlagsModifyEx( Connection *Conn, uint32 addFlags, uint32 removeFlags, const char *file, const int line )
{
	uint32 newFlags = -1;

	errno = 0;

	if( notNull( Conn ) ) {
		ConnectionPriv *conn = (ConnectionPriv *)Conn;
		RememberAPICalls( conn, "ConnTraceFlagsModify", file, line );
		newFlags = SocketTraceFlagsModify( conn->sp, addFlags, removeFlags );
	}

	return newFlags;
}

EXPORT void ConnTraceFlushEx( Connection *Conn, const char *file, const int line )
{
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnTraceFlush", file, line );
		SocketTraceFlush( conn->sp );
	}
}

static int
connTracePrintVEx( ConnectionPriv *conn, const char *format, va_list arguments, const char *file, const int line)
{
	char *out = NULL;
	int outSize;
	char stackBuffer[ STACK_BUFFER_SIZE ];
	int transferred;

	transferred = 0;

	outSize = XplVsnprintf( stackBuffer, sizeof( stackBuffer ), format, arguments );
	if( ( outSize >= 0 ) && ( outSize < sizeof( stackBuffer ) ) ) {
		/* fits in the stack buffer */
		SocketLogMessage( conn->sp, stackBuffer, file, line );
	} else {
		/* we need to allocate a buffer */
		CreateFormattedString( &out, &outSize, format, arguments );
		if( out ) {
			SocketLogMessage( conn->sp, out, file, line );
			MemFree( out );
		}
	}

	return( transferred );
}

EXPORT int
ConnTracePrintVEx( Connection *Conn, const char *format, va_list arguments, const char *file, const int line)
{
	int transferred;
	ConnectionPriv *conn = (ConnectionPriv *)Conn;

	errno = 0;
	transferred = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnTracePrintV", file, line );
		transferred = connTracePrintVEx( conn, format, arguments, file, line);
	}
	return transferred;
}

EXPORT int
ConnTracePrintFEx( Connection *Conn, const char *file, const int line, char *format, ...)
{
	int transferred;
	ConnectionPriv *conn = (ConnectionPriv *)Conn;
	va_list arguments;

	errno = 0;
	transferred = 0;

	if( notNull( conn ) ) {
		RememberAPICalls( conn, "ConnTracePrintF", file, line );
		va_start( arguments, format );
		transferred = connTracePrintVEx( conn, format, arguments, file, line );
		va_end( arguments);
	}
	return transferred;
}



#if defined(WIN32)
XplBool WINAPI DllMain(HINSTANCE hInst, DWORD Reason, LPVOID Reserved)
{
	(void)Reserved;

	switch (Reason) {
		case DLL_PROCESS_ATTACH: {
			DisableThreadLibraryCalls(hInst);
			break;
		}

		case DLL_THREAD_ATTACH:
		case DLL_THREAD_DETACH:
		case DLL_PROCESS_DETACH:
		default: {
			break;
		}
	}

	return(TRUE);
}
#elif defined(NETWARE) || defined(LIBC)
void ConnSignalHandler(int sigtype)
{
	static int	signaled = FALSE;

	if (!signaled && ((sigtype == SIGTERM) || (sigtype == SIGINT))) {
		;
	}

	return;
}

int main(int argc, char *argv[])
{
	return(0);
}
#endif
