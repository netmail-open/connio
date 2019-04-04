find_library(MEMMGR		memmgr libmemmgr		HINT /usr/lib/ NO_DEFAULT_PATH)

#TODO write detection utility
set(HAVE_UNIX_SOCKETS           1)

# Look for 1.0 compatability packages first. This is needed on arch and may be
# needed on other distros in the future. Updating to newer openSSL is a better
# long term solution though.
find_library(SSL_LIBRARY		ssl		HINT /usr/lib/openssl-1.0/ NO_DEFAULT_PATH)
find_library(CRYPTO_LIBRARY		crypto	HINT /usr/lib/openssl-1.0/ NO_DEFAULT_PATH)

if (SSL_LIBRARY AND CRYPTO_LIBRARY)
	# Use 1.0 headers as well
	include_directories(/usr/include/openssl-1.0/)
else()
	find_library(SSL_LIBRARY		ssl)
	find_library(CRYPTO_LIBRARY		crypto)
endif()

find_library(RT_LIBS			rt)
find_library(DL_LIBS			dl)
find_library(LTDL_LIBS			ltdl)

if (NOT SSL_LIBRARY OR NOT CRYPTO_LIBRARY)
	missingdep(openssl "OpenSSL is required" 1)
else()
	set(OPENSSL_LIBS ${SSL_LIBRARY} ${CRYPTO_LIBRARY})
endif()
set (OPENSSL_CFLAGS				"")
