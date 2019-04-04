#include "conniop.h"

void
logMessage( ConnectionPriv *conn, char *msg, const char *file, const int line )
{
	int errnobefore = errno;
	SocketLogMessage( conn->sp, msg, file, line );
	errno = errnobefore;
}

void
logConnBindSocket( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "socket %p bound to conn %p", conn->sp, conn );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}

void
logStateChange( ConnectionPriv *conn, ConnState newState, const char *file, const int line )
{
	char *msg;
	int errnobefore = errno;

	switch( newState )
		{
		case CONN_STATE_FREE:
			{
				msg = "is being destroyed";
				break;
			}
		case CONN_STATE_CACHED:
			{
				msg = "is cached";
				break;
			}
		case CONN_STATE_IDLE:
			{
				msg = "is idle";
				break;
			}
		case CONN_STATE_SYNCHRONOUS:
			{
				msg = "is doing synchronous io";
				break;
			}

		case CONN_STATE_ASYNCHRONOUS_CLIENT:
			{
				msg = "is queueing async events";
				break;
			}

		case CONN_STATE_ASYNCHRONOUS_ENGINE:
			{
				msg = "is done queueing async events";
				break;
			}
		case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
			{
				msg = "is done processing async events";
				break;
			}
		case CONN_STATE_SYNCHRONOUS_LISTENER:
			{
				msg = "is a synchronous listener";
				break;
			}
		case CONN_STATE_ASYNCHRONOUS_LISTENER:
			{
				msg = "is an asynchronous listener";
				break;
			}
		default:
			{
				msg = "is something random";
				break;
			}

		}
	SocketLogMessage( conn->sp, msg, file, line );
	errno = errnobefore;
}

void logQueueLock( ConnectionPriv *conn, const char *file, const int line )
{
	int errnobefore = errno;
	SocketLogMessage( conn->sp, "mutex acquired", file, line );
	errno = errnobefore;
}

void logQueueUnlock( ConnectionPriv *conn, const char *file, const int line )
{
	int errnobefore = errno;
	SocketLogMessage( conn->sp, "mutex released", file, line );
	errno = errnobefore;
}


void logConnEventAlloc( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p: - allocated", (void *)ce );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventFree( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d freed", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventEnqueue( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d queued", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventDequeue( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d dequeued", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void
logConnEventNext( ConnectionPriv *conn, char *message, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ConnEventNext() found CONNECT: %s RECEIVE: %s SEND: %s QUEUED: %lu PLAN: %s\n",
			  conn->activeEvent.connect ? "in progress": "idle",
			  conn->activeEvent.receive ? "in progress": "idle",
			  conn->activeEvent.send ? "in progress": "idle",
			  conn->async.transaction.queue.count,
			  message
		);
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventConnectSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.connect ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress COLLISION when trying to set!!!!! (class:connect)", (void *)ce );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress set (class:connect)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventConnectClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.connect == ce ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress clear (class:connect)", (void *)ce );
	} else if ( conn->activeEvent.connect ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p	the clearing event does not match the event in progress (%p)!!!(class:connect)",
				   (void *)ce,
				   (void *)conn->activeEvent.connect );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress NOT FOUND when trying to clear!!!!(class:connect)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventReceiveSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.receive ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress COLLISION when trying to set!!!!! (class:receive)", (void *)ce );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress set (class:receive)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventReceiveClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.receive == ce ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress clear (class:receive)", (void *)ce );
	} else if ( conn->activeEvent.receive ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p	the clearing event does not match the event in progress (%p)!!!(class:receive)",
				   (void *)ce,
				   (void *)conn->activeEvent.receive );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress NOT FOUND when trying to clear!!!!(class:receive)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventSendSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.send ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress COLLISION when trying to set!!!!! (class:send)", (void *)ce );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress set (class:send)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventSendClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( conn->activeEvent.send == ce ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress clear (class:send)", (void *)ce );
	} else if ( conn->activeEvent.send ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p	the clearing event does not match the event in progress (%p)!!!(class:send)",
				   (void *)ce,
				   (void *)conn->activeEvent.send );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p in progress NOT FOUND when trying to clear!!!!(class:send)", (void *)ce );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventProcess( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	DebugAssert( ( ce->conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) || ( ce->conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) );
	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d processing", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}

void logConnEventDone( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	if( ce->status == CONN_EVENT_COMPLETE ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d completed successfully", (void *)ce, ce->type );
	} else if( ce->status == CONN_EVENT_IN_PROGRESS ) {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d exited until async events are finished", (void *)ce, ce->type );
	} else {
		strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d ended with error %d", (void *)ce, ce->type, ce->error );
	}
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnEventKilled( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d killed", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}

void logConnEventCanceled( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d canceled", (void *)ce, ce->type );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


static char *
socketEventString( SocketEventType type )
{
	char *eventType;

	switch( type ) {
	case SOCK_EVENT_TYPE_READABLE:
		eventType = "read";
		break;
	case SOCK_EVENT_TYPE_WRITABLE:
		eventType = "write";
		break;
	case SOCK_EVENT_TYPE_LISTEN:
		eventType = "listen";
		break;
	case SOCK_EVENT_TYPE_CONNECT:
		eventType = "connect";
		break;
	case SOCK_EVENT_TYPE_DISCONNECT:
		eventType = "disconnect";
		break;
	default:
	case SOCK_EVENT_TYPE_NONE:
		DebugAssert( 0 );
		eventType = "NOT DEFINED";
		break;
	}
	return eventType;
}


void logSocketEventStarted( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d started %s event", (void *)ce, ce->type, socketEventString( type ) );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logSocketEventFailedToStart( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d failed to start %s event", (void *)ce, ce->type, socketEventString( type ) );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logSocketEventFinished( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "ce %p:%02d a %s event finished", (void *)ce, ce->type, socketEventString( type ) );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnQueueCompleted( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "picking up completed queue" );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnQueueWaiting( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "all queued events are finished. waiting for client to queue more or submit" );
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logConnQueueCallback( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "async queue callback");
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logListenerCallback( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "async listen callback");
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void logAcceptCallback( ConnectionPriv *conn, const char *file, const int line )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "async accept callback");
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}


void
logDataDropped( ConnectionPriv *conn, int count, char *why, const char *srcFile, const int srcLine )
{
	char buffer[ 256 ];
	int errnobefore = errno;

	strprintf( buffer, sizeof( buffer ), NULL, "%d bytes were %s", count, why );
	SocketLogMessage( conn->sp, buffer, srcFile, srcLine );
	errno = errnobefore;
}


void
logConnEventsProcess( ConnectionPriv *conn, ConnEventStatus status, char *message, const char *file, const int line )
{
	char buffer[ 256 ];
	char *stateString;
	char *statusString;
	int errnobefore = errno;

	switch( status )
	{
		case CONN_EVENT_IN_PROGRESS:
		{
			statusString = "in progress";
			break;
		}
		case CONN_EVENT_COMPLETE:
		{
			statusString = "complete";
			break;
		}
		case CONN_EVENT_FAILED:
		{
			statusString = "failed";
			break;
		}
		case CONN_EVENT_TIMED_OUT:
		{
			statusString = "timed out";
			break;
		}
		case CONN_EVENT_QUEUED:
		{
			statusString = "queued";
			break;
		}
		case CONN_EVENT_KILLED:
		{
			statusString = "killed";
			break;
		}
		case CONN_EVENT_PARTIAL:
		{
			statusString = "partial";
			break;
		}
		default:
		{
			statusString = "random";
			break;
		}
	}
	switch( conn->state )
	{
		case CONN_STATE_ASYNCHRONOUS_CLIENT:
		{
			stateString = "owned by client";
			break;
		}
		case CONN_STATE_ASYNCHRONOUS_ENGINE:
		{
			stateString = "owned by engine";
			break;
		}
		case CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE:
		{
			stateString = "owned by engine and complete";
			break;
		}
		case CONN_STATE_ASYNCHRONOUS_LISTENER:
		case CONN_STATE_SYNCHRONOUS_LISTENER:
		case CONN_STATE_CACHED:
		case CONN_STATE_FREE:
		case CONN_STATE_IDLE:
		case CONN_STATE_SYNCHRONOUS:
		{
			stateString = "Illegal engine state";
			break;
		}
		default:
		{
			stateString = "random";
			break;
		}
	}
	strprintf( buffer, sizeof( buffer ), NULL, "ConnEventsProcess() returning STATUS: %s STATE: %s CONNECT: %s RECEIVE: %s SEND: %s QUEUED: %lu THREADS: %d REASON: %s\n",
			  statusString,
			  stateString,
			  conn->activeEvent.connect ? "in progress": "idle",
			  conn->activeEvent.receive ? "in progress": "idle",
			  conn->activeEvent.send ? "in progress": "idle",
			  conn->async.transaction.queue.count,
			  conn->async.transaction.threads,
			  message
		);
	SocketLogMessage( conn->sp, buffer, file, line );
	errno = errnobefore;
}
