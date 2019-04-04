#include "conniop.h"

__inline static void
RememberAPICalls( ConnectionPriv *conn, char *api, const char *file, const int line )
{
	LogAPICall( conn, api, file, line );
	conn->pub.lastUsed.file = file;
	conn->pub.lastUsed.line = line;
}

__inline static void
connChangeState( ConnectionPriv *conn, int newState, const char *file, const int line )
{
	LogStateChange( conn, newState, file, line );
	conn->state = newState;
}

__inline static void
eventLockAcquire( ConnectionPriv *conn, const char *file, const int line )
{
	XplMutexLock( conn->mutex );
	LogQueueLock( conn, file, line );
}

__inline static void
eventLockRelease( ConnectionPriv *conn, const char *file, const int line )
{
	XplMutexUnlock( conn->mutex );
	LogQueueUnlock( conn, file, line );
}

__inline static void
connEventConnectInProgressSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventConnectSet( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_LISTENER ) );

	DebugAssert( conn->activeEvent.connect == NULL );				// make sure the slot is not busy
	conn->activeEvent.connect = ce;
}

__inline static void
connEventReceiveInProgressSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventReceiveSet( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) );

	DebugAssert( conn->activeEvent.receive == NULL );				// make sure the slot is not busy
	conn->activeEvent.receive = ce;
}

__inline static void
connEventSendInProgressSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventSendSet( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) );

	DebugAssert( conn->activeEvent.send == NULL );				// make sure the slot is not busy
	conn->activeEvent.send = ce;
}

/*
  since these socket events are not allocated, but part of
  the conn, the connEvent is set to NULL to show that they
  are no longer associated with an event.
*/

__inline static void
connEventConnectInProgressClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventConnectClear( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) ||
				 ( conn->state == CONN_STATE_ASYNCHRONOUS_LISTENER ) );

	DebugAssert( conn->activeEvent.connect == ce );				// make sure the slot has this conn event
	conn->activeEvent.connect = NULL;
}

__inline static void
connEventReceiveInProgressClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventReceiveClear( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) );

	DebugAssert( conn->activeEvent.receive == ce );				// make sure the slot has this conn event
	conn->activeEvent.receive = NULL;
}

__inline static void
connEventSendInProgressClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	LogConnEventSendClear( conn, ce, file, line );

	DebugAssert( ( conn->state == CONN_STATE_SYNCHRONOUS ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_CLIENT ) || ( conn->state == CONN_STATE_ASYNCHRONOUS_ENGINE ) );

	DebugAssert( conn->activeEvent.send == ce );					// make sure the slot has this conn event
	conn->activeEvent.send = NULL;
}

__inline static void
connEventInProgressSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	switch( ce->eventClass ) {
		case CONN_EVENT_CLASS_CONNECT: {
			connEventConnectInProgressSet( conn, ce, file, line );
			break;
		}
		case CONN_EVENT_CLASS_RECEIVE: {
			connEventReceiveInProgressSet( conn, ce, file, line );
			break;
		}
		case CONN_EVENT_CLASS_SEND: {
			connEventSendInProgressSet( conn, ce, file, line );
			break;
		}
	}
}

__inline static void
connEventInProgressClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line )
{
	switch( ce->eventClass ) {
		case CONN_EVENT_CLASS_CONNECT: {
			connEventConnectInProgressClear( conn, ce, file, line );
			break;
		}
		case CONN_EVENT_CLASS_RECEIVE: {
			connEventReceiveInProgressClear( conn, ce, file, line );
			break;
		}
		case CONN_EVENT_CLASS_SEND: {
			connEventSendInProgressClear( conn, ce, file, line );
			break;
		}
	}
}

#define ConnChangeState( conn, newState )			connChangeState( ( conn ), ( newState ), __FILE__, __LINE__ )
#define EventLockAcquire( conn )	eventLockAcquire( conn, __FILE__, __LINE__ )
#define EventLockRelease( conn )	eventLockRelease( conn, __FILE__, __LINE__ )
#define ConnEventConnectInProgressSet( conn, ce )	connEventConnectInProgressSet( conn, ce, __FILE__, __LINE__ )
#define ConnEventConnectInProgressClear( conn, ce )	connEventConnectInProgressClear( conn, ce, __FILE__, __LINE__ )
#define ConnEventReceiveInProgressSet( conn, ce )	connEventReceiveInProgressSet( conn, ce, __FILE__, __LINE__ )
#define ConnEventReceiveInProgressClear( conn, ce )	connEventReceiveInProgressClear( conn, ce, __FILE__, __LINE__ )
#define ConnEventSendInProgressSet( conn, ce )		connEventSendInProgressSet( conn, ce, __FILE__, __LINE__ )
#define ConnEventSendInProgressClear( conn, ce )	connEventSendInProgressClear( conn, ce, __FILE__, __LINE__ )
#define ConnEventInProgressSet( conn, ce )			connEventInProgressSet( conn, ce, __FILE__, __LINE__ )
#define ConnEventInProgressClear( conn, ce )		connEventInProgressClear( conn, ce, __FILE__, __LINE__ )
