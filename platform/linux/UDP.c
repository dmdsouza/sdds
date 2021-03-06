/*
 * =====================================================================================
 *
 *       Filename:  UDP.c
 *
 *    Description:  impl for network layer as a UDP service
 *
 *        Version:  1.0
 *        Created:  22.02.2010 16:10:17
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Kai Beckmann (kai), kai-oliver.beckmann@hs-rm.de
 *        Company:  Hochschule RheinMain - DOPSY Labor für verteilte Systeme
 *
 * =====================================================================================
 */

#include "sDDS.h"

#include <stdlib.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>

// taking into account ipv4 tunneling features
#define PLATFORM_LINUX_IPV6_MAX_CHAR_LEN 45

#ifndef PLATFORM_LINUX_SDDS_PORT
#define PLATFORM_LINUX_SDDS_PORT 23234
#endif

#ifndef PLATFORM_LINUX_SDDS_PROTOCOL
#define PLATFORM_LINUX_SDDS_PROTOCOL AF_INET6
#endif

#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_ADDRESS                   SDDS_BUILTIN_MULTICAST_ADDRESS
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PARTICIPANT_ADDRESS       SDDS_BUILTIN_PARTICIPANT_ADDRESS
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_TOPIC_ADDRESS             SDDS_BUILTIN_TOPIC_ADDRESS
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_SUB_PUB_ADDRESS           SDDS_BUILTIN_SUB_PUB_ADDRESS
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_LOCATION_ADDRESS           SDDS_BUILTIN_LOCATION_ADDRESS
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PAR_STATE_MSG_ADDRESS     SDDS_BUILTIN_PAR_STATE_MSG_ADDRESS

#ifndef PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PORT_OFF
#define PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PORT_OFF 20
#endif

#ifndef PLATFORM_LINUX_MULTICAST_SO_RCVBUF
#define PLATFORM_LINUX_MULTICAST_SO_RCVBUF SDDS_NET_MAX_BUF_SIZE
#endif

#ifndef PLATFORM_LINUX_SDDS_ADDRESS
#define PLATFORM_LINUX_SDDS_ADDRESS "::"
#endif

struct Network_t {
    int fd_uni_socket;
    int fd_multi_socket;
    pthread_t recvThread;
    pthread_t multiRecvThread;
    int port;
};

struct UDPLocator_t {
    Locator_t loc;
    struct sockaddr_storage addr_storage;
    socklen_t addr_len;
};

static struct Network_t net;
static NetBuffRef_t inBuff;
static NetBuffRef_t multiInBuff;

pthread_mutex_t recv_mutex = PTHREAD_MUTEX_INITIALIZER;

void*
recvLoop(void*);
static int
create_socket(struct addrinfo* address);

// for the builtintopic
// IF BUILTIN

Locator_t* builtinTopicNetAddress;
// ENDIF

size_t
Network_size(void) {
    return sizeof(struct Network_t);
}


rc_t
Network_Multicast_joinMulticastGroup(char* multicast_group_ip) {
    int ret;
    struct addrinfo* multicast_address;
    char multicast_port[PLATFORM_LINUX_IPV6_MAX_CHAR_LEN];
    unsigned int loop;

    struct addrinfo addrCriteria;                   // Criteria for address match
    memset(&addrCriteria, 0, sizeof(addrCriteria)); // Zero out structure
    addrCriteria.ai_family = AF_INET6;
    addrCriteria.ai_socktype = SOCK_DGRAM;          
    addrCriteria.ai_flags |= AI_NUMERICHOST;

    sprintf(multicast_port, "%d", (net.port + PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PORT_OFF));
    //  Get the multicast address for the provided multicast group ip

    if ((ret = getaddrinfo(multicast_group_ip, multicast_port, &addrCriteria, &multicast_address)) != 0) {
        Log_error("%d ERROR: getaddrinfo() failed: %s\n", __LINE__, gai_strerror(ret));
        return SDDS_RT_FAIL;
    }

    //  Copy the multicast address to the multicast request
    struct ipv6_mreq multicast_request;
    memcpy(&multicast_request.ipv6mr_multiaddr,
           &((struct sockaddr_in6*) (multicast_address->ai_addr))->sin6_addr,
           sizeof(multicast_request.ipv6mr_multiaddr));

    //  Try to get scope index for our interface
    if ((multicast_request.ipv6mr_interface = if_nametoindex(PLATFORM_LINUX_SDDS_IFACE)) < 0) {
        Log_warn("Couldn't get scope index for interface: %s: %s\n",
                 PLATFORM_LINUX_SDDS_IFACE,
                 strerror(errno));
        //  If scope index couldn't be obtained accept multicast from any
        //  interface
        multicast_request.ipv6mr_interface = 0;
    }

    //  Join the multicast group
    if (setsockopt(net.fd_multi_socket,
                   IPPROTO_IPV6, IPV6_JOIN_GROUP,
                   (char*) &multicast_request,
                   sizeof(multicast_request)) != 0) {
        Log_error("%d ERROR: setsockopt() failed: %s\n", __LINE__, strerror(errno));
        return SDDS_RT_FAIL;
    }

#ifdef FEATURE_SDDS_SECURITY_ENABLED
    //  Disable multicast loop
    loop = 0;    
    if (setsockopt(net.fd_multi_socket,
                   IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                   &loop,
                   sizeof(loop)) != 0) {
        Log_error("%d ERROR: setsockopt() failed: %s\n", __LINE__, strerror(errno));
        return SDDS_RT_FAIL;
    }
#endif
    return SDDS_RT_OK;
}


rc_t
Network_Multicast_init() {
    struct addrinfo* multicastAddr;     /* Multicast address */
    struct addrinfo* localAddr;     /* Local address to bind to */
    char* multicastIP = PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_ADDRESS;     /*
                                                                              Arg:
                                                                              IP
                                                                              Multicast
                                                                              address
                                                                              */

    char multicastPort[PLATFORM_LINUX_IPV6_MAX_CHAR_LEN];
    int multicastTTL;     /* Arg: TTL of multicast packets */
    struct addrinfo hints = { 0 };     /* Hints for name lookup */

    sprintf(multicastPort, "%d", (net.port + PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PORT_OFF));

    multicastTTL = 1;

    /* Resolve destination address for multicast datagrams */
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    int status;
    if ((status = getaddrinfo(PLATFORM_LINUX_SDDS_ADDRESS, multicastPort, &hints,
                              &multicastAddr)) != 0) {
        Log_error("%d ERROR: getaddrinfo() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }

    Log_info("Using %s\n", multicastAddr->ai_family == PF_INET6 ? "IPv6" : "IPv4");

    /* Create socket for sending multicast datagrams */
    if ((net.fd_multi_socket = socket(multicastAddr->ai_family, multicastAddr->ai_socktype, 0))
        < 0) {
        Log_error("%d ERROR: socket() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }

    /* select sender interface */
    unsigned int outif;
    if ((outif = if_nametoindex(PLATFORM_LINUX_SDDS_IFACE)) < 0) {
        Log_info("Ignoring unknown interface: %s: %s\n", PLATFORM_LINUX_SDDS_IFACE, strerror(errno));
        outif = 0;
    }
    if (setsockopt(net.fd_multi_socket, IPPROTO_IPV6, IPV6_MULTICAST_IF, &outif, sizeof(outif)) != 0) {
        Log_info("Could not join the multicast group: %s\n", strerror(errno));
        return SDDS_RT_FAIL;;
    }

    /* Set TTL of multicast packet */
    if (setsockopt(net.fd_multi_socket,
                   multicastAddr->ai_family == PF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP,
                   multicastAddr->ai_family == PF_INET6 ?
                   IPV6_MULTICAST_HOPS :
                   IP_MULTICAST_TTL,
                   (char*) &multicastTTL, sizeof(multicastTTL)) != 0) {
        Log_error("%d ERROR: setsockopt() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }


    /* Get a local address with the same family (IPv4 or IPv6) as our multicast
       group */
    hints.ai_family = multicastAddr->ai_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;     /* Return an address we can bind to */

    if (getaddrinfo(NULL, multicastPort, &hints, &localAddr) != 0) {
        Log_error("%d ERROR: getaddrinfo() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }

    int yes = 1;
    /* lose the pesky "Address already in use" error message */
    if (setsockopt(net.fd_multi_socket, SOL_SOCKET, SO_REUSEADDR, (char*) &yes, sizeof(int))
        == -1) {
        Log_error("%d ERROR: setsockopt() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }

    /* Bind to the multicast port */
    if (bind(net.fd_multi_socket, localAddr->ai_addr, localAddr->ai_addrlen) != 0) {
        Log_error("%d ERROR: bind() failed: %s\n", __LINE__, strerror(errno));
        return SDDS_RT_FAIL;
    }

    /* get/set socket receive buffer */
    int optval = 0;
    socklen_t optval_len = sizeof(optval);
    int dfltrcvbuf;
    if (getsockopt(net.fd_multi_socket, SOL_SOCKET, SO_RCVBUF, (char*) &optval, &optval_len)
        != 0) {
        Log_error("%d ERROR: getsockopt() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }
    dfltrcvbuf = optval;
    optval = PLATFORM_LINUX_MULTICAST_SO_RCVBUF;
    if (setsockopt(net.fd_multi_socket, SOL_SOCKET, SO_RCVBUF, (char*) &optval, sizeof(optval))
        != 0) {
        Log_error("%d ERROR: setsockopt() failed: %s\n", __LINE__, strerror(errno));
        return SDDS_RT_FAIL;
    }
    if (getsockopt(net.fd_multi_socket, SOL_SOCKET, SO_RCVBUF, (char*) &optval, &optval_len)
        != 0) {
        Log_error("%d ERROR: getsockopt() failed\n", __LINE__);
        return SDDS_RT_FAIL;
    }
    Log_info("tried to set socket receive buffer from %d to %d, got %d\n",
             dfltrcvbuf, PLATFORM_LINUX_MULTICAST_SO_RCVBUF, optval);

    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_ADDRESS);
    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PARTICIPANT_ADDRESS);
    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_SUB_PUB_ADDRESS);
    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_TOPIC_ADDRESS);
    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_LOCATION_ADDRESS);
    Network_Multicast_joinMulticastGroup(PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PAR_STATE_MSG_ADDRESS);

    NetBuffRef_init(&multiInBuff);
    Locator_t* loc;
    LocatorDB_newMultiLocator(&loc);
    multiInBuff.locators->add_fn(multiInBuff.locators, loc);
    // set up a thread to read from the udp multicast socket
    if (pthread_create(&net.multiRecvThread, NULL, recvLoop,
                       (void*) &multiInBuff) != 0) {
        exit(-1);
    }

    freeaddrinfo(multicastAddr);
    freeaddrinfo(localAddr);

    return SDDS_RT_OK;
}


rc_t
Network_init(void) {

    struct addrinfo* address;
    struct addrinfo hints;
    char port_buffer[6];

    net.port = PLATFORM_LINUX_SDDS_PORT;

    // clear hints, no dangling fields
    memset(&hints, 0, sizeof hints);

    // getaddrinfo wants its port parameter in string form
    sprintf(port_buffer, "%u", net.port);

    // returned addresses will be used to create datagram sockets
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = PLATFORM_LINUX_SDDS_PROTOCOL;
    hints.ai_flags = AI_PASSIVE;

    int gai_ret = getaddrinfo(PLATFORM_LINUX_SDDS_ADDRESS, port_buffer,
                              &hints, &address);
    if (gai_ret != 0) {
        Log_error("can't obtain suitable addresses for listening\n");
        return SDDS_RT_FAIL;
    }
    // implicit call of the network receive handler
    // should start from now ;)

    // search in the db for the locator
    // TODO do something better than this hack here....
    net.fd_uni_socket = create_socket(address);

    if (net.fd_uni_socket < 0) {
        freeaddrinfo(address);
        Log_error("Cant't create socket.\n");
        return SDDS_RT_FAIL;
    }

    // free up address
    freeaddrinfo(address);

    // init the incoming frame buffer and add dummy unicast locator
    NetBuffRef_init(&inBuff);
    Locator_t* loc;
    LocatorDB_newLocator(&loc);
    inBuff.locators->add_fn(inBuff.locators, loc);

    // set up a thread to read from the udp socket
    if (pthread_create(&net.recvThread, NULL, recvLoop, (void*) &inBuff)
        != 0) {
        Log_error("Cant't create thread.\n");
        return SDDS_RT_FAIL;
    }

#ifdef FEATURE_SDDS_MULTICAST_ENABLED
    Network_Multicast_init();
#endif

    return SDDS_RT_OK;
}

static int
create_socket(struct addrinfo* address) {
    int fd = socket(address->ai_family, address->ai_socktype, 0);

    if (fd < 0) {
        Log_error("can't create socket\n");
        return -1;
    }

    assert(address->ai_family == PLATFORM_LINUX_SDDS_PROTOCOL);

#if PLATFORM_LINUX_SDDS_PROTOCOL == AF_INET
    // IF NET AND BROADCAST
    int broadcast_permission = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast_permission, sizeof broadcast_permission) < 0) {
        Log_error("can't prepare IPv4 socket for broadcasting\n");
        close(fd);
        return -1;
    }
    // ENDIF
#elif PLATFORM_LINUX_SDDS_PROTOCOL == AF_INET6
    // IPv6 doesn't support broadcasting, use multicast when sending packets
#else
#error "Only AF_INET and AF_INET6 are understood linux protocols."
#endif

#if defined(SDDS_TOPIC_HAS_PUB) || defined(FEATURE_SDDS_DISCOVERY_ENABLED)
    // bind the socket
    if (bind(fd, address->ai_addr, address->ai_addrlen) != 0) {
        Log_error("Unable to bind socket\n");
        return -1;
    }
#endif

#ifdef UTILS_DEBUG
    // show which address was assigned
    {
        char address_buffer[NI_MAXHOST];

        if (
            getnameinfo(
                        address->ai_addr,
                        address->ai_addrlen,
                        address_buffer,
                        NI_MAXHOST,
                        NULL,
                        0,
                        NI_NUMERICHOST
                        ) != 0) {
            // ignore getnameinfo errors, just for debugging anyway
        }
        else
        {
            Log_debug("listening on [%s]:%u for udp packets\n", address_buffer, net.port);
        }
    }
#endif

    return fd;
}

void*
recvLoop(void* netBuff) {

    NetBuffRef_t* buff = (NetBuffRef_t*) netBuff;
    int sock;     // receive socket
    unsigned int sock_type;     // broad or multicast

    int count = 0;

    // Check the dummy locator for uni or multicast socket
    Locator_t* l = (Locator_t*) buff->locators->first_fn(buff->locators);
    sock_type = l->type;

    if (sock_type == SDDS_LOCATOR_TYPE_MULTI) {
        sock = net.fd_multi_socket;
        Log_info("Receive on multicast socket\n");
    }
    else if (sock_type == SDDS_LOCATOR_TYPE_UNI) {
        sock = net.fd_uni_socket;
        Log_info("Receive on unicast socket\n");
    }

    NetBuffRef_renew(buff);

    while (true) {
        // spare address field?
        struct sockaddr_storage addr;
        // size of addr
        socklen_t addr_len = sizeof(struct sockaddr_storage);
        ssize_t recv_size = recvfrom(sock, buff->buff_start,
                                     buff->frame_start->size, 0,
                                     (struct sockaddr*)&addr, &addr_len);


        if (recv_size == -1) {
            Log_error("%d Error while receiving a udp frame: %s\n", __LINE__, strerror(errno));
            continue;
        }
#ifdef FEATURE_SDDS_TRACING_ENABLED
#	ifdef FEATURE_SDDS_TRACING_RECV_NORMAL
#		ifdef FEATURE_SDDS_TRACING_RECV_PAKET
        Trace_point(SDDS_TRACE_EVENT_RECV_PAKET);
#		endif
#	endif
#endif
        Log_debug("[%u]%i bytes received.\n", sock_type, (int) recv_size);

        struct UDPLocator_t sloc;

        memcpy(&(sloc.addr_storage), &addr, addr_len);
        sloc.addr_len = addr_len;


        Locator_t* loc;

        if (LocatorDB_findLocator((Locator_t*) &sloc, &loc) == SDDS_RT_OK) {
            Locator_upRef(loc);
        }
        else {
            // not found we need a new one
            if (LocatorDB_newLocator(&loc) != SDDS_RT_OK) {
                Log_error("(%d) Cannot obtain free locator.\n", __LINE__);
                NetBuffRef_renew(buff);
                continue;
            }
            memcpy(&((struct UDPLocator_t*) loc)->addr_storage, &addr, addr_len);
            ((struct UDPLocator_t*) loc)->addr_len = addr_len;

        }

        loc->isEmpty = false;
        loc->isSender = true;
        loc->type = sock_type;

        rc_t ret = buff->locators->add_fn(buff->locators, loc);
        if (ret != SDDS_RT_OK) {
            NetBuffRef_renew(buff);
            continue;
        }


        if (DataSink_processFrame(buff) != SDDS_RT_OK) {
            Log_debug("Failed to process frame\n");
        }

        NetBuffRef_renew(buff);

    }
    return SDDS_RT_OK;
}

rc_t
Network_send(NetBuffRef_t* buff) {
#ifdef FEATURE_SDDS_TRACING_ENABLED
#ifdef FEATURE_SDDS_TRACING_SEND_PAKET
    Trace_point(SDDS_TRACE_EVENT_SEND_PAKET);
#endif
#endif
    int sock;
    unsigned int sock_type;
    // Check the locator for uni or multicast socket
    Locator_t* l = (Locator_t*) buff->locators->first_fn(buff->locators);
    if (l == NULL) {
        Log_error("(%d) NetBuff has no locator.\n", __LINE__);
        return SDDS_RT_FAIL;
    }
    sock_type = l->type;
    // add locator to the netbuffref
    if (sock_type == SDDS_LOCATOR_TYPE_MULTI) {
        sock = net.fd_multi_socket;
    }
    else if (sock_type == SDDS_LOCATOR_TYPE_UNI) {
        sock = net.fd_uni_socket;
    }
    Locator_t* loc = (Locator_t*) buff->locators->first_fn(buff->locators);

    Log_debug("Transmitting to %d recipients\n", buff->locators->size_fn(buff->locators));

    while (loc != NULL) {

        ssize_t transmitted;
        struct sockaddr_storage addr = ((struct UDPLocator_t*) loc)->addr_storage;

        transmitted = sendto(sock, buff->buff_start, buff->curPos, 0,
                             (struct sockaddr*) &addr, ((struct UDPLocator_t*) loc)->addr_len);

        if (transmitted == -1) {
            perror("ERROR");
            Log_error("can't send udp packet\n");
        }
        else {
            Log_debug("Transmitted %d bytes over %s\n", transmitted,
                      sock_type == SDDS_LOCATOR_TYPE_UNI ? "unicast" : "multicast");
        }

        loc = (Locator_t*) buff->locators->next_fn(buff->locators);
    }

    return SDDS_RT_OK;
}

void
Network_recvFrameHandler(Network _this) {
}

rc_t
Network_getFrameBuff(NetFrameBuff* buff) {
    if (buff == NULL) {
        return SDDS_RT_BAD_PARAMETER;
    }
    size_t size = SDDS_NET_MAX_BUF_SIZE * sizeof(byte_t);
    size += sizeof(struct NetFrameBuff_t);

    *buff = Memory_alloc(size);

    if (*buff == NULL) {
        return SDDS_RT_NOMEM;
    }
    memset(*buff, 0, size);

    (*buff)->size = SDDS_NET_MAX_BUF_SIZE;
    return SDDS_RT_OK;
}

rc_t
Network_getPayloadBegin(size_t* startByte) {
    // payload starts at the beginning of the buffer, address is provided
    // Separately
    // buffer starts after the struct
    *startByte = sizeof(struct NetFrameBuff_t);

    return SDDS_RT_OK;
}
size_t
Network_locSize(void) {
    return sizeof(struct UDPLocator_t);
}

rc_t
Network_setAddressToLocator(Locator_t* loc, char* endpoint) {
    return Network_set_locator_endpoint(loc, endpoint, net.port);
}

rc_t
Network_setPlatformAddressToLocator(Locator_t* loc) {
    return Network_set_locator_endpoint(loc, PLATFORM_LINUX_SDDS_ADDRESS, net.port);
}

rc_t
Network_set_locator_endpoint(Locator_t* loc, char* endpoint, int port) {
    assert(loc);
    assert(endpoint);
    Log_debug("Set locator endpoint to ip: %s, port: %d\n", endpoint, port);

    loc->type = SDDS_LOCATOR_TYPE_UNI;
    struct UDPLocator_t* udp_loc = (struct UDPLocator_t*) loc;

    // getaddrinfo wants its port parameter in string form
    char port_buffer[6];
    sprintf(port_buffer, "%u", port);

    // clear hints, no dangling fields
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);

    // returned addresses will be used to create datagram sockets
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = PLATFORM_LINUX_SDDS_PROTOCOL;

    struct addrinfo* address;
    int gai_ret = getaddrinfo(endpoint, port_buffer, &hints, &address);

    if (gai_ret != 0) {
        Log_error("can't obtain suitable addresses %s for setting UDP locator: %s\n", endpoint, gai_strerror(errno));
        return SDDS_RT_FAIL;
    }

#ifdef UTILS_DEBUG
    // show which address was assigned
    {
        char address_buffer[NI_MAXHOST];

        if (
            getnameinfo(
                        address->ai_addr,
                        address->ai_addrlen,
                        address_buffer,
                        NI_MAXHOST,
                        NULL,
                        0,
                        NI_NUMERICHOST
                        ) != 0) {
            // ignore getnameinfo errors, just for debugging anyway
        }
        else
        {
            Log_debug("created a locator for [%s]:%u\n", address_buffer, net.port);
        }
    }
#endif

    memcpy(&udp_loc->addr_storage, address->ai_addr, address->ai_addrlen);
    udp_loc->addr_len = address->ai_addrlen;

    // free up address
    freeaddrinfo(address);

    return SDDS_RT_OK;
}

rc_t
Network_setMulticastAddressToLocator(Locator_t* loc, char* addr) {
    assert (loc);
    assert (addr);

    loc->type = SDDS_LOCATOR_TYPE_MULTI;
    struct UDPLocator_t* l = (struct UDPLocator_t*) loc;

    struct addrinfo* address;
    struct addrinfo hints;
    char port_buffer[6];

    // clear hints, no dangling fields
    memset(&hints, 0, sizeof (hints));

    // getaddrinfo wants its port parameter in string form
    sprintf(port_buffer, "%u",
            net.port + PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_PORT_OFF);

    // returned addresses will be used to create datagram sockets
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = PLATFORM_LINUX_SDDS_PROTOCOL;

    int gai_ret = getaddrinfo(addr, port_buffer, &hints, &address);

    if (gai_ret != 0) {
        Log_error( "%d can't obtain suitable addresses %s for setting UDP locator\n",
                  __LINE__,
                  addr);

        return SDDS_RT_FAIL;
    }

#ifdef UTILS_DEBUG
    // show which address was assigned
    {
        char address_buffer[NI_MAXHOST];

        if (
            getnameinfo(
                        address->ai_addr,
                        address->ai_addrlen,
                        address_buffer,
                        NI_MAXHOST,
                        NULL,
                        0,
                        NI_NUMERICHOST
                        ) != 0) {
            // ignore getnameinfo errors, just for debugging anyway
        }
        else
        {
            Log_debug("created a locator for [%s]:%s\n", address_buffer, port_buffer);
        }
    }
#endif

    memcpy(&l->addr_storage, address->ai_addr, address->ai_addrlen);
    l->addr_len = address->ai_addrlen;

    // free up address
    freeaddrinfo(address);

    return SDDS_RT_OK;
}

rc_t
Network_createLocator(Locator_t** loc) {
    if (loc == NULL) {
        return SDDS_RT_BAD_PARAMETER;
    }

    *loc = Memory_alloc(sizeof(struct UDPLocator_t));

    if (*loc == NULL) {
        return SDDS_RT_NOMEM;
    }

    // set type for recvLoop
    (*loc)->type = SDDS_LOCATOR_TYPE_UNI;

    return Network_setAddressToLocator(*loc, PLATFORM_LINUX_SDDS_ADDRESS);
}

rc_t
Network_createMulticastLocator(Locator_t** loc) {
    if (loc == NULL) {
        return SDDS_RT_BAD_PARAMETER;
    }

    *loc = Memory_alloc(sizeof(struct UDPLocator_t));

    if (*loc == NULL) {
        return SDDS_RT_NOMEM;
    }

    // set type for recvLoop
    (*loc)->type = SDDS_LOCATOR_TYPE_MULTI;

    return Network_setMulticastAddressToLocator(*loc,
                                                PLATFORM_LINUX_SDDS_BUILTIN_MULTICAST_ADDRESS);
}

bool_t
Locator_isEqual(Locator_t* l1, Locator_t* l2) {
    if ((l1 == NULL) || (l2 == NULL)) {
        return false;
    }

    struct UDPLocator_t* a = (struct UDPLocator_t*) l1;
    struct UDPLocator_t* b = (struct UDPLocator_t*) l2;
#if PLATFORM_LINUX_SDDS_PROTOCOL == AF_INET
    struct sockaddr_in* addr[2];

    addr[0] = (struct sockaddr_in*)&a->addr_storage;
    addr[1] = (struct sockaddr_in*)&b->addr_storage;

    if (memcmp(&addr[0]->sin_addr.s_addr, &addr[1]->sin_addr.s_addr, 4) == 0) {
        return true;
    }
    else {
        return false;
    }
#elif PLATFORM_LINUX_SDDS_PROTOCOL == AF_INET6

    struct sockaddr_in6* addr[2];

    addr[0] = (struct sockaddr_in6*) &a->addr_storage;
    addr[1] = (struct sockaddr_in6*) &b->addr_storage;

    if (memcmp(&addr[0]->sin6_addr.s6_addr, &addr[1]->sin6_addr.s6_addr, 16) == 0) {
        return true;
    }
    else {
        return false;
    }
#endif
}

rc_t
Locator_getAddress(Locator_t* self, char* srcAddr, size_t max_addr_len) {
    assert(self);
    assert(srcAddr);
    struct sockaddr_storage* addr = &((struct UDPLocator_t*) self)->addr_storage;

    int rc = getnameinfo((struct sockaddr*) addr, ((struct UDPLocator_t*) self)->addr_len,
                         srcAddr, NI_MAXHOST,
                         NULL, 0,
                         NI_NUMERICHOST);
    if (rc != 0) {
        Log_error("Cannot obtain address from locator: %s\n", gai_strerror(errno));
        return SDDS_RT_FAIL;
    }
    return SDDS_RT_OK;
}

rc_t
Locator_copy(Locator_t* src, Locator_t* dst) {
    assert(src != dst);
    assert(src);
    assert(dst);

	memcpy(dst, src, sizeof(struct UDPLocator_t));
	dst->refCount = 0;
	return SDDS_RT_OK;
}
