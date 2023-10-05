
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    int timeout;
    int window;

    buffer_t* send_buffer;
    int snduna;
    int sndnxt;
    char snddone;
    buffer_t* rec_buffer;
    int rcvnxt;
    char rcvdone;
};
rel_t *rel_list;

long mstime() {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t* rel_create (conn_t* c, const struct sockaddr_storage* ss, const struct config_common* cc) {
    rel_t* r;
    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    r->timeout = cc->timeout;
    r->window = cc->window;

    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    r->snduna = 0;
    r->sndnxt = 1;
    r->snddone = 0;

    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    r-> rcvnxt = 1;
    r->rcvdone = 0;

    return r;
}

void rel_destroy (rel_t* r) {
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    buffer_clear(r->send_buffer);
    free(r->send_buffer);

    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    free(r);
}

struct ack_packet* makeack(int rcvnxt) {
    struct ack_packet* ackpkt;
    ackpkt = xmalloc(sizeof(*ackpkt));
    ackpkt->len = htons(8);
    ackpkt->ackno = htonl(rcvnxt);
    ackpkt->cksum = 0;
    ackpkt->cksum = cksum(ackpkt, 8);
    return ackpkt;
}

void rel_recvpkt (rel_t* r, packet_t* pkt, size_t n) {
    uint16_t len = ntohs(pkt->len);
    uint16_t hash = pkt->cksum;
    pkt->cksum = 0;
    if (n != len || hash != cksum((void*) pkt, n)) {
        return;
    }
    if (r->rcvdone && r->snddone && r->sndnxt == r->snduna) {
        rel_destroy(r);
        return;
    }

    uint32_t seqno = ntohl(pkt->seqno);
    uint32_t ackno = ntohl(pkt->ackno);
    if (n == 8) {
        r->snduna = ackno;
        buffer_remove(r->send_buffer, ackno);
        rel_read(r);
    }
    else if (!r->rcvdone && seqno <= r->rcvnxt + r->window) {
        if (r->rcvnxt <= seqno) {
            if (!buffer_contains(r->rec_buffer, seqno)) {
                buffer_insert(r->rec_buffer, pkt, mstime());
            }

            if (seqno == r->rcvnxt) {
                while (buffer_contains(r->rec_buffer, r->rcvnxt)) {
                    r->rcvnxt++;
                }
                rel_output(r);
            }
        }
        struct ack_packet* ackpkt = makeack(r->rcvnxt);
        conn_sendpkt(r->c, (packet_t*) ackpkt, 8);
        free(ackpkt);
    }
}

void rel_read (rel_t* r) {
    for (;r->sndnxt <= r->snduna + r->window; r->sndnxt++) {
        packet_t* sndpkt = xmalloc(sizeof (*sndpkt));
        int inputlength = conn_input(r->c, sndpkt->data, 500);
        if (!inputlength) {
            free(sndpkt);
            break;
        }
        if (inputlength == -1) {
            r->snddone = 1;
            inputlength = 0;
        }
        sndpkt->seqno = htonl(r->sndnxt);
        sndpkt->len = htons(12 + inputlength);
        sndpkt->cksum = 0;
        sndpkt->cksum = cksum(sndpkt, 12 + inputlength);

        buffer_insert(r->send_buffer, sndpkt, mstime());
        conn_sendpkt(r->c, sndpkt, 12 + inputlength);
        free(sndpkt);
    }
}

void rel_output (rel_t* r) {
    for (buffer_node_t* b=buffer_get_first(r->rec_buffer); b!=NULL && ntohl(b->packet.seqno) < r->rcvnxt; b=b->next) {
        conn_output(r->c, b->packet.data, ntohs(b->packet.len) - 12);
        if (ntohs(b->packet.len) == 12) {
            r->rcvdone = 1;
            return;
        }
        buffer_remove_first(r->rec_buffer);
    }
}

void rel_timer () {
    for (rel_t* cur = rel_list; cur != NULL; cur = rel_list->next) {
        for (buffer_node_t* slot = buffer_get_first(rel_list->send_buffer); slot != NULL; slot = slot->next) {
            long curtime = mstime();
            if (rel_list->timeout <= curtime - slot->last_retransmit) {
                conn_sendpkt(rel_list->c, &slot->packet, ntohs(slot->packet.len));
                slot->last_retransmit = curtime;
            }
        }
    }
}
