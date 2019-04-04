#ifndef _LISTENER_EVENT_H
#define _LISTENER_EVENT_H


typedef enum {
	LISTENER_SUBMITTED,
	LISTENER_RETURNED,
	LISTENER_WAITING,
	LISTENER_STARTED,
	LISTENER_KILLED,
	LISTENER_STOPPED,
	LISTENER_SUSPENDED,
	LISTENER_RESUMED,
	THROTTLING_STOPPED,
	THROTTLING_UPDATE,
	THROTTLING_STARTED
} ListenerEvent;

typedef void ( * ListenerEventCallback )( ListenerEvent event, unsigned long threshold, unsigned long threadcount, void *se, void *cs, int s );

#endif
