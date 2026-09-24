#ifndef PD_TCP_SERVICE_H
#define PD_TCP_SERVICE_H

/* Initialise GEM0/lwIP and listen on TCP port 6001. */
int pd_tcp_service_init(void);

/* Progress lwIP timers, RX processing and a pending GET transfer. */
void pd_tcp_service_poll(void);

#endif /* PD_TCP_SERVICE_H */
