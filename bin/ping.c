// Copyright (c) 2014-2017, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include <warpcore.h>


static void usage(const char * const name,
                  const uint32_t start,
                  const uint32_t inc,
                  const uint32_t end,
                  const uint32_t loops)
{
    printf("%s\n", name);
    printf("\t -i interface           interface to run over\n");
    printf("\t -d destination IP      peer to connect to\n");
    printf("\t[-r router IP]          router to use for non-local peers\n");
    printf("\t[-s start packet len]   optional, default %u\n", start);
    printf("\t[-c increment]          optional (0 = exponential), default %u\n",
           inc);
    printf("\t[-e end packet len]     optional, default %u\n", end);
    printf("\t[-l loop iterations]    optional, default %u\n", loops);
    printf("\t[-z]                    optional, turn off UDP checksums\n");
    printf("\t[-b]                    busy-wait\n");
}


// Subtract the struct timespec values x and y (x-y), storing the result in r.
// Inspired by timersub()
#define time_diff(r, x, y)                                                     \
    do {                                                                       \
        (r)->tv_sec = (x)->tv_sec - (y)->tv_sec;                               \
        (r)->tv_nsec = (x)->tv_nsec - (y)->tv_nsec;                            \
        if ((r)->tv_nsec < 0) {                                                \
            --(r)->tv_sec;                                                     \
            (r)->tv_nsec += 1000000000;                                        \
        }                                                                      \
    } while (0)


// global timeout flag
static bool done = false;


// set the global timeout flag
static void timeout(int signum __attribute__((unused)))
{
    done = true;
}


struct payload {
    uint32_t len;
    struct timespec ts __attribute__((packed));
};


int main(const int argc, char * const argv[])
{
    const char * ifname = 0;
    const char * dst = 0;
    const char * rtr = 0;
    uint32_t loops = 1;
    uint32_t start = sizeof(struct payload);
    uint32_t inc = 102;
    uint32_t end = 1458;
    bool busywait = false;
    uint8_t flags = 0;

    // handle arguments
    int ch;
    while ((ch = getopt(argc, argv, "hzi:d:l:r:s:c:e:b")) != -1) {
        switch (ch) {
        case 'i':
            ifname = optarg;
            break;
        case 'd':
            dst = optarg;
            break;
        case 'r':
            rtr = optarg;
            break;
        case 'l':
            loops = MIN(UINT32_MAX, (uint32_t)strtoul(optarg, 0, 10));
            break;
        case 's':
            start = MIN(UINT32_MAX, MAX(1, (uint32_t)strtoul(optarg, 0, 10)));
            break;
        case 'c':
            inc = MIN(UINT32_MAX, MAX(0, (uint32_t)strtoul(optarg, 0, 10)));
            break;
        case 'e':
            end = MIN(UINT32_MAX, MAX(1, (uint32_t)strtoul(optarg, 0, 10)));
            break;
        case 'b':
            busywait = true;
            break;
        case 'z':
            flags |= W_ZERO_CHKSUM;
            break;
        case 'h':
        case '?':
        default:
            usage(basename(argv[0]), start, inc, end, loops);
            return 0;
        }
    }

    if (ifname == 0 || dst == 0) {
        usage(basename(argv[0]), start, inc, end, loops);
        return 0;
    }

    if (end < start)
        end = start;

    const struct addrinfo hints = {.ai_family = PF_INET,
                                   .ai_protocol = IPPROTO_UDP};
    uint32_t rip = 0;
    if (rtr) {
        struct addrinfo * router;
        ensure(getaddrinfo(rtr, 0, &hints, &router) == 0, "getaddrinfo router");
        rip = ((struct sockaddr_in *)(void *)router->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(router);
    }

    // initialize a warpcore engine on the given network interface
    struct w_engine * w = w_init(ifname, rip);

    // bind a new socket to a random local source port
    struct w_sock * s = w_bind(w, (uint16_t)plat_random(), flags);

    // look up the peer IP address and our benchmark port
    struct addrinfo * peer;
    ensure(getaddrinfo(dst, "55555", &hints, &peer) == 0, "getaddrinfo peer");

    // connect to the peer
    w_connect(s, ((struct sockaddr_in *)(void *)peer->ai_addr)->sin_addr.s_addr,
              ((struct sockaddr_in *)(void *)peer->ai_addr)->sin_port);

    // free the getaddrinfo
    freeaddrinfo(peer);

    // set a timer handler (used with busywait)
    ensure(signal(SIGALRM, &timeout) != SIG_ERR, "signal");
    const struct itimerval timer = {.it_value.tv_sec = 1};

    // send packet trains of sizes between "start" and "end"
    puts("byte\tpkts\ttx\trx");

    // send "loops" number of payloads of len "len" and wait for reply
    struct pollfd fds = {.fd = w_fd(s), .events = POLLIN};
    for (uint32_t len = start; len <= end; len += (inc ? inc : .483 * len)) {
        // allocate tx tail queue
        struct w_iov_stailq o;
        w_alloc_len(w, &o, len, 0);
        long iter = loops;
        while (likely(iter--)) {
            // get the current time
            struct timespec before_tx;
            ensure(clock_gettime(CLOCK_MONOTONIC, &before_tx) != -1,
                   "clock_gettime");

            // stamp the data
            struct w_iov * v;
            STAILQ_FOREACH (v, &o, next) {
                struct payload * const p = v->buf;
                p->len = htonl(len);
                p->ts = before_tx;
            }

            // send the data, and wait until it is out
            w_tx(s, &o);
            while (w_tx_pending(&o))
                w_nic_tx(w);

            // get the current time
            struct timespec after_tx;
            ensure(clock_gettime(CLOCK_MONOTONIC, &after_tx) != -1,
                   "clock_gettime");

            // set a timeout
            ensure(setitimer(ITIMER_REAL, &timer, 0) == 0, "setitimer");
            done = false;

            warn(info, "sent %u byte%s", len, plural(len));

            // wait for a reply; loop until timeout or we have received all data
            struct w_iov_stailq i = STAILQ_HEAD_INITIALIZER(i);
            while (likely(w_iov_stailq_len(&i, 0) < len && done == false)) {
                if (unlikely(busywait == false))
                    // poll for new data
                    if (poll(&fds, 1, -1) == -1)
                        // if the poll was interrupted, move on
                        continue;

                // receive new data (there may not be any if busy-waiting)
                w_nic_rx(w);
                w_rx(s, &i);
            }

            // get the current time
            struct timespec after_rx;
            ensure(clock_gettime(CLOCK_MONOTONIC, &after_rx) != -1,
                   "clock_gettime");

            // stop the timeout
            const struct itimerval stop = {0};
            ensure(setitimer(ITIMER_REAL, &stop, 0) == 0, "setitimer");

            const uint32_t i_len = w_iov_stailq_len(&i, 0);
            if (i_len != len) {
                warn(warn, "received %u/%u byte%s", i_len, len, plural(i_len));
                continue;
            }

            // compute time difference between the packet and the current time
            struct timespec diff;
            char rx[256] = "NA";
            if (i_len == len) {
                time_diff(&diff, &after_rx, &before_tx);
                ensure(diff.tv_sec == 0, "time difference > 1 sec");
                snprintf(rx, 256, "%ld", diff.tv_nsec);
            }
            const uint32_t pkts = w_iov_stailq_cnt(&o);
            time_diff(&diff, &after_tx, &before_tx);
            ensure(diff.tv_sec == 0, "time difference > 1 sec");
            printf("%d\t%d\t%ld\t%s\n", len, pkts, diff.tv_nsec, rx);

            // we are done with the received data
            w_free(w, &i);
        }
        w_free(w, &o);
    }
    w_close(s);
    w_cleanup(w);
    return 0;
}
