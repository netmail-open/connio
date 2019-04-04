#define LOG_API_CALL						1 // FIXME This was temporarily added to DEBUGFORDAN 
//#define LOG_SOCKET_BIND					1
//#define LOG_CONN_DISPOSITION				1
#define LOG_STATE_CHANGE					1 // FIXME This was temporarily added to DEBUGFORDAN 

//#define LOG_LOCK							1
//#define LOG_UNLOCK						1
//#define LOG_CONN_EVENT_ALLOC				1
//#define LOG_CONN_EVENT_FREE				1
//#define LOG_CONN_EVENT_ENQUEUE			1
//#define LOG_CONN_EVENT_DEQUEUE			1
//#define LOG_CONN_EVENT_NEXT				1
//#define LOG_CONN_EVENT_CONNECT_SET		1
//#define LOG_CONN_EVENT_CONNECT_CLEAR		1
//#define LOG_CONN_EVENT_RECEIVE_SET		1
//#define LOG_CONN_EVENT_RECEIVE_CLEAR		1
//#define LOG_CONN_EVENT_SEND_SET			1
//#define LOG_CONN_EVENT_SEND_CLEAR			1
//#define LOG_CONN_EVENT_PROCESS			1
//#define LOG_CONN_EVENT_DONE				1
//#define LOG_CONN_EVENT_KILLED				1
//#define LOG_CONN_EVENT_CANCELED			1
//#define LOG_SOCK_EVENT_ENQUEUE			1
//#define LOG_SOCK_EVENT_ENQUEUE_FAILURE	1
//#define LOG_SOCK_EVENT_NOTIFY				1
//#define LOG_SOCK_EVENT_PICKUP				1
//#define LOG_SOCK_ACCEPT_NOTIFY			1
//#define LOG_CLIENT_COMPLETED_PICKUP		1
//#define LOG_ASYNC_QUEUE_DONE_WAIT			1
//#define LOG_ASYNC_QUEUE_CALLBACK			1
//#define LOG_ASYNC_LISTEN_CALLBACK			1
//#define LOG_ASYNC_ACCEPT_CALLBACK			1
//#define LOG_DATA_DROPPED					1


void logMessage( ConnectionPriv *conn, char *msg, const char *file, const int line );
#ifdef LOG_API_CALL
#define LogAPICall( conn, api, file, line )					logMessage( ( conn ), ( api ), ( file ), ( line ) )
#else
#define LogAPICall( conn, api, file, line )
#endif

void logConnBindSocket( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_SOCKET_BIND
#define LogConnBindSocket( conn )							logConnBindSocket( ( conn ), __FILE__, __LINE__ )
#else
#define LogConnBindSocket( conn )
#endif


#ifdef LOG_CONN_DISPOSITION
#define LogConnDisposition( conn, owner, file, line )		logMessage( ( conn ), ( owner ), ( file ), ( line ) )
#else
#define LogConnDisposition( conn, owner, file, line )
#endif

void logStateChange( ConnectionPriv *conn, ConnState newState, const char *file, const int line );
#ifdef LOG_STATE_CHANGE
#define LogStateChange( conn, newState, file, line )		logStateChange( ( conn ), ( newState ), ( file ), ( line ) )
#else
#define LogStateChange( conn, newState, file, line )
#endif

void logQueueLock( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_LOCK
#define LogQueueLock( conn, file, line )					logQueueLock( ( conn ), ( file ), ( line ) )
#else
#define LogQueueLock( conn, file, line )
#endif

void logQueueUnlock( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_UNLOCK
#define LogQueueUnlock( conn, file, line )					logQueueUnlock( ( conn ), ( file ), ( line ) )
#else
#define LogQueueUnlock( conn, file, line )
#endif

void logConnEventAlloc( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_ALLOC
#define LogConnEventAlloc( conn, ce )						logConnEventAlloc( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventAlloc( conn, ce )
#endif

void logConnEventFree( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_ALLOC
#define LogConnEventFree( conn, ce )						logConnEventFree( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventFree( conn, ce )
#endif

void logConnEventEnqueue( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_ENQUEUE
#define LogConnEventEnqueue( conn, ce )						logConnEventEnqueue( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventEnqueue( conn, ce )
#endif

void logConnEventDequeue( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_DEQUEUE
#define LogConnEventDequeue( conn, ce )						logConnEventDequeue( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventDequeue( conn, ce )
#endif

void logConnEventNext( ConnectionPriv *conn, char *message, const char *file, const int line );
#ifdef LOG_CONN_EVENT_NEXT
#define LogConnEventNext( conn, ce )						logConnEventNext( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventNext( conn, ce )
#endif


void logConnEventConnectSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_CONNECT_SET
#define LogConnEventConnectSet( conn, ce, file, line )		logConnEventConnectSet( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventConnectSet( conn, ce, file, line )
#endif

void logConnEventConnectClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_CONNECT_CLEAR
#define LogConnEventConnectClear( conn, ce, file, line )	logConnEventConnectClear( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventConnectClear( conn, ce, file, line )
#endif

void logConnEventReceiveSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_RECEIVE_SET
#define LogConnEventReceiveSet( conn, ce, file, line )		logConnEventReceiveSet( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventReceiveSet( conn, ce, file, line )
#endif

void logConnEventReceiveClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_RECEIVE_CLEAR
#define LogConnEventReceiveClear( conn, ce, file, line )	logConnEventReceiveClear( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventReceiveClear( conn, ce, file, line )
#endif

void logConnEventSendSet( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_SEND_SET
#define LogConnEventSendSet( conn, ce, file, line )			logConnEventSendSet( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventSendSet( conn, ce, file, line )
#endif

void logConnEventSendClear( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_SEND_CLEAR
#define LogConnEventSendClear( conn, ce, file, line )		logConnEventSendClear( ( conn ), ( ce ), ( file ), ( line ) )
#else
#define LogConnEventSendClear( conn, ce, file, line )
#endif

void logConnEventProcess( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_PROCESS
#define LogConnEventProcess( conn, ce )						logConnEventProcess( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventProcess( conn, ce )
#endif

void logConnEventDone( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_DONE
#define LogConnEventDone( conn, ce )						logConnEventDone( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventDone( conn, ce )
#endif

void logConnEventKilled( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_KILLED
#define LogConnEventKilled( conn, ce )						logConnEventKilled( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventKilled( conn, ce )
#endif

void logConnEventCanceled( ConnectionPriv *conn, ConnEvent *ce, const char *file, const int line );
#ifdef LOG_CONN_EVENT_CANCELED
#define LogConnEventCanceled( conn, ce )					logConnEventCanceled( ( conn ), ( ce ), __FILE__, __LINE__ )
#else
#define LogConnEventCanceled( conn, ce )
#endif

void logSocketEventStarted( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line );
#ifdef LOG_SOCK_EVENT_ENQUEUE
#define LogSocketEventStarted( conn, ce, type )				logSocketEventStarted( ( conn ), ( ce ), ( type ), __FILE__, __LINE__ )
#else
#define LogSocketEventStarted( conn, ce, type )
#endif

void logSocketEventFailedToStart( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line );
#ifdef LOG_SOCK_EVENT_ENQUEUE_FAILURE
#define LogSocketEventFailedToStart( conn, ce, type )		logSocketEventFailedToStart( ( conn ), ( ce ), ( type ), __FILE__, __LINE__ )
#else
#define LogSocketEventFailedToStart( conn, ce, type )
#endif

void logSocketEventFinished( ConnectionPriv *conn, ConnEvent *ce, SocketEventType type, const char *file, const int line );
#ifdef LOG_SOCK_EVENT_PICKUP
#define LogSocketEventFinished( conn, ce, type )			logSocketEventFinished( ( conn ), ( ce ), ( type ), __FILE__, __LINE__ )
#else
#define LogSocketEventFinished( conn, ce, type )
#endif

void logConnQueueCompleted( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_CLIENT_COMPLETED_PICKUP
#define LogConnQueueCompleted( conn )						logConnQueueCompleted( ( conn ), __FILE__, __LINE__ )
#else
#define LogConnQueueCompleted( conn )
#endif

void logConnQueueWaiting( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_ASYNC_QUEUE_DONE_WAIT
#define LogConnQueueWaiting( conn )							logConnQueueWaiting( ( conn ), __FILE__, __LINE__ )
#else
#define LogConnQueueWaiting( conn )
#endif

void logConnQueueCallback( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_ASYNC_QUEUE_CALLBACK
#define LogConnQueueCallback( conn )						logConnQueueCallback( ( conn ), __FILE__, __LINE__ )
#else
#define LogConnQueueCallback( conn )
#endif

void logListenerCallback( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_ASYNC_LISTEN_CALLBACK
#define LogListenerCallback( conn )							logListenerCallback( ( conn ), __FILE__, __LINE__ )
#else
#define LogListenerCallback( conn )
#endif

void logAcceptCallback( ConnectionPriv *conn, const char *file, const int line );
#ifdef LOG_ASYNC_ACCEPT_CALLBACK
#define LogAcceptCallback( conn )							logAcceptCallback( ( conn ), __FILE__, __LINE__ )
#else
#define LogAcceptCallback( conn )
#endif

void logDataDropped( ConnectionPriv *conn, int count, char *why, const char *srcFile, const int srcLine );
#ifdef LOG_DATA_DROPPED
#define LogDataDropped( conn, count, why )					logDataDropped( ( conn ), ( count ), ( why ), __FILE__, __LINE__ )
#else
#define LogDataDropped( conn, count, why )
#endif

void logConnEventsProcess( ConnectionPriv *conn, ConnEventStatus status, char *message, const char *file, const int line );
#ifdef LOG_CONN_EVENTS_PROCESS
#define LogConnEventsProcess( conn, status, message )		logConnEventsProcess( ( conn ), ( status ), ( message ), __FILE__, __LINE__ )
#else
#define LogConnEventsProcess( conn, status, message )
#endif
