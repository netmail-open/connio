/*! \file   connio.h
    \brief  The managed connection library public header.

            The managed connection library public header.
*/
#ifndef _CONNIO_H
#define _CONNIO_H

#include <stdarg.h>

#include <xplmem.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <threadengine.h>
#include <listener_event.h>

#ifdef __cplusplus
extern "C" {
#endif


#if defined( LINUX )
#include <linux/limits.h>
#endif

/* Limits */
#if defined (PATH_MAX)
# define XPL_MAX_PATH PATH_MAX
#elif defined (MAX_PATH)
# define XPL_MAX_PATH MAX_PATH
#elif defined (_PC_PATH_MAX)
# define XPL_MAX_PATH sysconf(_PC_PATH_MAX)
#elif defined (_MAX_PATH)
# define XPL_MAX_PATH _MAX_PATH
#else
# error "XPL_MAX_PATH is not implemented on this platform"
#endif

#if defined (NAME_MAX)
# define XPL_NAME_MAX FILENAME_MAX
#elif defined (FILENAME_MAX)
# define XPL_NAME_MAX FILENAME_MAX
#else
# error "XPL_NAME_MAX is not implemented on this platform"
#endif

#define HAVE_SYS_SOCKET_H 1  // FIXME - discover this
#define HAVE_NETINET_IN_H 1  // FIXME - discover this

#define SYSCONF_DIR "/opt/connio/etc"

#define CONN_IOB_SIZE					( ( 1024 * 64 ) - 1024)

#define DebugPrintErr  printf
#define DebugPrintf    printf

/* values that can be set in errno that are not in the standard definition */
#define ECONNCLOSEDBYPEER		777		/* Connection closed gracefully by peer */
#define ECONNIOBUG				778		/* Connio messed up */
#define ECONNIOMODE				779		/* Conn is not in a mode that is compatible with request */
#define ECONNIOINIT				780		/* ConnIO is not initialized to support this feature */
#define ECONNIOCBSHORT			781		/* ConnIOCB returned 'done' before count was satisfied */
#define ECONNIOCBLONG			782		/* ConnIOCB says it took more data than it was given */
#define ECONNSSLSTART			783		/* SSL or TLS failed to start  */
#define ECONNALREADYSECURED		784		/* Connection is already secured */
#define ECONNNOTSECURED			785		/* Connection is not secured */
#define ECONNMOREDATA			786		/* Event requires more data */
#define ECONNAMBIG				787		/* WSA error did not uniquely translate to errno */
#define ECONNHANDLESHUTDOWN		788		/* The conn handle is shutting down */

#define ESERVICENEW				801		/* Service has not been started */
#define ESERVICESTOPPED			802		/* Service has been stopped */

#define ECOMPLETE				901		/* Task complete */

#define EIDENTIFYFAILED			902		/* User identification failed */


#define RECOMMENDED_CIPHER_LIST										\
								"ECDHE-RSA-AES256-GCM-SHA384:"		\
								"ECDHE-RSA-AES128-SHA256:"			\
								"ECDHE-RSA-AES256-SHA384:"			\
								"HIGH:"								\
								"!aNULL:"							\
								"!eNULL:"							\
								"!EXPORT:"							\
								"!3DES:"							\
								"!DES:"								\
								"!RC4:"								\
								"!MD5:"								\
								"!PSK:"								\
								"!SRP:"								\
								"!CAMELLIA"

/*
	Disable these for now... More info on ECDHE:
	https://wiki.openssl.org/index.php/Elliptic_Curve_Diffie_Hellman
*/
#if 0
								"ECDHE-RSA-AES128-GCM-SHA256:"		\
								"ECDHE-ECDSA-AES128-GCM-SHA256:"	\
								"ECDHE-ECDSA-AES256-GCM-SHA384:"	\
								"DHE-RSA-AES128-GCM-SHA256:"		\
								"DHE-RSA-AES128-SHA256:"			\
								"DHE-RSA-AES256-SHA256:"
#endif

#define CONN_DEFAULT_IDENTITY           __FILE__

#define MAX_PATTERN_SIZE				7

/* flags that can be passed to ConnHandleAlloc */
#define CONN_HANDLE_SSL_CLIENT_ENABLED      	0x00000001
#define CONN_HANDLE_SSL_SERVER_ENABLED      	0x00000002

#define CONN_HANDLE_ASYNC_ENABLED           	0x00000010


/* flags that can be passed to ConnHandleFlagsModify */
#define CONN_HANDLE_DYN_FLAG_WAIT_FOR_SUBMIT    0x00000001


#define CONN_HANDLE_SSL_ENABLED			(CONN_HANDLE_SSL_CLIENT_ENABLED | CONN_HANDLE_SSL_SERVER_ENABLED)

/*
  All timeout argument are integers and will be interpreted as milliseconds.
  The following definitions can be used when appropriate.

  Zero should not be used. In debug builds it will assert.  In non-debug builds,
  it will be replaced with the default timeout.

  CONN_TIMEOUT_INFINITE should not be used for socket events.  In debug build,
  it will assert.  In non-debug builds, it will be replace by the default
  timeout.
*/
#define CONN_TIMEOUT_IMMEDIATE  -3
#define CONN_TIMEOUT_INFINITE   -2
#define CONN_TIMEOUT_DEFAULT    -1

#ifndef CONNIO_SOCKET_TIMEOUT
#define CONNIO_SOCKET_TIMEOUT
#if defined(LINUX) || defined(S390RH) || defined(SOLARIS) || defined(MACOSX)
typedef int SocketTimeout;
#define SOCKET_TIMEOUT_GRANULARITY	0x00000020	// 32 seconds
#elif defined(WIN32)
typedef struct timeval SocketTimeout;
#define SOCKET_TIMEOUT_GRANULARITY	0x00000001	// 1 second
#endif
#endif


typedef struct {
    struct Connection					*next;
    struct Connection					*previous;

    void								*data;

    void								(* free)(struct Connection *connection);
} ConnClientData;

typedef enum {
    CONN_EVENT_QUEUED = 0,
	CONN_EVENT_IN_PROGRESS,
	CONN_EVENT_PARTIAL,
    CONN_EVENT_COMPLETE,
	CONN_EVENT_TIMED_OUT,
    CONN_EVENT_FAILED,
	CONN_EVENT_KILLED
} ConnEventStatus;

typedef struct {
	int			  	error;
	const char		*file;
	int 			line;
} AsyncProblem;

typedef struct {
	ConnEventStatus status;
	struct {
		int sent;
		int received;
	} bytes;
	void *client;
	AsyncProblem problemReal;
	AsyncProblem *problem;
} AsyncSummary;

typedef enum {
	CONN_IOCBR_CONTINUE = 0,
	CONN_IOCBR_DONE,
	CONN_IOCBR_NEED_MORE_DATA,
	CONN_IOCBR_NEED_MORE_BUFFER,
	CONN_IOCBR_ERROR
} ConnIOCBResponse;

typedef ConnIOCBResponse (* ConnIOCB)( void *client, char *buffer, size_t *count );
typedef void (* ConnCreateCB)( struct Connection *conn, void *client );
typedef void (* ConnAListenCB)( struct Connection *conn );
typedef void (* ConnAFreeCB)( void *client );
typedef void (* ConnAEventCB)( struct Connection *conn, AsyncSummary *async );
typedef void (* ConnAAcceptCB)( struct Connection *conn, struct Connection *listener, void *context );
typedef int (* ConnShutdownCB)( struct Connection *conn );
/* the listener conn passed to the ConnAAcceptCB callback is guaranteed to be valid during the callback, but not after */

/*! \struct     Connection
    \brief      A managed connection structure.

                The managed connection library uses the Connection structure to
				perform buffered and un-buffered synchronous blocking,
				synchronous non-blocking and asynchronous network I/O operations
*/

typedef struct Connection {
	struct {
		const char				*file;
		int						line;
	} created;
	struct {
		const char				*file;
		int						line;
	} lastUsed;

    ConnClientData						client;			/*!< Consumer managed data; not for use within the library instrumentation! */
} Connection;

typedef struct ConnListener {
	Connection							*conn;
	void								*client;
} ConnListener;

typedef struct ConnListenerList {
	void	*obfuscated;	// some compilers wont accept typedefs that are blank
} ConnListenerList;

typedef struct ConnHandle {
	struct {
		SSL_CTX				*client;
		SSL_CTX				*server;
	} sslv23;

	struct {
		SSL_CTX				*client;
		SSL_CTX				*server;
	} tls;
} ConnHandle;

typedef struct {
	struct {
		unsigned int minimum;
		unsigned int maximum;

		struct {
			PoolEntryCB alloc;
			PoolEntryCB free;

			void *data;
		} cb;
	} connCache;

    struct {
		// timeouts in miliseconds
        unsigned int connect;
        unsigned int disconnect;
        unsigned int receive;
        unsigned int send;
        unsigned int shutdown;
    } timeOut;

	struct {
		AThreadEngine *internal;
		AThreadEngine *defaultConsumer;
	} threadEngine;
} ConnHandleConfig;

typedef struct {
	struct {
		int count;
		struct {
			int		count;
			time_t	time;
		} peak;
	} thread;

	struct {
		int			min;
		int			max;
		int			peak;
		int 		total;
		int			idle;
	} conns;

} ConnStats;

/* ---- Conn Handle Interfaces ---- */
#define ConnHandleAlloc( IDENTITY, FLAGS, CONFIG )  			ConnHandleAllocEx( IDENTITY, FLAGS, CONFIG, __FILE__, __LINE__)
#define ConnHandleConnectionsAbort( handle )					ConnHandleConnectionsAbortEx( ( handle ), NULL, __FILE__, __LINE__ );
#define ConnHandleConnectionsClose( handle )					ConnHandleConnectionsCloseEx( ( handle ), NULL, __FILE__, __LINE__ );

	/*
	  ConnHandleShutdown() has two timeouts.  The errorTimeout is the time in
	  milliseconds the function will wait before forcing an error on all active
	  transactions.  The closeTimeout is the time it will wait after canceling
	  transaction before it will close all connections associated with the
	  handle. If the function blocks longer than the combined total, this means
	  the consumer has still not freed one or more connections closed
	  connections.
	 */

#define ConnHandleShutdown( handle, eto, cto )					ConnHandleShutdownEx( (handle), (eto), (cto), __FILE__, __LINE__ );

EXPORT int ConnHandleShutdownEx( ConnHandle *handle, int errorTimeout, int closeTimeout, const char *file, const int line );
EXPORT int ConnHandleFree(ConnHandle *handle);

EXPORT uint32 ConnMarkEx( Connection *c, uint32 mark, const char *file, const int line );
#define ConnMark( c, m ) ConnMarkEx( ( c ), ( m ), __FILE__, __LINE__ )
EXPORT uint32 ConnUnmarkEx( Connection *c, uint32 mark, const char *file, const int line );
#define ConnUnmark( c, m ) ConnUnmarkEx( ( c ), ( m ), __FILE__, __LINE__ )

EXPORT XplBool ConnHandleWaitForConnectionsToExit( ConnHandle *handle, int timeout, int *waited );
EXPORT void ConnHandleConnectionsAbortEx( ConnHandle *handle, ConnListenerList *cllist, const char *file, const int line );
EXPORT void ConnHandleConnectionsCloseEx( ConnHandle *handle, ConnListenerList *cllist, const char *file, const int line );

EXPORT ConnHandle *ConnHandleAllocEx(char *identity, uint32 flags, ConnHandleConfig *config, const char *file, const int line);
EXPORT void ConnHandleStats( ConnHandle *handle, ConnStats *stats );
EXPORT void ConnHandlePreventNewListeners( ConnHandle *pubHandle );
EXPORT uint32 ConnHandleFlagsModify( ConnHandle *handle, uint32 addFlags, uint32 removeFlags );
EXPORT uint32 ConnHandleStaticFlags( ConnHandle *handle );

/* ---- SSL Handle Interfaces ---- */
EXPORT XplBool ConnLoadServerCertificate(ConnHandle *handle, const char *publicKeyPath, const char *privateKeyPath);


/* ---- Conn Interfaces ---- */
#define ConnFree( conn )			       						ConnFreeEx( ( conn ), __FILE__, __LINE__ )
EXPORT XplBool ConnFreeEx( Connection *conn, const char *file, const int line );
#define ConnRelease( conn )			       						ConnReleaseEx( ( conn ), __FILE__, __LINE__ )
EXPORT XplBool ConnReleaseEx( Connection **Conn, const char *file, const int line );

EXPORT int ConnSetShutdownCallbackEx( Connection *conn, ConnShutdownCB callback, const char *file, const int line );
#define ConnSetShutdownCallback( c, cb )  ConnSetShutdownCallbackEx( (c), (cb), __FILE__, __LINE__ )

/* ---- Conn Server Interfaces ---- */
#define ConnListen( conn, src, srcLen, backlog )			 	ConnListenEx( conn, src, srcLen, backlog, __FILE__, __LINE__)
#define ConnCloseListener(conn)									ConnCloseListenerEx(conn, __FILE__, __LINE__)
EXPORT Connection *ConnListenEx(ConnHandle *handle, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, const char *file, const int line);
EXPORT XplBool ConnCloseListenerEx(Connection *conn, const char *file, const int line);

/* ---- Conn Meta Interfaces ---- */
EXPORT XplBool ConnSyncReadyEx(Connection *Conn, const char *file, const int line);
#define ConnSyncReady(c)  ConnSyncReadyEx((c), __FILE__, __LINE__)
EXPORT XplBool ConnIsSecureEx(Connection *conn, const char *file, const int line);
#define ConnIsSecure(c)  ConnIsSecureEx((c), __FILE__, __LINE__)
EXPORT int ConnGetPeerAddrEx( Connection *conn, struct sockaddr *addr, socklen_t *addrLen, const char *file, const int line );
#define ConnGetPeerAddr( c, a, l )  ConnGetPeerAddrEx((c), (a), (l), __FILE__, __LINE__ )
EXPORT int ConnGetSockAddrEx( Connection *conn, struct sockaddr *addr, socklen_t *addrLen, const char *file, const int line );
#define ConnGetSockAddr( c, a, l )  ConnGetSockAddrEx((c), (a), (l), __FILE__, __LINE__ )

/* ---- Conn Connect Interfaces ---- */

#define ConnConnect(handle, dest, destLen, src, srcLen)			ConnConnectEx((handle), (dest), (destLen), (src), (srcLen), CONN_TIMEOUT_DEFAULT, NULL, NULL, __FILE__, __LINE__)
#define ConnSSLConnect(conn)									ConnSSLConnectEx((conn), CONN_TIMEOUT_DEFAULT, FALSE,__FILE__, __LINE__)
#define ConnTLSConnect(conn)									ConnSSLConnectEx((conn), CONN_TIMEOUT_DEFAULT, TRUE, __FILE__, __LINE__)
#define ConnSSLAccept(conn)										ConnSSLAcceptEx((conn), CONN_TIMEOUT_DEFAULT, FALSE, __FILE__, __LINE__)
#define ConnTLSAccept(conn)										ConnSSLAcceptEx((conn), CONN_TIMEOUT_DEFAULT, TRUE, __FILE__, __LINE__)

#define ConnAccept( server )       								ConnAcceptEx((server), __FILE__, __LINE__)
#define ConnKill( conn ) 										ConnKillEx( ( conn ), __FILE__, __LINE__ )
#define ConnDisconnect( conn )									ConnDisconnectEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__ )
#define ConnSSLDisconnect( conn )								ConnSSLDisconnectEx( conn, __FILE__, __LINE__ )
#define ConnClose( conn )										ConnDisconnect( conn )

EXPORT Connection *ConnConnectEx(ConnHandle *handle, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int timeout, ConnCreateCB cccb, void *cccbArg, char *file, int line);
EXPORT Connection *ConnAcceptEx(Connection *server, char *file, int line);
EXPORT XplBool ConnKillEx( Connection *conn, char *file, int line );
EXPORT XplBool ConnDisconnectEx( Connection *conn, int timeout, const char *file, const int line );
EXPORT XplBool ConnSSLConnectEx(Connection *conn, int timeout, XplBool tls, const char *file, const int line);
EXPORT XplBool ConnSSLAcceptEx(Connection *conn, int timeout, XplBool tls, const char *file, const int line);
EXPORT XplBool ConnSSLDisconnectEx( Connection *conn, const char *file, const int line );


/* ---- Conn Read Interfaces ---- */
EXPORT int ConnReadEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnReadCountEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnReadLineEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnReadCRLFEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnReadGetSEx(Connection *conn, char *buff, int buffSize, XplBool includeLF, int timeout, const char *file, const int line);
EXPORT int ConnReadAnswerEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnReadUntilPatternEx(Connection *conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line);

EXPORT int ConnEatCountEx(Connection *Conn, int count, int timeout, const char *file, const int line);

EXPORT int ConnPeekEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnPeekCountEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnPeekLineEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnPeekAnswerEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT int ConnPeekUntilPatternEx(Connection *conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line);

EXPORT int 	ConnReadToCBEx(Connection *conn, ConnIOCB cb, void *cbArg, int count, int timeout, const char *file, const int line);
EXPORT int 	ConnReadToConnEx(Connection *conn, Connection *dest, int count, int timeout, const char *file, const int line);

#define ConnRead( conn, buff, buffsize )													ConnReadEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnReadCount( conn, buff, buffsize )		   										ConnReadCountEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnReadLine( conn, buff, buffsize )		   										ConnReadLineEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnReadCRLF( conn, buff, buffsize )		   										ConnReadCRLFEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnGetS( conn, buff, buffsize )		   											ConnGetSEx( conn, buff, buffsize, TRUE, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnGetLine( conn, buff, buffsize )		   											ConnGetSEx( conn, buff, buffsize, FALSE, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnReadAnswer( conn, buff, buffsize )		   										ConnReadAnswerEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnReadUntilPattern( conn, buff, buffsize, pattern )								ConnReadUntilPatternEx( conn, buff, buffsize, pattern, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnEatCount( conn, count )		   										ConnEatCountEx( conn, ( count ), CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnPeek( conn, buff, buffsize )													ConnPeekEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnPeekCount( conn, buff, buffsize )												ConnPeekCountEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnPeekLine( conn, buff, buffsize )												ConnPeekLineEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnPeekAnswer( conn, buff, buffsize )												ConnPeekAnswerEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnPeekUntilPattern( conn, buff, buffsize, pattern )								ConnPeekUntilPatternEx( conn, buff, buffsize, pattern, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnReadToCB( conn, cb, cbArg)														ConnReadToCBEx( conn, cb, cbArg, 0, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnReadCountToCB( conn, cb, cbArg, count)											ConnReadToCBEx( conn, cb, cbArg, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnReadToConn(conn, dest, count)													ConnReadToConnEx( conn, dest, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)


/* ---- Conn Write Interfaces ---- */
#define ConnWrite(conn, buff, length)							ConnWriteEx( conn, buff, length, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnWriteString(conn, string)							ConnWriteStringEx( conn, string, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnWriteV(conn, format, args)							ConnWriteVEx( conn, format, args, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnWriteFromCB(conn, cb, cbArg)						ConnWriteFromCBEx( conn, cb, cbArg, 0, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnWriteCountFromCB(conn, cb, cbArg, count)			ConnWriteFromCBEx( conn, cb, cbArg, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnFlush( conn )										ConnFlushEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__ )
EXPORT int	ConnWriteEx( Connection *conn, char *buffer, int length, int timeout, const char *file, const int line );
EXPORT int	ConnWriteStringEx( Connection *conn, char *string, int timeout, const char *file, const int line );
EXPORT int 	ConnWriteF( Connection *conn, char *format, ...) XplFormatString(2,3);
EXPORT int 	ConnWriteVEx(Connection *conn, const char *format, va_list arguments, int timeout, const char *file, const int line);
EXPORT int  ConnWriteFromCBEx(Connection *conn, ConnIOCB cb, void * cbArg, int count, int timeout, const char *file, const int line);
EXPORT int 	ConnFlushEx(Connection *conn, int timeout, const char *file, const int line);

/* ---- Async Transition Interfaces ---- */

/*
  ConnASubmit()

  ConnASubmit() yeilds control of the conn to connio and signals that the
  callbacks may be called as soon as connio is done processing the queued
  events. The consumer must not attempt to use that conn until the callback
  is called.  ConnASubmit() can only be called successfully if the conn has at
  least one asynchronous conn event queued.

  If the conn is:
  - invalid
  - in the async engine ( owned by connio ) or
  - doing synchronous IO
  this api will return false and no callback will be called.  If submit is
  called with no events, submit will return true and the failure callback will
  be called.

  ConnANoop()

  ConnANoop() should be called before ConnASubmit if there are no queued events
  and a success callback is desired.

  ConnAQueuedEvents()

  ConnAQueuedEvents() can be used to determine if ansync events have been
  queued and not submitted.
*/

EXPORT XplBool ConnASubmitEx( Connection *Conn, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line );
EXPORT void ConnANoopEx( Connection *Conn, const char *file, const int line );
EXPORT XplBool ConnAQueuedEventsEx( Connection *Conn, const char *file, const int line );

#define ConnASubmit( conn, scb, fcb, clt )						ConnASubmitEx( ( conn ), ( scb ), ( fcb ), ( clt ), NULL, __FILE__, __LINE__ )
#define ConnANoop( conn )										ConnANoopEx( ( conn ), __FILE__, __LINE__ )
#define ConnAQueuedEvents( conn )								ConnAQueuedEventsEx( ( conn ), __FILE__, __LINE__ )

/* ---- Async Listener Interfaces ---- */
#define CONN_LISTENER_FRESH			0x00000001
#define CONN_LISTENER_ACTIVE		0x00000002
#define CONN_LISTENER_FAILED		0x00000004
#define CONN_LISTENER_STOPPED		0x00000008
#define CONN_LISTENER_SUSPENDED		0x00000010
#define CONN_LISTENER_EXPIRED		0x00000020
#define CONN_LISTENER_INDEPENDENT	0x00000040

#define ConnAAddListener(handle, name, sa, salen, backlog, acb, acbContext)	ConnAAddListenerEx((handle), (name), (sa), (salen), (backlog), (acb), (acbContext), NULL, FALSE, __FILE__, __LINE__)
#define ConnAAddPublicListener(handle, name, sa, salen, backlog, acb, acbContext)	ConnAAddListenerEx((handle), (name), (sa), (salen), (backlog), (acb), (acbContext), NULL, TRUE, __FILE__, __LINE__)
#define ConnARemoveListener(handle, which, listener)			ConnARemoveListenerEx((handle), (which), (listener), NULL, __FILE__, __LINE__)
#define ConnAStartListener(handle, which, listener)				ConnAStartListenerEx((handle), (which), (listener), __FILE__, __LINE__)
#define ConnAStopListener(handle, which, listener)				ConnAStopListenerEx((handle), (which), (listener), __FILE__, __LINE__)
#define ConnASuspendListener(handle, listener)					ConnASuspendListenerEx((handle), (listener), __FILE__, __LINE__)
#define ConnAResumeListener(handle, listener)					ConnAResumeListenerEx((handle), (listener), __FILE__, __LINE__)
#define ConnAListenerStatus(handle, which, listener)			ConnAListenerStatusEx((handle), (which), (listener), FALSE, __FILE__, __LINE__)
#define ConnAExpireListener(handle, which, listener)			ConnAExpireListenerEx((handle), (which), (listener), __FILE__, __LINE__)
#define ConnARestartListener(listener, sa, salen)				ConnARestartListenerEx((listener), (sa), (salen), __FILE__, __LINE__)
#define ConnAListenSuspend(conn)								ConnAListenSuspendEx((conn), __FILE__, __LINE__)
#define ConnAListenResume(conn)									ConnAListenResumeEx((conn), __FILE__, __LINE__)
#define ConnAListenerWaitForShutdown( handle )					ConnAListenerWaitForShutdownEx( ( handle ), __FILE__, __LINE__)
EXPORT void ConnAListenerWaitForShutdownEx( ConnHandle *handle, const char *file, const int line );
EXPORT ConnListener *ConnAAddListenerEx(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, XplBool openFirewall, const char *file, const int line);
//EXPORT ConnListener *ConnAAddPublicListener(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, const char *file, const int line);
EXPORT int ConnAListenerConfigure( ConnListener *listener, unsigned long threshold, ListenerEventCallback cb );
EXPORT int ConnARemoveListenerEx(ConnHandle *handle, int which, ConnListener *listener, ConnListenerList **list, const char *file, const int line);
EXPORT int ConnAStartListenerEx	(ConnHandle *handle, int which, ConnListener *listener, const char *file, const int line);
EXPORT int ConnAStopListenerEx	(ConnHandle *handle, int which, ConnListener *listener, const char *file, const int line);
EXPORT int ConnAListenerStatusEx(ConnHandle *handle, int which, ConnListener *listener, XplBool shuttingdown, const char *file, const int line);
EXPORT int ConnASuspendListenerEx(ConnHandle *pubHandle, ConnListener *Listener, const char *file, const int line);
EXPORT int ConnAResumeListenerEx(ConnHandle *pubHandle, ConnListener *Listener, const char *file, const int line);
EXPORT int ConnAExpireListenerEx(ConnHandle *handle, int which, ConnListener *listener, const char *file, const int line);
EXPORT int ConnARestartListenerEx(ConnListener *listener, struct sockaddr *addr, socklen_t addrlen, const char *file, const int line);
EXPORT XplBool ConnAListenSuspendEx( Connection *Conn, const char *file, const int line );
EXPORT XplBool ConnAListenResumeEx( Connection *Conn, const char *file, const int line );
EXPORT Connection *ConnAListenEx(ConnHandle *pubHandle, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int backlog, int listenerID, ConnAAcceptCB acb, void * acbContext, ConnAListenCB lcb, AThreadEngine *threadEngine, unsigned long throttlingThreshold, ListenerEventCallback lecb, const char *file, const int line);
#define ConnAListen( handle, src, srclen, backlog, acb, acbContext, lcb )	ConnAListenEx( (handle), (src), (srclen), (backlog), 0, (acb), (acbContext), (lcb), NULL,  0, NULL, __FILE__, __LINE__ )
EXPORT void ConnListenerListDestroy( ConnListenerList **list );
EXPORT void ConnAListenerShutdownEx( Connection *conn, const char *file, const int line );
#define ConnAListenerShutdown( conn )  ConnAListenerShutdownEx(conn, __FILE__, __LINE__ )

#define ConnChildrenShutdown( list, eto, cto )							ConnChildrenShutdownEx( ( list ), ( eto ), ( cto ), __FILE__, __LINE__ )
EXPORT int ConnChildrenShutdownEx( ConnListenerList *cllist, int errorTimeout, int closeTimeout, const char *file, const int line );

/*
	ConnAAddIndependentListener() acts exactly like ConnAAddListener(), except
	that the listener will be ignored by ConnARemoveListener(),
	ConnAStartListener(), ConnAStopListener(), ConnAListenerStatus(),
	ConnAExpireListener() and ConnARestartListener() when the listener argument
	is NULL.

	IMPORT: Any listener created with ConnAAddIndependentListener() must be
	managed on it's own.
*/
#define ConnAAddIndependentListener(handle, name, sa, salen, backlog, acb, acbContext) ConnAAddIndependentListenerEx((handle), (name), (sa), (salen), (backlog), (acb), (acbContext), NULL, __FILE__, __LINE__)
EXPORT ConnListener *ConnAAddIndependentListenerEx(ConnHandle *pubHandle, const char *name, struct sockaddr *addr, socklen_t addrlen,  int backlog, ConnAAcceptCB acb, void * acbContext, AThreadEngine *cbThreadEngine, const char *file, const int line);

/* ---- Async Connect Interfaces ---- */
#define ConnAConnect(handle, dst, dstlen, src, srclen)			ConnAConnectEx((handle), (dst), (dstlen), (src), (srclen), CONN_TIMEOUT_DEFAULT, NULL, NULL, __FILE__, __LINE__)
#define ConnADisconnect(conn)									ConnADisconnectEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__ )
#define ConnASSLConnect(conn)									ConnASSLConnectEx((conn), CONN_TIMEOUT_DEFAULT, FALSE, __FILE__, __LINE__)
#define ConnATLSConnect(conn)									ConnASSLConnectEx((conn), CONN_TIMEOUT_DEFAULT, TRUE, __FILE__, __LINE__)
#define ConnASSLAccept(conn)									ConnASSLAcceptEx((conn), CONN_TIMEOUT_DEFAULT, FALSE, __FILE__, __LINE__)
#define ConnATLSAccept(conn)									ConnASSLAcceptEx((conn), CONN_TIMEOUT_DEFAULT, TRUE, __FILE__, __LINE__)
#define AsynSSLDisconnect( conn )								ConnASSLDisconnectEx( conn, __FILE__, __LINE__ )
EXPORT Connection *ConnAConnectEx(ConnHandle *handle, struct sockaddr *destSaddr, socklen_t destSaddrLen, struct sockaddr *srcSaddr, socklen_t srcSaddrLen, int timeout, ConnCreateCB cccb, void *cccbArg, const char *file, const int line);

EXPORT void ConnADisconnectEx( Connection *conn, int timeout, const char *file, const int line );
EXPORT void ConnASSLConnectEx(Connection *conn, int timeout, XplBool tls, const char *file, const int line);
EXPORT void ConnASSLAcceptEx(Connection *conn, int timeout, XplBool tls, const char *file, const int line);
EXPORT void ConnASSLDisconnectEx( Connection *conn, const char *file, const int line );

/* ---- Async Read Interfaces ---- */
EXPORT void ConnAReadEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAReadCountEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAReadLineEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAReadAnswerEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAReadUntilPatternEx(Connection *conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line);
EXPORT void ConnAReadCRLFEx(Connection *Conn, char *buff, int buffSize, XplBool includeLF, int timeout, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line);
EXPORT void ConnAGetSEx(Connection *Conn, char *buff, int buffSize, XplBool includeLF, int timeout, ConnAEventCB successCB, ConnAEventCB failureCB, void *client, AThreadEngine *cbThreadEngine, const char *file, const int line);

EXPORT void ConnAEatCountEx(Connection *Conn, int count, int timeout, const char *file, const int line);

EXPORT void ConnAPeekEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAPeekCountEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAPeekLineEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAPeekAnswerEx(Connection *conn, char *buff, int buffSize, int timeout, const char *file, const int line);
EXPORT void ConnAPeekUntilPatternEx(Connection *conn, char *buff, int buffSize, char *pattern, int timeout, const char *file, const int line);

EXPORT void ConnATestReadEx(Connection *conn, int timeout, const char *file, const int line);
EXPORT void ConnATestReadCountEx(Connection *conn, int count, int timeout, const char *file, const int line);
EXPORT void ConnATestReadLineEx(Connection *conn, int timeout, const char *file, const int line);
EXPORT void ConnATestReadCRLFEx(Connection *conn, int timeout, const char *file, const int line);
EXPORT void ConnATestReadUntilPatternEx(Connection *conn, char *pattern, int timeout, const char *file, const int line);

EXPORT void ConnAReadToCBEx(Connection *conn, ConnIOCB cb, void *cbArg, int count, int timeout, const char *file, const int line);
EXPORT void	ConnAReadToConnEx(Connection *conn, Connection *dest, int count, int timeout, const char *file, const int line);

#define ConnARead( conn, buff, buffsize )  														ConnAReadEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAReadCount( conn, buff, buffsize )  												ConnAReadCountEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAReadLine( conn, buff, buffsize )  													ConnAReadLineEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAReadCRLF( conn, buff, buffsize, includeLF, scb, fcb, clt )  						ConnAReadCRLFEx( conn, buff, buffsize, ( includeLF ), CONN_TIMEOUT_DEFAULT, ( scb ), ( fcb ), ( clt ), NULL, __FILE__, __LINE__  )
#define ConnAGetS( conn, buff, buffsize, scb, fcb, clt  )  										ConnAGetSEx( conn, buff, buffsize, TRUE, CONN_TIMEOUT_DEFAULT, ( scb ), ( fcb ), ( clt ), NULL, __FILE__, __LINE__  )
#define ConnAGetLine( conn, buff, buffsize, scb, fcb, clt  )  									ConnAGetSEx( conn, buff, buffsize, FALSE, CONN_TIMEOUT_DEFAULT, ( scb ), ( fcb ), ( clt ), NULL, __FILE__, __LINE__  )
#define ConnAReadAnswer( conn, buff, buffsize )  												ConnAReadAnswerEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAReadUntilPattern( conn, buff, buffsize, pattern )  								ConnAReadUntilPatternEx( conn, buff, buffsize, pattern, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnAEatCount( conn, count )  															ConnAEatCountEx( conn, ( count ), CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnAPeek( conn, buff, buffsize )  														ConnAPeekEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAPeekCount( conn, buff, buffsize )  												ConnAPeekCountEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAPeekLine( conn, buff, buffsize )  													ConnAPeekLineEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAPeekAnswer( conn, buff, buffsize )  												ConnAPeekAnswerEx( conn, buff, buffsize, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnAPeekUntilPattern( conn, buff, buffsize, pattern )  								ConnAPeekUntilPatternEx( conn, buff, buffsize, pattern, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnATestRead( conn )  																	ConnATestReadEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnATestReadCount( conn, count )  														ConnATestReadCountEx( conn, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnATestReadLine( conn )  																ConnATestReadLineEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnATestReadCRLF( conn )  																ConnATestReadCRLFEx( conn, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )
#define ConnATestReadUntilPattern( conn, pattern )  											ConnATestReadUntilPatternEx( conn, pattern, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__  )

#define ConnAReadToCB( conn, cb, cbArg)															ConnAReadToCBEx( conn, cb, cbArg, 0, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAReadCountToCB( conn, cb, cbArg, count)												ConnAReadToCBEx( conn, cb, cbArg, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAReadToConn(conn, dest, count)														ConnAReadToConnEx( conn, dest, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)


/* ---- Async Write Interfaces ---- */
#define ConnAWriteBacklog( conn)								ConnAWriteBacklogEx( ( conn ), __FILE__, __LINE__)
#define ConnAWrite(conn, buff, length)							ConnAWriteEx( conn, buff, length, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAWriteFromBuffer(conn, buff, length, freeCB)	 	ConnAWriteFromBufferEx( conn, buff, length, freeCB, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAWriteString(conn, string )							ConnAWriteStringEx( conn, string, FALSE, NULL, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAWriteF(conn, format, ...)							ConnAWriteFEx( conn, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define ConnAWriteV(conn, format, args)							ConnAWriteVEx( conn, format, args, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAWriteFromCB(conn, cb, cbArg)						ConnAWriteFromCBEx( conn, cb, cbArg, 0, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAWriteCountFromCB(conn, cb, cbArg, count)			ConnAWriteFromCBEx( conn, cb, cbArg, count, CONN_TIMEOUT_DEFAULT, __FILE__, __LINE__)
#define ConnAFlush( conn )										ConnAFlushEx( conn, -1,__FILE__, __LINE__ )
EXPORT int ConnAWriteBacklogEx(Connection *conn, const char *file, const int line);
EXPORT void	ConnAWriteFromBufferEx(Connection *conn, char *data, int length, ConnAFreeCB freeCB, int timeout, const char *file, const int line);
EXPORT void	ConnAWriteEx(Connection *conn, char *data, int length, int timeout, const char *file, const int line);
EXPORT void ConnAWriteStringEx(Connection *conn, char *string, XplBool freeBuff, ConnAFreeCB freeCB, int timeout, const char *file, const int line);
EXPORT void	ConnAWriteFEx( Connection *conn, const char *file, const int line, char *format, ...) XplFormatString(4,5);
EXPORT void ConnAWriteVEx( Connection *conn, char *format, va_list arguments, int timeout, const char *file, const int line);
EXPORT void ConnAWriteFromCBEx(Connection *conn, ConnIOCB cb, void * cbArg, int count, int timeout, const char *file, const int line);
EXPORT void ConnAFlushEx(Connection *conn, int timeout, const char *file, const int line);


/* callbacks.c */
/* standard callback interfaces for WriteFromCB ReadToCB */

#define ConnFileReader	((ConnIOCB )_ConnFileReader)
EXPORT ConnIOCBResponse _ConnFileReader( FILE *fp, char *buffer, size_t *size );
#define ConnFileWriter	((ConnIOCB )_ConnFileWriter)
EXPORT ConnIOCBResponse _ConnFileWriter( FILE *fp, char *buffer, size_t *size );
#define ConnFileWriterEOS	((ConnIOCB )_ConnFileWriterEOS)
EXPORT ConnIOCBResponse _ConnFileWriterEOS( FILE *fp, char *buffer, size_t *size );

/* trace.c */
/* trace events */
#define TRACE_EVENT_RECV	 				0x00000001
#define TRACE_EVENT_READABLE 				0x00000002
#define TRACE_EVENT_SEND					0x00000004
#define TRACE_EVENT_WRITEABLE				0x00000008
#define TRACE_EVENT_ACCEPT_DONE 			0x00000010
#define TRACE_EVENT_ACCEPT_START			0x00000020
#define TRACE_EVENT_CONNECT_DONE 			0x00000040
#define TRACE_EVENT_CONNECT_START			0x00000080
#define TRACE_EVENT_DISCONNECT_DONE 		0x00000100
#define TRACE_EVENT_DISCONNECT_START		0x00000200
#define TRACE_EVENT_SSL_ACCEPT_DONE			0x00000400
#define TRACE_EVENT_SSL_ACCEPT_START		0x00000800
#define TRACE_EVENT_SSL_CONNECT_DONE		0x00001000
#define TRACE_EVENT_SSL_CONNECT_START		0x00002000
#define TRACE_EVENT_SSL_DISCONNECT_DONE		0x00004000
#define TRACE_EVENT_SSL_DISCONNECT_START	0x00008000
#define TRACE_EVENT_ERROR 					0x00010000
#define TRACE_EVENT_CLOSE 					0x00020000
#define TRACE_ALL							0x000fffff

#define TRACE_EVENT_READ		 			0x00100000
#define TRACE_EVENT_WRITE					0x00200000
#define TRACE_API_EVENTS					0x00f00000

/* trace output directives */
#define TRACE_TO_CONSOLE					0X01000000
#define TRACE_FLUSH_WHEN_FULL				0X02000000  // only relevant when a positve buffSize is passed to ConnTraceBeginEx
#define TRACE_FLUSH_WHEN_CLOSED				0X04000000  // only relevant when a positve buffSize is passed to ConnTraceBeginEx
#define TRACE_FLUSH_ALL						0x08000000

#define TRACE_DELETE_LOG					0x10000000

/* trace format directives */
#define TRACE_SUPRESS_DATA					0x20000000
#define TRACE_DUMP_DATA						0x40000000
#define TRACE_PROCESS_CPU					0x80000000


EXPORT XplBool ConnTraceBeginEx( Connection *conn, uint32 flags, size_t buffSize, Connection *tracedConn, char *file, int line );
#define ConnTraceBegin( c, f, tc )			ConnTraceBeginEx( ( c ), ( f ), 0, ( tc ), __FILE__, __LINE__ )
EXPORT void ConnTraceFlushEx( Connection *conn, const char *file, const int line );
#define ConnTraceFlush( c )   ConnTraceFlushEx((c), __FILE__, __LINE__)
EXPORT void ConnTraceEndEx( Connection *conn, const char *file, const int line );
#define ConnTraceEnd( c )   ConnTraceEndEx((c), __FILE__, __LINE__)
EXPORT uint32 ConnTraceFlagsModifyEx( Connection *conn, uint32 addFlags, uint32 removeFlags, const char *file, const int line );
#define ConnTraceFlagsModify( c, af, rf )   ConnTraceFlagsModifyEx((c), (af), (rf), __FILE__, __LINE__)

#define ConnTracePrintF( conn, format, ... ) ConnTracePrintFEx( ( conn ), __FILE__, __LINE__, "consumer: " format, ##__VA_ARGS__ )
EXPORT int ConnTracePrintFEx( Connection *conn, const char *file, const int line, char *format, ...) XplFormatString(4,5);
#define ConnTracePrintV( conn, format, args ) ConnTracePrintVEx( ( conn ), b"consumer: " format, args, __FILE__, __LINE__)
EXPORT int 	ConnTracePrintVEx(Connection *conn, const char *format, va_list arguments, const char *file, const int line);




#ifdef __cplusplus
}
#endif

#endif /* _CONNIO_H */
