/* Uncomment 1 or more lines below to enable various types of debug messages */
//#define LOG_SOCK_VALUE				1
//#define LOG_SOCK_EVENT				1
//#define LOG_SOCK_STATE_CHANGE			1


void socketLogValueEx( SOCK *socket, char *format, int val, const char *file, const int line );
#ifdef LOG_SOCK_VALUE
#define socketLogValue( socket, format, val )						socketLogValueEx( ( socket ), ( format ), ( val ), __FILE__, __LINE__ )
#else
#define socketLogValue( socket, format, val )
#endif

void socketLogEventEx( SOCK *socket, SocketEvent *se, void *cbArg, char *event, char *state, const char *file, const int line );
#ifdef LOG_SOCK_EVENT
#define socketLogEvent( socket, se, cbArg, event, state )			socketLogEventEx( ( conn ), ( se ), ( cbArg ), ( event ), ( state ), __FILE__, __LINE__ )
#else
#define socketLogEvent( socket, se, cbArg, event, state )
#endif

void
socketLogStateChangeEx( SOCK *socket, SocketEvent *se, void *cbArg, char *state1, char *state2, const char *file, const int line );
#ifdef LOG_SOCK_STATE_CHANGE
#define socketLogStateChange( socket, se, cbArg, state1, state2 )	socketLogStateChangeEx( ( conn ), ( se ), ( cbArg ), ( state1 ), ( state2 ), __FILE__, __LINE__ )
#else
#define socketLogStateChange( socket, se, cbArg, state1, state2 )
#endif
