/**
 * Copyright 2015 Rapid7
 * @brief Durable multi-transport client network connection
 * @file network-client.h
 */

#include <ev.h>
#include <eio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#endif
#include <unistd.h>

#include "buffer_queue.h"
#include "log.h"
#include "bufferev.h"
#include "util.h"

struct bufferev {
	struct ev_timer connect_timer;
	struct ev_loop *loop;

	char *uri;
	enum network_proto proto;
	int sock, connected;
	struct ev_io data_ev;

	struct buffer_queue *tx_queue;
	struct buffer_queue *rx_queue;

	bufferev_data_cb read_cb;
	bufferev_data_cb write_cb;
	bufferev_event_cb event_cb;
	void *cb_arg;

	char *host;
	char **services;
	int num_services;
};

struct {
	enum network_proto proto;
	const char *str;
} proto_list[] = {
	{network_proto_udp, "udp"},
	{network_proto_tcp, "tcp"},
	{network_proto_tcp, "tls"},
};

const char
*network_proto_to_str(enum network_proto proto)
{
	for (int i = 0; i < COUNT_OF(proto_list); i++) {
		if (proto_list[i].proto == proto) {
			return proto_list[i].str;
		}
	}
	return "unknown";
}

enum network_proto
network_str_to_proto(const char *proto)
{
	for (int i = 0; i < COUNT_OF(proto_list); i++) {
		if (!strcasecmp(proto_list[i].str, proto)) {
			return proto_list[i].proto;
		}
	}
	return network_proto_tcp;
}

void bufferev_setcbs(struct bufferev *be,
	bufferev_data_cb read_cb,
	bufferev_data_cb write_cb,
	bufferev_event_cb event_cb,
	void *cb_arg)
{
    be->read_cb = read_cb;
    be->write_cb = write_cb;
    be->event_cb = event_cb;
    be->cb_arg = cb_arg;
}

struct buffer_queue * bufferev_rx_queue(struct bufferev *be)
{
	return be->rx_queue;
}

size_t bufferev_bytes_available(struct bufferev *be)
{
	return buffer_queue_len(be->rx_queue);
}

size_t bufferev_peek(struct bufferev *be, void *buf, size_t buflen)
{
	return buffer_queue_copy(be->rx_queue, buf, buflen);
}

size_t bufferev_read(struct bufferev *be, void *buf, size_t buflen)
{
	return buffer_queue_remove(be->rx_queue, buf, buflen);
}

ssize_t bufferev_write(struct bufferev *be, void *buf, size_t buflen)
{
	ssize_t off = 0, rc;
	ssize_t sent_bytes = 0;

	switch (be->proto) {
	case network_proto_udp:
		return send(be->sock, buf, buflen, 0);
	case network_proto_tcp:
		do {
			rc = send(be->sock, buf + off, buflen - off, 0);
			if (rc > 0) {
				off += rc;
				sent_bytes += rc;
			}
		} while (rc > 0 || (rc < 0 && (errno == EAGAIN || errno == EINTR)));
		return sent_bytes;

	case network_proto_tls:
		return buffer_queue_add(be->tx_queue, buf, buflen);
	}

	return -1;
}

static void
on_read(struct ev_loop *loop, struct ev_io *w, int events)
{
	struct bufferev *be = w->data;

	ssize_t bytes_read = 0;
	char buf[4096];
	ssize_t rc;
	while ((rc = recv(be->sock, buf, sizeof(buf), 0)) > 0) {
		bytes_read += rc;
		buffer_queue_add(be->rx_queue, buf, rc);
		if (be->read_cb) {
			be->read_cb(be, be->cb_arg);
		}
	}

	if (bytes_read <= 0) {
		ev_io_stop(be->loop, &be->data_ev);
		if (be->event_cb) {
			be->event_cb(be, BEV_EOF, be->cb_arg);
		}
	}
}

static void
close_sock(struct bufferev *be)
{
	if (be->sock >= 0) {
		close(be->sock);
		be->sock = -1;
	}
}

static void
on_connect_timeout(struct ev_loop *loop, struct ev_timer *w, int revents)
{
	struct bufferev *be = (struct bufferev *)w;

	ev_timer_stop(be->loop, &be->connect_timer);

	if (!be->connected) {
		close_sock(be);
		ev_io_stop(be->loop, &be->data_ev);

		if (be->event_cb) {
			be->event_cb(be, BEV_ERROR | BEV_TIMEOUT, be->cb_arg);
		}
	}
}

static void
on_connect(struct ev_loop *loop, struct ev_io *w, int events)
{
	struct bufferev *be = w->data;

	ev_io_stop(be->loop, &be->data_ev);
	ev_timer_stop(be->loop, &be->connect_timer);

	int status;
	socklen_t len = sizeof(status);
	getsockopt(be->sock, SOL_SOCKET, SO_ERROR, &status, &len);
	if (status != 0) {
		if (be->event_cb) {
			be->event_cb(be, BEV_ERROR, be->cb_arg);
		}
		return;
	}

	if (be->event_cb) {
		be->event_cb(be, BEV_CONNECTED, be->cb_arg);
	}

	ev_io_init(&be->data_ev, on_read, be->sock, EV_READ);
	be->data_ev.data = be;
	ev_io_start(be->loop, &be->data_ev);
	be->connected = 1;
}

int bufferev_connect_addrinfo(struct bufferev *be,
	struct addrinfo *src, struct addrinfo *dst, float timeout_s)
{
	be->sock = socket(dst->ai_family, dst->ai_socktype, dst->ai_protocol);
	if (be->sock < 0) {
		if (be->event_cb) {
			be->event_cb(be, BEV_ERROR, be->cb_arg);
		}
		return -1;
	}

	make_socket_nonblocking(be->sock);

	if (dst->ai_protocol == IPPROTO_UDP) {
		be->proto = network_proto_udp;
	} else {
		be->proto = network_proto_tcp;
	}
	if (src) {
		bind(be->sock, src->ai_addr, src->ai_addrlen);
	}

	int rc = connect(be->sock, dst->ai_addr, dst->ai_addrlen);
	if (rc == 0 || errno == EINPROGRESS) {
		ev_io_init(&be->data_ev, on_connect, be->sock, EV_WRITE);
		be->data_ev.data = be;
		ev_io_start(be->loop, &be->data_ev);

		ev_timer_init(&be->connect_timer, on_connect_timeout, timeout_s, 0);
		ev_timer_start(be->loop, &be->connect_timer);
	} else {
		close_sock(be);
		if (be->event_cb) {
			be->event_cb(be, BEV_ERROR, be->cb_arg);
		}
		return -1;
	}
	return 0;
}

int bufferev_connect_tcp_sock(struct bufferev *be, int sock)
{
	be->sock = sock;

	make_socket_nonblocking(be->sock);

	be->proto = network_proto_tcp;

	ev_io_init(&be->data_ev, on_read, be->sock, EV_READ);
	be->data_ev.data = be;
	ev_io_start(be->loop, &be->data_ev);

	if (be->event_cb) {
		be->event_cb(be, BEV_CONNECTED, be->cb_arg);
	}

	return 0;
}

static char *
parse_sockaddr(struct sockaddr_storage *addr, uint16_t *port)
{
	char host[INET6_ADDRSTRLEN] = { 0 };

	if (addr->ss_family == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)&addr;
		*port = ntohs(s->sin_port);
		inet_ntop(AF_INET, &s->sin_addr, host, INET6_ADDRSTRLEN);
	} else if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
		*port = ntohs(s->sin6_port);
		inet_ntop(AF_INET6, &s->sin6_addr, host, INET6_ADDRSTRLEN);
	}

	return strdup(host);
}

char * bufferev_get_local_addr(struct bufferev *be, uint16_t *port)
{
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);

	if (getsockname(be->sock, (struct sockaddr *)&addr, &len) == -1) {
		return NULL;
	}

	return parse_sockaddr(&addr, port);
}

char * bufferev_get_peer_addr(struct bufferev *be, uint16_t *port)
{
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);

	if (getpeername(be->sock, (struct sockaddr *)&addr, &len) == -1) {
		return NULL;
	}

	return parse_sockaddr(&addr, port);
}

void bufferev_free(struct bufferev *be)
{
	if (be) {
		ev_io_stop(be->loop, &be->data_ev);
		buffer_queue_free(be->rx_queue);
		buffer_queue_free(be->tx_queue);
		close_sock(be);
		free(be);
	}
}

struct bufferev * bufferev_new(struct ev_loop *loop)
{
	struct bufferev *be = calloc(1, sizeof(*be));
	if (!be) {
		return NULL;
	}

	be->rx_queue = buffer_queue_new();
	if (be->rx_queue == NULL) {
		goto err;
	}

	be->tx_queue = buffer_queue_new();
	if (be->tx_queue == NULL) {
		goto err;
	}

	be->loop = loop;

	return be;

err:
	bufferev_free(be);
	return NULL;
}
