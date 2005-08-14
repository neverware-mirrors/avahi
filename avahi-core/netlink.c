/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <assert.h>

#include <avahi-common/malloc.h>
#include "netlink.h"
#include "log.h"

struct AvahiNetlink {
    int fd;
    unsigned seq;
    AvahiNetlinkCallback callback;
    void* userdata;
    uint8_t* buffer;
    size_t buffer_length;

    const AvahiPoll *poll_api;
    AvahiWatch *watch;
};

int avahi_netlink_work(AvahiNetlink *nl, int block) {
    assert(nl);

    for (;;) {
        ssize_t bytes;
        struct nlmsghdr *p;

        for (;;) {
            if ((bytes = recv(nl->fd, nl->buffer, nl->buffer_length, block ? 0 : MSG_DONTWAIT)) < 0) {

                if (errno == EAGAIN || errno == EINTR)
                    return 1;
                
                avahi_log_error(__FILE__": recv() failed: %s", strerror(errno));
                return 0;
            }

            break;
        }

        p = (struct nlmsghdr *) nl->buffer;
        
        if (nl->callback) {
            for (; bytes > 0; p = NLMSG_NEXT(p, bytes)) {
                if (!NLMSG_OK(p, (size_t) bytes)) {
                    avahi_log_warn(__FILE__": packet truncated");
                    return 0;
                }

                nl->callback(nl, p, nl->userdata);
            }
        }

        if (block)
            return 1;
    }
}

static void socket_event(AvahiWatch *w, int fd, AvahiWatchEvent event, void *userdata) {
    AvahiNetlink *nl = userdata;

    assert(w);
    assert(nl);
    assert(fd == nl->fd);

    avahi_netlink_work(nl, 0);
}

AvahiNetlink *avahi_netlink_new(const AvahiPoll *poll_api, uint32_t groups, void (*cb) (AvahiNetlink *nl, struct nlmsghdr *n, void* userdata), void* userdata) {
    int fd = -1;
    struct sockaddr_nl addr;
    AvahiNetlink *nl = NULL;

    assert(poll_api);
    assert(cb);

    if ((fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE)) < 0) {
        avahi_log_error(__FILE__": socket(PF_NETLINK): %s", strerror(errno));
        return NULL;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = groups;
    addr.nl_pid = getpid();

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        avahi_log_error(__FILE__": bind(): %s", strerror(errno));
        goto fail;
    }

    if (!(nl = avahi_new(AvahiNetlink, 1))) {
        avahi_log_error(__FILE__": avahi_new() failed.");
        goto fail;
    }

    nl->poll_api = poll_api;
    nl->fd = fd;
    nl->seq = 0;
    nl->callback = cb;
    nl->userdata = userdata;

    if (!(nl->buffer = avahi_new(uint8_t, nl->buffer_length = 64*1024))) {
        avahi_log_error(__FILE__": avahi_new() failed.");
        goto fail;
    }

    if (!(nl->watch = poll_api->watch_new(poll_api, fd, AVAHI_WATCH_IN, socket_event, nl))) {
        avahi_log_error(__FILE__": Failed to create watch.");
        goto fail;
    }
    
    return nl;

fail:

    if (fd >= 0)
        close(fd);

    if (nl) {
        if (nl->buffer)
            avahi_free(nl);
        
        avahi_free(nl);
    }

    return NULL;
}

void avahi_netlink_free(AvahiNetlink *nl) {
    assert(nl);

    if (nl->watch)
        nl->poll_api->watch_free(nl->watch);

    if (nl->fd >= 0)
        close(nl->fd);
    
    avahi_free(nl->buffer);
    avahi_free(nl);
}

int avahi_netlink_send(AvahiNetlink *nl, struct nlmsghdr *m, unsigned *ret_seq) {
    assert(nl);
    assert(m);
    
    m->nlmsg_seq = nl->seq++;
    m->nlmsg_flags |= NLM_F_ACK;

    if (send(nl->fd, m, m->nlmsg_len, 0) < 0) {
        avahi_log_error(__FILE__": send(): %s\n", strerror(errno));
        return -1;
    }

    if (ret_seq)
        *ret_seq = m->nlmsg_seq;

    return 0;
}
