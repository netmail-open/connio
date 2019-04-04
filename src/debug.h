void debugUnreleasedConns( ConnHandlePriv *handle, ConnListenerListPriv *cllist, int waited );

#if DEBUG

#define DebugUnreleasedConns( handle, llist, waited )     debugUnreleasedConns( ( handle ), ( llist ), ( waited ) )

#else

#define DebugUnreleasedConns( handle, llist, waited )

#endif
