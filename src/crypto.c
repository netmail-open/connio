#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "conniop.h"

#include <openssl/lhash.h>
#include <openssl/crypto.h>
#include <openssl/buffer.h>
#include <openssl/x509.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef WIN32
typedef pthread_mutex_t		__Mutex;

#define __MutexLock(m)		pthread_mutex_lock( &(m) )
#define __MutexUnlock(m)	pthread_mutex_unlock( &(m) )
#define __MutexInit(m)		pthread_mutex_init( &(m), NULL )
#else
typedef HANDLE				__Mutex;

#define __MutexLock(m)		WaitForSingleObject( (m), INFINITE )
#define __MutexUnlock(m)	ReleaseMutex( (m) )
#define __MutexInit(m)		_MInit( &m )

static void _MInit(HANDLE *m){ *m = CreateMutex( NULL, FALSE, NULL ); }
#endif

static __Mutex				*CryptoLocks		= NULL;
static uint32				CryptoLockCount		= 0;
static uint32				CryptoLockConsumers	= 0;

void CryptoLockCallback(int mode, int lockn, const char *file, int line)
{
	/*
		The documentation states that this "sets the n-th lock", which implies
		a 1 based offset.
	*/
	DebugAssert(lockn > 0);
	DebugAssert(lockn <= CryptoLockCount);

    if (mode & CRYPTO_LOCK) {
		__MutexLock(CryptoLocks[lockn - 1]);
    } else {
		__MutexUnlock(CryptoLocks[lockn - 1]);
    }

    return;
}

static unsigned long CryptoLockIDCallback(void)
{
#if defined(LINUX)
	return((unsigned long)pthread_self());
	/* Warning: treating the return value of pthread_self() as a number
	   may be problematic on some platforms.  The following is taken from
	   the man page:

       POSIX.1 allows an implementation wide freedom in choosing the type
       used to represent a thread ID; for example, representation using
       either an arithmetic type or a structure is permitted.  Therefore,
       variables of type pthread_t can't portably be compared using the C
       equality operator (==); use pthread_equal(3) instead.

       Thread identifiers should be considered opaque: any attempt to use a
       thread ID other than in pthreads calls is nonportable and can lead to
       unspecified results.
	*/
#elif defined(WIN32)
	return((unsigned long)GetCurrentThreadId());
#else
# error "A thread id function is not yet defined for this platform"
#endif
	/*
	  The xpl thread id function (XplGetThreadID()) is problematic for ssl.  We
	  are not sure why.  The working theory is that ssl starts thread that xpl
	  does not track.
	*/
}

EXPORT void ConnCryptoLockInit(void)
{
    int		i;

	CryptoLockConsumers++;
	if (CryptoLocks) {
		/* Already initialized */
		errno = 0;
		return;
	}

	CryptoLockCount	= CRYPTO_num_locks();
	CryptoLocks		= calloc(CryptoLockCount, sizeof(__Mutex));

	if (!CryptoLocks) {
		errno = ENOMEM;
		DebugAssert(0);
		return;
	}

	for (i = 0; i < CryptoLockCount; i++) {
		__MutexInit(CryptoLocks[i]);
	}

	CRYPTO_set_locking_callback(CryptoLockCallback);
	CRYPTO_set_id_callback(CryptoLockIDCallback);
	errno = 0;
    return;
}

EXPORT void ConnCryptoLockDestroy(void)
{
	CryptoLockConsumers--;

    if (0 == CryptoLockConsumers && CryptoLocks) {
        CRYPTO_set_locking_callback(NULL);
		CRYPTO_set_id_callback(NULL);

        free(CryptoLocks);
        CryptoLocks = NULL;
    }

    return;
}

