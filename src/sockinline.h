/* Socket Locked Events */

/* Socket Locked Flag Events */

__inline static unsigned long SeGetFlags( SocketEvent *se )
{
	DebugAssert( XplLockValue( &( se->cs->slock ) ) ); // You need to hold the lock to examine the flags
	return se->flags;
}

__inline static void SeSetFlags( SocketEvent *se, unsigned long flags )
{
	DebugAssert( XplLockValue( &( se->cs->slock ) ) ); // You need to hold the lock to examine the flags
	se->flags = flags;
}

#if DEBUG


#define SeAddFlags( SE, FLAGS )             DebugAssert( XplLockValue( ( &(SE)->cs->slock ) ) ); (SE)->flags |= (FLAGS)  // the caller is required to hold the SOCK->slock
#define SeRemoveFlags( SE, FLAGS )          DebugAssert( XplLockValue( ( &(SE)->cs->slock ) ) ); (SE)->flags &= ~(FLAGS) // the caller is required to hold the SOCK->slock

#else

#define SeAddFlags( se, FLAGS )              (se)->flags |= (FLAGS)
#define SeRemoveFlags( se, FLAGS )           (se)->flags &= ~(FLAGS)

#endif

/* Socket Locked Link Events */

__inline static XplBool SeLinkCe( SocketEvent *se, SocketEventType type, SocketEventCB ecb, AThreadEngine *ecbThreadEngine, AcceptEventCB acb, void *cbArg, const char *file, long line )
{
	DebugAssert( XplLockValue( &( se->cs->slock ) ) ); // You need to hold the lock change the link status
	DebugAssert( se->cb.arg == NULL );
	if( !se->cb.arg ) {
		se->cb.arg = cbArg;
		se->type = type;
		se->submitter.file = file;
		se->submitter.line = line;
		se->cb.eventFunc = ecb;
		se->cb.acceptFunc = acb;
		AThreadInit( &( se->cb.thread ), ecbThreadEngine, HandleSocketEvent, se, FALSE );
		se->error = 0;
		return TRUE;
	}
	return FALSE;
}

__inline static void SeUnlinkCe( SocketEvent *se, void *cbArg )
{
	DebugAssert( XplLockValue( &( se->cs->slock ) ) ); // You need to hold the lock change the link status
	DebugAssert( cbArg != NULL && cbArg == se->cb.arg );
	se->cb.arg = NULL;
	se->cb.eventFunc = NULL;
	se->cb.acceptFunc = NULL;
}

/* iob util functions */
__inline static int iobDumpToFile( IOBuffer *iob, FILE *file )
{
	int err;
	if( iob->w > iob->r ) {
		if( 1 != fwrite( iob->r, iob->w - iob->r, 1, file ) ) {
			err = errno;
			errno = 0;
		} else {
			err = 0;
		}
	} else {
		if( ( 1 != fwrite( iob->r, iob->end - iob->r, 1, file ) ) ||
			( 1 != fwrite( iob->buffer, iob->w - iob->buffer, 1, file ) ) ) {
			err = errno;
			errno = 0;
		} else {
			err = 0;
		}
	}
	return err;
}
