// Copyright (c) 2014-2016, NetApp, Inc.
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

#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>

#ifdef __linux__
#include <netinet/in.h>
#else
#include <arpa/inet.h>
#endif

#include "warpcore.h"

struct w_sock;


static void usage(const char * const name)
{
    printf("%s\n", name);
    printf("\t -i interface           interface to run over\n");
    printf("\t[-b]                    busy-wait\n");
}

// global termination flag
static bool done = false;


// set the global termination flag
static void terminate(int signum __attribute__((unused)))
{
    done = true;
}


int main(const int argc, char * const argv[])
{
    const char * ifname = 0;
    bool busywait = false;

    // handle arguments
    int ch;
    while ((ch = getopt(argc, argv, "hi:b")) != -1) {
        switch (ch) {
        case 'i':
            ifname = optarg;
            break;
        case 'b':
            busywait = true;
            break;
        case 'h':
        case '?':
        default:
            usage(basename(argv[0]));
            return 0;
        }
    }

    if (ifname == 0) {
        usage(basename(argv[0]));
        return 0;
    }

    // bind this app to a single core
    plat_setaffinity();

    // initialize a warpcore engine on the given network interface
    struct warpcore * w = w_init(ifname, 0);

    // install a signal handler to clean up after interrupt
    assert(signal(SIGTERM, &terminate) != SIG_ERR, "signal");
    assert(signal(SIGINT, &terminate) != SIG_ERR, "signal");

    // start four inetd-like "small services"
    const uint16_t port[] = {7, 9, 13, 37};
    struct w_sock * const srv[] = {
        w_bind(w, htons(port[0])), w_bind(w, htons(port[1])),
        w_bind(w, htons(port[2])), w_bind(w, htons(port[3]))};
    const uint16_t n = sizeof(srv) / sizeof(struct w_sock *);
    struct pollfd fds[] = {{.fd = w_fd(srv[0]), .events = POLLIN},
                           {.fd = w_fd(srv[1]), .events = POLLIN},
                           {.fd = w_fd(srv[2]), .events = POLLIN},
                           {.fd = w_fd(srv[3]), .events = POLLIN}};

    // serve requests on the four sockets until an interrupt occurs
    while (done == false) {
        if (busywait == false)
            // if we aren't supposed to busy-wait, poll for new data
            poll(fds, n, -1);
        else
            // otherwise, just pull in whatever is in the NIC rings
            w_nic_rx(w);

        // for each of the small services...
        for (uint16_t s = 0; s < n; s++) {
            // ...check if any new data has arrived on the socket
            struct w_iov_chain * i = w_rx(srv[s]);
            if (i == 0)
                continue;

            const uint32_t i_len = w_iov_chain_len(i);
            struct w_iov_chain * o = 0; // w_iov for outbound data
            switch (s) {
            // echo received data back to sender (zero-copy)
            case 0:
                o = i;
                break;

            // discard; nothing to do
            case 1:
                break;

            // daytime
            case 2: {
                const time_t t = time(0);
                const char * c = ctime(&t);
                const uint16_t l = (uint16_t)strlen(c);
                struct w_iov * v;
                STAILQ_FOREACH (v, i, next) {
                    memcpy(v->buf, c, l); // write a timestamp
                    v->len = l;
                }
                o = i;
                break;
            }

            // time
            case 3: {
                const time_t t = time(0);
                struct w_iov * v;
                STAILQ_FOREACH (v, i, next) {
                    memcpy(v->buf, &t, sizeof(t)); // write a timestamp
                    v->len = sizeof(t);
                }
                o = i;
                break;
            }

            default:
                die("unknown service");
            }

            // if the current service requires replying with data, do so
            if (o) {
                w_tx(srv[s], o);
                w_nic_tx(w);
            }

            // track how much data was served
            const uint32_t o_len = w_iov_chain_len(o);
            if (i_len || o_len)
                warn(info, "port %d handled %d byte%s in, %d byte%s out",
                     port[s], i_len, plural(i_len), o_len, plural(o_len));


            // we are done serving the received data
            w_free(w, i);
        }
    }

    // we only get here after an interrupt; clean up
    for (uint16_t s = 0; s < n; s++)
        w_close(srv[s]);
    w_cleanup(w);
    return 0;
}
