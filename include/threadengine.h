#ifndef THREAD_ENGINE_H
#define THREAD_ENGINE_H

struct AThreadEngine;

typedef int ( * AThreadFunction )( void *arg );

typedef struct _AThread
{
	AThreadFunction func;
	void *arg;
	struct AThreadEngine *engine;
	XplBool freeAThread;
} AThread;

typedef void (* FreeContextCB)( void *context );
typedef void (* AThreadInitFunc)( AThread *cb );
typedef unsigned long (* AThreadCountFunc )( void *aThreadContext );


typedef struct AThreadEngine {
	struct AThreadEngine *next;
	void *context;
	FreeContextCB fccb;
	struct {
		AThreadInitFunc init;
		AThreadCountFunc count;
	} thread;
} AThreadEngine;

void AThreadInit( AThread *cb, AThreadEngine *threadEngine, AThreadFunction threadFunc, void *threadArg, XplBool freeAThread );
void AThreadBegin( AThread *cb );
int AThreadStart( AThreadEngine *threadEngine, AThreadFunction threadFunc, void *threadArg );

unsigned long AThreadCountRead( void );
unsigned long AThreadCountUpdate( void );
unsigned long AThreadPeakCount( void );
time_t AThreadPeakTime( void );


AThreadEngine *AThreadEngineCreate( AThreadInitFunc threadInit, AThreadCountFunc aThreadCount, void *context, FreeContextCB fccb );

void AThreadEngineDestroy( AThreadEngine *engine );


/* Default thread engine callback functions */
void *AThreadDefaultCreateContext( char *identity, unsigned long stacksize );
void AThreadDefaultFreeContext( void *context );
void AThreadDefaultThreadInit( AThread *aThread );
unsigned long AThreadDefaultThreadCount( void *context );

#endif /* THREAD_ENGINE_H */
#ifndef THREAD_ENGINE_H
#define THREAD_ENGINE_H

struct AThreadEngine;

typedef int ( * AThreadFunction )( void *arg );

typedef struct _AThread
{
	struct _AThread *next;
	AThreadFunction func;
	void *arg;
	struct AThreadEngine *engine;
} AThread;

typedef struct AThreadList
{
	XplLock		lock;
	AThread	*head;
	AThread	**tail;
} AThreadList;

typedef void (* FreeContextCB)( void *context );
typedef void (* AThreadInitFunc)( AThread *cb );
typedef unsigned long (* AThreadCountFunc )( void *aThreadContext );


typedef struct AThreadEngine {
	struct AThreadEngine *next;
	void *context;
	FreeContextCB fccb;
	struct {
		AThreadInitFunc init;
		AThreadCountFunc count;
	} thread;
} AThreadEngine;


void AThreadBegin( AThread *cb );
void AThreadInit( AThread *cb, AThreadFunction threadFunc, void *threadArg, AThreadEngine *threadEngine );
unsigned long AThreadCountRead( void );
unsigned long AThreadCountUpdate( void );

AThreadEngine *AThreadEngineCreate( AThreadInitFunc threadInit, AThreadCountFunc aThreadCount, void *context, FreeContextCB fccb );

void AThreadEngineDestroy( AThreadEngine *engine );


/* Default thread engine callback functions */
void *AThreadDefaultCreateContext( char *identity, unsigned long stacksize );
void AThreadDefaultFreeContext( void *context );
void AThreadDefaultThreadInit( AThread *aThread );
unsigned long AThreadDefaultThreadCount( void *context );

#endif /* THREAD_ENGINE_H */
