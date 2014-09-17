#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netdb.h>

#include "warpcore.h"
#include "ip.h"


static void usage(const char * const name, const uint32_t size, const uint32_t loops)
{
	printf("%s\n", name);
	printf("\t -i interface           interface to run over\n");
	printf("\t -d destination IP      peer to connect to\n");
	printf("\t -p destination port    peer port to connect to\n");
	printf("\t[-s transfer size]      optional, default %d\n", size);
	printf("\t[-l loop interations]   optional, default %d\n", loops);
}


// static void iov_fill(struct w_iov *v)
// {
// 	while (v) {
// 		// log(5, "%d bytes in buf %d", v->len, v->idx);
// 		for (uint16_t l = 0; l < v->len; l++) {
// 			v->buf[l] = (char)(l % 0xff);
// 		}
// 		v = SLIST_NEXT(v, next);
// 	}
// }


// static void iov_dump(struct w_iov *v)
// {
// 	while (v) {
// 		log(5, "%d bytes in buf %d", v->len, v->idx);
// 		hexdump(v->buf, v->len);
// 		v = SLIST_NEXT(v, next);
// 	}
// }


int main(int argc, char *argv[])
{
	const char *ifname = 0;
	const char *dst = 0;
	const char *port = 0;
	uint32_t size = 1500 - sizeof(struct ip_hdr) - sizeof(struct udp_hdr);
	uint32_t loops = 1;

	int ch;
	while ((ch = getopt(argc, argv, "hi:d:p:s:l:")) != -1) {
		switch (ch) {
		case 'i':
			ifname = optarg;
			break;
		case 'd':
			dst = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 's':
			size = strtol(optarg, 0, 10);
			break;
		case 'l':
			loops = strtol(optarg, 0, 10);
			break;
		case 'h':
		case '?':
		default:
			usage(argv[0], size, loops);
			return 0;
		}
	}

	if (ifname == 0 || dst == 0 || port == 0) {
		usage(argv[0], size, loops);
		return 0;
	}

	// struct addrinfo hints, *res;
	// memset(&hints, 0, sizeof(hints));
	// hints.ai_family = PF_INET;
	// hints.ai_protocol = IP_P_UDP;
	// if (getaddrinfo(dst, port, &hints, &res) != 0)
	// 	die("getaddrinfo");
	// dst = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	// port = ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port);
	// freeaddrinfo(res);

	struct warpcore *w = w_init(ifname);
	log(1, "main process ready");

	// start some inetd-like services
	struct w_sock *echo = w_bind(w, IP_P_UDP, htons(7));
	struct w_sock *disc = w_bind(w, IP_P_UDP, htons(9));

	while (w_poll(w, POLLIN, -1)) {
		// echo service
		struct w_iov *i = w_rx(echo);
		while (i) {
			log(5, "echo %d bytes in buf %d", i->len, i->idx);
			// echo the data
			struct w_iov *o = w_tx_alloc(echo, i->len);
			memcpy(o->buf, i->buf, i->len);
			w_connect(echo, i->ip, i->port);
			w_tx(echo);
			i = SLIST_NEXT(i, next);
		}
		w_rx_done(echo);

		// discard service
		i = w_rx(disc);
		while (i) {
			log(5, "discard %d bytes in buf %d", i->len, i->idx);
			// discard the data
			i = SLIST_NEXT(i, next);
		}
		w_rx_done(disc);
	}
	w_close(echo);
	w_close(disc);

#if 0
	struct w_sock *s = w_bind(w, IP_P_UDP, random());
	if (s == 0)
		die("w_bind");
	w_connect(s, ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr,
	          ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port));

	for (long n = 1; n <= loops; n++) {
		// TODO: figure out why 128 is too much here
		uint32_t len = size;
		struct w_iov *o = w_tx_alloc(s, len);
		iov_fill(o);
		// iov_dump(o);
		w_tx(s);

#ifdef NDEBUG
		if (n % 100 == 0) {
			printf("%ld\r", n);
			fflush(stdout);
		}
#endif

		// while (len > 0) {
			// run the receive loop
			if (w_poll(s, POLLIN, -1) == false)
				goto done;

			// read any data
			struct w_iov *i = w_rx(s);

			// access the read data
			while (i) {
				// log(5, "%d bytes in buf %d", i->len, i->idx);
				len -= i->len;
				// hexdump(i->buf, i->len);
				i = SLIST_NEXT(i, next);
			}

			// read is done, release the iov
			w_rx_done(s);
		// }
	}
done:
	// keep running
	log(1, "polling");
	while (w_poll(s, POLLIN, -1)) {
		w_rx(s);
	}

	w_close(s);

#ifdef NDEBUG
	printf("\n");
#endif
#endif

	log(1, "main process exiting");
	w_cleanup(w);

	return 0;
}
