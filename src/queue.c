#define DETECT_DUPLICATE_SOCKET_EVENTS 1

#include <xplip.h>
#include <xplmem.h>

#include <stdio.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "conniop.h"

/* this caller of this function must hold the event lock */
static void ConnEventFreeListAdd( ConnEvent *ce )
{
	ce->previous = NULL;
	ce->next = ce->conn->async.transaction.freeList;
	ce->conn->async.transaction.freeList = ce;
}

/* this caller of this function must hold the event lock */
static ConnEvent *
ConnEventFreeListGrab( ConnectionPriv *conn )
{
	ConnEvent *ce;

	ce = conn->async.transaction.freeList;
	conn->async.transaction.freeList = NULL;
	return ce;
}

/* this caller of this function should NOT hold the event lock */
static void ConnEventFreeListDestroy( ConnectionPriv *conn, ConnEvent *freeList )
{
	ConnEvent *ce;
	ConnEvent *next;

	ce = freeList;
	while( ce ) {
		next = ce->next;
		ConnEventFree( conn, ce );
		ce = next;
	}
}

 
INLINE static ConnEvent *
EventQueuePeek( ConnectionPriv *conn )
{
	return( conn->async.transaction.queue.head );
}


int EventQueueCountFlushes( ConnectionPriv *conn )
{
	ConnEvent *ce;
	int flushesQueued;

	flushesQueued = 0;
	ce = conn->async.transaction.queue.head;
	while( ce ) {
		if( ce->type == CONN_EVENT_FLUSH ) {
			flushesQueued++;
		}
		ce = ce->next;
	}
	return flushesQueued;
}

static void
EventQueueAppendEvent( ConnectionPriv *conn, ConnEvent *connEvent )
{
	ConnEventList *list = &( conn->async.transaction.queue );

	connEvent->next = NULL;
	connEvent->status = CONN_EVENT_QUEUED;
	if( list->tail ) {
		list->tail->next = connEvent;
		connEvent->previous = list->tail;
	} else {
		list->head = connEvent;
		connEvent->previous = NULL;
	}
	list->tail = connEvent;
	list->count++;
	LogConnEventEnqueue( conn, connEvent );

#ifdef DEBUG
	if (list->count && !(list->count % 100)) {
		printf("Conn %p has %d events\n", conn, (int)list->count);
	}
#endif
}

static void
EventQueueRemoveEvent( ConnectionPriv *conn, ConnEvent *connEvent )
{
	ConnEventList *list = &( conn->async.transaction.queue );

	LogConnEventDequeue( conn, connEvent );
	list->count--;
	if( connEvent->previous ) {
		connEvent->previous->next = connEvent->next;
	} else {
		list->head = connEvent->next;
	}
	if( connEvent->next ) {
		connEvent->next->previous = connEvent->previous;
	} else {
		list->tail = connEvent->previous;
	}
	connEvent->next = connEvent->previous = NULL;
}


static ConnEvent *
GetNextEnqueuedEvent( ConnectionPriv *conn )
{
	ConnEvent *event;

	event = EventQueuePeek( conn );
	if( event ) {
		DebugAssert( event->type );
		EventQueueRemoveEvent( conn, event );
	}
	return( event );
}

static ConnEvent *
GetNextEnqueuedSendEvent( ConnectionPriv *conn )
{
	ConnEvent *event;

	event = EventQueuePeek( conn );
	while( event ) {
		switch( event->type ) {
			/* read events */
			case CONN_EVENT_READ:
			case CONN_EVENT_READ_COUNT:
			case CONN_EVENT_READ_UNTIL_PATTERN:
			case CONN_EVENT_READ_TO_CB:
			case CONN_EVENT_READ_TO_CONN:
			{
				event = event->next;
				/* there is a recv event in progress, */
				/* but that doesn't prevent us from starting a send */
				continue;
			}

			/* send events */
			case CONN_EVENT_WRITE_FROM_CB:
			case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
			case CONN_EVENT_FLUSH: {
				EventQueueRemoveEvent( conn, event );
				return( event );
			}

			/* state changing events */
			case CONN_EVENT_CONNECT:
			case CONN_EVENT_ACCEPT:
			case CONN_EVENT_DISCONNECT:
			case CONN_EVENT_SSL_ACCEPT:
			case CONN_EVENT_TLS_ACCEPT:
			case CONN_EVENT_SSL_CONNECT:
			case CONN_EVENT_TLS_CONNECT:
			case CONN_EVENT_SSL_DISCONNECT: {
				return( NULL );
			}
		}
		event = event->next;
	}
	/* we did not find a receive event before the next state changing event */
	return( NULL );
}

static ConnEvent *
GetNextEnqueuedReceiveEvent( ConnectionPriv *conn )
{
	ConnEvent *event;

	event = EventQueuePeek( conn );

	while( event ) {
		switch( event->type ) {
			/* read events */
			case CONN_EVENT_READ:
			case CONN_EVENT_READ_COUNT:
			case CONN_EVENT_READ_UNTIL_PATTERN:
			case CONN_EVENT_READ_TO_CB:
			case CONN_EVENT_READ_TO_CONN:
			{
				EventQueueRemoveEvent( conn, event );
				return( event );
			}

			/* send events */
			case CONN_EVENT_WRITE_FROM_CB:
			case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
			case CONN_EVENT_FLUSH: {
				event = event->next;
				/* there is a send event in progress, */
				/* but that doesn't prevent us from starting a read */
				continue;
			}

			/* state changing events */
			case CONN_EVENT_CONNECT:
			case CONN_EVENT_ACCEPT:
			case CONN_EVENT_DISCONNECT:
			case CONN_EVENT_SSL_ACCEPT:
			case CONN_EVENT_TLS_ACCEPT:
			case CONN_EVENT_SSL_CONNECT:
			case CONN_EVENT_TLS_CONNECT:
			case CONN_EVENT_SSL_DISCONNECT: {
				/* we did not find a receive event before the next state changing event */
				return( NULL );
			}
		}
		event = event->next;
	}
	/* we did not find a receive event before the next state changing event */
	return( NULL );
}

static void
CancelEvent( ConnEvent *ce )
{
	LogConnEventCanceled( ce->conn, ce );
	if( ( ce->type == CONN_EVENT_SSL_CONNECT ) || ( ce->type == CONN_EVENT_TLS_CONNECT ) || ( ce->type == CONN_EVENT_SSL_ACCEPT ) || ( ce->type == CONN_EVENT_TLS_ACCEPT ) ) {
		if( ce->socket->ssl.new ) {
			SocketSSLFree( ce->socket->ssl.new );
			ce->socket->ssl.new = NULL;
		}
	}
	ce->error = ECANCELED;
}

INLINE static XplBool
ConnEventsInProgress( ConnectionPriv *conn )
{
	if( conn->activeEvent.connect || conn->activeEvent.receive || conn->activeEvent.send ) {
		return( TRUE );
	}
	return( FALSE );
}

static void
CancelEventsInProgress( ConnectionPriv *conn, ConnEvent *currentConnEvent )
{
	/*
	  These events are doing things right now and we need them to stop what they are doing.  We will do
	  couple of things to get their attention:

	  1. set the error in the conn event to ECANCELED so the process functions will not work on
	     it anymore.
	  2. call SocketEventKill() on events that may still be in the socket event engine.  The socket
	     event engine will ignore attempts to kill socket events that do not belong to it.
	*/
	if( ConnEventsInProgress( conn ) ) {
		/* there is still at least one outstanding event */
		if( conn->sp->number != SOCKET_INVALID ) {
			if( ( conn->activeEvent.connect ) && ( conn->activeEvent.connect != currentConnEvent ) ) {
				if( ( conn->activeEvent.connect->type != CONN_EVENT_DISCONNECT ) && ( conn->activeEvent.connect->type != CONN_EVENT_SSL_DISCONNECT ) ) {
					LogConnEventKilled( conn, conn->activeEvent.connect );
					conn->activeEvent.connect->error = ECANCELED;
					SocketEventKill( conn->sp, SOCK_EVENT_TYPE_CONNECT );
				}
			} else {
				if( ( conn->activeEvent.send ) && ( conn->activeEvent.send != currentConnEvent ) ) {
					LogConnEventKilled( conn, conn->activeEvent.send );
					conn->activeEvent.send->error = ECANCELED;
					SocketEventKill( conn->sp, SOCK_EVENT_TYPE_WRITABLE );
				}
				if( ( conn->activeEvent.receive ) && ( conn->activeEvent.receive != currentConnEvent ) ) {
					LogConnEventKilled( conn, conn->activeEvent.receive );
					conn->activeEvent.receive->error = ECANCELED;
					SocketEventKill( conn->sp, SOCK_EVENT_TYPE_READABLE );
				}
			}
		}
	}
}

static void
CancelQueuedEvents( ConnectionPriv *conn )
{
	ConnEvent *ce;

	while( ( ce = GetNextEnqueuedEvent( conn ) ) ) {
		ConnEventFreeListAdd( ce );
	}
}

static XplBool SetAsyncProblem( ConnectionPriv *conn, ConnEventStatus status, int error, const char *file, int line )
{
	if( !conn->async.transaction.problem ) {
		/* this is the first event to have a problem */
		conn->async.transaction.problemReal.status = status;
		conn->async.transaction.problemReal.error = error;
		conn->async.transaction.problemReal.file = file;
		conn->async.transaction.problemReal.line = line;
		conn->async.transaction.problem = &( conn->async.transaction.problemReal );
		return TRUE;
	}
	return FALSE;
}

void EventQueueAbort( ConnectionPriv *conn, ConnEvent *connEvent, ConnEventStatus status, int error, const char *file, int line )
{
	if( SetAsyncProblem( conn, status, error, file, line ) ) {
		/* this is the first problem, kill the rest of the queue */
		CancelQueuedEvents( conn );
		CancelEventsInProgress( conn, connEvent );
	}
}

/*
   ConnEventNext() searches the queue for events that can be started
   it returns a NULL when:
   - the queue is empty
   - none of the queued events can be started at the moment
   The consumer must hold the event lock when calling this function.
*/
static ConnEvent *
ConnEventNext( ConnectionPriv *conn )
{
	ConnEvent *nextConnEvent;

	if( conn->async.transaction.problem ) {
		/* another event had a problem */
		LogConnEventNext( conn, "NA queue aborted" );
		return( NULL );
	}

	if( !( conn->async.transaction.queue.head ) ) {
		/* there are no queued events */
		LogConnEventNext( conn, "NA no queued events" );
		return( NULL );
	}

	if( conn->activeEvent.connect ) {
		/* a socket connect/disconnect event is underway - no queued events can be started */
		LogConnEventNext( conn, "NA connect/disconnect event in progress" );
		return( NULL );
	}

	if( conn->activeEvent.receive ) {
		/* a receive event is in progress */
		if( conn->activeEvent.send ) {
			/* a send event is in progress */
			LogConnEventNext( conn, "NA send and receive events in progress" );
			return( NULL );
		}
		/* nothing is being sent right now */
		nextConnEvent = GetNextEnqueuedSendEvent( conn );
		if( !nextConnEvent ) {
			/* there isn't anything ready to send in the queue */
			LogConnEventNext( conn, "NA no queued send" );
			return( NULL );
		}

		/* start send event */
		LogConnEventNext( conn, "start send event" );
		return( nextConnEvent );
	}

	/* nothing is being received right now */
	if( conn->activeEvent.send ) {
		/* a send event is in progress */
		nextConnEvent = GetNextEnqueuedReceiveEvent( conn );
		if( !nextConnEvent ) {
			/* there isn't anything ready to be received in the queue */
			LogConnEventNext( conn, "NA no queued receive" );
			return( NULL );
		}

		/* start receive event */
		LogConnEventNext( conn, "start receive event" );
		return( nextConnEvent );
	}

	/* there are no running events */
	nextConnEvent = GetNextEnqueuedEvent( conn );
	if( nextConnEvent ) {
		LogConnEventNext( conn, "start next event" );
		return( nextConnEvent );
	}
	LogConnEventNext( conn, "NA what the . . .?" );
	return( NULL );
}

/*
   ConnEventsProcess() will keep working on queued conn events until
   they are all processed or blocking network IO is required.

   The caller must hold the Event Lock

   ConnEventsProcess() purposefully releases the event lock while
   processing individual conn events so that more than one thread
   can work on a conn's queue at the same time. The number of threads
   in ConnEventsProcess() is counted so that only the last thread out
   will trigger a consumer callback.
*/
static XplBool
ConnEventsProcess( ConnectionPriv *conn, ConnEvent *engineConnEvent )
{
	ConnEvent *connEvent;
	ConnEvent *freeList;
	ConnEventStatus status;

	conn->async.transaction.threads++;

	connEvent = engineConnEvent;
	/*
	  engineConnEvent, if passed in, was started previously, marked
	  in progress, submitted to the async engine and the engine has
	  just notified this layer that something has just completed
	  on the socket associated with this event. the process function
	  for this conn event needs to be called again.
	*/

	/* process events until nothing else can be worked on */
	for( ; ; ) {
		if( !connEvent ) {
			/* look for queued work */
			connEvent = ConnEventNext( conn );
			if( !connEvent ) {
				/* nothing else can be started at the moment */
				break;
			}
			ConnEventInProgressSet( conn, connEvent );
		}
		freeList = ConnEventFreeListGrab( conn );

		EventLockRelease( conn );   /***** UNLOCK *****/

		ConnEventFreeListDestroy( conn, freeList );
		LogConnEventProcess( conn, connEvent );
		status = ConnEventProcess( connEvent );
		LogConnEventDone( conn, connEvent );

		EventLockAcquire( conn );   /***** LOCK *****/

		if( CONN_EVENT_IN_PROGRESS == status ) {
			/*
			  if ConnEventProcess() returns 'CONN_EVENT_IN_PROGRESS', the returning
			  thread has given up control of the conn event to the asynchronous event
			  engine and is not allowed to work on or modify that conn event in any way.
			  Setting the status in the conn event to CONN_EVENT_IN_PROGRESS must be
			  done in the process() function before it hands-off the conn event to
			  the async engine.
			*/
			connEvent = NULL;
			continue;
		}

		/* the event is complete or it failed.
		  either way, there are things that we
		  need to do while holding the event lock.
		*/

		/* clear in progress */
		ConnEventInProgressClear( conn, connEvent );
		/* capture transaction stats */
		if( connEvent->eventClass == CONN_EVENT_CLASS_RECEIVE ) {
			conn->async.transaction.bytes.received += connEvent->transferred;
		} else if( connEvent->eventClass == CONN_EVENT_CLASS_SEND ) {
			conn->async.transaction.bytes.sent += connEvent->transferred;
		}
		/* handle errors */
		if( conn->async.transaction.problem ) {
			/* another event had a problem - cancel this one */
			CancelEvent( connEvent );
		} else if( ( connEvent->status == CONN_EVENT_TIMED_OUT ) || ( connEvent->status == CONN_EVENT_FAILED ) || ( connEvent->status == CONN_EVENT_KILLED ) ) {
			/* this event had a problem - abort the whole queue */
			EventQueueAbort( conn, connEvent, connEvent->status, connEvent->error, connEvent->source.file, connEvent->source.line );
		} else if( connEvent->status == CONN_EVENT_PARTIAL ) {
			/* Create a problem object so we can remember the status */
			SetAsyncProblem( conn, connEvent->status, connEvent->error, connEvent->source.file, connEvent->source.line );
		} else if ( conn->connHandle->shuttingDown &&
					( connEvent->type != CONN_EVENT_FLUSH ) &&
					( connEvent->type != CONN_EVENT_SSL_DISCONNECT ) &&
					( connEvent->type != CONN_EVENT_DISCONNECT ) ) {
			/* the handle is trying to shut down and this is not a shutdown event */
			CancelEvent( connEvent );
			EventQueueAbort( conn, connEvent, CONN_EVENT_KILLED, ESHUTDOWN, connEvent->source.file, connEvent->source.line );
		}
		/* recycle event */
		ConnEventFreeListAdd( connEvent );
		connEvent = NULL;
	}
	conn->async.transaction.threads--;

	/* determine queue status */
	if( conn->async.transaction.queue.head ) {
		LogConnEventsProcess( conn, CONN_EVENT_IN_PROGRESS, "queued events remain" );
		return( TRUE );
	}

	/* there are no more queued events */

	if( ConnEventsInProgress( conn ) ) {
		LogConnEventsProcess( conn, CONN_EVENT_IN_PROGRESS, "events in progress" );
		return( TRUE );
	}

	/* no events are being worked on */

	if( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) {
		LogConnEventsProcess( conn, CONN_EVENT_IN_PROGRESS, "waiting for submit" );
		return( TRUE );
	}

	/* the queue has been submitted */
	DebugAssert( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE );

	if( conn->async.transaction.threads > 0 ) {
		/*
		  we do not want to trigger the callback by returning something other
		  than CONN_STATE_IN_PROGRESS while other threads are working
		   on this queue.  The last thread out will do that.
		*/
		LogConnEventsProcess( conn, CONN_EVENT_IN_PROGRESS, "other threads" );
		return( TRUE );
	}

	/* this is the last thread out */
	ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE );

	if( conn->async.transaction.problem ) {
		_AsyncProblem *problem = ( _AsyncProblem * )conn->async.transaction.problem;

		if( problem->status != CONN_EVENT_PARTIAL ) {
			LogConnEventsProcess( conn, problem->status, "queue failed" );
		} else {
			LogConnEventsProcess( conn, problem->status, "queue partial" );
		}
		DebugAssert( ( problem->status == CONN_EVENT_FAILED ) || ( problem->status == CONN_EVENT_TIMED_OUT ) || ( problem->status == CONN_EVENT_KILLED ) || ( problem->status == CONN_EVENT_PARTIAL ) );
		return( FALSE );
	}

	LogConnEventsProcess( conn, CONN_EVENT_COMPLETE, "queue complete" );
	return( FALSE );
}


/* the caller of queuingPermitted() must hold the event lock */

INLINE static XplBool
queueingPermitted( ConnectionPriv *conn, const char *file, const int line )
{
	switch( conn->state ) {
		case CONN_STATE_IDLE:
		case CONN_STATE_ASYNCHRONOUS_CLIENT: {
			return( TRUE );
		}
		case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
		case CONN_STATE_ASYNCHRONOUS_ENGINE: {
			DebugPrintf("CONNIO(%s:%d): EventEnqueue() failed because the conn is currently owned by the asynchronous engine!\r\n", file, line );
			DebugAssert( 0 );
			errno = EBUSY;
			break;
		}
		case CONN_STATE_SYNCHRONOUS: {
			DebugPrintf("CONNIO(%s:%d): EventEnqueue() failed because the conn is currently being used for synchronous events!\r\n", file, line );
			DebugAssert( 0 );
			errno = EBUSY;
			break;
		}
		case CONN_STATE_SYNCHRONOUS_LISTENER:
		case CONN_STATE_ASYNCHRONOUS_LISTENER: {
			DebugPrintf("CONNIO(%s:%d): EventEnqueue() failed because the conn is currently being used for listening on a server socket!\r\n", file, line );
			DebugAssert( 0 );
			errno = EBUSY;
			break;
		}
		case CONN_STATE_CACHED:
		case CONN_STATE_FREE: {
			DebugPrintf("CONNIO(%s:%d): EventEnqueue() failed because the conn is NOT valid!\r\n", file, line );
			DebugAssert( 0 );
			errno = EINVAL;
			break;
		}
	}
	return( FALSE );
}

/* the caller of submitProblem() must hold the event lock */
INLINE static int
submitProblem( ConnectionPriv *conn, XplBool queryOnly, const char *file, const int line )
{
	int err = 0;
	char *errFormat;

	switch( conn->state ) {
		case CONN_STATE_ASYNCHRONOUS_CLIENT:
			break;
		case CONN_STATE_IDLE:
			errFormat = "CONNIO(%s:%d): submit will call failure callback because no asynchronous events exist!\r\n";
			err = EPERM;
			break;
		case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
		case CONN_STATE_ASYNCHRONOUS_ENGINE:
			errFormat = "CONNIO(%s:%d): submit would fail because the conn is currently owned by the asynchronous engine!\r\n";
			err = EBUSY;
			break;
		case CONN_STATE_SYNCHRONOUS:
			errFormat = "CONNIO(%s:%d): submit would fail because the conn is currently being used for synchronous events!\r\n";
			err = EBUSY;
			break;
		case CONN_STATE_SYNCHRONOUS_LISTENER:
		case CONN_STATE_ASYNCHRONOUS_LISTENER:
			errFormat = "CONNIO(%s:%d): submit would fail because the conn is currently being used for listening on a server socket!\r\n";
			err = EBUSY;
			break;
		case CONN_STATE_CACHED:
		case CONN_STATE_FREE:
			errFormat = "CONNIO(%s:%d): submit would fail because the conn is NOT valid!\r\n";
			err = EINVAL;
			break;
	}
	if( err ) {
		if( !queryOnly ) {
			DebugPrintf( errFormat, file, line );
			DebugAssert( 0 );
		}
	}
	return err;
}


INLINE static void
EventEngineQueueAdd( ConnectionPriv *conn, ConnEvent *connEvent )
{
	if( !( conn->async.transaction.problem ) ) {
		if( !( conn->connHandle->shuttingDown ) ) {
			EventQueueAppendEvent( conn, connEvent );
			return;
		}
		/* we are shutting down so only allow shutdown events */
		switch( connEvent->type) {
			case CONN_EVENT_DISCONNECT:
			case CONN_EVENT_SSL_DISCONNECT:
			case CONN_EVENT_FLUSH:
			{
				EventQueueAppendEvent( conn, connEvent );
				return;
			}
			case CONN_EVENT_ACCEPT:
			case CONN_EVENT_CONNECT:
			case CONN_EVENT_SSL_ACCEPT:
			case CONN_EVENT_TLS_ACCEPT:
			case CONN_EVENT_SSL_CONNECT:
			case CONN_EVENT_TLS_CONNECT:
			case CONN_EVENT_READ:
			case CONN_EVENT_READ_COUNT:
			case CONN_EVENT_READ_UNTIL_PATTERN:
			case CONN_EVENT_READ_TO_CB:
			case CONN_EVENT_READ_TO_CONN:
			case CONN_EVENT_WRITE_FROM_CB:
			case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
			{
				SetAsyncProblem( conn, CONN_EVENT_KILLED, ESHUTDOWN, connEvent->source.file, connEvent->source.line );
				break;
			}
		}
	}
	ConnEventFreeListAdd( connEvent );
	return;
}

static void
PreserveAsyncSummary( AsyncTransaction *transaction,  AsyncSummary *summary )
{

	summary->bytes.sent = transaction->bytes.sent;
	summary->bytes.received = transaction->bytes.received;
	summary->client = transaction->client;
	if( !transaction->problem ) {
		summary->problem = NULL;
		summary->status = CONN_EVENT_COMPLETE;
		return;
	}
	summary->problem = &( summary->problemReal );
	summary->status = ( ConnEventStatus )( transaction->problem->status );
	summary->problem->error = transaction->problem->error;
	summary->problem->file = transaction->problem->file;
	summary->problem->line = transaction->problem->line;
}

/* the caller must hold the event lock */
static void PrepareForCallback( ConnectionPriv *conn, AsyncSummary *summary, ConnEvent **freeList )
{
	DebugAssert( conn );
	DebugAssert( ( conn->activeEvent.connect == NULL) && ( conn->activeEvent.receive == NULL) && ( conn->activeEvent.send == NULL) );
	/*
	   we need to clear out the async transaction before the callback.
	   The callback may use it to submit a new transaction.  Before
	   clearing it, we need to preserve any information we still need.
	*/
	*freeList = ConnEventFreeListGrab( conn );

	PreserveAsyncSummary( &(conn->async.transaction), summary );
	memset( &( conn->async.transaction ), 0, sizeof( AsyncTransaction ) );

	ConnChangeState( conn, CONN_STATE_IDLE );
}

/* the caller must NOT hold the event lock */
static void CallbackAsyncConsumer( ConnectionPriv *conn, AsyncSummary *summary, ConnEvent *freeList )
{
	/* clean up any remaining conn events */
	ConnEventFreeListDestroy( conn, freeList );

	/* time for the actual call back */
	LogConnQueueCallback( conn );
	RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( conn->async.cb.qSuccess );  // the connio consumer did not provide a callback!!
	if( ( summary->status == CONN_EVENT_COMPLETE ) || ( summary->status == CONN_EVENT_PARTIAL ) )  {
		TLogWrite( "CONNIO WORKER: calling consumer's success callback" );
		DebugLoopCountReset( conn );
		conn->async.cb.qSuccess( ( Connection * )conn, summary );
	} else if ( !conn->async.cb.qFailure )  {
		TLogWrite( "CONNIO WORKER: calling consumer's callback" );
		DebugLoopCountCheck( conn, 10 );
		conn->async.cb.qSuccess( ( Connection * )conn, summary );
	} else {
		TLogWrite( "CONNIO WORKER: calling consumer's failure callback" );
		DebugLoopCountCheck( conn, 10 );
		conn->async.cb.qFailure( ( Connection * )conn, summary );
	}

	/* the callback could have freed the conn by now so we don't want to touch it. */
}

static int QueueCompleteCallback( void *arg )
{
	AsyncSummary summary;
	ConnEvent *freeList;

	ConnectionPriv *conn = ( ConnectionPriv * )( arg );

	TLogWrite( "CONNIO WORKER: working on SOCK:%p", conn->sp );
	LogConnQueueCompleted( conn );

	EventLockAcquire( conn );   /***** LOCK *****/

	DebugAssert( SocketIsIdle( conn->sp ) );

	PrepareForCallback( conn, &summary, &freeList );

	EventLockRelease( conn );   /***** UNLOCK *****/

	CallbackAsyncConsumer( conn, &summary, freeList );
	return 0;
}

static void QueueCompleteNotify( ConnectionPriv *conn )
{
	DebugAssert( SocketIsIdle( conn->sp ) );

	AThreadBegin( &( conn->async.cb.thread ) );
}

static XplBool
sslDisconnectEventNeeded( ConnectionPriv *conn, ConnEvent *ce )
{
	if( ce->type != CONN_EVENT_DISCONNECT ) {
		if( ce->type == CONN_EVENT_SSL_CONNECT ) {
			conn->async.transaction.sslEnabled = TRUE;
		} else if( ce->type == CONN_EVENT_SSL_DISCONNECT ) {
			/* the consumer of connio explicitly asked for an ssl disconnect */
			conn->async.transaction.sslEnabled = FALSE;
		}
		return( FALSE );
	}

	/* the current event is a normal 'disconnect' */
	if( !conn->async.transaction.sslEnabled ) {
		/* the connecton is not doing ssl */
		return( FALSE );
	}
	/* ssl is enabled - insert an ssl disconnect */
	conn->async.transaction.sslEnabled = FALSE;
	return( TRUE );
}

static void
AsyncQueueStatusInit( ConnectionPriv *conn )
{
	if( ( conn->sp->number != SOCKET_INVALID ) && ( conn->sp->ssl.conn ) ) {
		/* ssl is active on this connection */
		conn->async.transaction.sslEnabled = TRUE;
	}
}

XplBool
EventEnqueue( ConnectionPriv *conn, ConnEvent *connEvent, const char *file, const int line )
{
	XplBool eventsInProgress;
	ConnEvent *newEvent;

	EventLockAcquire( conn );   /***** LOCK *****/

	if( !queueingPermitted( conn, file, line ) ) {

		EventLockRelease( conn );   /***** UNLOCK *****/

		return( FALSE );
	}
	if( conn->state != CONN_STATE_ASYNCHRONOUS_CLIENT ) {
		AsyncQueueStatusInit( conn );
		ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_CLIENT );
	}

	if( connEvent ) {
		if( FlushAEventNeededEx( conn, connEvent, &newEvent, file, line ) ) {
			if( newEvent ) {
				newEvent->inserted = TRUE;
				EventEngineQueueAdd( conn, newEvent );
			} else {
				EventQueueAbort( conn, NULL, CONN_EVENT_FAILED, ENOMEM, file, line );
			}
		}

		if( sslDisconnectEventNeeded( conn, connEvent ) ) {
			newEvent = SSLDisconnectEventCreate( conn, -1, file, line );
			if( newEvent ) {
				newEvent->inserted = TRUE;
				EventEngineQueueAdd( conn, newEvent );
			} else {
				EventQueueAbort( conn, NULL, CONN_EVENT_FAILED, ENOMEM, file, line );
			}
		}

		EventEngineQueueAdd( conn, connEvent );
	}

	if( ( conn->connHandle->flags.dynamic & CONN_HANDLE_DYN_FLAG_WAIT_FOR_SUBMIT ) == 0 ) {
		eventsInProgress = ConnEventsProcess( conn, NULL );
	} else {
		eventsInProgress = CONN_EVENT_IN_PROGRESS;
	}

	EventLockRelease( conn );   /***** UNLOCK *****/

	if( !eventsInProgress ) {
		QueueCompleteNotify( conn );
	}

	errno = 0;
	return( TRUE );
}

void
EventEnqueueFailure( ConnectionPriv *conn, const char *file, const int line )
{
	EventLockAcquire( conn );   /***** LOCK *****/

    /*
	  EventEnqueueFailure() calls queueingPermitted() to see if EventEnqueue()
	  would have worked.  We do not want to abort the queue if the conn is
	  doing synchronous io or if it has already been submitted to the engine.
	*/
	if( queueingPermitted( conn, file, line ) ) {
		if( conn->state != CONN_STATE_ASYNCHRONOUS_CLIENT ) {
			AsyncQueueStatusInit( conn );
			ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_CLIENT );
		}
		EventQueueAbort( conn, NULL, CONN_EVENT_FAILED, errno, file, line );
		/*
		  We clear errno here. Async functions that have a void return value
		  should never return with errno set.  Consumer do not look for errno
		  when the function is called and errno could cause spurrious problems
		  for the consumer code.  They look for error in the callback
		  passed to submit.  If errno has a value when passed to EventQueueAbort
		  and no other error has occurred earlier on the same queue, it will
		  be  captured and returned to the consumer in the problem structure
		  provided by the submit callback.
		*/
		errno = 0;
	}

	EventLockRelease( conn );   /***** UNLOCK *****/
}


static void
DebugWarnAboutBufferedWriteData( ConnectionPriv *conn, const char *file, const int line )
{
#ifdef DEBUG
	int queuedFlushes;

	queuedFlushes = EventQueueCountFlushes( conn );
	if( queuedFlushes > 2 ) {
		DebugPrintf( "Connio had %d write buffers queued when ConnASubmit() was called at %s:%d.\n\tConsider using ConnAWriteFromCB().\n", queuedFlushes, file, line );
	}
#endif
}

XplBool EventsQueued( ConnectionPriv *conn, const char *file, const int line )
{
	if( !submitProblem( conn, TRUE, file, line ) ) {
		return TRUE;
	}
	return FALSE;
}

XplBool EventsSubmit( ConnectionPriv *conn, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *threadEngine, const char *file, const int line )
{
	XplBool eventsInProgress;
	AThreadEngine *useThreadEngine;
	int err;

	EventLockAcquire( conn );   /***** LOCK *****/

	if( ( err = submitProblem( conn, FALSE, file, line ) ) ) {
		if( err != EPERM ) {
			EventLockRelease( conn );   /***** UNLOCK *****/
			return FALSE;
		}
		/* Somebody did a submit with out any events.
		   Point out the problem and allow the failureCB to be called.
		 */
		SetAsyncProblem( conn, CONN_EVENT_FAILED, err, file, line );
	}
	/*
	  this is the point of no return.
	  no FALSE can be returned from here on

	  turn conn over to the async engine
	*/
	ConnChangeState( conn, CONN_STATE_ASYNCHRONOUS_ENGINE );

	DebugWarnAboutBufferedWriteData( conn, file, line );

	useThreadEngine = threadEngine ? threadEngine : conn->connHandle->threadEngine.defaultConsumer;
	AThreadInit( &( conn->async.cb.thread ), useThreadEngine, QueueCompleteCallback, conn, FALSE );
	conn->async.cb.qSuccess = successCB;
	conn->async.cb.qFailure = failureCB;
	conn->async.transaction.client = client;

	eventsInProgress = ConnEventsProcess( conn, NULL );

	EventLockRelease( conn );   /***** UNLOCK *****/

	if( !eventsInProgress ) {
		QueueCompleteNotify( conn );
	}
	return TRUE;
}

void AcceptCallback( SOCK *listenerCS, SOCK **sp, void *CE )
{
	ConnEvent *connEvent = (ConnEvent *)CE;
	ConnectionPriv *listener = connEvent->conn;
	ConnectionPriv *conn;

	/* create a conn for the newly accepted socket */
	conn = _ConnAllocEx( listener->connHandle, *sp, connEvent->source.file, connEvent->source.line );
	if( conn ) {
		TLogWrite( "CONNIO WORKER: working on SOCK:%p", conn->sp );
		RememberAPICalls( conn, "ConnAAccept", __FILE__, __LINE__ );
		conn->sp->listenerID = listener->sp->listenerID;
		XplSocketSetMode( conn->sp->number, XPL_SOCKET_MODE_NON_BLOCKING );
		XplSocketSetMode( conn->sp->number, XPL_SOCKET_MODE_DISABLE_NAGLE );
		LogAcceptCallback( conn );
		TLogWrite( "CONNIO WORKER: calling consumer's accept callback" );
		listener->async.cb.accept( ( Connection * )conn, ( Connection * )listener, listener->async.cb.acceptContext );

		return;
	}

	SocketRelease( sp );  // no conn got created. Close the SOCK ptr
	return;
}

void AsyncListenerCallback( SOCK *socket, void *CE, SocketEventType type, int error )
{
	ConnEvent *ce;
	ConnectionPriv *conn;

	ce = (ConnEvent *)(CE);
	ce->error = error;
	conn = ce->conn;

	LogSocketEventFinished( conn, ce, type );
	DebugAssert( conn->state == CONN_STATE_ASYNCHRONOUS_LISTENER );
	EventLockAcquire( conn );   /***** LOCK *****/

	ConnEventConnectInProgressClear( conn, ce );
	ConnChangeState( conn, CONN_STATE_IDLE );

	EventLockRelease( conn );   /***** UNLOCK *****/

	ConnEventFree( conn, ce );

	/* close the listener socket */
	SocketClose( conn->sp );

	LogListenerCallback( conn );
	TLogWrite( "CONNIO WORKER: calling consumer's listen callback" );
	conn->async.cb.listen( ( Connection * )conn );
}

void AsyncEventCallback( SOCK *socket, void *CE, SocketEventType type, int error )
{
	ConnEvent *ce;
	ConnectionPriv *conn;
	XplBool stillWorking;

	ce = (ConnEvent *)(CE);
	ce->error = error;
	conn = ce->conn;

	LogSocketEventFinished( conn, ce, type );
	switch( type )
	{
		case SOCK_EVENT_TYPE_READABLE:
			if( ce->error && ( ce->type == CONN_EVENT_DISCONNECT ) && ( ce->error == ECONNRESET ) ) {
				/* this is a disconnect; we got what we asked for */
				ce->error = 0;
			}
			/* fall through */
		case SOCK_EVENT_TYPE_DISCONNECT:
		case SOCK_EVENT_TYPE_WRITABLE:
		case SOCK_EVENT_TYPE_CONNECT:
		{

			EventLockAcquire( conn );   /***** LOCK *****/

			stillWorking = ConnEventsProcess( conn, ce );

			EventLockRelease( conn );   /***** UNLOCK *****/

			if( !stillWorking ) {
				QueueCompleteNotify( conn );
			}

			return;
		}

		default:
			DebugPrintf("CONNIO ERROR!!!!!! A deprecated event came out of the socket engine!");
			/* fall through */
		case SOCK_EVENT_TYPE_LISTEN:
			RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( 0 );
	}
}
