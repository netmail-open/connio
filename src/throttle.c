#include <xplmem.h>

#include <stdio.h>

#include "sockiop.h"
#include "conniop.h"

static XplBool listenerThresholdExceeded( SocketEvent *se, XplBool update, unsigned long *threadCount )
{
	unsigned long count;

	if( update ) {
		count = AThreadCountUpdate();
	} else {
		count = AThreadCountRead();
	}
	if( threadCount ) {
		*threadCount = count;
	}
	return ( count > se->cs->listener->throttlingThreshold );
}

/*
   ListenerShouldThrottle() can be called without the lock ( it only reads)
   when you need to quickly assess the engine status.
*/
XplBool listenerShouldThrottle( SocketEvent *se )
{
	if( !se->cs->listener ) {
		/* this is not currently an async listener */
		return FALSE;
	}

	if( SeGetFlags( se ) & FSE_LISTENER_SUSPENDED ) {
		return TRUE;
	}

	if( !se->cs->listener->throttlingThreshold ||
		!listenerThresholdExceeded( se, FALSE, NULL ) ) {
		return FALSE;
	}

	return TRUE;
}

/* listenerStartThrottling does listenerShouldThrottle checks while holding the lock
   if all the condition exist, it can set 'throttled' */
XplBool listenerStartThrottling( SocketEvent *se )
{
	unsigned long count = 0;
	unsigned long threshold;

	if( !( SeGetFlags( se ) & FSE_LISTENER_SUSPENDED ) ||
		!se->cs->listener ||
		!se->cs->listener->throttlingThreshold ||
		!listenerThresholdExceeded( se, TRUE, &count ) ) {
		return FALSE;
	}

	if( se->cs->listener ) {
		threshold = se->cs->listener->throttlingThreshold;
	} else {
		threshold = 0;
	}

	if( SeGetFlags( se ) & FSE_LISTENER_SUSPENDED ) {
		count = AThreadCountUpdate();
	}

	if( se->cs->listener->eventCallback ) {
		se->cs->listener->eventCallback( LISTENER_STOPPED, se->cs->listener->throttlingThreshold, count, se, se->cs, se->cs->number );
		se->cs->listener->eventCallback( THROTTLING_STARTED, se->cs->listener->throttlingThreshold, count, se, se->cs, se->cs->number );
	}
	SeAddFlags( se, FSE_LISTENER_PAUSED );
	return TRUE;
}

XplBool listenerThrottling( SocketEvent *se )
{
	unsigned long flags = SeGetFlags( se );
	return( !( flags & FSE_LISTENER_KILLED ) && ( flags & FSE_LISTENER_PAUSED ) );
}

void listenerSpinWhileThrottling( SocketEvent *se )
{
	unsigned long flags;
	unsigned long threadCount = 0;
	unsigned long loopCount;

	//DebugPrintf( "SOCKIO: pausing listener!\n" );
	/* the system is busy - the listener has stopped accepting connections */
	loopCount = 0;
	for( ; ; ) {
		loopCount++;
		flags = SeGetFlags( se );
		if( ( flags & FSE_LISTENER_KILLED ) ) {
			/* the listener was shutdown - another callback will happen */
			return;
		}

		if( !( flags & FSE_LISTENER_SUSPENDED ) &&
			!listenerThresholdExceeded( se, TRUE, &threadCount ) ) {
			if( se->cs->listener->eventCallback ) {
				se->cs->listener->eventCallback( THROTTLING_STOPPED, se->cs->listener->throttlingThreshold, threadCount, se, se->cs, se->cs->number );
			}
			break;
		}

		if( se->cs->listener->eventCallback && ( 0 == ( loopCount % 16 ) ) ) {
			if( flags & FSE_LISTENER_SUSPENDED ) {
				threadCount = AThreadCountUpdate();
			}
			se->cs->listener->eventCallback( THROTTLING_UPDATE, se->cs->listener->throttlingThreshold, threadCount, se, se->cs, se->cs->number );
		}

		/* sit on the listener a while */
		XplLockRelease( &( se->cs->slock ) );
		XplDelay( 500 );
		XplLockAcquire( &( se->cs->slock ) );
	}

}
