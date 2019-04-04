#if !defined(CONNIOP_H)
#define CONNIOP_H

/*
	Private ConnIO structures

	All structures needed by the private portions of public connio structures
	must be defined before including connio.h
*/
#include <stdarg.h>

#include <xplip.h>
#include <xpltimer.h>
#include <xplthread.h>
#include <xplmem.h>
#include <iobuf.h>
#include <threadengine.h>
#include <listener_event.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <wjelement.h>

#if defined(LINUX)
#include <sys/epoll.h>
#endif

#ifdef LINUX
#include <sys/syscall.h>
#endif



#ifdef __cplusplus
extern "C" {
#endif

/*
	Request a size a bit smaller than the 16k pool to avoid going into the next
	pool.  The entire buffer avaliable will be used.
*/



#define ENGINE_STACK_SIZE			( 1024 * 32 )
#define STACK_BUFFER_SIZE			( 1023 )
#define ENGINE_INITIALIZED			0
#define ENGINE_RUNNING				1
#define ENGINE_SHUTDOWN				2

#define FSE_IN_ENGINE				0x00000002  // This event is currently in the socket layer
#define FSE_TIMEOUT_PENDING			0x00000004  // A timer has been set on this event that has not been cleared
#define FSE_LISTENER_KILLED			0x00000008  // LISTENERS ONLY - This listener was killed and should be sent to the consumer next time it comes out of the engine
#define FSE_LISTENER_PAUSED			0x00000010  // LISTENERS ONLY - This listener is alive but not accepting new connections
#define FSE_LISTENER_SUSPENDED		0x00000020  // LISTENERS ONLY - This listener is alive but not accepting new connections by consumer request
#define FSE_CANCELED				0x00000040  // Socket event has been killed ( Windows only )
#define FSE_TIMEDOUT				0x00000080  // Socket event has timed out ( Windows only )

#define POLL_READ					0
#define POLL_WRITE					1
#define POLL_CONTROL				2

#define SOCKET_TIMEOUT_LISTS		0x00000040	// 64 lists (about 34 minutes max)

#define TIMEOUT_INDEX( t )	( ( ( t ) / SOCKET_TIMEOUT_GRANULARITY ) % SOCKET_TIMEOUT_LISTS )

#define SPECIAL_PATTERN_CRLF		"SP_CRLF"

#define ACCEPT_EVENT_FAST_CACHE_SIZE 5

#if defined(LINUX)

#define CONNIO_SO_REUSEADDR                 SO_REUSEADDR

#elif defined(WIN32)

#define CONNIO_SO_REUSEADDR                 0

#endif

#define SOCK_LIB_FLAG_ASYNC				0x00000001  // Enable the socket event engine
#define SOCK_LIB_FLAG_SSL				0x00000002  // Enable ssl

typedef struct {
	/* content */
	uint32 flags;
	XplTimer startTime;
	unsigned long sequence;
	/* identity */
	unsigned int id;
	void *subject;
	char *consumer;
	/* output */
	int useCount;
	IOBuffer *iob;
	FILE *file;
} TraceConfig;

typedef	struct {
	const char				*file;
	int						line;
} CodeSource;

typedef enum {
	/* State Events */
	SOCK_EVENT_TYPE_NONE = 0,
	SOCK_EVENT_TYPE_LISTEN,
    SOCK_EVENT_TYPE_READABLE,
    SOCK_EVENT_TYPE_WRITABLE,
    SOCK_EVENT_TYPE_CONNECT,
    SOCK_EVENT_TYPE_DISCONNECT,
} SocketEventType;

typedef void (* workerFunction)( void *arg );


#if defined(WIN32)
  typedef struct _seTimer {
  struct _seTimer *next;
  struct SocketEvent *se;
  XplLock lock;  // protects link to socket event
  HANDLE handle;

  XplBool started;
	unsigned int len; // in milliseconds
} seTimer;

typedef enum {
	IOCP_EVENT_STATE_IDLE = 0,
	IOCP_EVENT_STATE_WAIT_ACTIVE,
} IOEventStates;

#endif

struct SOCK;


typedef void (* SocketEventCB)( struct SOCK *socket, void *callbackArg, SocketEventType type, int error );
typedef void (* AcceptEventCB)( struct SOCK *socket, struct SOCK **sp, void *callbackArg );

typedef struct _ListMember {
	struct _ListMember *next;
} ListMember;

typedef struct MemberList
{
	XplLock		lock;
	/*
	   this lock protects this list, and
	   it is also abused to protect listener 'inAccept' count
	 */

	ListMember	*head;
	ListMember	**tail;
} MemberList;

typedef struct SocketEvent {
	ListMember 					async;
	struct SocketEvent			*next;
	CodeSource					submitter;
	struct SOCK					*cs;
	SocketEventType				type;			// event type
	int							error;			// event completion state
	unsigned long				flags;			// flags for engine use
	struct {
		AcceptEventCB			acceptFunc;		// callback function when new connection is accepted
		SocketEventCB			eventFunc;		// callback function for the current event;
		void *					arg;			// argument to return to the consumer when event is complete
		AThread					thread;			// the thread meta data that is needed to create the callback thread
	} cb;

#if defined(LINUX)
	struct MemberList		*timeoutList;
	struct SocketEvent			*timeoutNext;
	struct SocketEvent			*timeoutPrev;
  struct epoll_event	        ev;
#elif defined(WIN32)
  IOEventStates                 state;

  WSAOVERLAPPED		            ovl;
  char                          dbuf[ 4 ];
  WSABUF				        *wsabuf;
  union {
	char                        *acceptBuf;     // ACCEPT events only - Windows AcceptEx requires a buffer for accept
	seTimer                     *timer;         // READABLE/WRITABLE events only - used to time out events
  };
  XplBool                       timedout;       // windows timeout thread uses this to tell the iocp thread
	                                            // that the event timed out (did not spontaneosly come out of the engine)
  XplSocket					    acceptSocket;	// needed on windows listeners
#endif
} SocketEvent;

typedef struct AcceptEvent {
	ListMember 					async;
	XplSocket					socket;		// socket
	struct SOCK					*listenerSp;// the SOCK of the listener
	AcceptEventCB               cb;			// cb provided by creator of listener
	void				  		*cbArg;		// arg provided by creator of listener
	AThread						thread;		// information needed to create the thread needed for the accept callback
} AcceptEvent;


typedef struct
{
#ifdef WIN32
	MemberList	selectList;
#endif
#if defined(LINUX) || defined(SOLARIS) || defined(S390RH) || defined(MACOSX)
	int				efd;
#endif
	int				type;
	time_t			now;
	int				timeoutIndex;
	MemberList		timeoutList[SOCKET_TIMEOUT_LISTS];
}SPollInfo;

typedef struct {
	MemberList safeList; // this cache requires a lock to modify
	AcceptEvent *fastList[ ACCEPT_EVENT_FAST_CACHE_SIZE + 1 ];
	int next;
} AcceptEventCache;

typedef struct SocketEventEngine
{
	AcceptEventCache	aeCache;
	unsigned long		state;		// engine state
	XplThreadGroup		threadGroup;// event engine thread group
	SPollInfo			pollIn;		// for poll threads
	SPollInfo			pollOut;	// for poll threads
	FILE				*log;		// log file
	/* add platform specific stuff here */
#ifdef WIN32
	XplBool (*AcceptEx)( SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED );
	XplBool (*ConnectEx)( SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED );
	XplBool (*DisconnectEx)( SOCKET, LPOVERLAPPED, DWORD, DWORD );
	XplBool (*TransmitFile)( SOCKET, HANDLE, DWORD, DWORD, LPOVERLAPPED, LPTRANSMIT_FILE_BUFFERS, DWORD );
	void (*GetAcceptExSockaddrs)( PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT );
    struct {
	  seTimer *head;
	  XplLock	lock;
	} timerQ;
    HANDLE iocp;
	MemPool *timerpool;
	int		iocpThreads;
#endif
}SocketEventEngine;

typedef struct {
	XplLock lock;
	unsigned long nextListener;
	unsigned long nextTrace;
	int useCount;
	struct {
		XplBool enabled;
	} ssl;
	SocketEventEngine *eventEngine;
} SockLibGlobals;

extern SockLibGlobals SockLib;

typedef struct {
	struct {
		PoolEntryCB alloc;
		PoolEntryCB free;

		void 		*data;
	} cb;

	void			*cache;
} ConnCache;

typedef struct {
	struct ConnectionPriv	*head;
	struct ConnectionPriv	*tail;
	int						count;
	int						peak;
	int						limit;
	XplMutex 				mutex;
} ConnList;

typedef struct {
	unsigned long throttlingThreshold;
	ListenerEventCallback eventCallback;
	AThreadEngine *acceptThreadEngine;
	int	inAccept;
	/* inAccept
	   used by listeners to track the number of children still in accept
	*/
} AsyncListener;

typedef struct SOCK {
	XplSocket		 			number;
	XplLock						slock;
	/* this lock is shared by all socket embedded socket event objects.
	   It isco used to protect:
	   - socket event flags;
	   - submitting socket events to the engine
	   - linking socket and conn events together.
	   - closing the socket
	*/
	struct {
		SSL						*new;
		SSL 					*conn;
	} ssl;
	AsyncListener				*listener;
#ifdef WIN32
    XplBool                     iocpLinked;
    struct {
	  LPFN_ACCEPTEX             func;
	  XplBool                   defined;
	} accept;
#endif
	struct {
		SocketEvent				connect;
		SocketEvent				receive;
		SocketEvent				send;
	} event;
	int							listenerID;
	/* listenerID

	   for listener conns, this is the identifier of the ConnListener * used
	   to start the listener

	   for a server connection, this is the identifier of the parent listener

	   for a client connection, this will be NULL
	*/

	TraceConfig					*trace;
} SOCK;


#include <connio.h>

typedef struct {
	int 						remaining;
	char 						*read;
	char 						*write;
} ClientBuffer;

typedef enum {
    CONNECT_EVENT_STAGE_INITIALIZED = 0,
	CONNECT_EVENT_STAGE_STARTED,
    CONNECT_EVENT_STAGE_FINISHED
} ConnectEventStages;

typedef struct {
	struct ConnEvent *head;
	struct ConnEvent *tail;
	unsigned long count;
} ConnEventList;

typedef struct {
	int				status;
	int			  	error;
	const char		*file;
	int 			line;
} _AsyncProblem;

typedef struct {
	ConnEventList		queue;
	struct ConnEvent	*freeList;
	struct {
		int				sent;
		int				received;
	} bytes;
	void 				*client;

	/* 'problem' is null when all is well.
	   When 'problem' has a value, the queue
	   has been aborted and the structure it
	   it points to has a copy of important
	   information from the first event to
	   encounter a problem.
	*/
	_AsyncProblem 		problemReal;
	_AsyncProblem 		*problem;
	int 				threads;
	/*
	  sslEnabled is used to determine when an ssl disconnect
	  event needs to be inserted into the event queue.
	  Specifically an ssl disconnect event needs to be inserted
	  before a disconnect if ssl is enabled and ssl disconnected
	  is not explicitly added to the queue.
	*/
	XplBool				sslEnabled;
} AsyncTransaction;

typedef struct {
	struct ConnEvent 	*connect;
	struct ConnEvent 	*send;
	struct ConnEvent 	*receive;
} ActiveEvent;

typedef enum {
    CONN_STATE_FREE = 0,
    CONN_STATE_CACHED,
	CONN_STATE_IDLE,
    CONN_STATE_SYNCHRONOUS,
    CONN_STATE_ASYNCHRONOUS_CLIENT,
	CONN_STATE_ASYNCHRONOUS_ENGINE,
	CONN_STATE_ASYNCHRONOUS_ENGINE_COMPLETE,
	CONN_STATE_SYNCHRONOUS_LISTENER,
	CONN_STATE_ASYNCHRONOUS_LISTENER
} ConnState;

#ifdef __cplusplus
extern }
#endif

/*
	Include the public definitions

	All public definitions are included here.  Any structure that is not needed
	by the private portion of the public structures should be defined after this
	point.
*/

typedef struct ConnectionPriv {
	Connection				pub;
	ConnShutdownCB	shutdownCallback;

#ifdef DEBUG
	struct {
		int 				count;
		time_t				lastTime;
	} failureLoop;

	int writers;
#endif


	/*
		The previous and next nodes used link this connection
		into a list of conns
	*/
	struct {
		struct ConnectionPriv	*next;
		struct ConnectionPriv	*previous;
	} list;

	struct {
		SocketTimeout		connect;
		SocketTimeout		disconnect;
		SocketTimeout		send;
		SocketTimeout		receive;
	} timeout;

	struct {
		union {
			struct {
				ConnAEventCB	qSuccess;
				ConnAEventCB	qFailure;
				AThread			thread;
			};
			struct {
				ConnAAcceptCB	accept;
				void            *acceptContext;
				ConnAListenCB	listen;
			};
		} cb;
		AsyncTransaction		transaction;
	} async;

	ActiveEvent				activeEvent;
	ConnState				state;

	/*
		Any thead that changes 'state' or anythting in 'event' must hold
		this mutex.

		The structure above 'mutex' (public and private) is initialized
		with 0s when the conn is cached.  The structure below is
		initialized by connectionInit()
	*/
	XplMutex				mutex;

	/*
		'sendBuffer' is owned by the connio consumer apis
		it should never be touched by async engine code
		or by event process functions

		'receiveBuffer' is owned by the ConnEvent stored in
		conn->event.receive

	*/
	IOBuffer	*sendBuffer;
	IOBuffer	*receiveBuffer;

	SOCK				*sp;
	/* the handle used to allocate this conn */
	struct ConnHandlePriv		*connHandle;
	uint32					marked;
} ConnectionPriv;


typedef struct ConnListenerPriv {
	ConnListener			pub;
	ConnAAcceptCB			callback;
	void					*acceptContext;
	unsigned long			throttlingThreshold;
	ListenerEventCallback	eventCallback;
	AThreadEngine			*cbThreadEngine;
	struct sockaddr			*sa;
	socklen_t				salen;

	XplBool					openFirewall;

	int						backlog;

	int						id;

	struct ConnHandlePriv	*handle;

	struct ConnListenerPriv	*next;
	struct ConnListenerPriv	*previous;
	XplBool					stopped;

	int						state;
	char					*name;
} ConnListenerPriv;

typedef struct ConnListenerListPriv {
	ConnListenerList		pub;
	struct ConnHandlePriv	*handle;
	WJElement				list;
} ConnListenerListPriv;

typedef struct ConnHandlePriv {
	ConnHandle					pub;
	struct ConnHandlePriv		*next;
	struct ConnHandlePriv		*previous;

	XplMutex mutex;

	ConnCache					connCache;

	ConnList					inUse;
	XplAtomic					inCleanup;

	struct {
		struct ConnListenerPriv	*head;
		struct ConnListenerPriv	*tail;
		int 					last;
		XplBool					preventNew;
	} listener;

	struct {
		SocketTimeout		accept;
		SocketTimeout		connect;
		SocketTimeout		disconnect;
		SocketTimeout		receive;
		SocketTimeout		send;
		SocketTimeout		shutdown;
	} timeOut;

	struct {
		uint32 				fixed;
		uint32				dynamic;
	} flags;

	char					*identity;
	XplBool					shuttingDown;
	time_t 					startTime;
	struct {
		AThreadEngine		*defaultConsumer;
		AThreadEngine		*internal;
	} threadEngine;
	ConnHandleConfig		*config;
} ConnHandlePriv;





#ifdef __cplusplus
extern "C" {
#endif

#if !defined(SOCKET_ERROR)
#define SOCKET_ERROR (-1)
#endif

#if !defined(SOCKET_INVALID)
#define SOCKET_INVALID (XplSocket)SOCKET_ERROR
#endif

#define CONN_TCP_MTU                    ((4096 - 0xA0) - 1) /* anything bigger than 4k causes data loss in OpenSSL */
#define CONN_TCP_THRESHOLD              256

#define CONN_HANDLE_IDENT_LENGTH        (255)

#define CONN_DEFAULT_TIMEOUT_READ       (15 * 60 * 1000)
#define CONN_DEFAULT_TIMEOUT_WRITE      (15 * 60 * 1000)
#define CONN_DEFAULT_TIMEOUT_CONNECT    (2 * 60 * 1000)
#define CONN_DEFAULT_TIMEOUT_DISCONNECT (3 * 1000)
#define CONN_DEFAULT_SHUTDOWN_TIMEOUT	(3 * 1000)
#define CONN_DEFAULT_POOL_MINIMUM       250
#define CONN_DEFAULT_POOL_MAXIMUM       1000

#define CONN_INVALID_CONNECTION         ((Connection *)NULL)
#define CONN_INVALID_HANDLE             ((ConnHandlePriv *)NULL)

#define CONN_EVENT_INFINITE_TIMEOUT     (0xFFFFFFFF)
#define CONN_EVENT_DEFAULT_TIMEOUT      (0xFFFFFFFE)

#define CONN_EVENT_ACCEPT_CACHE_MIN		8


typedef enum {
	CONN_EVENT_ACCEPT = 1,
    CONN_EVENT_CONNECT,
    CONN_EVENT_DISCONNECT,
    CONN_EVENT_SSL_ACCEPT,
	CONN_EVENT_TLS_ACCEPT,
	CONN_EVENT_SSL_CONNECT,
	CONN_EVENT_TLS_CONNECT,
	CONN_EVENT_SSL_DISCONNECT,

	CONN_EVENT_READ,
	CONN_EVENT_READ_COUNT,
	CONN_EVENT_READ_UNTIL_PATTERN,
	CONN_EVENT_READ_TO_CB,
    CONN_EVENT_READ_TO_CONN,

    CONN_EVENT_WRITE_FROM_CB,
	CONN_EVENT_WRITE_FROM_CLIENT_BUFFER,
    CONN_EVENT_FLUSH,
} ConnEventType;

typedef enum {
	CONN_EVENT_CLASS_CONNECT = 1,
	CONN_EVENT_CLASS_RECEIVE,
	CONN_EVENT_CLASS_SEND
} ConnEventClass;

typedef enum {
    CONN_EVENT_MODE_SYNCHRONOUS,
	CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING,
    CONN_EVENT_MODE_ASYNCHRONOUS
} ConnEventMode;

/* Read Flags */
#define CONN_READ_COPY				0x02000000
#define CONN_READ_DELETE			0x04000000
#define CONN_READ_TERMINATE			0x08000000
#define CONN_READ_INCLUDE_PATTERN	0x10000000
#define CONN_READ_REPLACE_PATTERN	0x20000000
#define CONN_READ_PARTIAL_OK		0x40000000


#ifndef CONNIO_SOCKET_TIMEOUT
#define CONNIO_SOCKET_TIMEOUT
#if defined(LINUX) || defined(S390RH) || defined(SOLARIS) || defined(MACOSX)
typedef int SocketTimeout;
#elif defined(WIN32)
typedef struct timeval SocketTimeout;
#endif
#endif

typedef union {
	struct {
		char				*start;
		unsigned long		size;
		ConnAFreeCB		 	freeCB;
		char			 	pattern[ MAX_PATTERN_SIZE + 1 ];
	} buffer;

	struct {
		FILE				*handle;
		unsigned long		count;
	} file;

	struct {
		struct sockaddr		*dst;
		unsigned long		dstSize;
		struct sockaddr		*src;
		unsigned long		srcSize;
	} addr;

	struct {
		struct ConnectionPriv 	*handle;
		unsigned long		count;
	} conn;

	struct {
		ConnIOCB		 	func;
		void 			   	*arg;
		int					count;
	} cb;

	ConnAListenCB			listenCB;
} ClientArgs;

typedef struct ConnEvent {
	/* initialized to 0 in ConnEventAssign() */
	int							error;
	ConnEventStatus				status;
	unsigned long				transferred;
	ClientArgs					args;

	/* initialized to values in ConnEventAssign() */
	ConnEventType				type;
	ConnEventClass				eventClass;
	CodeSource					source;
	SocketTimeout		 		timeOut;

	/* Initialized to 0 every time an event is assigned */
	XplBool					inserted;

	/* Initialized every time an event is assigned */
	ConnEventMode			mode;

	/* Initialized when certain event types are assigned */
	uint32					dataMode;
	ClientBuffer			clientBuffer;
	IOBuffer				*ioBuffer;

	/* Initialized once before lock-in */
	struct ConnectionPriv	*conn;
	SOCK					*socket;
	struct ConnEvent		*next;
	struct ConnEvent		*previous;
} ConnEvent;

typedef enum {
    CONN_EVENT_HANDLE_INFINITE_TIMEOUT  = (1 << 0),

    CONN_EVENT_HANDLE_MAX_DEFINED       = (1 << 1)
} ConnEventHandleFlags;

/* ConnioShouldAlwaysAssertIfNot asserts no matter what */

#  define RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode(x) if (!(x)) {								\
		int assertErrno = errno;								\
		printf("WARNING: Assert failed at %s:%d (errno: %d)\n",		\
			__FILE__, __LINE__, assertErrno);					\
		*((char *)NULL) = (char) assertErrno;					\
	}



/*
    connio.c
*/

XplBool connChild( ConnListenerListPriv *cllist, ConnectionPriv *conn );
void ConnectAddressesFreeCopy( ConnEvent *ce );
int ConnVPrintf( ConnectionPriv *conn, char *format, va_list args );
int ConnPrintf( ConnectionPriv *conn, char *format, ... ) XplFormatString(2,3);
int ConnReadBuffer( ConnectionPriv *conn, char *dest, int length, int *state );
int ConnSetReadBufferMode( ConnectionPriv *conn, unsigned long mode, unsigned long bytes );
int connWriteEx( ConnectionPriv *conn, char *data, int length, int timeout, const char *file, const int line);
int ConnSetWriteBufferMode( ConnectionPriv *conn, unsigned long mode, unsigned long bytes );
void ConnWorkerStats( unsigned long *idle, unsigned long *total, unsigned long *active, unsigned long *ready, unsigned long *work );
int _ConnWrite( ConnectionPriv *conn, char *buffer, int length );


#ifdef __cplusplus
}
#endif


/*! \def    CONTAINING_RECORD(address, definition, member)
    \brief  Computes the address of 'definition' using the address of a member
            within definition.
*/
#if !defined(CONTAINING_RECORD)
#define CONTAINING_RECORD(address, definition, member)  \
    ((definition *)((char *)(address) - (unsigned long)(&((definition *)0)->member)))
#endif

typedef enum {
    CONNIO_STATE_LOADED = 0,
    CONNIO_STATE_INITIALIZING,
    CONNIO_STATE_RUNNING,
    CONNIO_STATE_SHUTTING_DOWN,
    CONNIO_STATE_SHUTDOWN,

    CONNIO_MAX_STATES
} ConnIOStates;


typedef struct ConnIO {
    ConnIOStates state;

    XplMutex mutex;

    struct {
        ConnHandlePriv *head;
        ConnHandlePriv *tail;
		int count;
    } handles;

	struct {
		AThreadEngine *internal;
		AThreadEngine *defaultConsumer;
	} threadEngine;

	WJElement config;
} ConnIOGlobals;


/*
    connio.c
*/


extern ConnIOGlobals ConnIO;


XplBool FlushAEventNeededEx( ConnectionPriv *conn, ConnEvent *connEvent, ConnEvent **flushEvent, const char *file, const int line );
ConnEvent *SSLDisconnectEventCreate( ConnectionPriv *conn, int timeout, const char *file, const int line );

ConnectionPriv *_ConnAllocEx(ConnHandlePriv *handle, SOCK *sp, const char *file, const int line);
void ConnEventFreeResources( ConnEvent *ce );
void ConnEventFree( ConnectionPriv *conn, ConnEvent *ce );

unsigned long pthreads_thread_id(void );


/* crypto.c */
EXPORT void ConnCryptoLockInit(void);
EXPORT void ConnCryptoLockDestroy(void);

void CryptoLockCallback(int mode, int type, const char *file, int line);

/* queue.c */

int EventQueueCountFlushes( ConnectionPriv *conn );
XplBool EventEnqueue( ConnectionPriv *conn, ConnEvent *connEvent, const char *file, const int line );
void EventEnqueueFailure( ConnectionPriv *conn, const char *file, const int line );
XplBool EventsQueued( ConnectionPriv *conn, const char *file, const int line );
XplBool EventsSubmit( ConnectionPriv *conn, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *threadEngine, const char *file, const int line );
void EventQueueAbort( ConnectionPriv *conn, ConnEvent *connEvent, ConnEventStatus status, int error, const char *file, int line );
void AcceptCallback( SOCK *listenerCS, SOCK **sp, void *CE );
void AsyncListenerCallback( SOCK *socket, void *CE, SocketEventType type, int error );
void AsyncEventCallback( SOCK *socket, void *CE, SocketEventType type, int error );

/* process.c */

                /* connect events */
ConnEventStatus ConnEventProcess( ConnEvent *connEvent );

/*
   public functions in sockcommon.c
*/
void SocketLogMessage( SOCK *socket, char *msg, const char *path, const int line );
int SocketGetPeerAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen );
int SocketGetSockAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen );
int SocketClose( SOCK *socket );
void SocketReset( SOCK *socket );
int  SocketValid( SOCK *socket );
int  SocketSyncConnectStart( SOCK *socket, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn );
void SocketConnectFinish( SOCK *socket );
void SocketConnectAbort( SOCK *socket, int error );
void SocketDisconnectFinish( SOCK *socket );

int SocketDisconnect( SOCK *socket, SocketTimeout *timeOut );

SSL *SocketSSLAlloc( SOCK *socket, SSL_CTX *context );
void SocketSSLFree( SSL *socket );

void SocketSSLAcceptFinish( SOCK *socket );
void SocketSSLConnectFinish( SOCK *socket );
void SocketSSLDisconnectFinish( SOCK *socket );
void SocketSSLAbort( SOCK *socket , int error );

int  SocketSSLAccept( SOCK *socket, SSL_CTX *context, SocketEventType *blockedOn );
int  SocketSSLConnect( SOCK *socket, SSL_CTX *context, SocketEventType *blockedOn );
int  SocketSSLDisconnect( SOCK *socket, SocketEventType *blockedOn );

int  SocketReceive(SOCK *socket, char *buffer, size_t length, SocketTimeout *timeOut, SocketEventType *blockedOn, int *error );
int  SocketSend( SOCK *socket, char *buffer, size_t count, SocketEventType *blockedOn, int *error );
int  SocketWait( SOCK *socket, SocketEventType waitOn, SocketTimeout *timeOut );

XplBool SocketIsIdle( SOCK *socket );

SOCK *SocketAccept( SOCK *listenerSp );
int SocketListen( SOCK *sp, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, int listenerID );
void SocketInit( SOCK *socket, char *identity );
/*
  SocketAlloc creates the lock.  It does not aquire or release it
  making socketAlloc() and SocketAlloc() interchangeable.
  sockAlloc() is defined only to maintain convention.
 */
#define socketAlloc( l )     SocketAlloc( l )
SOCK *SocketAlloc( int listenerId );
void SocketEventsClean( SOCK *socket );
void SocketRelease( SOCK **sp );
int SocketEventWaitEx( SOCK *cs, SocketEventType type, SocketTimeout *timeOut, struct sockaddr *dst, socklen_t dstLen, SocketEventCB ecb, AThreadEngine *ecbThreadEngine, AcceptEventCB acb, void *acbArg, AThreadEngine *acbThreadEngine, unsigned long throttlingThreshold, ListenerEventCallback lecb, const char *file, long line );
XplBool SocketListenerSuspend( SOCK *cs );
XplBool SocketListenerResume( SOCK *cs );
void SocketEventKill( SOCK *cs, SocketEventType type );
SocketEventEngine *SocketEventEngineAlloc( void );
int SocketEventEngineFree( SocketEventEngine *engine );
XplBool SocketLibStart( uint32 flags );
void SocketLibStop( void );


// public functions in sockunix.c and sockwin.c
EXPORT int SocketTimeoutSet( SocketTimeout *timeout, int milliseconds );
EXPORT int SocketTimeoutGet( SocketTimeout *timeout );
int SocketAsyncConnectStart( SOCK *socket, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn );
int SocketAsyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn );
int SocketSyncDisconnectStart( SOCK *socket, SocketEventType *blockedOn, SocketTimeout *timeout );

// public functions in trace.c
uint32 SocketTraceFlagsModify( SOCK *socket, uint32 addFlags, uint32 removeFlags );
void SocketTraceRemoveFiles( char *consumer );
XplBool SocketTraceBeginEx( SOCK *socket, char *consumer, void *subject, uint32 flags, size_t bufferSize, SOCK *tracedSocket, char *file, int line );
void SocketTraceEnd( SOCK *socket );
void SocketTraceEvent( SOCK *socket, uint32 event, char *buffer, long len, const char *file, const int line );
void SocketTraceFlush( SOCK *socket );
void SocketTraceDumpToFile( SOCK *socket, FILE *file );

/*
   internal functions in sockcommon.c
*/
int HandleSocketEvent( void *arg );
void socketLogMessage( SOCK *socket, char *msg, const char *path, const int line );
int socketGetPeerAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen );
int socketGetSockAddr( SOCK *socket, struct sockaddr *addr, socklen_t *addrLen );
int socketClose( SOCK *socket );
void socketReset( SOCK *socket );
int socketSyncConnectStart( SOCK *sock, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, SocketEventType *blockedOn );
int socketValid( SOCK *socket );
void socketConnectFinish( SOCK *socket );
int socketSSLDisconnect( SOCK *socket, SocketEventType *blockedOn );
int socketDisconnect( SOCK *socket, SocketTimeout *timeOut );
void socketDebug( int context, SocketEvent *event, int type, char *format, ... );
#define AECLock( c )	XplLockAcquire( &( ( c )->safeList.lock ) )
#define AECUnlock( c )	XplLockRelease( &( ( c )->safeList.lock ) )
#define AEMalloc()		MemMalloc( sizeof( AcceptEvent ) )
AcceptEvent *acceptEventPop( AcceptEventCache *aeCache );
AcceptEvent *acceptEventAlloc( AcceptEventCache *aeCache );
int acceptEventRelease( AcceptEvent *acceptEvent );
void acceptEventInit( AcceptEvent *acceptEvent, SocketEvent *se, XplSocket soc );
void acceptReady( AcceptEvent *event );


/*
   internal functions in sockunix.c and sockwin.c
*/
int socCreate( XplBool iocpThread );
int sockDisconnect( XplSocket s );
void sockPrintSysErrorEx( const char *function, char *file, int line );
#define sockPrintSysError() sockPrintSysErrorEx( __func__, __FILE__, __LINE__ )
SocketEventType sockEventTranslate( SocketEventType type );
int sockEventAdd( SOCK *cs, SocketEvent *se, SocketTimeout *timeOut, struct sockaddr *dst, socklen_t dstLen, XplBool *doneAlready );
XplBool sockEventListenerRequeue( SOCK *socket, SocketEvent *event );
int sockEngineInit( SocketEventEngine *engine );
int sockEngineShutdown( SocketEventEngine *engine );
int sockEventInit( SOCK *cs, SocketEvent *event );
XplBool sockEventKill( SocketEvent *event );
void sockEventClean( SocketEvent *se );
void sockReset( SOCK *socket );
int sockWaitForReadable( XplSocket sock, SocketTimeout *timeout );
int sockWaitForWritable( XplSocket sock, SocketTimeout *timeout );
void sockCreatorStart( void);
void sockCreatorStop( void );

/* internal functions in throttle.c */
XplBool listenerShouldThrottle( SocketEvent *se );
XplBool listenerStartThrottling( SocketEvent *se );
XplBool listenerThrottling( SocketEvent *se );
void listenerSpinWhileThrottling( SocketEvent *se );

/* internal functions in trace.c */
void socketTraceEvent( SOCK *socket, uint32 event, char *buffer, long len, const char *file, const int line );
XplBool socketTraceAppend( SOCK *socket, char *buff, size_t len );
void socketTraceDumpToFile( SOCK *socket, FILE *file );

#ifndef min
#define min(a, b)   (((a) < (b))? (a): (b))
#endif

#ifndef max
#define max(a, b)   (((a) > (b))? (a): (b))
#endif

#define XplStrError( arg ) ""  // FIXME - return the string version of the error

#ifdef DEBUG

__inline static void debugLoopCountCheck( ConnectionPriv *conn, int count )
{
	time_t current = time( NULL );

	if( ( current - conn->failureLoop.lastTime ) < 2 ) {   /* we are only looking for tight loops */
		conn->failureLoop.count++;
		conn->failureLoop.lastTime = current;
		DebugAssert( conn->failureLoop.count > count );
	} else {
		conn->failureLoop.count = 0;
		conn->failureLoop.lastTime = 0;
	}
}

#define DebugLoopCountReset( CONN )						( CONN )->failureLoop.count = 0; ( CONN )->failureLoop.lastTime = 0
#define DebugLoopCountCheck( CONN, COUNT )				debugLoopCountCheck( ( CONN ), ( COUNT ) )

#else

#define DebugLoopCountReset( CONN )
#define DebugLoopCountCheck( CONN, COUNT )

#endif

#define traceEvent( s, e, b, l ) 					    socketTraceEvent( ( s ), ( e ), ( b ), ( l ), "UNDEFINED", 0 )
#define traceApiEvent( s, e, b, l, f, ln ) 				socketTraceEvent( ( s ), ( e ), ( b ), ( l ), ( f ), ( ln ) )
#define TraceEvent( s, e, b, l ) 					    SocketTraceEvent( ( s ), ( e ), ( b ), ( l ), "UNDEFINED", 0 )
#define TraceApiEvent( s, e, b, l, f, ln ) 				SocketTraceEvent( ( s ), ( e ), ( b ), ( l ), ( f ), ( ln ) )


#include "sockinline.h"
#include "socklog.h"
#include "debug.h"
#include "connlog.h"
#include "conninline.h"

#endif
