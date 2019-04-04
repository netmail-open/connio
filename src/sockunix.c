#include <errno.h>

#include <xplip.h>
#include <xplmem.h>

#include "sockiop.h"
#include "conniop.h"

// Enable support for timeouts
#define TIMEOUTS 1

#if defined(LINUX)
#include <sys/utsname.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#define IPerror() errno
#define IPclear() errno = 0

#define EVENT_HANDLE_RECEIVER_LIMIT 16
#define ACCEPT_STACK_CACHE_SIZE 5

typedef struct _CONN_IOCP_PENDING {
    XplMutex mutex;

    Connection *head;
    Connection *tail;
} EPOLLPending;

typedef struct _CONN_EPOLL_HANDLE {
    struct _CONN_EPOLL_HANDLE *next;
    struct _CONN_EPOLL_HANDLE *previous;

    ConnEventHandleFlags flags;

    struct {
        ConnHandle *handle;

        Connection *shutdown;
    } conn;

    struct {
        EPOLLPending accept;
        EPOLLPending connect;
        EPOLLPending disconnect;
        EPOLLPending receive;
        EPOLLPending send;
    } pending;

    struct {
        XplMutex mutex;

        int send;
        int accept;
        int receive;
    } port;

    struct {
        XplMutex mutex;

        int count;
        int timeOut;

        struct {
            Connection **next;
            Connection *list[EVENT_HANDLE_RECEIVER_LIMIT + 1];
        } conn;
    } wait;
} EPOLLHandle;

typedef struct _CONN_EPOLL_GLOBALS {
    struct {
        XplMutex mutext;

        EPOLLHandle *head;
        EPOLLHandle *tail;
    } allocated;
} EPOLLGlobals;

EPOLLGlobals EPOLL;



// temporary, determine a good max and create multiple lists with one thread servicing each list
#define EPOLL_QUEUE_LEN		1000
#define ENGINE_INITIALIZED	0
#define ENGINE_RUNNING		1
#define ENGINE_SHUTDOWN		2
#define MAX_EVENTS			100
#define EPOLL_TIMEOUT		2000

int socCreate( XplBool iocpThread )
{
	return XplIpSocket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
}

void sockPrintSysErrorEx( const char *function, char *file, int line )
{
	DebugPrintf("Connio Encountered System Error %d %s\n\tfunction: %s\n\tsource: %s:%d\n", errno, XplStrError( errno ), function, file, line );
}


// PUBLIC ENTRY no lock needed because this function does not touch a socket
int SocketTimeoutSet( SocketTimeout *timeout, int milliseconds )
{
	int	previous = *timeout;

	errno= 0;

	*timeout = milliseconds;

	return( previous );
}

// PUBLIC ENTRY no lock needed because this function does not touch a socket
int SocketTimeoutGet( SocketTimeout *timeout )
{
	errno = 0;
    return( *timeout );
}


int sockWaitForReadable( XplSocket sock, SocketTimeout *timeout )
{
    int				ccode;
    struct pollfd	pfd;
#ifdef DEBUG
	int 			loops = 0;
#endif

	pfd.fd = (int)sock;
	pfd.events = POLLIN | POLLPRI;

	do {
		errno = 0;

		/* poll() will wait for timeout if called with a -1 */
		if (-1 == (int) sock) {
			errno = EINVAL;
			return XPL_SOCKET_ERROR;
		}

		ccode = poll(&pfd, 1, *timeout);

		if (ccode > 0) {
			if (0 == (pfd.revents & (POLLHUP | POLLNVAL | POLLERR))) {
				if (pfd.revents & (POLLIN | POLLPRI)) {
					return 0;
				}
			}

			if (pfd.revents & POLLHUP) {
				errno = ECONNRESET;
			} if (pfd.revents & POLLNVAL) {
				errno = EINVAL;
			} else {
				errno = EIO;
			}
			return XPL_SOCKET_ERROR;
		}

		if (!ccode) {
			errno = ETIMEDOUT;
			return XPL_SOCKET_ERROR;
		}

		if (errno != EINTR) {
			return XPL_SOCKET_ERROR;
		}
#ifdef DEBUG
		loops++;
		if( loops > 10 ) {
			printf("POLL: socket %d has been interrupted %d times\n", sock, loops );
		}
#endif
	} while( TRUE );
}

int sockWaitForWritable( XplSocket sock, SocketTimeout *timeout )
{
    int				ccode;
    struct pollfd	pfd;

	pfd.fd = (int)sock;
	pfd.events = POLLOUT;

	errno = 0;

	ccode = poll(&pfd, 1, *timeout);

	if (ccode > 0) {
		if( 0 == ( pfd.revents & ( POLLHUP | POLLERR | POLLNVAL ) ) ) {
			if (pfd.revents & (POLLOUT)) {
				return 0;
			}
		}

		if ( pfd.revents & POLLHUP ) {
			errno = ECONNRESET;
		} else if (pfd.revents & POLLNVAL) {
			errno = EINVAL;
		} else {
			errno = EIO;
		}
	} else if (!ccode) {
		errno = ETIMEDOUT;
	} else if (errno == EINTR) {
		/*
			EINTR appears to be returned on a closed socket.  Let recv deal
			with it.
		*/
		return 0;
	}

    return XPL_SOCKET_ERROR;
}

// PUBLIC ENTRY
int SocketAsyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( sock->slock ) );

	/* it turns out there is no difference in the UNIX case between an async start and a synchronous start */
	ret = socketSyncConnectStart( sock, destSaddr, destSaddrLen, srcSaddr, srcSaddrLen, blockedOn );

	XplLockRelease( &( sock->slock ) );

	return ret;
}

INLINE static int
socketSyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn, SocketTimeout *timeout )
{
	traceEvent( socket, TRACE_EVENT_DISCONNECT_START, NULL, 0 );
	XplIpShutdown( socket->number, 2 );
	*blockedOn = SOCK_EVENT_TYPE_DISCONNECT;
	return( EWOULDBLOCK );
}

// PUBLIC ENTRY
int SocketAsyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	/* it turns out there is no difference in the UNIX case between an async start and a synchronous start */
	ret = socketSyncDisconnectStart( socket, blockedOn, NULL );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

// PUBLIC ENTRY
int SocketSyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn, SocketTimeout *timeout )
{
	int ret;

	XplLockAcquire( &( socket->slock ) );

	/* it turns out there is no difference in the UNIX case between an async start and a synchronous start */
	ret = socketSyncDisconnectStart( socket, blockedOn, timeout );

	XplLockRelease( &( socket->slock ) );

	return ret;
}

int sockDisconnect( XplSocket s )
{
	int err;
	/*
	  With unix non-blocking sockets, shutdown begins the shutdown and
	  returns immediately
	*/
	err = XplIpShutdown( s, 2 );

	if( SOCKET_ERROR == err ) {
		err = errno;
		errno = 0;
	}

	return err;
}

static void _SocketTimeoutAdd( SPollInfo *pollInfo, SocketEvent *event, SocketTimeout *timeOut )
{
	int				timeout;
#ifdef TIMEOUTS
	int				timeoutIndex;
	MemberList	*timeoutList;
#endif

	DebugAssert( event && event->cb.eventFunc );
	DebugAssert( SOCKET_ERROR != event->cs->number );
#ifdef TIMEOUTS
	timeout = SocketTimeoutGet( timeOut );
	if( ( timeout ) && ( SOCK_EVENT_TYPE_LISTEN != event->type ) )
	{
		if( !event->timeoutList )
		{
			timeoutIndex = TIMEOUT_INDEX( pollInfo->now + ( timeout / 1000 ) + ( SOCKET_TIMEOUT_GRANULARITY - 1 ) );
			timeoutList = &pollInfo->timeoutList[timeoutIndex];

			TLogWrite( "SOCKENGINE: Adding timeout for SOCK [%p]", event->cs );
			TLogFlush();
			XplLockAcquire( &timeoutList->lock );
			event->timeoutList = timeoutList;
			if( event->timeoutNext = ( SocketEvent *)( timeoutList->head ) )
			{
				event->timeoutNext->timeoutPrev = event;
			}
			timeoutList->head = ( ListMember * )event;
			XplLockRelease( &timeoutList->lock );
		}
		SeAddFlags( event, FSE_TIMEOUT_PENDING );
	}
#endif
}

static void _SocketTimeoutDelete( SocketEvent *event )
{
#ifdef TIMEOUTS
	MemberList	*timeoutList;
#endif

	DebugAssert( event );
#ifdef TIMEOUTS
	if( ( event->timeoutList ) && ( SOCK_EVENT_TYPE_LISTEN != event->type ) )
	{
		if( timeoutList = event->timeoutList )
		{
	//		printf( "Timeout delete [%p][%p] socket %d\n", event->timeoutList, event, *event->socket );
			TLogWrite( "SOCKENGINE: Removing timeout for SOCK [%p]", event->cs );
			TLogFlush();
			XplLockAcquire( &timeoutList->lock );
			DebugAssert( event->timeoutList );
			if( event->timeoutPrev )
			{
				event->timeoutPrev->timeoutNext = event->timeoutNext;
			}
			else
			{
				timeoutList->head = ( ListMember * )( event->timeoutNext );
			}
			if( event->timeoutNext )
			{
				event->timeoutNext->timeoutPrev = event->timeoutPrev;
			}
			event->timeoutPrev = NULL;
			event->timeoutNext = NULL;
			event->timeoutList = NULL;
			XplLockRelease( &timeoutList->lock );
		}
	}
#endif
	SeRemoveFlags( event, FSE_TIMEOUT_PENDING );
}



static SPollInfo *getEpollHandle( SocketEvent *se )
{
	SPollInfo *pollInfo = NULL;

	switch( se->type ) {
	case SOCK_EVENT_TYPE_LISTEN:
	case SOCK_EVENT_TYPE_READABLE:
		pollInfo = &( SockLib.eventEngine->pollIn );
		break;
	case SOCK_EVENT_TYPE_WRITABLE:
		pollInfo = &( SockLib.eventEngine->pollOut );
		break;
	case SOCK_EVENT_TYPE_DISCONNECT:
	case SOCK_EVENT_TYPE_CONNECT:
	case SOCK_EVENT_TYPE_NONE:
	default:
		DebugAssert( 0 );
		break;
	}
	return pollInfo;
}

static int _epollEventDelete( SocketEvent *event )
{
	SPollInfo	*pollInfo;
	int			idx;

	pollInfo = getEpollHandle( event );

	for( idx = 0; idx < 5; idx++ ) {
		errno = 0;
		if( !epoll_ctl( pollInfo->efd, EPOLL_CTL_DEL, event->cs->number, &event->ev ) ) {
			return 0;
		}

		socketDebug( 4, event, event->type, "epoll_ctl EPOLL_CTL_DEL failure %d", errno );
		traceEvent( event->cs, TRACE_EVENT_ERROR, NULL, errno );
	}
	errno = 0;
	return -1;
}

static XplBool SocketEventComplete( SocketEvent *event )
{
	if( SeGetFlags( event ) & FSE_TIMEOUT_PENDING ) {
		_SocketTimeoutDelete( event );
	}
	if( SeGetFlags( event ) & FSE_IN_ENGINE ) {
		_epollEventDelete( event );
		SeRemoveFlags( event, FSE_IN_ENGINE );
		DebugAssert( ( 0 == SeGetFlags( event ) ) || ( FSE_LISTENER_KILLED == SeGetFlags( event ) ) );
		return TRUE;
	}
	return FALSE;
}

/* calling function must hold the socket lock */
static void killActiveListener( SocketEvent *event )
{
	int sock;
	int err;
	struct sockaddr_in addr;
	int addrSize;

	if( event )	{
		/* mark it killed so that it will not get put back in the epoll queue */
		SeAddFlags( event, FSE_LISTENER_KILLED );
		/* connect to the listener to make sure it comes out of epoll */
		if( event->cs->number > 0 ) {
			addrSize = sizeof( struct sockaddr_in );
			XplIpGetSockName( event->cs->number, (struct sockaddr *)&addr, &addrSize );
			DebugAssert( addr.sin_port );
			if( addr.sin_addr.s_addr == 0 ) {
				addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
			}

			sock = socCreate( FALSE );
			if( sock != -1 ) {
				err = XplIpConnect( sock, (struct sockaddr *)&addr, addrSize );
				if( err ) {
					DebugPrintf("%s ret: %d  errno: %d\n", __func__, err, errno );
				}
				XplIpClose( sock ); /* no SOCK.  This is a temp socket
									   created for the sole purpose of waking up
									   a listener. */
			}
		}
	}
}

// INTERNAL the caller must handle the returned event if there is one
XplBool sockEventKill( SocketEvent *event )
{
	XplBool complete = FALSE;

	if( event ) {
		event->error = ECANCELED;
		if( ( SOCK_EVENT_TYPE_LISTEN == event->type ) ) {
			killActiveListener( event );
		} else {
			complete = SocketEventComplete( event );
		}
	}
	return complete;
}

void sockEventClean( SocketEvent *se )
{
	se->error = 0;
	SeSetFlags( se, 0 );
}


void sockReset( SOCK *socket )
{
}

// INTERNAL
int sockEventInit( SOCK *cs, SocketEvent *event )
{
	memset( event, 0, sizeof( SocketEvent ) );
	event->cs = cs;
	event->ev.data.ptr = event;
	event->error = 0;
	return 0;
}

// INTERNAL
SocketEventType sockEventTranslate( SocketEventType type )
{
	if( type == SOCK_EVENT_TYPE_DISCONNECT ) {
		return SOCK_EVENT_TYPE_READABLE;
	}
	if( type == SOCK_EVENT_TYPE_CONNECT ) {
		return SOCK_EVENT_TYPE_WRITABLE;
	}
	return type;
}

// INTERNAL caller must hold slock
int sockEventAdd( SOCK *cs, SocketEvent *se, SocketTimeout *timeOut, struct sockaddr *dst, socklen_t dstLen, XplBool *doneAlready )
{
	int err;
	SPollInfo *pollInfo;

	errno = 0;
	*doneAlready = FALSE; // always FALSE on UNIX

	DebugAssert( !( SeGetFlags( se ) ) ); /* no flags should be set when adding an event to the engine */

	pollInfo = getEpollHandle( se );
	if( POLL_READ == pollInfo->type ) {
		se->ev.events = EPOLLET|EPOLLONESHOT|EPOLLHUP|EPOLLIN;
	} else {
		se->ev.events = EPOLLET|EPOLLONESHOT|EPOLLHUP|EPOLLOUT;
	}

	SeAddFlags( se, FSE_IN_ENGINE );
	if( epoll_ctl( pollInfo->efd, EPOLL_CTL_ADD, cs->number, &se->ev ) ) {
		SeRemoveFlags( se, FSE_IN_ENGINE );
		socketDebug( 3, se, se->type, "epoll_ctl EPOLL_CTL_ADD failure %d", errno );
		err = errno;
		errno = 0;
		return err;
	}
	if( se->cs->listener && se->cs->listener->eventCallback ) {
		se->cs->listener->eventCallback( LISTENER_STARTED, 0, 0, se, cs, cs->number );
	}
	_SocketTimeoutAdd( pollInfo, se, timeOut );
	return 0;
}

/*
  INUSE needs to already be set
  Caller must hold the socket lock
 */
static int _SocketEventRequeue( SPollInfo *pollInfo, SocketEvent *event )
{

	DebugAssert( SeGetFlags( event ) & FSE_IN_ENGINE );
	event->error = 0;
	if( !epoll_ctl( pollInfo->efd, EPOLL_CTL_MOD, event->cs->number, &event->ev ) )
	{
		return 0;
	}
	event->error = errno;
	socketDebug( 3, event, event->type, "epoll_ctl EPOLL_CTL_MOD failure %d", errno );
	traceEvent( event->cs, TRACE_EVENT_ERROR, NULL, errno );

	return -1;
}

XplBool sockEventListenerRequeue( SOCK *socket, SocketEvent *se )
{
	SeRemoveFlags( se, ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED ) );
	if( !_SocketEventRequeue( &( SockLib.eventEngine->pollIn ), se ) ) {
		/* successfully requeued */
		return FALSE;
	}
	/* requeue has failed; */
	return SocketEventComplete( se );
}

#ifdef MYREALBOX
static void _SocketError( SocketEventEngine *engine, const char *format, ... )
{
	va_list	args;
	char	buffer[1024];

	if( !engine->log )
	{
		strprintf( buffer, sizeof( buffer ), NULL, "%s.log", MemConsumer() );
		engine->log = fopen( buffer, "ab+" );
	}
	if( engine->log )
	{
		va_start( args, format );
		vstrprintf( buffer, sizeof( buffer ), NULL, format, args );
		va_end( args );
		fwrite( buffer, 1, strlen( buffer ), engine->log );
		fflush( engine->log );
	}
}
#endif

// caller must hold the slock
static XplBool _socketEventHandle( SocketEventEngine *engine, SocketEvent *event, uint32 events, AcceptEvent **allocatedAcceptEvent )
{
	int				sLen;
	AcceptEvent		*acceptEvent;
	XplSocket		socket;
	struct			sockaddr_in sa;
	XplBool			complete;

	complete = FALSE;

#ifdef DEBUG_SOCKET_EVENTS
	socketDebug( 2, event, event->type, NULL );
#endif
	if( events & ( POLLERR | POLLHUP | POLLNVAL ) ) {
		if( events & POLLHUP ) {
			event->error = ECONNRESET;
		} else if ( events & POLLNVAL ) {
			event->error = EINVAL;
		} else {
			event->error = EIO;
		}
	}
	switch( event->type )
	{
		case SOCK_EVENT_TYPE_LISTEN:
			TLogWrite( "SOCKENGINE: Handling SOCK [%p] LISTEN", event->cs );
			TLogFlush();
			if( SeGetFlags( event ) & FSE_LISTENER_KILLED ) {
				if( event->cs->listener && event->cs->listener->eventCallback ) {
					event->cs->listener->eventCallback( LISTENER_KILLED, 0, 0, event, event->cs, event->cs->number );
					event->cs->listener->eventCallback( LISTENER_STOPPED, 0, 0, event, event->cs, event->cs->number );
				}

				complete = SocketEventComplete( event );
				break;
			}

			sLen = sizeof( struct sockaddr_in );
			socket = XplIpAccept( event->cs->number, (struct sockaddr *)&sa, &sLen );
			if( socket != SOCKET_ERROR ) {
				if( *allocatedAcceptEvent ) {
					acceptEvent = *allocatedAcceptEvent;
					*allocatedAcceptEvent = NULL;
					acceptEventInit( acceptEvent, event, socket );
#ifdef DEBUG_SOCKET_EVENTS
					socketDebug( 2, acceptEvent, acceptEvent->type, NULL );
#endif
					acceptReady( acceptEvent );
				} else {
					/*
					  We must be really low on memory.  But we can't
					  afford to block in this thread.  The best we can
					  do is close the new socket.
					*/
                    XplIpClose( socket );
                    socket = SOCKET_INVALID;
				}

				/* Throttled Listeners
				   - When the engine is busy, all throttled listeners get sent to the event layer
				   as they naturally come out of epoll with new connections.
				   - the event layer resubmits these listeners when the engine is no longer busy.
				*/

				if( ( event->type == SOCK_EVENT_TYPE_LISTEN ) &&
					listenerShouldThrottle( event ) &&
					listenerStartThrottling( event ) )
				{	/* pause the listener - the engine is busy

					   This event is not really complete, it is just disabled.
					   We do not call SocketEventComplete() because it would
					   delete the socket from the epoll queue. Instead, we call
					   AsyncEventInit(). It will create the same kind of
					   argument needed to start a thread.  That thread will
					   recongnize this event as a paused listener and start to
					   monitor load until it drops below the throttling
					   threshold.  At that point, the thread will reenable
					   the listener in epoll and exit.
					*/
					DebugAssert( ( ( FSE_LISTENER_PAUSED | FSE_IN_ENGINE ) == SeGetFlags( event ) ) ||
								 ( ( FSE_LISTENER_PAUSED | FSE_IN_ENGINE | FSE_LISTENER_KILLED ) == SeGetFlags( event ) ) ||
								 ( ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED | FSE_IN_ENGINE ) == SeGetFlags( event ) ) ||
								 ( ( FSE_LISTENER_PAUSED | FSE_LISTENER_SUSPENDED | FSE_IN_ENGINE | FSE_LISTENER_KILLED ) == SeGetFlags( event ) ) );
					// These are the only flag combinations we expect here
					complete = TRUE;
				}
				else if( _SocketEventRequeue( &engine->pollIn, event ) )
				{
					if( event->cs->listener && event->cs->listener->eventCallback ) {
						event->cs->listener->eventCallback( LISTENER_STOPPED, 0, 0, event, event->cs, event->cs->number );
					}
					complete = SocketEventComplete( event );
				}
			}
			else if( _SocketEventRequeue( &engine->pollIn, event ) )
			{
				if( event->cs->listener && event->cs->listener->eventCallback ) {
					event->cs->listener->eventCallback( LISTENER_STOPPED, 0, 0, event, event->cs, event->cs->number );
				}
				complete = SocketEventComplete( event );
			}
			break;

		case SOCK_EVENT_TYPE_READABLE:
			TLogWrite( "SOCKENGINE: Handling SOCK [%p] READABLE", event->cs );
			TLogFlush();
			complete = SocketEventComplete( event );
			break;

		case SOCK_EVENT_TYPE_WRITABLE:
			TLogWrite( "SOCKENGINE: Handling SOCK [%p] WRITABLE", event->cs );
			TLogFlush();
			complete = SocketEventComplete( event );
			break;

		default:
			TLogWrite( "SOCKENGINE: Handling SOCK [%p] INVALID", event->cs );
			TLogFlush();
			event->error = EINVAL;
			complete = SocketEventComplete( event );
			break;

	}
	return complete;
}

/*
  AECRefill(), AECFlush(), and acceptEventAcquire() are NOT designed to be
  thread safe on purpose. Only one thread monitors listeners in the unix version
  of this library.  Only one thread will ever call them so they can take a few
  short-cuts.
*/
static void
AECRefill( AcceptEventCache *cache )
{
	if( cache->next > 0 ) {
		AECLock( cache );
		while( cache->next > 0 ) {
			cache->next--;
			cache->fastList[ cache->next ] = acceptEventPop( cache );
			if( !cache->fastList[ cache->next ] ) {
				cache->next++;
				break;
			}
			//printf("[%p] refilled %d\n", cache, cache->next );
		}
		AECUnlock( cache );

		while( cache->next > 0 ) {
			cache->next--;
			cache->fastList[ cache->next ] = AEMalloc();
			if( !cache->fastList[ cache->next ] ) {
				cache->next++;
				break;
			}
			//printf("[%p] created %d\n", cache, cache->next );
		}
	}
}

static void
AECFlush( AcceptEventCache *cache )
{
	while( cache->fastList[ cache->next ] ) {
		acceptEventRelease( cache->fastList[ cache->next ] );
		cache->fastList[ cache->next ] = NULL;
		cache->next++;
	}
}

static AcceptEvent *
acceptEventAcquire( AcceptEventCache *cache )
{
	AcceptEvent *acceptEvent;

	if( cache->fastList[ cache->next ] ) {
		acceptEvent = cache->fastList[ cache->next ];
		cache->fastList[ cache->next ] = NULL;
		//printf("[%p] found fast %d\n", cache, cache->next );
		cache->next++;
	} else {
		acceptEvent = acceptEventAlloc( cache );
		//printf("[%p] found slow\n", cache );
	}
	return acceptEvent;
}

static int _PollThread( SocketEventEngine *engine, SPollInfo *pollInfo )
{
	struct SocketEvent	*event;
	XplBool eventComplete;
	uint32 eventbits;
#ifdef TIMEOUTS
	struct SocketEvent	*events, *next;
#endif
	int					ready, l, timeoutIndex;
	struct epoll_event	eventArray[MAX_EVENTS];
	AcceptEvent *acceptEvent = NULL;

	timeoutIndex = pollInfo->timeoutIndex;

	if( POLL_READ == pollInfo->type ) {
		acceptEvent = acceptEventAlloc( &( engine->aeCache ) );
	}

	while( ENGINE_RUNNING == engine->state )
	{
		if( POLL_READ == pollInfo->type ) {
			AECRefill( &( engine->aeCache ) );		 // fill the fast cache
		}
		while (((ready = epoll_wait( pollInfo->efd, eventArray, MAX_EVENTS, EPOLL_TIMEOUT )) < 0) && (errno == EINTR));
		if( ready > 0 ) {
			for(l=0;l<ready;l++)
			{
				event = eventArray[l].data.ptr;
				eventComplete = FALSE;
				eventbits = eventArray[l].events;
				XplLockAcquire( &( event->cs->slock ) );
				if( SeGetFlags( event ) & FSE_IN_ENGINE ) {
					eventComplete = _socketEventHandle( engine, event, eventbits, &acceptEvent );
				} else {
					socketDebug( 5, event, event->type, "Throwing away spurrious event" );
				}
				XplLockRelease( &( event->cs->slock ) );
				if( eventComplete ) {
					AThreadBegin( &( event->cb.thread ) );
				}
				if( POLL_READ == pollInfo->type ) {
					if( !acceptEvent ) {
						/* get one ready for the next inbound connection */
						acceptEvent = acceptEventAcquire( &( engine->aeCache ) );
					}
				} else {
					AThreadCountUpdate();
				}
			}
		}
#ifdef TIMEOUTS
		pollInfo->now = time( NULL );
		timeoutIndex = TIMEOUT_INDEX( pollInfo->now );
		if( pollInfo->timeoutIndex != timeoutIndex )
		{
			pollInfo->timeoutIndex = timeoutIndex;
			XplLockAcquire( &pollInfo->timeoutList[timeoutIndex].lock );
			events = ( SocketEvent * )( pollInfo->timeoutList[timeoutIndex].head );
			pollInfo->timeoutList[timeoutIndex].head = NULL;
			pollInfo->timeoutList[timeoutIndex].tail = &pollInfo->timeoutList[timeoutIndex].head;
			for( event = events; event; event = ( SocketEvent * )( event->async.next ) )
			{
				XplLockAcquire( &( event->cs->slock ) );
				DebugAssert( SeGetFlags( event ) & FSE_TIMEOUT_PENDING );
				SeRemoveFlags( event, FSE_TIMEOUT_PENDING );
				DebugAssert( event->timeoutList );
				event->timeoutList = NULL;
				event->timeoutPrev = NULL;
				event->async.next = ( ListMember * )event->timeoutNext;
				event->timeoutNext = NULL;
				XplLockRelease( &( event->cs->slock ) );
			}
			XplLockRelease( &pollInfo->timeoutList[timeoutIndex].lock );
			for(event=events;event;event=next)
			{
				next = ( SocketEvent *)( event->async.next );
#if 0
				switch( event->type )
				{
					case SOCK_EVENT_TYPE_LISTEN:
						printf( "LISTEN     " );
						break;

					case SOCK_EVENT_TYPE_ACCEPT:
						printf( "ACCEPT     " );
						break;

					case SOCK_EVENT_TYPE_CONNECT:
						printf( "CONNECT    " );
						break;

					case SOCK_EVENT_TYPE_DISCONNECT:
						printf( "DISCONNECT " );
						break;

					case SOCK_EVENT_TYPE_READABLE:
						printf( "READABLE   " );
						break;

					case SOCK_EVENT_TYPE_WRITABLE:
						printf( "WRITABLE   " );
						break;

					case SOCK_EVENT_TYPE_RECEIVE:
						printf( "RECEIVE    " );
						break;

					case SOCK_EVENT_TYPE_SEND:
						printf( "SEND       " );
						break;

					default:
						printf( "UNKNOWN    " );
						break;
				}
				printf( "Timeout        [%p][%p] socket %d\n", &pollInfo->timeoutList[timeoutIndex], event, *event->socket );
#endif
				eventComplete = FALSE;

				XplLockAcquire( &( event->cs->slock ) );

				event->error = ETIMEDOUT;
				eventComplete = SocketEventComplete( event );

				XplLockRelease( &( event->cs->slock ) );

				if( eventComplete ) {
					AThreadBegin( &( event->cb.thread ) );
				}
			}
		}
#endif
	}
	if( POLL_READ == pollInfo->type ) {
		if( acceptEvent ) {
			acceptEventRelease( acceptEvent );
			acceptEvent = NULL;
		}

		AECFlush( &( engine->aeCache ) );		 // flush the fast cache
	}

	return 0;
}

static int _PollIn( XplThread thread )
{
	SocketEventEngine *engine = (SocketEventEngine *)( thread->context );
	return _PollThread( engine, &engine->pollIn );
}

static int _PollOut( XplThread thread )
{
	SocketEventEngine *engine = (SocketEventEngine *)( thread->context );
	return _PollThread( engine, &engine->pollOut );
}

static int _SocketPollInit( SPollInfo *pollInfo, int type )
{
	if( -1 != ( pollInfo->efd = epoll_create( EPOLL_QUEUE_LEN ) ) )
	{
		pollInfo->type = type;
		return 0;
	}
	return -1;
}

int sockEngineInit( SocketEventEngine *engine )
{
	int l;


	if( !_SocketPollInit( &engine->pollIn, POLL_READ ) )
	{
		if( !_SocketPollInit( &engine->pollOut, POLL_WRITE ) )
		{
			engine->pollIn.now = engine->pollOut.now = time( NULL );
			engine->pollIn.timeoutIndex = engine->pollOut.timeoutIndex = TIMEOUT_INDEX( engine->pollIn.now );
			for(l=0;l<SOCKET_TIMEOUT_LISTS;l++)
			{
				XplLockInit( &engine->pollIn.timeoutList[l].lock );
				engine->pollIn.timeoutList[l].head = NULL;
				engine->pollIn.timeoutList[l].tail = &engine->pollIn.timeoutList[l].head;
				XplLockInit( &engine->pollOut.timeoutList[l].lock );
				engine->pollOut.timeoutList[l].head = NULL;
				engine->pollOut.timeoutList[l].tail = &engine->pollOut.timeoutList[l].head;
			}
			XplThreadStart( engine->threadGroup, _PollIn, engine, NULL );
			XplThreadStart( engine->threadGroup, _PollOut, engine, NULL );
			return 0;
		}
	}
	return -1;
}

int sockEngineShutdown( SocketEventEngine *engine )
{
	while( XplThreadCount( engine->threadGroup ) > 0 ) {
		XplDelay( 500 );
	}
	return 0;
}

void sockCreatorStart( void )
{
	/* this does nothing on linux */
}

void sockCreatorStop( void )
{
	/* this does nothing on linux */
}

XplBool sockLibInit( void )
{
	/*
	  this gets called only once when SocketLibStart() is called the first time.
	  Linux specific library startup code should happen here
	*/
	return TRUE;
}


#endif //LINUX
