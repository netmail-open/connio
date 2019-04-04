#include <xplip.h>
#include <xplmem.h>
#include <xplthread.h>

#include <stdio.h>
#include <threadengine.h>

#define LIB_UNINITIALIZED 0xAAA00AAA
#define LIB_INITIALIZED 0x11100111

unsigned long AThreadLibInitialized = LIB_UNINITIALIZED;

typedef struct {
	XplLock lock;
	AThreadEngine *list;
	unsigned long count;
} AThreadGlobals;

AThreadGlobals ATHREAD;


/**************** Begin default thread engine implementation ****************/

void *AThreadDefaultCreateContext( char *identity, unsigned long stacksize )
{
	XplThreadGroup group;

	group = XplThreadGroupCreate( identity );
	if( group && stacksize ) {
		XplThreadGroupStackSize( group, stacksize );
	}
	return group;
}

void AThreadDefaultFreeContext( void *context )
{
	XplThreadGroup group;

	group = ( XplThreadGroup )context;
	if( group ) {
		XplThreadGroupDestroy( &group );
	}
}

static int DefaultThreadFunc( XplThread thread )
{
	AThread *aThread = ( AThread *)thread->context;
	int ret;

	if( !aThread->freeAThread ) {
	    ret = aThread->func( aThread->arg );
	} else {
		AThreadFunction func = aThread->func;
		void *funcArg = aThread->arg;

		MemFree( aThread );
		ret = func( funcArg );
	}
	return ret;
}

void AThreadDefaultThreadInit( AThread *aThread )
{
	XplThreadStart( ( XplThreadGroup )( aThread->engine->context ), DefaultThreadFunc, aThread, NULL );
}

unsigned long AThreadDefaultThreadCount( void *context )
{
	int ret;

	ret = XplThreadCount( ( XplThreadGroup )( context ) );
	if( ret > -1 ) {
		return ( unsigned long )ret;
	}
	return 0;
}

//typedef unsigned long (* AThreadCountFunc )( void *aThreadContext );


/**************** End default thread engine implementation ****************/


void AThreadBegin( AThread *aThread )
{
	DebugAssert( aThread );
	DebugAssert( aThread->engine );
	DebugAssert( ( aThread->engine->thread.init ) );
	aThread->engine->thread.init( aThread );
}

void AThreadInit( AThread *athread, AThreadEngine *threadEngine, AThreadFunction threadFunc, void *threadArg, XplBool freeAThread )
{
	DebugAssert( athread );
	DebugAssert( threadFunc );

	athread->func = threadFunc;
	athread->arg = threadArg;
	athread->engine = threadEngine;
	athread->freeAThread = freeAThread;
}

static int athreadstart( void *arg )
{
	AThread *athread = ( AThread * )arg;

	AThreadFunction func = athread->func;
	void *funcArg = athread->arg;

	MemFree( athread );

	return func( funcArg );
}

int AThreadStart( AThreadEngine *threadEngine, AThreadFunction threadFunc, void *threadArg )
{
	AThread *athread;
	AThread *callback;
	int err;

	if( !( callback = MemMalloc( sizeof( AThread ) ) ) ) {
		errno = ENOMEM;
		err = -1;
	} else if( !( athread = MemMalloc( sizeof( AThread ) ) ) ) {
		MemFree( callback );
		errno = ENOMEM;
		err = -1;
	} else {
		AThreadInit( callback, NULL, threadFunc, threadArg, FALSE );
		AThreadInit( athread, threadEngine, athreadstart, callback, TRUE );
		AThreadBegin( athread );
		err = 0;
	}

	return err;
}

unsigned long AThreadCountRead( void )
{
	if( LIB_INITIALIZED == AThreadLibInitialized ) {
		return ATHREAD.count;
	} else {
		return 0;
	}
}

unsigned long AThreadCountUpdate( void )
{
	AThreadEngine *engine;
	unsigned long threadCount;

	threadCount = 0;

	if( LIB_INITIALIZED == AThreadLibInitialized ) {
		XplLockAcquire( &ATHREAD.lock );
		engine = ATHREAD.list;
		while( engine ) {
			threadCount += engine->thread.count( engine->context );
			engine = engine->next;
		}
		ATHREAD.count = threadCount;
		XplLockRelease( &ATHREAD.lock );
	}

	return threadCount;
}

unsigned long AThreadPeakCount( void )
{
	// not implemented yet
	return 0;
}

time_t AThreadPeakTime( void )
{
	// not implemented yet
	return 0;
}

static void AThreadLibInit( void )
{
	XplLockInit( &ATHREAD.lock );
	ATHREAD.list = NULL;
	ATHREAD.count = 0;
}

AThreadEngine *AThreadEngineCreate( AThreadInitFunc aThreadInit, AThreadCountFunc aThreadCount, void *context, FreeContextCB fccb )
{
	AThreadEngine *engine;

	if( AThreadLibInitialized == LIB_UNINITIALIZED ) {
		AThreadLibInit();
		AThreadLibInitialized = LIB_INITIALIZED;
	}

	engine = MemMalloc( sizeof( AThreadEngine ) );
	if( engine ) {
		engine->context = context;
		engine->fccb = fccb;
		engine->thread.init = aThreadInit;
		engine->thread.count = aThreadCount;
		XplLockAcquire( &ATHREAD.lock );
		engine->next = ATHREAD.list;
		ATHREAD.list = engine;
		XplLockRelease( &ATHREAD.lock );
	}
	return engine;
}

void AThreadEngineDestroy( AThreadEngine *engine )
{
	AThreadEngine *current;
	AThreadEngine *last;
	if( engine ) {
		XplLockAcquire( &ATHREAD.lock );
		current = ATHREAD.list;
		last = NULL;
		while( current ) {
			if( current == engine ) {
				if( last ) {
					last->next = current->next;
				} else {
					ATHREAD.list = current->next;
				}
				break;
			}
			last = current;
			current = current->next;
		}
		XplLockRelease( &ATHREAD.lock );
		if( engine->fccb ) {
			engine->fccb( engine->context );
		}
		MemFree( engine );
	}
}
