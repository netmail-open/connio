#include "conniop.h"


/* Conn Leak Report Code */

typedef struct _Consumer {
	struct _Consumer *next;
	CodeSource source;
	unsigned long count;
} Consumer;

#define MAX_64_BIT_DECIMAL_DIGITS 20
#define OUTSTANDING_FORMAT_1 "Connio Consumer "
#define OUTSTANDING_FORMAT_2 "%s"
#define OUTSTANDING_FORMAT_3a " has not released the following connections in the "
#define OUTSTANDING_FORMAT_3b " has not released the following children in the "
#define OUTSTANDING_FORMAT_4 "%d"
#define OUTSTANDING_FORMAT_5 " seconds since all connections were closed:\n"

#define CONSUMER_LINE_1 "%20lu"
#define CONSUMER_LINE_2 " "
#define CONSUMER_LINE_3 "%s"
#define CONSUMER_LINE_4 ":"
#define CONSUMER_LINE_5 "%d"
#define CONSUMER_LINE_6 "\n"

static XplBool writeString( char **ptr, char *endPtr, char *string )
{
	unsigned long stringLen;
	int bufferLen;

	stringLen = strlen( string );
	bufferLen = endPtr - *ptr;
	if( ( bufferLen > 0) && ( bufferLen > stringLen ) ) {
		memcpy( *ptr, string, stringLen );
		*ptr += stringLen;
		**ptr = '\0';
		return TRUE;
	} else {
		return FALSE;
	}
}

void debugUnreleasedConns( ConnHandlePriv *handle, ConnListenerListPriv *cllist, int waited )
{
	char numBuff[ MAX_64_BIT_DECIMAL_DIGITS + 1 ];
	char *msg;
	char *next;
	ConnectionPriv *conn;
	Consumer *consumer;
	Consumer *consumerList;
	unsigned long uniqueCount;
	unsigned long connCount;
	unsigned long msgSize;

	msg = NULL;
	consumerList = NULL;
	uniqueCount = 0;
	connCount = 0;
	msgSize = 0;
	conn = handle->inUse.head;
	while( conn ) {
		if( !cllist || connChild( cllist, conn ) ) {
			consumer = consumerList;
			while( consumer ) {
				if( consumer->source.line = conn->pub.created.line ) {
					if( !strcmp( consumer->source.file, conn->pub.created.file ) ) {
						break;
					}
				}
				consumer = consumer->next;
			}
			if( !consumer ) {
				consumer = MemMallocWait( sizeof( Consumer ) );
				consumer->next = consumerList;
				consumerList = consumer;
				consumer->source.file = conn->pub.created.file;
				consumer->source.line = conn->pub.created.line;
				consumer->count = 0;
				uniqueCount++;
				msgSize += snprintf( numBuff, sizeof( numBuff ), CONSUMER_LINE_1, consumer->count );
				msgSize += strlen( CONSUMER_LINE_2 );
				msgSize += strlen( consumer->source.file );
				msgSize += strlen( CONSUMER_LINE_4 );
				msgSize += snprintf( numBuff, sizeof( numBuff ), CONSUMER_LINE_5, consumer->source.line );
				msgSize += strlen( CONSUMER_LINE_6 );
			}
			consumer->count++;
		}
		conn = conn->list.next;
		connCount++;
	}

	msgSize += strlen( OUTSTANDING_FORMAT_1 );
	msgSize += strlen( handle->identity );
	if( !cllist ) {
		msgSize += strlen( OUTSTANDING_FORMAT_3a );
	} else {
		msgSize += strlen( OUTSTANDING_FORMAT_3b );
	}
	msgSize += snprintf( numBuff, sizeof( numBuff ), OUTSTANDING_FORMAT_4, waited  );
	msgSize += strlen( OUTSTANDING_FORMAT_5 );
	msgSize ++; // terminator
	msg = MemMalloc( msgSize );
	if( msg ) {
		next = msg;
		writeString( &next, msg + msgSize, OUTSTANDING_FORMAT_1 );
		writeString( &next, msg + msgSize, handle->identity );
		if( !cllist ) {
			writeString( &next, msg + msgSize, OUTSTANDING_FORMAT_3a );
		} else {
			writeString( &next, msg + msgSize, OUTSTANDING_FORMAT_3b );
		}
		snprintf( numBuff, sizeof( numBuff ), OUTSTANDING_FORMAT_4, waited  );
		writeString( &next, msg + msgSize, numBuff);
		writeString( &next, msg + msgSize, OUTSTANDING_FORMAT_5 );
		consumer = consumerList;
		while( consumer ) {
			snprintf( numBuff, sizeof( numBuff ), CONSUMER_LINE_1, consumer->count );
			writeString( &next, msg + msgSize, numBuff );
			writeString( &next, msg + msgSize, CONSUMER_LINE_2 );
			writeString( &next, msg + msgSize, (char *)consumer->source.file );
			writeString( &next, msg + msgSize, CONSUMER_LINE_4 );
			snprintf( numBuff, sizeof( numBuff ), CONSUMER_LINE_5, consumer->source.line );
			writeString( &next, msg + msgSize, numBuff );
			writeString( &next, msg + msgSize, CONSUMER_LINE_6 );
			consumer = consumer->next;
		}
		printf( "%s\n", msg );
		MemFree( msg );
	}

	/* free the list */
	consumer = consumerList;
	while( consumer ) {
		consumerList = consumer->next;
		MemFree( consumer );
		consumer = consumerList;
	}

}
