#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <argp.h>

#include <time.h>

#include "ubpf.h"
#include "agent.h"
#include "ebpf_consts.h"
#include "sfc_consts.h"
#include "nsh.h"
#include "DevType.pb-c.h"

#ifndef likely
    #define likely(x)        __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
    #define unlikely(x)        __builtin_expect(!!(x), 0)
#endif

#ifndef __aligned_tpacket
    #define __aligned_tpacket    __attribute__((aligned(TPACKET_ALIGNMENT)))
#endif
#ifndef __align_tpacket
    #define __align_tpacket(x)    __attribute__((aligned(TPACKET_ALIGN(x))))
#endif
struct ring {
    struct iovec *rd;
    uint8_t *map;
    struct tpacket_req req;
    int size;
    int frame_num;
};

struct port {
    int fd;
    struct ring rx_ring;
    struct ring tx_ring;
};

struct dataplane {
    unsigned long long dpid;
    int port_count;
    struct port *ports;
    uint64_t pktid;
    DevType type;
    uint8_t address[ETH_ALEN];
} dataplane;

struct pushpop {
	uint16_t new_pkt_len;
}pushpop;

static sig_atomic_t sigint = 0;

union frame_map {
    struct {
        struct tpacket2_hdr tp_h __aligned_tpacket;
        struct sockaddr_ll s_ll __align_tpacket(sizeof(struct tpacket2_hdr));
    } *v2;
    void *raw;
};

static void sighandler(int num)
{
    sigint = 1;
}

static void voidhandler(int num) {} // NOTE: do nothing prevent mininet from killing the softswitch

static int setup_ring(int fd, struct ring* ring, int ring_type)
{
    int err;
    unsigned int blocknum = 256;

    memset(&ring->req, 0, sizeof(ring->req));

    ring->req.tp_block_size = getpagesize() << 2;
    ring->req.tp_frame_size = TPACKET_ALIGNMENT << 7;
    ring->req.tp_block_nr = blocknum;
    ring->req.tp_frame_nr = ring->req.tp_block_size /
                            ring->req.tp_frame_size *
                            ring->req.tp_block_nr;

    ring->size = ring->req.tp_block_size * ring->req.tp_block_nr;

    err = setsockopt(fd, SOL_PACKET, ring_type, &ring->req, sizeof(ring->req));
    if (err < 0) {
        perror("setsockopt");
        exit(1);
    }

    return 0;
}

static int setup_socket(struct port *port, char *netdev)
{
    int err, i, fd, ifindex, v = TPACKET_V2;
    struct sockaddr_ll ll;

    ifindex = if_nametoindex(netdev);
    if (ifindex == 0) {
        perror("interface");
        exit(1);
    }
    // printf("interface index %d\n", ifindex);

    // Opens a raw socket for this port
    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    port->fd = fd;

    err = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &v, sizeof(v));
    if (err < 0) {
        perror("setsockopt");
        exit(1);
    }

    // NOTE: disable qdisc, trivial performance improvement
    // int one = 1;
    // setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &one, sizeof(one));

    setup_ring(fd, &port->rx_ring, PACKET_RX_RING);
    setup_ring(fd, &port->tx_ring, PACKET_TX_RING);

    port->rx_ring.map = mmap(NULL, port->rx_ring.size + port->tx_ring.size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);

    if (port->rx_ring.map == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    port->tx_ring.map = port->rx_ring.map + port->rx_ring.size;

    // rd_num * sizeof(*ring->rd)
    int rx_len_iovec = port->rx_ring.req.tp_frame_nr * sizeof(*port->rx_ring.rd);
    int tx_len_iovec = port->tx_ring.req.tp_frame_nr * sizeof(*port->tx_ring.rd);

    port->rx_ring.rd = malloc(rx_len_iovec); // allocate iovec for each block
    port->tx_ring.rd = malloc(tx_len_iovec);

    // why not use calloc?
    memset(port->rx_ring.rd, 0, rx_len_iovec);
    memset(port->tx_ring.rd, 0, tx_len_iovec);

    // TODO check if ring->rd is allocated properly
    // printf("number of frames: %d\n", port->rx_ring.req.tp_frame_nr);
    for (i = 0; i < port->rx_ring.req.tp_frame_nr; ++i) {
        port->rx_ring.rd[i].iov_base = port->rx_ring.map + (i * port->rx_ring.req.tp_frame_size);
        port->rx_ring.rd[i].iov_len = port->rx_ring.req.tp_frame_size;
    }

    for (i = 0; i < port->tx_ring.req.tp_frame_nr; ++i) {
        port->tx_ring.rd[i].iov_base = port->tx_ring.map + (i * port->tx_ring.req.tp_frame_size);
        port->tx_ring.rd[i].iov_len = port->tx_ring.req.tp_frame_size;
    }

    //
    memset(&ll, 0, sizeof(ll));
    ll.sll_family = PF_PACKET;
    ll.sll_protocol = htons(ETH_P_ALL);
    ll.sll_ifindex = ifindex;
    ll.sll_hatype = 0;
    ll.sll_pkttype = 0;
    ll.sll_halen = 0;

    err = bind(fd, (struct sockaddr *) &ll, sizeof(ll));
    if (err < 0) {
        perror("bind");
        exit(1);
    }

    return fd;
}

static void teardown_socket(struct port *port)
{
    munmap(port->tx_ring.map, port->tx_ring.size);
    munmap(port->rx_ring.map, port->rx_ring.size);

    free(port->tx_ring.rd);
    free(port->rx_ring.rd);

    close(port->fd);
}

static inline int v2_rx_kernel_ready(struct tpacket2_hdr *hdr)
{
    return ((hdr->tp_status & TP_STATUS_USER) == TP_STATUS_USER);
}

static inline void v2_rx_user_ready(struct tpacket2_hdr *hdr)
{
    hdr->tp_status = TP_STATUS_KERNEL;
    __sync_synchronize();
}

static inline int v2_tx_kernel_ready(struct tpacket2_hdr *hdr)
{
    return !(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING));
}

static inline void v2_tx_user_ready(struct tpacket2_hdr *hdr)
{
    hdr->tp_status = TP_STATUS_SEND_REQUEST;
    __sync_synchronize();
}

int tx_frame(struct port* port, void *data, int len) {
    // add the packet to the port tx queue
    struct ring *tx_ring = &port->tx_ring;

    // TODO: Drop if tx queue is full? (drop-tail)
    if (v2_tx_kernel_ready(tx_ring->rd[tx_ring->frame_num].iov_base)) {
        union frame_map ppd_out;
        ppd_out.raw = tx_ring->rd[tx_ring->frame_num].iov_base;

        // copy the packet from ppd to ppd_out
        // ppd_out.v2->tp_h.tp_snaplen = ppd.v2->tp_h.tp_snaplen;
        // ppd_out.v2->tp_h.tp_len = ppd.v2->tp_h.tp_len;
        ppd_out.v2->tp_h.tp_snaplen = len;
        ppd_out.v2->tp_h.tp_len = len;

        //  printf("start pointer: %p  tp_mac offset: %d  hdrlen: %d  sockadd_ll: %d\n", ppd_out.raw, ppd_out.v2->tp_h.tp_mac, TPACKET2_HDRLEN, sizeof(struct sockaddr_ll));

        // Can this be zerocopy too? I guess not with the fixed allocation of rings
        // assert(ppd.v2->tp_h.tp_len == ppd.v2->tp_h.tp_snaplen);
        // printf("ppd_out.tp_mac %d\n", ppd_out.v2->tp_h.tp_mac);

        memcpy((uint8_t *) ppd_out.raw + TPACKET2_HDRLEN - sizeof(struct sockaddr_ll),
            (uint8_t *) data,
            len);

        ppd_out.v2->tp_h.tp_status = TP_STATUS_SEND_REQUEST;

        //
        tx_ring->frame_num = (tx_ring->frame_num + 1) % tx_ring->req.tp_frame_nr;

        return 0;
    }

    return -1; // Kernel not ready, dropping the packet
}

const char *argp_program_version = "ebpf-switch 0.1";
const char *argp_program_bug_address = "<simon.jouet@glasgow.ac.uk>";
static char doc[] = "eBPF-switch -- eBPF user space switch";
static char args_doc[] = "interface1 interface2 [interface3 ...]";

static struct argp_option options[] = {
    {"verbose",  'v',      0,      0, "Produce verbose output" },
    {"dpid"   ,  'd', "dpid",      0, "Datapath id of the switch"},
    {"controller", 'c', "address", 0, "Controller address default to 127.0.0.1:9000"},
    {"type", 't', "type",   0, "Device type (switch or service function)"},
    {"address", 'a', "addr", 0, "MAC address"},
    { 0 }
};

#define MAX_INTERFACES 255

struct arguments
{
    char *interfaces[MAX_INTERFACES];
    int interface_count;
    unsigned long long dpid;
    char *controller;
    DevType type;
    int verbose;
    uint8_t address[ETH_ALEN];
};

DevType parse_type(char* arg){
    if( strcmp(arg,"SF") == 0 )
        return DEV_TYPE__FUNCTION;
    else if(strcmp(arg,"SW") == 0)
        return DEV_TYPE__SWITCH;
    else if(strcmp(arg,"GW") == 0)
        return DEV_TYPE__GATEWAY;
    else
        return DEV_TYPE__UNKNOWN;
}

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;
    DevType type;
    int ret;

    switch (key)
    {
        case 'v':
            arguments->verbose = 1;
            break;

        case 'd':
            arguments->dpid = strtoull(arg, NULL, 10);
            break;

        case 'c':
            arguments->controller = arg;
            break;

        case 't':
            type = parse_type(arg);

            if(type == UNKNOWN){
                printf("ERROR: Unknown device type. Exiting...");
                exit(1);
            }else
                arguments->type = type;

            break;
        case 'a':
            ret = sscanf(arg,"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                &arguments->address[0],
                &arguments->address[1],
                &arguments->address[2],
                &arguments->address[3],
                &arguments->address[4],
                &arguments->address[5]);
            if(ret == EOF){
                printf("ERROR: Wrong MAC address format...");
                exit(1);
            }
            break;
        case ARGP_KEY_ARG:
            arguments->interfaces[state->arg_num] = arg;
            arguments->interface_count++;
            break;

        case ARGP_KEY_END:
            if (state->arg_num < 1) /* Not enough arguments. */
                argp_usage (state);
            break;

        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

unsigned long long random_dpid() {
    srand(time(NULL));
    unsigned long long dpid = 0;

    for (int i = 0; i < 5; i++) {
        dpid = (dpid << 15) | (rand() & 0x7FFF);
    }

    return dpid & 0xFFFFFFFFFFFFFFFFULL;
}

void pop_header(struct packet *pkt, int offset, int encap_len){
    char* eth_frame = (char*) pkt->eth;

    // Edge case
    if(offset >= pkt->metadata->length || encap_len >= pkt->metadata->length){
        printf("Error, packet too short\n");
        return;
    }

    memmove(eth_frame + offset,eth_frame + offset + encap_len,pkt->metadata->length-offset);
    pkt->metadata->length -= encap_len;
    pushpop.new_pkt_len = pkt->metadata->length;
}

void push_header(struct packet *pkt, int offset, int encap_len, void *data){
    char* eth_frame = (char*) pkt->eth;

    // Check MTU violation
    if(pkt->metadata->length + encap_len > 2048){
        printf("Error, exceeded MTU size\n");
        return; // Error
    }

    // Make room for encapsulation
    memmove(eth_frame + offset + encap_len, eth_frame + offset, pkt->metadata->length-offset);

    // Copy encapsulation
    memmove(eth_frame + offset, (char*) data, encap_len);
    pkt->metadata->length += encap_len;
    pushpop.new_pkt_len = pkt->metadata->length;

    // printf("encapsulating packet...\n");
}

// flags is the hack to force transmission
void transmit(struct metadatahdr *buf, int len, uint32_t port, int flags) {
    int i;
    void *eth_frame = (uint8_t *)buf + sizeof(struct metadatahdr);
    int eth_len = len - sizeof(struct metadatahdr);
    void *enc_eth_frame = NULL;

    // Check if we performed encap/decap
    if(pushpop.new_pkt_len != 0){
        eth_len = pushpop.new_pkt_len;
        pushpop.new_pkt_len = 0; // Reset flag
    }

    switch (port) {
        case FLOOD:
            // printf("Flooding the packet\n");
            for (i = 0; i < dataplane.port_count; i++) {
                if (i != buf->in_port) {
                    // printf("sending frame from port %d to port %d on switch %llu\n", buf->in_port, i, dataplane.dpid);
                    tx_frame(&dataplane.ports[i], eth_frame, eth_len);
                }
            }

            // HACK, the packets are only sent after poll() however this
            // can be called asynchronously on packet from the controller and
            // therefore delay the packet transmission until the next packet is received
            if (flags) {
                for (i = 0; i < dataplane.port_count; i++) {
                    send(dataplane.ports[i].fd, NULL, 0, MSG_DONTWAIT);
                }
            }

            break;
        //
        case CONTROLLER:
            // printf("Sending to controller\n");
            agent_packetin(buf, len);
            break;
        //
        case DROP:
            // printf("Dropping the packet\n");
            break;

        default:
            // printf("Forwarding the packet\n");
            // printf("in_port %d out_port %lu data_len %lu\n", buf->in_port, port, len - sizeof(struct metadatahdr));
            tx_frame(&dataplane.ports[port], eth_frame, eth_len);
    }
}

void print_mac_be(const char* desc, const char* mac){
    
    printf("%s : %02x:%02x:%02x:%02x:%02x:%02x\n",
            desc,
            mac[5],
            mac[4],
            mac[3],
            mac[2],
            mac[1],
            mac[0]);
}

int main(int argc, char **argv)
{
    int i;

    /* Argument Parsing */
    struct arguments arguments;
    arguments.interface_count = 0;
    arguments.dpid = random_dpid();
    arguments.controller = "127.0.0.1:9000";
    arguments.type = SWITCH; // switch type is the default
    
    arguments.address[0] = 0;
    arguments.address[1] = 0;
    arguments.address[2] = 0;
    arguments.address[3] = 0;
    arguments.address[4] = 0;
    arguments.address[5] = 0;
    
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    printf("Controller address set to: %s\n",arguments.controller);

    /* */
    dataplane.dpid = arguments.dpid;
    dataplane.port_count = arguments.interface_count;
    dataplane.ports = calloc(dataplane.port_count, sizeof(struct port));
    dataplane.pktid = 0;
    dataplane.type = arguments.type;
    memcpy(dataplane.address,arguments.address,ETH_ALEN);

    // printf("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
    //             dataplane.address[5],
    //             dataplane.address[4],
    //             dataplane.address[3],
    //             dataplane.address[2],
    //             dataplane.address[1],
    //             dataplane.address[0]);

    print_mac_be("MAC address",dataplane.address);
    /* */
    pushpop.new_pkt_len = 0;
    /* */
    struct pollfd pfds[dataplane.port_count];

    // signal(SIGINT, sighandler);
    signal(SIGINT, voidhandler);
    signal(SIGKILL, sighandler);

    /* setup all the interfaces */
    printf("(%llu) Setting up %d interfaces\n", dataplane.dpid, dataplane.port_count);
    for (i = 0; i < dataplane.port_count; i++) {
        // Create the socket, allocate the tx and rx rings and create the frame io vectors
        setup_socket(&dataplane.ports[i], arguments.interfaces[i]);

        // Create the array of pollfd for poll()
        pfds[i].fd = dataplane.ports[i].fd;
        pfds[i].events = POLLIN | POLLERR;
        pfds[i].revents = 0;

        //
        printf("(%llu) Interface %s, index %d, fd %d\n", dataplane.dpid, arguments.interfaces[i], i, dataplane.ports[i].fd);
    }
    printf("\n");

    /* */
    ubpf_jit_fn ubpf_fn[MAX_FUNCS], fn;
    for(i = 0 ; i < MAX_FUNCS ; i++)
        ubpf_fn[i] = NULL;

    struct agent_options options = {
        .dpid = dataplane.dpid,
        .controller = arguments.controller
    };


    agent_start(ubpf_fn, (tx_packet_fn)transmit, (pop_header_fn) pop_header, (push_header_fn) push_header , &options);

    //
    union frame_map ppd;
    uint64_t ret = DROP; //default value

    while (likely(!sigint)) {
        //
        for (i = 0; i < dataplane.port_count; i++) {
            //
            struct ring *rx_ring = &dataplane.ports[i].rx_ring;

            // process all the packets received in the rx_ring
            while (v2_rx_kernel_ready(rx_ring->rd[rx_ring->frame_num].iov_base)) {
                ppd.raw = rx_ring->rd[rx_ring->frame_num].iov_base;

                // printf("metadatahdr len %lu\n", sizeof(struct metadatahdr)); // Should be  ppd.v2->tp_h.tp_mac - TPACKET2_HDRLEN

                /**/
                struct metadatahdr *metadatahdr = (struct metadatahdr *)((uint8_t *)ppd.raw + TPACKET2_HDRLEN);
                metadatahdr->in_port = i;
                metadatahdr->sec = ppd.v2->tp_h.tp_sec;
                metadatahdr->nsec = ppd.v2->tp_h.tp_nsec;
                metadatahdr->length = (uint16_t)ppd.v2->tp_h.tp_len;
                // metadatahdr->id = dataplane.pktid++;

                // 0 -> proxy
                // 1 -> sf
                // 2 -> fwd
                
                struct packet pkt = {.metadata = metadatahdr, .eth = (struct ethhdr *) ((uint8_t *)metadatahdr + sizeof(struct metadatahdr))};
                // hexDump("== Packet from SW ==",pkt.eth,pkt.metadata.length);
                //void* p = (uint8_t *)metadatahdr + sizeof(struct metadatahdr);

                // if(ubpf_fn[0] != NULL){
                //     ret = ubpf_fn[0]((void*) &pkt,sizeof(struct packet));
                //     // printf("(%llu) ret =  0x%8" PRIx64 "...\n",dataplane.dpid,ret);
                // }

                switch(dataplane.type){
                    case DEV_TYPE__FUNCTION:
                        if(ntohs(pkt.eth->h_proto) == ETH_P_NSH && 
                            ubpf_fn[SF_VM] != NULL && 
                            ubpf_fn[FWD_VM] != NULL &&
                            memcmp(dataplane.address,pkt.eth->h_dest,ETH_ALEN) == 0){

                            // Size of outer encapsulation: Ethernet + NSH
                            unsigned int encap_size = ETH_HLEN + NSH_HLEN_NO_META;
                
                            // "Remove" encapsulation
                            pkt.eth = (struct ethhdr *) ((uint8_t *)pkt.eth + encap_size);
                            pkt.metadata->length -= encap_size;
                            
                            // sf processing
                            ubpf_fn[SF_VM](&pkt,sizeof(struct packet));
                            
                            // forwarding
                            // "Insert" encapsulation again
                            pkt.eth = (struct ethhdr *) ((uint8_t *)pkt.eth - encap_size);
                            pkt.metadata->length += encap_size;
                            ret = ubpf_fn[FWD_VM](&pkt, sizeof(struct packet));
                            memcpy(pkt.eth->h_source,dataplane.address,ETH_ALEN);
                        }else{
                            // Packets not encapsulated with NSH will just be dropped
                            ret = DROP;
                        }
                        break;
                    case DEV_TYPE__SWITCH:
                        if(ubpf_fn[0] != NULL){
                            ret = ubpf_fn[0](&pkt, sizeof(struct packet));
                        }
                        break;
                    case DEV_TYPE__GATEWAY:
                        if(ubpf_fn[0] != NULL && ubpf_fn[1] != NULL){
                            ubpf_fn[0](&pkt,sizeof(struct packet));
                            ret = ubpf_fn[1](&pkt, sizeof(struct packet));
                        }
                        break;
                    default:
                        ret = DROP;
                }

                // Default value of ret is DROP
                transmit(metadatahdr, ppd.v2->tp_h.tp_len + sizeof(struct metadatahdr), (uint32_t)ret, 0);

                // Frame has been used, release the buffer space
                v2_rx_user_ready(ppd.raw);
                rx_ring->frame_num = (rx_ring->frame_num + 1) % rx_ring->req.tp_frame_nr;
            }
        }

        // Send all the pendings packets for each interface
        for (i = 0; i < dataplane.port_count; i++) {
            send(dataplane.ports[i].fd, NULL, 0, MSG_DONTWAIT); // Should we use POLLOUT and just queue the messages to transmit then call send() once
        }

        // Poll for the next socket POLLIN or POLLERR
        poll(pfds, dataplane.port_count, -1);
    }

    /* House keeping */
    agent_stop();
    printf("Terminating ...\n");
    for (i = 0; i < dataplane.port_count; i++) {
        teardown_socket(&dataplane.ports[i]);
    }

    return 0;
}
