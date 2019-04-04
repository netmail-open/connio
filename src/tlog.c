#include<iobuf.h>
#include<xpltimer.h>
#include<xpldir.h>

#ifdef LINUX
#include <sys/syscall.h>
#endif

// temporary
// move thread log and IOB's into memmanager

#define MAX_TLOG_LINE	1024
#define TL_HASH_MASK	0x0000003f
#define TL_HASH_NODES	TL_HASH_MASK + 1

typedef struct TLog
{
	struct TLog	*next;
	int			tid;
	FILE		*fp;
	IOBuffer	*iob;
	XplTimer 	startTimer;
	XplTimer    lastTimer;
	time_t		startTime;
}TLog;

typedef struct
{
	XplLock	lock;
	TLog	*list;
}TLogList;

typedef struct
{
	int			consumers;
	int			running;
	XplAtomic	entryID;
	TLogList	hash[TL_HASH_NODES];
	char		consumer[MAX_TLOG_LINE];
}TLGlobals;

TLGlobals threadLog;

static int TID( void )
{
#ifdef LINUX
	return syscall( SYS_gettid );
#else
	return (int)XplGetThreadID();
#endif
}

static TLog *_TLGet( void )
{
	int		tid, h;
	TLog	*log;

	tid = TID();
	h = tid & TL_HASH_MASK;
	XplLockAcquire( &threadLog.hash[h].lock );
	for(log=threadLog.hash[h].list;log;log=log->next)
	{
		if( log->tid == tid )
		{
			break;
		}
	}
	XplLockRelease( &threadLog.hash[h].lock );
	return log;
}

static TLog *_TLReplace( TLog *log )
{
	int		tid, h;
	TLog	*old, **tp;

	if( log )
	{
		tid = log->tid;
	}
	else
	{
		tid = TID();
	}
	h = tid & TL_HASH_MASK;
	XplLockAcquire( &threadLog.hash[h].lock );
	for(tp=&threadLog.hash[h].list;*tp;tp=&(*tp)->next)
	{
		if( (*tp)->tid == tid )
		{
			old = *tp;
			if( log )
			{
				log->next = old->next;
				*tp = log;
			}
			else
			{
				*tp = old->next;
			}
			XplLockRelease( &threadLog.hash[h].lock );
			return old;
		}
	}
	if( log )
	{
		log->next = NULL;
		*tp = log;
	}
	XplLockRelease( &threadLog.hash[h].lock );
	return NULL;
}

static int _TLFlush( TLog *log, char *data, int size )
{
	int		bytes;
#if FLUSH_ONLY_LINES
	int		remaining, matchLen;
	char	*match, *lastMatch;
#endif
	char	buff[1024];

	if( !log->fp )
	{
		strprintf( buff, sizeof( buff), NULL, "tl-%s/%d.txt", threadLog.consumer, log->tid );
		if( log->fp = fopen( buff, "at" ) )
		{
			struct tm *ltime;

			tzset();
			/*TODO: hans compile warning in VS here*/
			ltime = localtime( &( log->startTime ) );
			strftime( buff, 80, "%a, %d %b %Y %H:%M:%S", ltime );

			fprintf( log->fp, "---=== Log Opened %s  ===---\n", buff );
		}
	}
#if FLUSH_ONLY_LINES
	lastMatch = data;
	remaining = size;
	do
	{
		bytes = MemSearch( lastMatch, remaining, "\n", 1, SEARCH_IGNORE_CR|SEARCH_REPLACE_NULL, &match, &matchLen );
		if( match )
		{
			lastMatch = match + matchLen;
			remaining -= ( bytes + matchLen );
		}
	}while( match );
	bytes = fwrite( data, 1, size - remaining, log->fp );
#else
	bytes = fwrite( data, 1, size, log->fp );
#endif
	fflush( log->fp );
	return bytes;
}

EXPORT int TLogStartup( const char *consumer )
{
	int			l;
	XplDirMatch	*dir;
	XplDirMatch	*ent;
	char		name[1024];

	if( !threadLog.consumers++ )
	{
		strprintf( threadLog.consumer, sizeof( threadLog.consumer ), NULL, "%s", consumer );
		XplSafeInit( threadLog.entryID, 0 );
		for(l=0;l<TL_HASH_NODES;l++)
		{
			XplLockInit( &threadLog.hash[l].lock );
			threadLog.hash[l].list = NULL;
		}
		strprintf( name, sizeof( name ), NULL, "tl-%s", threadLog.consumer );
		XplMakeDir( name );
		strprintf( name, sizeof( name ), NULL, "tl-%s/tl-*.txt", threadLog.consumer );
		if( dir = XplOpenDirMatch( name ) )
		{
			while( ent = XplReadDirMatch( dir ) )
			{
				strprintf( name, sizeof( name ), NULL, "tl-%s/%s", threadLog.consumer, ent->d_name );
				remove( name );
			}
			XplCloseDirMatch( dir );
		}
		threadLog.running = 1;
	}
	return 0;
}

EXPORT int TLogShutdown( void )
{
	return 0;
}

EXPORT int TLogOpen( void )
{
	TLog	*log;

	if( threadLog.running )
	{
		TLogClose();
		if( log = (TLog *)MemMalloc( sizeof( TLog ) ) )
		{
			memset( log, 0, sizeof( TLog ) );
			log->tid = TID();
			XplTimerStart( &( log->startTimer ) );
			log->lastTimer.sec = log->startTimer.sec;
			log->lastTimer.usec = log->startTimer.usec;
			log->startTime = time( NULL );
			if( log->iob = IOBAlloc( 0 ) )
			{
				IOBSetFlushCB( log->iob, (int(*)(void *, char *, int))_TLFlush, log );
				_TLReplace( log );
				return 0;
			}
			MemFree( log );
		}
	}
	return -1;
}

EXPORT int TLogWrite( const char *format, ... )
{
	int				len;
	unsigned long	entryID;
	va_list			args;
	TLog			*log;
	XplTimer 		split;
	XplTimer 		lap;
	char			buffer[MAX_TLOG_LINE];

	if( threadLog.running )
	{
		if( log = _TLGet() )
		{
			entryID = XplSafeIncrement( threadLog.entryID );
#if 0
			sprintf( buffer, "%08lx ", entryID );
#else
			XplTimerSplitAndLap( &( log->startTimer ), &split, &( log->lastTimer ), &lap );
			sprintf( buffer, "%08lx %lu.%03lus %lu.%03lus ", entryID, split.sec, split.usec / 1000, lap.sec, lap.usec / 1000 );
#endif
			IOBWrite( log->iob, buffer, strlen( buffer ) );
			va_start( args, format );
			vstrprintf( buffer, MAX_TLOG_LINE-2, NULL, format, args );
			va_end( args );
			len = strlen( buffer );
			if( buffer[len-1] != '\n' )
			{
				buffer[len] = '\n';
				buffer[len+1] = '\0';
			}
			IOBWrite( log->iob, buffer, strlen( buffer ) );
			return 0;
		}
	}
	return -1;
}

EXPORT int TLogFlush( void )
{
	TLog	*log;

	if( threadLog.running )
	{
		if( log = _TLGet() )
		{
			IOBFlush( log->iob );
//			IOBDefrag( log->iob );
//			IOBIncreaseFree( log->iob );
			return 0;
		}
	}
	return -1;
}

EXPORT int TLogClose( void )
{
	int		bytes;
	char	*data;
	TLog	*log;

	if( threadLog.running )
	{
		if( log = _TLReplace( NULL ) )
		{
			bytes = IOBGetUsed( log->iob, &data );
			_TLFlush( log, data, bytes );
			if( log->fp )
			{
				fprintf( log->fp, "---=== Log Closed ===---\n" );
				fclose( log->fp );
			}
			IOBFree( log->iob );
			MemFree( log );
			return 0;
		}
	}
	return -1;
}

