#include <xplmem.h>

#include <errno.h>
#include <log.h>

#include "sockiop.h"
#include "conniop.h"

#define getLastWsaErrno()      getLastWsaErrnoEx( __FILE__,  __LINE__ )
#define getLastErrno()         getLastErrnoEx( __FILE__,  __LINE__ )

#define SOC_FACTORY_CONCURRENCY_LIMIT 5

#if 0
#define SFPrintf(format, ...)							printf( format, ##__VA_ARGS__)
#else
#define SFPrintf(format, ...)
#endif

typedef enum {
	SOC_FACTORY_STOPPED = 0,
	SOC_FACTORY_STOPPING_FACTORY_THREAD,
	SOC_FACTORY_STOPPING_REQUEST_THREADS,
	SOC_FACTORY_RUNNING
} SocFactoryState;

typedef struct SocFactoryStruct {
	XplSemaphore createSem;
	XplSemaphore pickupSem;
	XplLock lock;
	int requests;
	int slot;
	int lastErr;
	SocFactoryState state;
	int list[ SOC_FACTORY_CONCURRENCY_LIMIT ];
} SocFactoryStruct;

SocFactoryStruct SocFactory;

static GUID acceptGUID = WSAID_ACCEPTEX;
static GUID disconnectGUID = WSAID_DISCONNECTEX;
static GUID connectGUID = WSAID_CONNECTEX;

static int wsaErrorToErrno( int wsaerr, char *file, int line )
{
	int unixerr;
	unixerr = XplTranslateError( wsaerr );
	if( unixerr == ECONNAMBIG ) {
		printf("XplTranslateError() failed to translate windows error: %d\n", wsaerr);
	}
	return unixerr;
}

static int getLastWsaErrnoEx( char *file, int line )
{
	int wsaerr = WSAGetLastError();

	if( wsaerr == ERROR_IO_PENDING ) {
		return wsaerr;
	}
	return wsaErrorToErrno( wsaerr, file, line );
}

static int getLastErrnoEx( char *file, int line )
{
	int err = GetLastError();

	return wsaErrorToErrno( err, file, line );
}

void sockPrintSysErrorEx( const char *function, char *file, int line )
{
	DebugPrintf("Connio Encountered System Error %d %s\n\tlast error: %d\n\tfunction: %s\n\tsource: %s:%d\n", errno, XplStrError( errno ), GetLastError(), function, file, line );
}

static void socFactoryInit( void )
{
	unsigned long idx;

	SocFactory.lastErr = 0;
	SocFactory.state = SOC_FACTORY_STOPPED;
	SocFactory.requests = 0;
	SocFactory.slot = -1;
	XplLockInit( &SocFactory.lock );

	for( idx = 0; idx < SOC_FACTORY_CONCURRENCY_LIMIT; idx++ ) {
		SocFactory.list[ idx ] = SOCKET_INVALID;
	}
}
/* the goals of the socFactory thread are as follows:
   - to create all sockets in a single thead and keep that thread alive so that
   sockets do not get closed when the thread that created them exits.
   - to not create any more sockets than have already been requested
   - to not use cpu resources when not creating sockets
   - be capabale of stopping and restarting
*/
static int socFactoryThread( XplThread_ thread )
{
	int soc;
	int requests;

	soc = SOCKET_INVALID;

	while( SocFactory.state == SOC_FACTORY_RUNNING ) {
		/* wait until somebody wants a socket */
		SFPrintf( "SFTHREAD[%d]: waiting for a request\n", __LINE__ );
		XplSemaWait( SocFactory.createSem );
		if( !SocFactory.state == SOC_FACTORY_RUNNING ) {
			/* shut down the factory */
			SFPrintf( "SFTHREAD[%d]: stopping because status is not RUNNING\n", __LINE__ );
			break;
		}

		SFPrintf( "SFTHREAD[%d]: request signaled!\n", __LINE__ );

		if( soc == SOCKET_INVALID ) {
			SFPrintf( "SFTHREAD[%d]: attempting to create a soc\n", __LINE__ );
			errno = 0;
			soc = XplIpSocket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
			if( soc == SOCKET_INVALID ) {
				SFPrintf( "SFTHREAD[%d]: failed to create a soc\n", __LINE__ );
				SocFactory.lastErr = errno;
				/*
				  the OS failed to create a socket; we do not want the
				  requester to block so we signal pickup anyway to cause cause a
				  requesting thread to get the error.
				*/
				XplSemaPost( SocFactory.pickupSem );
				continue;
			} else {
				SFPrintf( "SFTHREAD[%d]: created soc (%d)\n", __LINE__, soc );
			}
		} else {
			SFPrintf( "SFTHREAD[%d]: already have soc (%d) waiting.\n", __LINE__, soc );
		}

		/* attempt to add the new sock to the pickup list */
		XplLockAcquire( &SocFactory.lock );
		if( ( SocFactory.slot + 1 )  < SOC_FACTORY_CONCURRENCY_LIMIT ) {
			SocFactory.slot++;
			SocFactory.list[ SocFactory.slot ] = soc;
			SFPrintf( "SFTHREAD[%d]: soc (%d) stored in slot (%d)\n", __LINE__, soc, SocFactory.slot );
			soc = SOCKET_INVALID;
		} else {
			SFPrintf( "SFTHREAD[%d]: factory cache is already full!\n", __LINE__ );
		}
		XplLockRelease( &SocFactory.lock );

		if( soc == SOCKET_INVALID ) {
			/* A new soc was added to the pickup list. Signal the pickup sem
			   to tell the requester a soc is ready
			*/
			SFPrintf( "SFTHREAD[%d]: soc ready. Signaling pickup threads\n", __LINE__ );
			XplSemaPost( SocFactory.pickupSem );
		} else {
			/*
			  The list was full because the pickup threads have not run yet;
			  Since we failed to add the new soc to the pickup list, we need to
			  signal the create sem again until there is a place in the list
			  for it.
			*/
			SFPrintf( "SFTHREAD[%d]: looping while holding soc (%d)\n", __LINE__, soc );
			XplSemaPost( SocFactory.createSem );
		}
	}

	/*
	  Clean-up

	  If there are still requests, we want them to fail.  So we close
	  anything in 'soc' or that was saved in the list.
	*/

	SFPrintf( "SFTHREAD[%d]: shutting down\n", __LINE__ );
	if( soc != SOCKET_INVALID ) {
		SFPrintf( "SFTHREAD[%d]: closing soc (%d)\n", __LINE__, soc );
		XplIpClose( soc );
	}

	XplLockAcquire( &SocFactory.lock );
	while( SocFactory.slot > -1 ) {
		SFPrintf( "SFTHREAD[%d]: closing soc (%d) stored in slot (%d)\n", __LINE__, SocFactory.list[ SocFactory.slot ], SocFactory.slot );
		XplIpClose( SocFactory.list[ SocFactory.slot ] );
		SocFactory.list[ SocFactory.slot ] = SOCKET_INVALID;
		SocFactory.slot--;
	}
	XplLockRelease( &SocFactory.lock );

	SocFactory.state = SOC_FACTORY_STOPPING_REQUEST_THREADS;

	SFPrintf( "SFTHREAD[%d]: exiting\n", __LINE__ );
	return 0;
}

int socCreate( XplBool iocpThread )
{
	int soc;
	unsigned long idx;
	XplBool running;

	soc = SOCKET_INVALID;

	if( iocpThread ) {
		/* this is an iocp thread and it won't go away so it is ok to create a socket here */
		soc = XplIpSocket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
	} else {
		/*
		  this thread could go away, so we will request a socket created by the
		  socFactory thread.
		*/
		XplLockAcquire( &SocFactory.lock );
		if( SocFactory.state == SOC_FACTORY_RUNNING ) {
			SocFactory.requests++;
			running = TRUE;
		} else {
			running = FALSE;
			errno = ENETDOWN;
		}
		XplLockRelease( &SocFactory.lock );

		if( running ) {
			SFPrintf( "R THREAD[%d][%d]: requesting a soc\n", XplGetThreadID(), __LINE__ );
			XplSemaPost( SocFactory.createSem );
			SFPrintf( "R THREAD[%d][%d]: waiting for a soc\n", XplGetThreadID(), __LINE__ );
			XplSemaWait( SocFactory.pickupSem );

			XplLockAcquire( &SocFactory.lock );
			if( SocFactory.slot > -1 ) {
				soc = SocFactory.list[ SocFactory.slot ];
				SFPrintf( "R THREAD[%d][%d]: picked up soc (%d) from slot (%d)\n", XplGetThreadID(), __LINE__, soc, SocFactory.slot );
				SocFactory.list[ SocFactory.slot ] = SOCKET_INVALID;
				SocFactory.slot--;
			}
			SocFactory.requests--;
			XplLockRelease( &SocFactory.lock );
			if( soc == SOCKET_INVALID ) {
				errno = SocFactory.lastErr;
			}
		} else {
			SFPrintf( "R THREAD[%d][%d]: factory not running\n", XplGetThreadID(), __LINE__ );
		}
	}
	return soc;
}


/* socFactoryStart() blocks until the soc Factory is started */
static void socFactoryStart( void )
{
	XplLockAcquire( &SocFactory.lock );
	while( SocFactory.state != SOC_FACTORY_STOPPED ) {
		XplLockRelease( &SocFactory.lock );
		XplDelay( 1000 );
		XplLockAcquire( &SocFactory.lock );
	}
	XplSemaInit( SocFactory.createSem, 0 );
	XplSemaInit( SocFactory.pickupSem, 0 );
	SocFactory.state = SOC_FACTORY_RUNNING;
	XplLockRelease( &SocFactory.lock );

	XplThreadStart( NULL, socFactoryThread, NULL, NULL );
}

/* This function is called when the library use count drops below 1. It will
   block until the soc Factory is stopped */
static void socFactoryStop( void )
{
	XplLockAcquire( &SocFactory.lock );
	while( SocFactory.state != SOC_FACTORY_RUNNING ) {
		XplLockRelease( &SocFactory.lock );
		XplDelay( 1000 );
		XplLockAcquire( &SocFactory.lock );
	}
	SocFactory.state = SOC_FACTORY_STOPPING_FACTORY_THREAD;

	/* To guarantee the factory thread will exit, the semaphore is over-
	   signaled  */
	XplSemaPost( SocFactory.createSem );

	XplLockRelease( &SocFactory.lock );

	/* wait for the factory thread to exit */
	while( SocFactory.state != SOC_FACTORY_STOPPING_REQUEST_THREADS ) {
		XplDelay( 300 );
	}
	/*
	  signal any remaining request threads, and
	  wait for request threads to exit
	*/
	for( ; ; ) {
		int semaValue;
		XplSemaValue( SocFactory.pickupSem, &semaValue );
		while( semaValue < 0 ) {
			XplSemaPost( SocFactory.pickupSem );
			XplSemaValue( SocFactory.pickupSem, &semaValue );
		}
		if( SocFactory.requests < 1 ) {
			break;
		}
		XplDelay( 300 );
	}

	XplSemaDestroy( SocFactory.createSem );
	XplSemaDestroy( SocFactory.pickupSem );

	SocFactory.state = SOC_FACTORY_STOPPED;
}


// PUBLIC ENTRY no lock needed because this function does not touch a socket
EXPORT int SocketTimeoutSet( SocketTimeout *timeout, int milliseconds )
{
    int	previous;

	errno = 0;
    if( milliseconds < 4294968 ) {
        previous = ( timeout->tv_sec * 1000 ) + ( timeout->tv_usec / 1000 );

		timeout->tv_sec = milliseconds / 1000;
		timeout->tv_usec = (milliseconds % 1000) * 1000;

#if defined(DEBUG_SOCKWIN)
		XplConsolePrintf("Socket Timeout set to %d ms -> %lu sec %lu usec\n",milliseconds,timeout->tv_sec,timeout->tv_usec);
#endif
        return( previous );
    }

    errno = ERANGE;
    return SOCKET_ERROR;
}

// PUBLIC ENTRY no lock needed because this function does not touch a socket
EXPORT int SocketTimeoutGet( SocketTimeout *timeout )
{
	errno = 0;
    return( ( timeout->tv_sec * 1000 ) + ( timeout->tv_usec / 1000 ) );
}

int sockWaitForReadable( XplSocket sock, SocketTimeout *timeout )
{
    int ret;
	int err;
	socklen_t errLen;
    fd_set	rfds;
    fd_set	efds;

    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
	FD_ZERO(&efds);
    FD_SET(sock, &efds);

    if( !XplIpSelect( 0, &rfds, NULL, &efds, timeout ) ) {
		ret = SOCKET_ERROR;
        errno = ETIMEDOUT;
    } else if( FD_ISSET( sock, &efds ) ) {
		ret = SOCKET_ERROR;
		errLen = sizeof( err );
		if( getsockopt( sock, SOL_SOCKET, SO_ERROR, (void *)&err, &errLen ) ) {
			errno = EIO;
		} else {
			errno = XplTranslateError( err );
		}
	} else {
		errno = 0;
		ret = 0;
	}

    return ret;
}

int sockWaitForWritable( XplSocket sock, SocketTimeout *timeout )
{
    int ret;
	int err;
	socklen_t errLen;
    fd_set	wfds;
    fd_set	efds;

    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
	FD_ZERO(&efds);
    FD_SET(sock, &efds);

    if( !XplIpSelect( 0, NULL, &wfds, &efds, timeout ) ) {
		ret = SOCKET_ERROR;
        errno = ETIMEDOUT;
    } else if( FD_ISSET( sock, &efds ) ) {
		ret = SOCKET_ERROR;
		errLen = sizeof( err );
		if( getsockopt( sock, SOL_SOCKET, SO_ERROR, (void *)&err, &errLen ) ) {
			errno = EIO;
		} else {
			errno = XplTranslateError( err );
		}
	} else {
		errno = 0;
		ret = 0;
	}

    return ret;
}

static int
socketAsyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn )
{
	int error = 0;
	struct sockaddr_storage sourceAddr;
	struct sockaddr *sourceAddrPtr;
	socklen_t sourceAddrPtrSize;

	if( SOCKET_ERROR != ( sock->number = socCreate( FALSE ) ) ) {
		socketLogValue( sock, "Socket %d bound to this SOCK", sock->number );
		if( !srcSaddr ) {
			memset( &sourceAddr, 0, sizeof( sourceAddr ) );
			sourceAddrPtr = ( struct sockaddr *)&sourceAddr;
			sourceAddrPtrSize = sizeof( sourceAddr );

			if( destSaddr->sa_family == AF_INET ) {
				struct sockaddr_in *sa4 = ( struct sockaddr_in * )sourceAddrPtr;
				sa4->sin_family = AF_INET;
				sa4->sin_port = 0;
				sa4->sin_addr.s_addr = INADDR_ANY;
			} else if( destSaddr->sa_family == AF_INET ) {
				struct sockaddr_in6 *sa6 = ( struct sockaddr_in6 * )sourceAddrPtr;
				sa6->sin6_family = AF_INET6;
				sa6->sin6_port = 0;
				/* won't compile on watcom */
				//sa6->sin6_addr = in6addr_any;
			}
		} else {
			sourceAddrPtr = srcSaddr;
			sourceAddrPtrSize = srcSaddrLen;
		}
		if( !XplIpBind( sock->number, sourceAddrPtr, sourceAddrPtrSize ) ) {
			if( SOCKET_ERROR != XplSocketSetMode( sock->number, XPL_SOCKET_MODE_NON_BLOCKING ) ) {
			  *blockedOn = SOCK_EVENT_TYPE_CONNECT;
			  return EWOULDBLOCK;
			}
		}
		error = errno; // save errno just in case close() changes it
		socketClose( sock );
		sock->number = SOCKET_ERROR;
		errno = error;
	}
	error = errno;
	errno = 0;
	*blockedOn = SOCK_EVENT_TYPE_NONE;
	return( error );
}

int
SocketAsyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( sock->slock ) );

	ret = socketAsyncConnectStart( sock, destSaddr, destSaddrLen, srcSaddr, srcSaddrLen, blockedOn );

	XplLockRelease( &( sock->slock ) );

	return ret;
}

static int
socketAsyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn )
{
	traceEvent( socket, TRACE_EVENT_DISCONNECT_START, NULL, 0 );
	*blockedOn = SOCK_EVENT_TYPE_DISCONNECT;
	return( EWOULDBLOCK );
}

int
SocketAsyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketAsyncDisconnectStart( socket, blockedOn );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

static int
socketSyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn, SocketTimeout *timeout )
{
	int err;

	traceEvent( socket, TRACE_EVENT_DISCONNECT_START, NULL, 0 );
	err = socketDisconnect( socket, timeout );
	/*
	   The windows DisconnectEx function blocks when the overlapped arg is NULL.
	   There is no need to return EWOULDBLOCK.  Just finish off the socket now
	   and return success.
	 */
	*blockedOn = SOCK_EVENT_TYPE_DISCONNECT;
	return( err );
}

int
SocketSyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn, SocketTimeout *timeout )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	ret = socketSyncDisconnectStart( socket, blockedOn, timeout );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

int sockDisconnect( XplSocket s )
{
	LPFN_DISCONNECTEX disconnectFunc;
	DWORD bytes;

	if( s != SOCKET_INVALID ) {
		WSAIoctl( s, SIO_GET_EXTENSION_FUNCTION_POINTER,
				  &disconnectGUID, sizeof( disconnectGUID ),
				  &( disconnectFunc ), sizeof( disconnectFunc ),
				  &bytes, NULL, NULL );
		/*
		  The windows DisconnectEx function blocks when the overlapped arg is NULL.
		  even if the socket is non-blocking
		*/
		if( disconnectFunc( s, NULL, 0, 0 ) ) {
			return 0;
		}
		return getLastWsaErrno();
	}
	return EINVAL;
}

static int
timerAllocCB(void *buffer, void *data)
{
	return 0;
}

static int
timerFreeCB(void *buffer, void *data)
{
	return 0;
}

static seTimer *seTimerAlloc( SocketTimeout *timeout, int *err )
{
  unsigned int ms;
  seTimer *timer;

  ms = SocketTimeoutGet( timeout ); // get timeout in milliseconds
  if( !ms ) {
	  // 0 timeout means infinite or no timeout
	  if( err ) *err = 0;
	  return NULL;
  }

  timer = MemPoolGet( SockLib.eventEngine->timerpool );
  if( timer ) {
	  memset(timer, 0, sizeof(seTimer));
	  timer->handle = CreateWaitableTimer( NULL, TRUE, NULL );
	  if( timer->handle ) {
		  timer->started = FALSE;
		  timer->se = NULL;
		  timer->len = ms;
		  XplLockInit( &timer->lock );
		  if( err ) *err = 0;

          MemAssert(timer);
		  return timer;
	  }
	  if( err ) {
		  *err = getLastWsaErrno();
	  }
	  MemFree( timer );
  } else {
	  if( err ) *err = ENOMEM;
  }

  return NULL;
}

static void seTimerFree( seTimer *timer )
{
  if( timer ) {
	MemAssert(timer);
	timer->se = NULL;
	CloseHandle( timer->handle );
	MemFree( timer );
  }
}

static void seTimerLink( SocketEvent *se, seTimer *timer )
{
  if( timer && se ) {
	MemAssert(timer);
	timer->se = se;
	se->timer = timer;
  }
}

static void seTimerUnlink( SocketEvent *se, seTimer *timer )
{
  if( timer ) {
	MemAssert(timer);
	timer->se = NULL;
  }

  if( se ) {
	se->timer = NULL;
  }
}

static void timerEnqueue( SocketEvent *se, seTimer *timer )
{
  if( timer ) {
	MemAssert(timer);
	DebugAssert(timer->se);

	XplLockAcquire( &( SockLib.eventEngine->timerQ.lock ) );
	timer->next = SockLib.eventEngine->timerQ.head;
	SockLib.eventEngine->timerQ.head = timer;
	XplLockRelease( &( SockLib.eventEngine->timerQ.lock ) );
  }
}

// INTERNAL must own slock
static void eventTimeout( SocketEvent *se ) {
	if( ( IOCP_EVENT_STATE_WAIT_ACTIVE == se->state ) && ( SOCK_EVENT_TYPE_DISCONNECT != se->type ) ) {
		se->timer = NULL;
		se->timedout = TRUE;
		CancelIoEx( (HANDLE)se->cs->number, &( se->ovl ) );
		/*
		  We are not sure if the event is in the engine or has recently come
		  out.  CancelIOEx is called just in case it is still in the engine.
		  The flags ensure that the event will error out either way.
		*/
	}
}

static void TimerKill( SocketEvent *se )
{
	XplBool started = FALSE;
	seTimer *timer = NULL;
	XplBool freeTimer = FALSE;

	if( se->timer ) {
		timer = se->timer;
		MemAssert(timer);

		XplLockAcquire( &timer->lock );
		seTimerUnlink( se, timer );
		if( !timer->started  ) {
			/*
			  if the timer did not start yet, the timer thread will free the
			  timer when it finds that it is not linked to the socket
			  event (se).
			*/
			freeTimer = FALSE;
		} else if( !CancelWaitableTimer( timer->handle ) ) {
			/*
			  The timer fired before we could cancel it, the timeout callback
			  will free the timer.

			  We do not free the timer in this thread because the timer callback
			  will free the timer.
			 */
			freeTimer = FALSE;
		} else {
			/* Awesome! We killed the timer before it could fire. */
			freeTimer = TRUE;
		}
		XplLockRelease( &timer->lock );

		if( timer && freeTimer ) {
			seTimerFree( timer );
		}
	}

	if( se->timedout ) {
		se->error = ETIMEDOUT;
		socketLogEvent( se->cs, se, se->cb.arg, "TIMEOUT", "ACTIVE" );
	}
}

// INTERNAL ENTRY from OS
void timerCallback( LPVOID arg, DWORD lowDateTime, DWORD highDateTime )
{
	seTimer *timer = ( seTimer * )arg;
	SocketEvent *se;

	MemAssert(timer);
	XplLockAcquire( &timer->lock );
	if( timer->se ) { /* even after getting the lock, the timer is not obsolete */
		se = timer->se;
		seTimerUnlink( se, timer );
		eventTimeout( se );
	}
	XplLockRelease( &timer->lock );
	seTimerFree( timer );
	return;
}

// INTERNAL ENTRY (from timer thread)
static XplBool timerStart( seTimer *timer )
{
  SocketEvent *se;
  LARGE_INTEGER dueTime;

  MemAssert(timer);

  dueTime.QuadPart = timer->len;  // move len (ms)  to the 64 bit number before multiplying
  dueTime.QuadPart *= -10000;     /* Weird! SetWaitableTimer() has units of 100ns.
									 It  needs to be negative to indicate that it is in the
									 future reletive to now.
								  */

  XplLockAcquire( &timer->lock );
  if( !timer->se ) {
	  XplLockRelease( &timer->lock );
	  return FALSE;
  }

  /* we now have a locked timer */
  if( !SetWaitableTimer( timer->handle, &dueTime, 0, (PTIMERAPCROUTINE)timerCallback, timer, 0 ) ) {
	  /*
		The timeout did not start.
		Make the event error out immediately.
	  */
	  se = timer->se;
	  seTimerUnlink( se, timer );
	  eventTimeout( se );
	  XplLockRelease( &timer->lock );

	  DebugAssert( 0 );

	  return FALSE;
  }

  /* the timer has begun */
  timer->started = TRUE;
  XplLockRelease( &timer->lock );
  return TRUE;
}

static int _TimerThread( XplThread thread )
{
	unsigned long loops;
	seTimer *currentTimer;
	seTimer *nextTimer;
	SocketEventEngine *engine = ( SocketEventEngine * )( thread->context );

	loops = 0;

	XplLockAcquire( &engine->timerQ.lock );
	MemAssert(engine->timerQ.head);
	XplLockRelease( &engine->timerQ.lock );

	while( ENGINE_RUNNING == engine->state ) {
		// This could conceiveably sleep until the next scheduled timeout
		// I'm leaving it as is for now but when we revisit for tuning
		// we may want to use XplThreadCatch() with a timeout matching
		// the next timeout and watch for SIGTERM
		// This could get rid of the ENGINE_RUNNING checks also
		// Thane
		loops++;
		SleepEx( 200, TRUE );
		// SleepEx( 20000, TRUE );
		if (ENGINE_RUNNING != engine->state) {
			break;
		}

		/* pull the timer queue */
		XplLockAcquire( &engine->timerQ.lock );
		currentTimer = engine->timerQ.head;
		MemAssert(currentTimer);
		engine->timerQ.head = NULL;
		XplLockRelease( &engine->timerQ.lock );

		/* start the timers */
		while( currentTimer ) {
			MemAssert(currentTimer);

			nextTimer = currentTimer->next;
			if (nextTimer) {
				MemAssert(nextTimer);
			}

			if( !timerStart( currentTimer ) ) {
				seTimerFree( currentTimer );
			}
			currentTimer = nextTimer;
		}
		if( loops % 8 ) {
			AThreadCountUpdate();
		}
	}
	return 0;
}

void sockEventClean( SocketEvent *se )
{
	se->error = 0;
	SeSetFlags( se, 0 );
	if( se->wsabuf ) {
		MemFree( se->wsabuf );
		se->wsabuf = NULL;
	}
}

XplBool sockEventKill( SocketEvent *se )
{
	if( se ) {
		if( ( IOCP_EVENT_STATE_WAIT_ACTIVE == se->state ) && ( SOCK_EVENT_TYPE_DISCONNECT != se->type ) ) {
			socketLogEvent( se->cs, se, se->cb.arg, "KILLED", "ACTIVE" );
			if( ( se->type == SOCK_EVENT_TYPE_LISTEN ) ) {
				SeAddFlags( se, FSE_LISTENER_KILLED );
			} else if( 0 == ( ( FSE_CANCELED | FSE_TIMEDOUT ) & SeGetFlags( se ) ) ) {
				SeAddFlags( se, FSE_CANCELED );
			}
			CancelIoEx( (HANDLE)se->cs->number, &( se->ovl ) );
			/*
			  We are not sure if the event is in the engine or has recently come
			  out.  CancelIOEx is called just in case it is still in the engine.
			  The flags ensure that the event will error out either way.
			*/
		}
	}
	return FALSE; /* we always wait for the event to come out on windows */
}

int sockEventInit( SOCK *cs, SocketEvent *event )
{
	memset( event, 0, sizeof( SocketEvent ) );
	event->cs = cs;
	event->error = 0;
	return 0;
}

void sockReset( SOCK *socket )
{
	socket->iocpLinked = FALSE;
}

static int acceptNext( SOCK *cSoc, SocketEvent *se, XplBool iocpThread )
{
  DWORD expected = sizeof(struct sockaddr_storage ) + 16;
  int err;

  memset( &( se->ovl ), 0, sizeof( WSAOVERLAPPED ) );
  if( se->acceptBuf ) {
	;
  } else {
	/* This is Microsoft wierdness.
	   The data, and the source and destination
	   addresses are written to the same buffer
	*/
	se->acceptBuf = MemCallocWait( 1, 0 + ( 2 * ( sizeof( struct sockaddr_storage ) + 16 ) ) );
  }
  se->acceptSocket = socCreate( iocpThread );
  if( ( se->acceptSocket == SOCKET_ERROR ) || !cSoc->accept.func( se->cs->number, se->acceptSocket, se->acceptBuf, 0, expected, expected, NULL, &( se->ovl ) ) ) {
	  err = getLastWsaErrno();
	  if( err != ERROR_IO_PENDING ) {
		  return err;
	  }
  }
  socketLogEvent( se->cs, se, se->cb.arg, "START ACCEPT", "ACTIVE" );
  return 0;
}

SocketEventType sockEventTranslate( SocketEventType type )
{
	return type;
}

// INTERNAL caller must hold slock
int sockEventAdd( SOCK *cs, SocketEvent *se, SocketTimeout *timeOut, struct sockaddr *dst, socklen_t dstLen, XplBool *doneAlready )
{
	DWORD flags;
	int err;
	seTimer *timer;

	DebugAssert( se->cb.eventFunc );
	DebugAssert( !( SeGetFlags( se ) ) ); /* no flags should be set when adding an event to the engine */

	*doneAlready = FALSE;
	se->error = 0;
	se->timedout = FALSE;

	if( !cs->iocpLinked ) {
		if( CreateIoCompletionPort( ( HANDLE )(cs->number), SockLib.eventEngine->iocp, (ULONG_PTR)cs, 0) != SockLib.eventEngine->iocp ) {
			se->error = getLastErrno();
			DebugAssert( 0 );
			return -1;
		}
		socketLogValue( cs, "Socket bound to IOCP engine %p", (int)SockLib.eventEngine->iocp  );
		cs->iocpLinked = TRUE;
	}

	memset( &( se->ovl ), 0, sizeof( WSAOVERLAPPED ) );
	switch( se->type ) {
	case SOCK_EVENT_TYPE_WRITABLE:
	case SOCK_EVENT_TYPE_READABLE:
		/* prepare resources */
		if( !se->wsabuf ) {
			se->wsabuf = MemMallocWait( sizeof( WSABUF ) );
		}
		se->wsabuf[0].len = 0;
		se->wsabuf[0].buf = se->dbuf;
		timer = seTimerAlloc( timeOut, &err );
		if( err ) {
			se->error = err;
			return -1;
		}
		/* verify state */
		if( se->state == IOCP_EVENT_STATE_IDLE ) {
			/*
			   IDLE
			   This is how it is supposed to happen
			*/
			seTimerLink( se, timer );
			socketLogEvent( se->cs, se, se->cb.arg, "ADD", "IDLE" );
			/* IOCP_EVENT_STATE_IDLE state - submit readable/writeable event */
			if( se->type == SOCK_EVENT_TYPE_READABLE ) {
				flags = 0;
				err = WSARecv( cs->number, se->wsabuf, 1, NULL, &flags, &( se->ovl ), NULL );
			} else {
				err = WSASend( cs->number, se->wsabuf, 1, NULL, 0, &( se->ovl ), NULL );
			}

			if( err ) {
				err = getLastWsaErrno();
				if( err != WSA_IO_PENDING ) {
					/* socket error */
					/* leave in IOCP_EVENT_STATE_IDLE state */
					se->error = err;
					seTimerUnlink( se, timer );
					seTimerFree( timer );
					return -1;
				}
			}

			/* socket event successfully submitted - start timer */
			timerEnqueue( se, timer );
			socketLogStateChange( se->cs, se, se->cb.arg, "IDLE", "ACTIVE" );
			se->state = IOCP_EVENT_STATE_WAIT_ACTIVE;
			return 0;
		}

		socketLogEvent( se->cs, se, se->cb.arg, "ADD", "UNKNOWN" );
		DebugPrintf( "A read/write event was submitted that is already actively waiting.\n" );
		DebugAssert( 0 );
		seTimerFree( timer );
		return -1;

	case SOCK_EVENT_TYPE_DISCONNECT: {
		LPFN_DISCONNECTEX disconnectFunc;
		DWORD bytes;

		if( IOCP_EVENT_STATE_IDLE != se->state  ) {
			DebugPrintf( "A disconnect event was submitted that is not in an idle state.\n" );
			DebugAssert( 0 );
			return -1;
		}

		timer = seTimerAlloc( timeOut, &err );
		if( err ) {
			se->error = err;
			return -1;
		}
		seTimerLink( se, timer );
		WSAIoctl( cs->number, SIO_GET_EXTENSION_FUNCTION_POINTER,
				  &disconnectGUID, sizeof( disconnectGUID ),
				  &( disconnectFunc ), sizeof( disconnectFunc ),
				  &bytes, NULL, NULL );

		if( disconnectFunc( cs->number, &( se->ovl ), 0, 0 ) ) {
			/* disconnect is done already */
			socketLogEvent( se->cs, se, se->cb.arg, "CONNECT/DISCONNECT COMPLETE", "ACTIVE" );
			*doneAlready = TRUE;
			seTimerUnlink( se, timer );
			seTimerFree( timer );
			return 0;
		} else if( WSA_IO_PENDING == ( err = getLastWsaErrno() ) ) {
			/* disconnect event submitted to the IOCP engine */
			socketLogStateChange( se->cs, se, se->cb.arg, "IDLE", "ACTIVE" );
			se->state = IOCP_EVENT_STATE_WAIT_ACTIVE;
			timerEnqueue( se, timer );
			return 0;
		} else {
			/* socket error */
			/* leave in IOCP_EVENT_STATE_IDLE state */
			se->error = err;
			seTimerUnlink( se, timer );
			seTimerFree( timer );
			return -1;
		}
	}
	case SOCK_EVENT_TYPE_CONNECT: {
		LPFN_CONNECTEX connectFunc;
		DWORD bytes;

		if( IOCP_EVENT_STATE_IDLE != se->state  ) {
			DebugPrintf( "A connect event was submitted that is not in an idle state.\n" );
			DebugAssert( 0 );
			return -1;
		}

		timer = seTimerAlloc( timeOut, &err );
		if( err ) {
			se->error = err;
			return -1;
		}
		seTimerLink( se, timer );

		WSAIoctl( cs->number, SIO_GET_EXTENSION_FUNCTION_POINTER,
				  &connectGUID, sizeof( connectGUID ),
				  &( connectFunc ), sizeof( connectFunc ),
				  &bytes, NULL, NULL );

		if( connectFunc( cs->number, dst, dstLen, NULL, 0, NULL, &( se->ovl ) ) ) {
			/* connect is done already */
			socketLogEvent( se->cs, se, se->cb.arg, "CONNECT/DISCONNECT COMPLETE", "ACTIVE" );
			*doneAlready = TRUE;
			seTimerUnlink( se, timer );
			seTimerFree( timer );
			return 0;
		} else if( WSA_IO_PENDING == ( err = getLastWsaErrno() ) ) {
			/* connect event submitted to the IOCP engine */
			socketLogStateChange( se->cs, se, se->cb.arg, "IDLE", "ACTIVE" );
			se->state = IOCP_EVENT_STATE_WAIT_ACTIVE;
			timerEnqueue( se, timer );
			return 0;
		} else {
			/* socket error */
			/* leave in IOCP_EVENT_STATE_IDLE state */
			se->error = err;
			seTimerUnlink( se, timer );
			seTimerFree( timer );
			return -1;
		}
	}

	case SOCK_EVENT_TYPE_LISTEN:
		/* verify state */
	   DebugAssert( IOCP_EVENT_STATE_IDLE == se->state  );
		if( se->state == IOCP_EVENT_STATE_IDLE ) {
			/*
			   IDLE
			   This is how it is supposed to happen
			*/
			DWORD bytes;

			socketLogEvent( se->cs, se, se->cb.arg, "ADD", "IDLE" );
			if( !cs->accept.defined ) {
				WSAIoctl( cs->number, SIO_GET_EXTENSION_FUNCTION_POINTER,
						  &acceptGUID, sizeof( acceptGUID ),
						  &( cs->accept.func ), sizeof( cs->accept.func ),
						  &bytes, NULL, NULL );
				cs->accept.defined = TRUE;
			}

			if( ( se->error = acceptNext( cs, se, FALSE ) ) == 0 ) {
				se->state = IOCP_EVENT_STATE_WAIT_ACTIVE;
				return 0;
			}
			DebugPrintf( "An accept failed to start\n" );
			DebugAssert( 0 );
			return -1;
		}
		DebugPrintf( "A listen event was submitted that not in an idle state.\n" );
		DebugAssert( 0 );
		return -1;
	}
	return 0;
}

XplBool sockEventListenerRequeue( SOCK *socket, SocketEvent *se )
{

	SeRemoveFlags( se, ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED ) );

	se->error = acceptNext( socket, se, FALSE );
	if( se->error ) {
		DebugAssert( ( 0 == SeGetFlags( se ) ) || ( FSE_LISTENER_KILLED == SeGetFlags( se ) ) );
		return TRUE;
	}
	return FALSE;
}

static int _IOCPThread( XplThread thread )
{
	SocketEventEngine *engine = ( SocketEventEngine * )( thread->context );
	DWORD length;
	XplBool resultOk;
	WSAOVERLAPPED *ovl;
	SocketEvent *se;
	XplBool eventCompleted;
	SOCK *cSoc;
	AcceptEvent *acceptEvent = NULL;

	for(;;){
		if( !acceptEvent ) {
			/*
			  allocate a new accept event while we are not holding the slock of
			  any SOCK object and before we could possibly block waiting for an
			  event.
			*/
			acceptEvent = acceptEventAlloc( &( engine->aeCache ) );
		}
		ovl = NULL;
		se = NULL;
		resultOk = GetQueuedCompletionStatus( engine->iocp, &length, (PULONG_PTR)&cSoc, &ovl, INFINITE );
		if( !ovl )
		{
			DebugAssert( ENGINE_RUNNING != engine->state );	// should only happen when shutting down engine
			break;
		}
		DebugAssert( ovl ); // this code needs to change if ovl can come back NULL (rodney)
		se = XplParentOf( ovl, SocketEvent, ovl );
		eventCompleted = FALSE;
		XplLockAcquire( &( se->cs->slock ) );
		switch( se->type ) {
			case SOCK_EVENT_TYPE_WRITABLE:
			case SOCK_EVENT_TYPE_READABLE:
				if( se->state == IOCP_EVENT_STATE_WAIT_ACTIVE ) {
					/*
					ACTIVE
					This is how it is supposed to happen
					*/
					if( FSE_CANCELED  == SeGetFlags( se ) ) {
						SeRemoveFlags( se, FSE_CANCELED );
						se->error = ECANCELED;
						socketLogEvent( se->cs, se, se->cb.arg, "CANCEL READ/WRITE EVENT ENCOUNTERED", "ACTIVE" );
					} else if( FSE_TIMEDOUT  == SeGetFlags( se ) ) {
						SeRemoveFlags( se, FSE_TIMEDOUT );
						se->error = ETIMEDOUT;
						socketLogEvent( se->cs, se, se->cb.arg, "TIMEDOUT READ/WRITE EVENT ENCOUNTERED", "ACTIVE" );
					} else if( !resultOk ) {
						se->error = getLastErrno();
					} else {
						socketLogEvent( se->cs, se, se->cb.arg, "COMPLETE", "ACTIVE" );
					}
					DebugAssert( se->cb.eventFunc );
					socketLogStateChange( se->cs, se, se->cb.arg, "ACTIVE", "IDLE" );
					se->state = IOCP_EVENT_STATE_IDLE;
					TimerKill( se );
					DebugAssert( 0 == SeGetFlags( se ) );
					eventCompleted = TRUE;
				} else if( se->state == IOCP_EVENT_STATE_IDLE ) {
					/* IDLE */
					socketLogEvent( se->cs, se, se->cb.arg, "COMPLETE", "IDLE" );
					DebugPrintf( "An unexpected read/write event surfaced in Socket Event Engine\n" );
					DebugAssert( 0 );
				} else {
					/* UNKNOWN  */
					socketLogEvent( se->cs, se, se->cb.arg, "COMPLETE", "UNKNOWN" );
					socketLogStateChange( se->cs, se, se->cb.arg, "UNKNOWN", "IDLE" );
					RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( 0 );  // these should never come out of the engine
					se->state = IOCP_EVENT_STATE_IDLE;
				}
				break;

			case SOCK_EVENT_TYPE_CONNECT:
			case SOCK_EVENT_TYPE_DISCONNECT:
				if( se->state == IOCP_EVENT_STATE_WAIT_ACTIVE ) {
					/*
					ACTIVE
					This is how it is supposed to happen
					*/
					if( FSE_CANCELED == SeGetFlags( se ) ) {
						SeRemoveFlags( se, FSE_CANCELED );
						se->error = ECANCELED;
						socketLogEvent( se->cs, se, se->cb.arg, "CANCELED CONNECT/DISCONNECT EVENT ENCOUNTERED", "ACTIVE" );
					} else if( FSE_TIMEDOUT  == SeGetFlags( se ) ) {
						SeRemoveFlags( se, FSE_TIMEDOUT );
						se->error = ETIMEDOUT;
						socketLogEvent( se->cs, se, se->cb.arg, "TIMEDOUT CONNECT/DISCONNECT EVENT ENCOUNTERED", "ACTIVE" );
					} else if( !resultOk ) {
						se->error = getLastErrno();
						socketLogEvent( se->cs, se, se->cb.arg, "CONNECT/DISCONNECT FAILURE", "ACTIVE" );
					} else {
						socketLogEvent( se->cs, se, se->cb.arg, "CONNECT/DISCONNECT COMPLETE", "ACTIVE" );
					}

					DebugAssert( se->cb.eventFunc );
					TimerKill( se );
					se->state = IOCP_EVENT_STATE_IDLE;
					socketLogStateChange( se->cs, se, se->cb.arg, "ACTIVE", "IDLE" );
					DebugAssert( 0 == SeGetFlags( se ) );
					eventCompleted = TRUE;
					break;
				}

				if( se->state == IOCP_EVENT_STATE_IDLE ) {
					/* IDLE */
					socketLogEvent( se->cs, se, se->cb.arg, "CONNECT/DISCONNECT COMPLETE", "IDLE" );
					DebugPrintf( "An unexpected accept event surfaced in Socket Event Engine\n" );
					DebugAssert( 0 );
					break;
				}

				/* UNKNOWN  */
				RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( 0 );  // these should never come out of the engine
				break;

			case SOCK_EVENT_TYPE_LISTEN:
				if( se->state == IOCP_EVENT_STATE_WAIT_ACTIVE ) {
					/*
					ACTIVE
					This is how it is supposed to happen
					*/
					socketLogEvent( se->cs, se, se->cb.arg, "ACCEPT COMPLETE", "ACTIVE" );
					DebugAssert( se->cb.eventFunc );
					DebugAssert( ( SeGetFlags( se ) & FSE_LISTENER_PAUSED ) == 0 );  // a paused listener should not come out of the engine (rodney)
					if( resultOk ) {
						if( FSE_LISTENER_KILLED == SeGetFlags( se ) ) {
							socketLogEvent( se->cs, se, se->cb.arg, "KILLED LISTENER_ENCOUNTERED", "ACTIVE" );
						} else {
							if( acceptEvent ) {
								acceptEventInit( acceptEvent, se, se->acceptSocket );
								/* updates the context
								   - this is commentted out because it seems to kill the socket
								   setsockopt( se->acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)(&(*(se->socket))), sizeof( XplSocket ) );
								   err = getLastWsaErrno();
								*/
								// This one seems to work (thane)
								setsockopt( se->acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&(se->cs->number), sizeof( SOCKET ) );

								acceptReady( acceptEvent );
								acceptEvent = NULL;
							} else {
								/*
								  We must be really low on memory.  But we can't
								  afford to block in this thread.  The best we can
								  do is close the new socket.
								*/
								XplIpClose( se->acceptSocket ); /* no accept event */
								se->acceptSocket = SOCKET_INVALID;
							}

							if( listenerShouldThrottle( se ) &&
								listenerStartThrottling( se ) ) {
								/* pause the listener - the engine is busy */
								DebugAssert( ( FSE_LISTENER_PAUSED == SeGetFlags( se ) ) ||
											 ( ( FSE_LISTENER_PAUSED | FSE_LISTENER_KILLED ) == SeGetFlags( se ) ) ||
											 ( ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED ) == SeGetFlags( se ) ) ||
											 ( ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED | FSE_LISTENER_KILLED ) == SeGetFlags( se ) ) );
								eventCompleted = TRUE;
								break;
							} else {
								/* all is good - go get another connection */
								se->error = acceptNext( se->cs, se, TRUE );
								if( !se->error ) {
									break;
								}
								socketLogEvent( se->cs, se, se->cb.arg, "ACCEPT ERROR", "ACTIVE" );
							}
						}
						// listener killed or accept failed - fall through
					}
					se->state = IOCP_EVENT_STATE_IDLE;
					socketLogStateChange( se->cs, se, se->cb.arg, "ACTIVE", "IDLE" );
					DebugAssert( ( 0 == SeGetFlags( se ) ) || ( FSE_LISTENER_KILLED == SeGetFlags( se ) ) );
					eventCompleted = TRUE;
					break;
				}

				if( se->state == IOCP_EVENT_STATE_IDLE ) {
					/* IDLE */
					socketLogEvent( se->cs, se, se->cb.arg, "ACCEPT COMPLETE", "IDLE" );
					DebugPrintf( "An unexpected accept event surfaced in Socket Event Engine\n" );
					DebugAssert( 0 );
					break;
				}

				/* UNKNOWN  */
				RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( 0 );  // these should never come out of the engine
				break;
		}
		XplLockRelease( &( se->cs->slock ) );
		se->timedout = FALSE;

		if( eventCompleted ) {
			AThreadBegin( &( se->cb.thread ) );
		}
		//DebugLogAppendToTrace( se->cs, "IOCP Event", TRUE ); // Enable this to make sure IOCP completion events are flushed
	}

	if( acceptEvent ) {
		/* clean up leftover accept event */
		acceptEventRelease( acceptEvent );
	}

	return 0;
}

int sockEngineInit( SocketEventEngine *engine )
{
	SYSTEM_INFO	sysInfo;
	int			l;

	// LogSetDefaultLevel( "connwinsock", LOG_LEVEL_ERROR );
	XplLockInit( &engine->timerQ.lock );
	engine->timerQ.head = NULL;
	// a zero for concurrency sets the concurrency value to the number of processors
	engine->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if( engine->iocp ) {
		/*
		  We only use IOCP threads for event notification.
		  2 to 4 threads should be sufficient.
		 */
		GetSystemInfo( &sysInfo );
		engine->iocpThreads = sysInfo.dwNumberOfProcessors + 1;
		if( engine->iocpThreads > 4 ) {
			engine->iocpThreads = 4;
		}
		engine->timerpool = MMPoolAlloc( "timers",
					 sizeof( seTimer ),
					 0,
					 timerAllocCB,
					 NULL,
					 timerFreeCB,
					 NULL,
					 __FILE__, __LINE__ );
		for(l=0;l<engine->iocpThreads;l++)
		{
			XplThreadStart( engine->threadGroup, _IOCPThread, engine, NULL );
		}
		XplThreadStart( engine->threadGroup, _TimerThread, engine, NULL );
		return 0;
	}
	return -1;
}

int sockEngineShutdown( SocketEventEngine *engine )
{
	HANDLE	iocp;
	int		l;

	iocp = engine->iocp;
	engine->iocp = NULL;
	// one for each thread
	for(l=0;l<engine->iocpThreads;l++)
	{
		PostQueuedCompletionStatus( iocp, (DWORD)0, (ULONG_PTR)NULL, (LPOVERLAPPED)NULL );
	}
	CloseHandle( iocp );
	MemPrivatePoolFree( engine->timerpool );
	engine->timerpool = NULL;
	return 0;
}

void sockCreatorStart( void )
{
	/* Create a thread to create all sockets used by the library */
	socFactoryStart();
}

void sockCreatorStop( void )
{
	/* Cause the socket creator thread to exit */
	socFactoryStop();
}

XplBool sockLibInit( void )
{
	/*
	  this gets called only once when SocketLibStart() is called the first time.
	  Windows specific library startup code should happen here
	*/
	WORD		versionRequested = MAKEWORD(2, 2);
	WSADATA		winsockData;

	WSAStartup(versionRequested, &winsockData);

	socFactoryInit();
	return TRUE;
}

