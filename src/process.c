#include <xplip.h>
#include <xplmem.h>

#include <stdio.h>

#ifdef HAVE_SYS_SOCET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "conniop.h"

/* WARNING !!!!!
   The *EventProcess functions (the highest level functions in this source file)
   can not set errno and return because they are running in worker threads.  All
   errors must be captured in connEvent->error.
*/

static int
flushSendBuffer( ConnEvent *ce, SocketEventType *blockedOn )
{
	int count;
	int moved;
	char *buff;
	int err;

	do {
		count = IOBGetUsed( ce->ioBuffer, &buff );
		if( !count ) {
			return( 0 );
		}
		moved = SocketSend( ce->socket, buff, count, blockedOn, &err );
		if( moved > 0 ) {
			IOBMarkFree( ce->ioBuffer, moved );
			if( moved == count ) {
				continue;
			}
		}
		break;
	} while( TRUE );

	return( err );
}

static int
sendFromClientBuffer( ConnEvent *ce, SocketEventType *blockedOn )
{
	int sent;
	int err;

	while( ce->clientBuffer.remaining > 0  ) {
		sent = SocketSend( ce->socket, ce->clientBuffer.read, ce->clientBuffer.remaining, blockedOn, &err );
		if( sent > 0 ) {
			ce->clientBuffer.remaining -= sent;
			ce->clientBuffer.read += sent;
			continue;
		}
		return err;
	}
	return 0;
}

static int
writeFromCB( ConnEvent *ce, SocketEventType *blockedOn )
{
	int bufferSpace;
	int requestSize;
	size_t count;
	char *buff;
	ConnIOCBResponse cbResult;
	int err;

	for(;;)
	{
		/* figure out how much free space is in the buffer */
		if( !(bufferSpace = IOBGetFree( ce->ioBuffer, &buff ) ) )
		{
			/* buffer is full */
			if( ( err = flushSendBuffer( ce, blockedOn  ) ) != 0 )
			{
				return err;
			}
			bufferSpace = IOBGetFree( ce->ioBuffer, &buff );
			DebugAssert( bufferSpace );
		}
		/* figure out how much data to ask for */
		if( ce->args.cb.count ) {
			/* the consumer specified a count */
			if( ce->transferred >= ce->args.cb.count ) {
				/* we are done calling the callback when we have read a total of cb.count bytes. */
				if( ( err = flushSendBuffer( ce, blockedOn  ) ) != 0 ) {
					/* we have still not put everthing on the wire */
					return err;
				}
				/* done and done */
				return( 0 );
			}
			requestSize = min( bufferSpace, ce->args.cb.count - ce->transferred );
		} else {
			requestSize = bufferSpace;
		}

		count = requestSize;
		cbResult = ce->args.cb.func( ce->args.cb.arg, buff, &count );
		err = errno;
		errno = 0;
		if( count > 0 ) {
			if( count > requestSize ) {
				return( ECONNIOCBLONG );
			}

			TraceApiEvent( ce->socket, TRACE_EVENT_WRITE, buff, count, ce->source.file, ce->source.line );
			IOBMarkUsed( ce->ioBuffer, count );
			ce->transferred += count;

			if( ce->args.cb.count && ( ce->transferred >= ce->args.cb.count ) ) {
				/* we are done calling the callback when we have read a total of cb.count bytes. */
				if( ( err = flushSendBuffer( ce, blockedOn  ) ) != 0 ) {
					/* we have still not put everthing on the wire */
					return err;
				}
				/* done and done */
				return( 0 );
			}
		}

		switch( cbResult ) {
			case CONN_IOCBR_CONTINUE:
			{
				DebugAssert( count );
				continue;
			}
			case CONN_IOCBR_NEED_MORE_BUFFER:
			{
				if( IOBIncreaseFree( ce->ioBuffer ) ) {
					/* we made more room by shuffling data in the buffer */
					break;
				}

				if( ( err = flushSendBuffer( ce, blockedOn  ) ) != 0 )
				{
					return err;
				}
				break;
			}
			case CONN_IOCBR_DONE:
			{
				if( !ce->args.cb.count ) {
					/* the callback says it is done

					   The length passed to Conn(A)WriteFromCB() gets stored in ce->args.cb.count.
					   When this length is 0, the callback is called repeatedly until the callback
					   returns an error or CONN_IOCBR_DONE.  Once CONN_IOCBR_DONE is returned,
					   ce->args.cb.count is set to ce->transferred to prevent the callback from
					   getting called again.  Otherwise, it would get called too many times if
					   flushSendBuffer() blocked trying to drain the buffer.
					*/
					ce->args.cb.count = ce->transferred;
					if( ( err = flushSendBuffer( ce, blockedOn  ) ) != 0 ) {
						/* we have still not put everthing on the wire */
						return err;
					}
					/* done and done */
					return( 0 );
				}
				/*
				  ConnWriteCountFromCB defined a count but the callback function
				  returned a 'DONE' before count was reached
				*/
				return( ECONNIOCBSHORT );
			}
			default:
			case CONN_IOCBR_ERROR:
			{
				/*
				   when the callback returns CONN_IOCBR_ERROR,
				   errno will be captured and returned to the consumer.

				*/
				if( err == EWOULDBLOCK ) {
					/* callback functions are not allowed to set EWOULDBLOCK */
					err = ECONNIOMODE;
				}
				if( err ) {
					return( err );
				}
//				DebugAssert( 0 );
				/* the callback did not set errno or returned an unrecognized value */
				return( ECONNIOBUG );
			}
			case CONN_IOCBR_NEED_MORE_DATA:
			{
				return( ECONNIOMODE );
			}
		}
	}
	return ECONNIOBUG;
}

static int
drainWire( ConnEvent *ce, SocketEventType *blockedOn )
{
	int count;
	int received;
	int moved;
	char *buff;
	int err;

	/* this event is comming out of the async engine or finishing the synchronous disconnect */
	count = IOBGetFree( ce->conn->receiveBuffer, &buff );
	DebugAssert( *( ce->conn->receiveBuffer->end ) == '\0' );  // check for overruns
	moved = 0;
	do {
		received = SocketReceive( ce->socket, buff, count, NULL, blockedOn, &err );
		if( received > 0 ) {
			LogDataDropped( ce->conn, received, "read from the wire and discarded." );
			moved += received;
		} else {
			break;
		}
	} while( TRUE );

	DebugAssert( *( ce->conn->receiveBuffer->end ) == '\0' );  // check for overruns

	if( err == ECONNCLOSEDBYPEER ) {
		err = 0;
		*blockedOn = SOCK_EVENT_TYPE_NONE;
	} else if ( err == EWOULDBLOCK ) {
		*blockedOn = SOCK_EVENT_TYPE_READABLE;
	} else {
		*blockedOn = SOCK_EVENT_TYPE_NONE;
	}
	return( err );
}

static int
drainReceiveBuffer( ConnEvent *ce, char **ptr )
{
	int count = 0;

	if( ce->conn->receiveBuffer ) {
		if( IOBGetUsed( ce->conn->receiveBuffer, NULL ) > 0 ) {
			IOBDefrag( ce->conn->receiveBuffer );
			count = IOBGetUsed( ce->conn->receiveBuffer, ptr );
			IOBMarkFree( ce->conn->receiveBuffer, count );
			LogDataDropped( ce->conn, count, "read from the wire but not read by the connio consumer" );
		}
	}
	return count;
}

static int
fillRecvBuffer( ConnEvent *ce, SocketEventType *blockedOn )
{
	int err;
	int receiveCount;
	char *receiveStart;
	int ioBufferSpace;

	ioBufferSpace = IOBGetFree( ce->ioBuffer, &receiveStart );
	if( ioBufferSpace ) {
		/* we have room in the buffer, try to get data from the wire, without blocking */
		DebugAssert( *( ce->ioBuffer->end ) == '\0' );  // check for overruns
		receiveCount = SocketReceive( ce->socket, receiveStart, ioBufferSpace, NULL, blockedOn, &err );
		DebugAssert( *( ce->conn->receiveBuffer->end ) == '\0' );  // check for overruns
		if( 0 < receiveCount ) {
			/* we got something */
			IOBMarkUsed( ce->ioBuffer, receiveCount );
			return 0;
		}
		return err;
	}
	/* the conn buffer is full!!! */
	return ENOBUFS;
}

static void
TerminateClientBuffer( ConnEvent *ce )
{
	*( ce->clientBuffer.write ) = '\0';
}

static void
CopyBufferedDataToClient( ConnEvent *ce, char *buff, int count )
{
	ClientBuffer *cbuf = ( ClientBuffer * )( &( ce->clientBuffer ) );

	if( count ) {
		memcpy( cbuf->write, buff, count );

		cbuf->remaining		-= count;
		cbuf->write			+= count;
		ce->transferred 	+= count;
	}
}

static int
moveFinalConditionalDataToClient( ConnEvent *ce, char *data, int dataCount, int patternLen )
{
	int err;
	int replacementPatternLen;
	int terminatorLen;
	int extraLen;

	err = 0;
	if( ce->dataMode & ( CONN_READ_COPY ) ) {
		/* the client wants a copy - make sure there is room */
		if( ce->dataMode & ( CONN_READ_TERMINATE ) ) {
			terminatorLen = 1;
		} else {
			terminatorLen = 0;
		}
		if( ce->dataMode & CONN_READ_REPLACE_PATTERN ) {
			extraLen = 0;
			replacementPatternLen = strlen( ce->args.buffer.pattern );
		} else if( ce->dataMode & CONN_READ_INCLUDE_PATTERN ) {
			extraLen = patternLen;
			replacementPatternLen = 0;
		} else {
			extraLen = 0;
			replacementPatternLen = 0;
		}
		if( ( dataCount + extraLen + replacementPatternLen + terminatorLen ) > ce->clientBuffer.remaining ) {
			err = EMSGSIZE;
			dataCount = min( dataCount, ce->clientBuffer.remaining );
			extraLen = 0;
			patternLen = 0;
			replacementPatternLen = 0;
			terminatorLen = 0;
		}
		/* copy response to the client buffer */
		CopyBufferedDataToClient( ce, data, dataCount + extraLen );
		if( replacementPatternLen ) {
			CopyBufferedDataToClient( ce, ce->args.buffer.pattern, replacementPatternLen );
		}
		if( terminatorLen ) {
			TerminateClientBuffer( ce );
		}
	} else if( ( ce->dataMode & ( CONN_READ_DELETE ) ) == 0 ) {
		/* ConnATest case

		nothing was actually transferred, but transferred is set
		to what would have been transferred
		*/
		ce->transferred = dataCount;
	}
	if( ce->dataMode & ( CONN_READ_DELETE ) ) {
		IOBMarkFree( ce->ioBuffer, dataCount + patternLen );
	}
	return err;
}

static int
movePartialConditionalDataToClient( ConnEvent *ce, char *moveableData, int moveableCount )
{
	int err;

	err = ECONNMOREDATA;
	if( ( ce->dataMode & ( CONN_READ_DELETE ) ) && moveableCount ) {
		/* we can make room in the buffer by
		   passing some data on to the client */
		if( ce->dataMode & ( CONN_READ_COPY ) ) {
			if( ce->clientBuffer.remaining < moveableCount ) {
				/* the client buffer does not have enough room */
				err = EMSGSIZE;
				moveableCount = ce->clientBuffer.remaining;
			}
			CopyBufferedDataToClient( ce, moveableData, moveableCount );
		}
		IOBMarkFree( ce->ioBuffer, moveableCount );
	}
	return err;
}

/*
   if the condition is satisfied TRUE will be returned;
   'data' points to an octet string that can be processed
   'count' is the number of bytes that can be processed.
   count may be non-zero even if the condition has not been satified.
   This means that the first 'count' bytes are not needed to
   determine if the condition is satisified and can be moved out of the
   buffer.
*/
static XplBool
EventConditionSatisfied( ConnEvent *ce, char **data, int *count, int *patternLen )
{
	switch( ce->type ) {
		case CONN_EVENT_READ:
		{
			*patternLen = 0;
			*count = IOBGetUsed( ce->ioBuffer, data );
			if( *count > 0 ) {
				if( *count >= ce->clientBuffer.remaining ) {
					*count = ce->clientBuffer.remaining;
				}
				return( TRUE );
			}
			return( FALSE );
		}
		case CONN_EVENT_READ_COUNT:
		{
			*patternLen = 0;
			*count = IOBGetUsed( ce->ioBuffer, data );
			if( *count >= ce->clientBuffer.remaining ) {
				*count = ce->clientBuffer.remaining;
				return( TRUE );
			}
			return( FALSE );
		}
		case CONN_EVENT_READ_UNTIL_PATTERN:
		{
			char *match;

			if( strcmp( ce->args.buffer.pattern, SPECIAL_PATTERN_CRLF ) ) {
				*count = IOBSearch( ce->ioBuffer, ce->args.buffer.pattern, SEARCH_IGNORE_CR | SEARCH_REPLACE_NULL, data, &match, patternLen );
			} else {
				*count = IOBSearch( ce->ioBuffer, NULL, SEARCH_ANY_CR | SEARCH_REPLACE_NULL, data, &match, patternLen );
			}
			if( match ) {
				return( TRUE );
			}
			return( FALSE );
		}

		case CONN_EVENT_READ_TO_CB:
		case CONN_EVENT_READ_TO_CONN:
		{
			DebugPrintf( "CONNIO (%s:%d): readEventProccess() was called with a non-buffered read event\n", __FILE__, __LINE__ );
			return( FALSE );
		}

		case CONN_EVENT_ACCEPT:
		case CONN_EVENT_CONNECT:
		case CONN_EVENT_DISCONNECT:
		case CONN_EVENT_SSL_ACCEPT:
		case CONN_EVENT_TLS_ACCEPT:
		case CONN_EVENT_SSL_CONNECT:
		case CONN_EVENT_TLS_CONNECT:
		case CONN_EVENT_SSL_DISCONNECT:
		case CONN_EVENT_WRITE_FROM_CB:
	    case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
		case CONN_EVENT_FLUSH:
		{
			DebugPrintf( "CONNIO (%s:%d): readEventProccess() was called with a non-read event\n", __FILE__, __LINE__ );
			RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( 0 );
			return( FALSE );
		}
	}

	return( FALSE );
}

static int
SatisfyEvent( ConnEvent *ce )
{
	int err;
	char *moveableData = NULL;
	int moveableCount = 0;
	int patternLen;

	if( EventConditionSatisfied( ce, &moveableData, &moveableCount, &patternLen ) ) {
		/* event conditions are satisfied with data in the buffer
		   and the rest of the data can be moved to the client */
		err = moveFinalConditionalDataToClient( ce, moveableData, moveableCount, patternLen );
	} else {
		/* more data is needed to satify the condition */
		err = movePartialConditionalDataToClient( ce, moveableData, moveableCount );
	}
	return err;
}

static int
conditionalReadProcess( ConnEvent *ce, SocketEventType *blockedOn )
{
	int err;

	for( ; ; ) {
		err = SatisfyEvent( ce );
		if( err != ECONNMOREDATA ) {
			return err;
		}
		/* we need more data */
		err = fillRecvBuffer( ce, blockedOn );
		if( err ) {
			if( ( err == ENOBUFS ) && ( 0 == ( ( CONN_READ_DELETE | CONN_READ_COPY ) & ce->dataMode ) ) ) {
				/* ConnATest case

				nothing was actually transferred, but transferred is set
				to what is available in the buffer
				*/
				ce->transferred = IOBGetUsed( ce->ioBuffer, NULL );
				err = 0;
			}
			RodneyNeedsToKnowYouSawThisBecauseConnioShouldNotExecuteThisCode( err != ENOBUFS );
			return err;
		}
		/* we got more data, try again */
	}
}

INLINE static int
readToCallbackProcess( ConnEvent *ce, SocketEventType *blockedOn )
{
	int err;
	int used;
	size_t count;
	int needed;
	char *buff;
	ConnIOCBResponse cbResult;

	for( ; ; ) {
		/* figure out how much data is in the buffer */
		used = IOBGetUsed( ce->ioBuffer, &buff );
		if( used > 0 ) {
			/* we have data to give the consumer */
			if( ce->args.cb.count ) {
				/* ConnReadCountToCB defined a count */
				if( ce->transferred >= ce->args.cb.count ) {
					/* we are done! we have read a total of cb.count bytes to the callback. */
					return( 0 );
				}
				/* make sure we do not pass more than was asked for */
				needed = ce->args.cb.count - ce->transferred;
				if( needed < used ) {
					/* only give what the consumer asked for */
					used = needed;
				}
			}

			count = used;
			cbResult = ce->args.cb.func( ce->args.cb.arg, buff, &count );
			if( count > 0 ) {
				TraceApiEvent( ce->socket, TRACE_EVENT_READ, buff, count, ce->source.file, ce->source.line );
				/* consumer took at least some of the data */
				if( count > used ) {
					return ECONNIOCBLONG;
				}
				IOBMarkFree( ce->ioBuffer, count );
				ce->transferred += count;

				if( ce->args.cb.count && ( ce->transferred >= ce->args.cb.count ) ) {
					/* ConnReadCountToCB defined a count and count has been reached */
					return 0;
				}
			}

			switch( cbResult ) {
				case CONN_IOCBR_CONTINUE:
				{
					continue;
				}
				case CONN_IOCBR_DONE:
				{
					if( !ce->args.cb.count ) {
						/* ConnReadCountToCB did not define a count so its callback function can return a 'DONE' any time if wants to */
						return 0;
					} else {
						/* ConnReadCountToCB defined a count but the callback function returned a 'DONE' before count was reached */
						return ECONNIOCBSHORT;
					}
				}
				case CONN_IOCBR_NEED_MORE_DATA:
				{
					if( count == used ) {
						/* the entire block was consumed, go on normally */
						continue;
					}
					/* buffer might be fragmented; try to join them */
					if( IOBDefrag( ce->ioBuffer ) ) {
						/*
						  after joining the data fragments, we have more data than the
						  consumer left behind.  Let the consumer look at that.
						*/
						continue;
					}
					/* all we have is data the consumer left behind. */
					if( !IOBGetFree( ce->ioBuffer, &buff ) ) {
						/*
						  the buffer is full, there is nothing more we can do
						  for the consumer
						*/
						return ENOBUFS;
					}
					/* there is space in the buffer - read more from the wire. */
					break;
				}
				case CONN_IOCBR_NEED_MORE_BUFFER:
				{
					return ECONNIOMODE;
				}
				case CONN_IOCBR_ERROR:
				{
					DebugPrintf("CONNIO[%s:%d]: callback returned an error to ReadToCBEventProcess. Returning EIO\n", __FILE__, __LINE__ );
					return EIO;
				}
			}
		}

		/* we need more data */
		err = fillRecvBuffer( ce, blockedOn );
		if( err ) {
			return err;
		}
	}
}

static int
readToConnProcess( ConnEvent *ce, SocketEventType *blockedOn )
{
	int err;
	int bufferCount;
	int stillNeeded;
	int sendCount;
	int sent;
	char *buff;

	for( ; ; ) {
		if( ce->transferred >= ce->args.conn.count ) {
			return( 0 );
		}
		stillNeeded = ce->args.conn.count - ce->transferred;
		bufferCount = IOBGetUsed( ce->ioBuffer, &buff );
		err = 0;
		if( bufferCount < stillNeeded ) {
			/* buffer might be fragmented; try to join them */
			if( IOBDefrag( ce->ioBuffer ) ) {
				/* after joining the data fragments, we have more data */
				bufferCount = IOBGetUsed( ce->ioBuffer, &buff );
			}
			while( bufferCount < stillNeeded ) {
				err = fillRecvBuffer( ce, blockedOn );
				if( err ) {
					if( err == ENOBUFS ) {
						break;
					}
					return err;
				}
				bufferCount = IOBGetUsed( ce->ioBuffer, &buff );
			}
		}
		/* the buffer is full or there is enough data to finish */
		sendCount = min( bufferCount, stillNeeded );
		sent = connWriteEx( ce->args.conn.handle, buff, sendCount, CONN_TIMEOUT_DEFAULT, ce->source.file, ce->source.line );
		if( sent ) {
			TraceApiEvent( ce->socket, TRACE_EVENT_READ, buff, sent, ce->source.file, ce->source.line );
			IOBMarkFree( ce->ioBuffer, sent );
			ce->transferred += sent;
		}
		if( errno ) {
			return errno;
		}
	}
}

static int
socketEventProcess( ConnEvent *ce, SocketEventType *blockedOn )
{
	int err;

	switch( ce->type ) {
		case CONN_EVENT_WRITE_FROM_CB:
		{
			return( writeFromCB( ce, blockedOn ) );
		}
		case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
		{
			return( sendFromClientBuffer( ce, blockedOn ) );
		}
		case CONN_EVENT_FLUSH:
		{
			return( flushSendBuffer( ce, blockedOn ) );
		}
		case CONN_EVENT_READ:
		case CONN_EVENT_READ_COUNT:
		case CONN_EVENT_READ_UNTIL_PATTERN:
		{
			if( ce->status == CONN_EVENT_IN_PROGRESS ) {
				/*
				  we only get here if we previously blocked trying to read
				  data into the recv buffer and now the wire is readable again.
				*/
				err = fillRecvBuffer( ce, blockedOn );
				if( err ) {
					if( ( err == ENOBUFS ) && ( 0 == ( ( CONN_READ_DELETE | CONN_READ_COPY ) & ce->dataMode ) ) ) {
						/* ConnATest case
						   Nothing actually gets transferred, but transferred is set
						   to what is available in the buffer
						*/
						ce->transferred = IOBGetUsed( ce->ioBuffer, NULL );
						return( 0 );
					}
					return( err );
				}
				/* we got more data - drop through and process it */
			}
			return( conditionalReadProcess( ce, blockedOn ) );
		}
		case CONN_EVENT_READ_TO_CONN:
		{
			if( ce->status == CONN_EVENT_IN_PROGRESS ) {
				/*
				  we only get here if we previously blocked trying to read
				  data into the recv buffer and now the wire is readable again.
				*/
				err = fillRecvBuffer( ce, blockedOn );
				if( err ) {
					return( err );
				}
				/* we got more data - drop through and process it */
			}
			return( readToConnProcess( ce, blockedOn ) );
		}
		case CONN_EVENT_READ_TO_CB:
		{
			if( ce->status == CONN_EVENT_IN_PROGRESS ) {
				/*
				  we only get here if we previously blocked trying to read
				  data into the recv buffer and now the wire is readable again.
				*/
				err = fillRecvBuffer( ce, blockedOn );
				if( err ) {
					return( err );
				}
				/* we got more data - drop through and process it */
			}
			return( readToCallbackProcess( ce, blockedOn ) );
		}
		case CONN_EVENT_CONNECT:
		{
			if( ce->status != CONN_EVENT_IN_PROGRESS ) {
				if( CONN_EVENT_MODE_ASYNCHRONOUS == ce->mode ) {
					return( SocketAsyncConnectStart( ce->socket, ce->args.addr.dst, ce->args.addr.dstSize, ce->args.addr.src, ce->args.addr.srcSize, blockedOn ) );
				} else {
					return( SocketSyncConnectStart( ce->socket, ce->args.addr.dst, ce->args.addr.dstSize, ce->args.addr.src, ce->args.addr.srcSize, blockedOn ) );
				}
			}
			/* this event is comming out of the async engine */
			return( 0 );
		}
		case CONN_EVENT_DISCONNECT:
		{
			if( ce->status != CONN_EVENT_IN_PROGRESS ) {
				/* start disconnect */
				char *ptr = NULL;
				int count;

				count = drainReceiveBuffer( ce, &ptr );

				if( !SocketValid( ce->socket ) ) {
					/* no socket to disconnect */
					return( 0 );
				}
				if( CONN_EVENT_MODE_ASYNCHRONOUS == ce->mode ) {
					return( SocketAsyncDisconnectStart( ce->socket, blockedOn ) );
				} else {
					return( SocketSyncDisconnectStart( ce->socket, blockedOn, &( ce->timeOut ) ) );
				}
			}
			/* this event is comming out of the async engine or finishing the synchronous disconnect */
			err = drainWire( ce, blockedOn );
			if( err == ECONNCLOSEDBYPEER || !err ) {
				return( 0 );
			}
			return( err );
		}

		case CONN_EVENT_SSL_ACCEPT:
		{
			int err;

			if( 0 == IOBGetUsed( ce->conn->receiveBuffer, NULL ) ) {
				err = SocketSSLAccept( ce->socket, ce->conn->connHandle->pub.sslv23.server, blockedOn );
			} else {
				DebugPrintf( "SSLAccept: Someone tried to do a read on this connection before SSL was established\n" );
				err = EOVERFLOW;
			}
			return err;
		}
		case CONN_EVENT_TLS_ACCEPT:
		{
			int err;

			if( 0 == IOBGetUsed( ce->conn->receiveBuffer, NULL ) ) {
				err = SocketSSLAccept( ce->socket, ce->conn->connHandle->pub.tls.server, blockedOn );
			} else {
				DebugPrintf( "StartTLS 'man in the middle' attack detected and prevented.\n" );
				err = EOVERFLOW;
			}
			return err;
		}
		case CONN_EVENT_SSL_CONNECT:
		{
			int err;

			if( 0 == IOBGetUsed( ce->conn->receiveBuffer, NULL ) ) {
				err = SocketSSLConnect( ce->socket, ce->conn->connHandle->pub.sslv23.client, blockedOn );
			} else {
				DebugPrintf( "SSLConnect: Someone tried to do a read on this connection before SSL was established\n" );
				err = EOVERFLOW;
			}
			return err;
		}
		case CONN_EVENT_TLS_CONNECT:
		{
			int err;

			if( 0 == IOBGetUsed( ce->conn->receiveBuffer, NULL ) ) {
			  err = SocketSSLConnect( ce->socket, ce->conn->connHandle->pub.tls.client, blockedOn );
			} else {
				DebugPrintf( "StartTLS 'man in the middle' attack detected and prevented.\n" );
				err = EOVERFLOW;
			}
			return err;
		}
		case CONN_EVENT_SSL_DISCONNECT:
		{
			return( SocketSSLDisconnect( ce->socket, blockedOn ) );
		}
		case CONN_EVENT_ACCEPT:
		{
			DebugPrintf( "CONNIO (%s:%d): EventProccess() was called for an ACCEPT event\n", __FILE__, __LINE__ );
			break;
		}
	}

	return( ECONNIOBUG );
}

static int
socketEventComplete( ConnEvent *ce )
{
	switch( ce->type ) {
	    case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
			if( ce->args.buffer.start ) {
				ce->args.buffer.freeCB( ce->args.buffer.start );
				ce->args.buffer.start = NULL;
			}
			return 0;
		case CONN_EVENT_WRITE_FROM_CB:
		case CONN_EVENT_FLUSH:
		{
			return( 0 );
		}
		case CONN_EVENT_READ:
		case CONN_EVENT_READ_COUNT:
		case CONN_EVENT_READ_UNTIL_PATTERN:
		{
			TraceApiEvent( ce->socket, TRACE_EVENT_READ, ce->args.buffer.start, ce->transferred, ce->source.file, ce->source.line );
			return( 0 );
		}
		case CONN_EVENT_READ_TO_CONN:
		case CONN_EVENT_READ_TO_CB:
		{
			return( 0 );
		}
		case CONN_EVENT_CONNECT:
		{
			SocketConnectFinish( ce->socket );
			return( 0 );
		}
		case CONN_EVENT_DISCONNECT:
		{
			SocketDisconnectFinish( ce->socket );
			return( 0 );
		}
		case CONN_EVENT_SSL_ACCEPT:
		case CONN_EVENT_TLS_ACCEPT:
		{
			SocketSSLAcceptFinish( ce->socket );
			return( 0 );
		}
		case CONN_EVENT_SSL_CONNECT:
		case CONN_EVENT_TLS_CONNECT:
		{
			SocketSSLConnectFinish( ce->socket );
			return( 0 );
		}
		case CONN_EVENT_SSL_DISCONNECT:
		{
			SocketSSLDisconnectFinish( ce->socket );
			return( 0 );
		}
		case CONN_EVENT_ACCEPT:
		{
			DebugPrintf( "CONNIO (%s:%d): EventProccess() was called for an unsupported event\n", __FILE__, __LINE__ );
			break;
		}
	}

	return( ECONNIOBUG );
}

static int
socketEventAbort( ConnEvent *ce )
{
	switch( ce->type ) {
		case CONN_EVENT_WRITE_FROM_CLIENT_BUFFER:
			if( ce->args.buffer.start ) {
				ce->args.buffer.freeCB( ce->args.buffer.start );
				ce->args.buffer.start = NULL;
			}
			return 0;
		case CONN_EVENT_WRITE_FROM_CB:
		case CONN_EVENT_FLUSH:
		{
			return( 0 );
		}
		case CONN_EVENT_READ:
		case CONN_EVENT_READ_COUNT:
		case CONN_EVENT_READ_UNTIL_PATTERN:
		{
			TraceApiEvent( ce->socket, TRACE_EVENT_READ, ce->args.buffer.start, ce->transferred, ce->source.file, ce->source.line );
			return( 0 );
		}
		case CONN_EVENT_READ_TO_CONN:
		case CONN_EVENT_READ_TO_CB:
		{
			return( 0 );
		}
		case CONN_EVENT_CONNECT:
		case CONN_EVENT_DISCONNECT:
		{
			SocketConnectAbort( ce->socket, ce->error );
			return( 0 );
		}
		case CONN_EVENT_SSL_ACCEPT:
		case CONN_EVENT_TLS_ACCEPT:
		case CONN_EVENT_SSL_CONNECT:
		case CONN_EVENT_TLS_CONNECT:
		case CONN_EVENT_SSL_DISCONNECT:
		{
			SocketSSLAbort( ce->socket, ce->error );
			return( 0 );
		}
		case CONN_EVENT_ACCEPT:
		{
			DebugPrintf( "CONNIO (%s:%d): EventProccess() was called for an ACCEPT event\n", __FILE__, __LINE__ );
			break;
		}
	}

	return( ECONNIOBUG );
}

INLINE static ConnEventStatus
EventProcess( ConnEvent *ce )
{
	int err;
	SocketEventType blockedOn;

	DebugAssert( ce->type && ( ce->type != CONN_EVENT_ACCEPT ) );

	blockedOn = 0;
	if( ce->error == 0 ) {
		for( ; ; ) {
			err = socketEventProcess( ce, &blockedOn );
			if( !err ) {
				socketEventComplete( ce );
				return( CONN_EVENT_COMPLETE );
			}

			if( ( err != EWOULDBLOCK ) || ( ce->mode == CONN_EVENT_MODE_SYNCHRONOUS_NON_BLOCKING ) ) {
				;
			} else if( ce->mode == CONN_EVENT_MODE_SYNCHRONOUS ) {
				err = SocketWait( ce->socket, blockedOn, &( ce->timeOut ) );
				if(! err ) {
					ce->status = CONN_EVENT_IN_PROGRESS;
					continue;
				}
				ce->error = err;
			} else if( ce->mode == CONN_EVENT_MODE_ASYNCHRONOUS ) {
				if( ce->conn->async.transaction.problem ) {
					err = ECANCELED;
				} else {
					ce->status = CONN_EVENT_IN_PROGRESS;
					err = SocketEventWaitEx( ce->socket, blockedOn, &ce->timeOut, ce->args.addr.dst, ce->args.addr.dstSize, AsyncEventCallback, ce->conn->connHandle->threadEngine.internal, NULL, ce, NULL, 0, NULL, __FILE__, __LINE__ );
					if( !err ) {
						LogSocketEventStarted( ce->conn, ce, blockedOn );
						return( CONN_EVENT_IN_PROGRESS );
					}
					LogSocketEventFailedToStart( ce->conn, ce, blockedOn );
					DebugPrintf("CONNIO[%s:%d]: Failed to submit an event. Returning error %d\n", __FILE__, __LINE__, err );
					ce->status = CONN_EVENT_FAILED;
				}
			} else {
				err = ECONNIOBUG;
			}
			break;
		}
		ce->error = err;
	}
	socketEventAbort( ce );
	switch ( ce->error ) {
		case ETIMEDOUT:
			return( CONN_EVENT_TIMED_OUT );
		case EMSGSIZE:
			if( ce->dataMode & CONN_READ_PARTIAL_OK ) {
				return( CONN_EVENT_PARTIAL );
			}
			/* fall through */
		default:
			return( CONN_EVENT_FAILED );
	}
}

ConnEventStatus
ConnEventProcess( ConnEvent *ce )
{
	ConnEventStatus status;

	status = EventProcess( ce );
	if( status != CONN_EVENT_IN_PROGRESS ) {
		ConnEventFreeResources( ce );
		ce->status = status;
		if( ( status != CONN_EVENT_PARTIAL ) && ( status != CONN_EVENT_COMPLETE ) ) {
			/* EventProcess() did not end well */
			if( ce->error ) {
				if( ce->mode != CONN_EVENT_MODE_ASYNCHRONOUS ) {
					errno = ce->error;
				} else {
					/* setting errno would be bad for async events because errno is thread specific */
				}
			} else {
				//DebugAssert( 0 ); /* there is a connio bug because you should not be able to get here with a 0 error */
				errno = ECONNIOBUG;
			}
		}
	}
	return status;
}
