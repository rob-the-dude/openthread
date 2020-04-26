/*
 *  Copyright (c) 2018, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the platform network on Linux.
 */

#include "openthread-posix-config.h"
#include "platform-posix.h"

#if defined(__APPLE__)

//	NOTE: on mac OS, the utun driver is present on the system and "works" --
//	but in a limited way.  In particular, the mac OS "utun" driver is marked IFF_POINTTOPOINT,
//	and you cannot clear that flag with SIOCSIFFLAGS (it's part of the IFF_CANTCHANGE definition
//	in xnu's net/if.h [but removed from the mac OS SDK net/if.h]).  And unfortuntately, mac OS's
//	build of mDNSResponder won't allow for mDNS over an interface marked IFF_POINTTOPOINT
//	(see comments near definition of MulticastInterface in mDNSMacOSX.c for the bogus reasoning).
//
//	There is an alternative.  An open-source tuntap kernel extension is available here:
//
//		<http://tuntaposx.sourceforge.net>
//		<https://sourceforge.net/p/tuntaposx/code/ci/master/tree/>
//
//	and can be installed via homebrew here:
//
//		<https://formulae.brew.sh/cask/tuntap>
//
//	Building from source and installing isn't trivial, and it's
//	not getting easier (https://forums.developer.apple.com/thread/79590).
//
//	If you want mDNS support, then you can't use Apple utun.  I use the non-Apple driver
//	pretty much exclusively, because mDNS is a requirement.

#define USE_APPLE_UTUN 0
#define USE_TUN 1

#if USE_TUN && USE_APPLE_UTUN
#error "You may not use both tun and utun."
#endif

#endif

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <ifaddrs.h>
#ifdef __linux__
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
#include <netinet/in.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_var.h>
#endif
#include <net/route.h>
#include <netinet6/in6_var.h>
//
// 	we need the definition of ND6_INFINITE_LIFETIME, but on
// 	on mac OS, you can't include nd6.h, at least not from C++, because
//	prf_ra isn't defined.  But it actually IS defined -- just inside
//	another struct in <netinet6/in6_var.h>, which was just included.
//
#if !(defined(__APPLE__) || defined(__FreeBSD__))
#include <netinet6/nd6.h> // ND6_INFINITE_LIFETIME
#else
#define ND6_INFINITE_LIFETIME 0xffffffff
#endif
#include <net/if_dl.h> // struct sockaddr_dl

#ifdef __APPLE__
#if USE_APPLE_UTUN
#include <net/if_utun.h>
#endif

#if USE_TUN
#include <sys/ioccom.h>
// FIX ME: include the tun_ioctl.h file (challenging, as it's location depends on where the developer puts it)
#define TUNSIFHEAD _IOW('t', 96, int)
#define TUNGIFHEAD _IOR('t', 97, int)
#endif

#include <sys/kern_control.h>
#endif

#ifdef __NetBSD__
#include <net/if_tun.h>
#endif

#ifdef __FreeBSD__
#include <net/if_tun.h>
#endif

#include <errno.h>
#include <string.h>
#endif

#include <openthread/icmp6.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/message.h>

#include "common/code_utils.hpp"
#include "common/logging.hpp"
#include "net/ip6_address.hpp"

#if OPENTHREAD_CONFIG_PLATFORM_NETIF_ENABLE

#ifndef OPENTHREAD_POSIX_TUN_DEVICE

#ifdef __linux__
#define OPENTHREAD_POSIX_TUN_DEVICE "/dev/net/tun"
#elif defined(__NetBSD__) || defined(__FreeBSD__)
#define OPENTHREAD_POSIX_TUN_DEVICE "/dev/tun0"
#elif defined(__APPLE__)
#if USE_APPLE_UTUN
#define OPENTHREAD_POSIX_TUN_DEVICE // not used - calculated dynamically
#elif USE_TUN
#define OPENTHREAD_POSIX_TUN_DEVICE "/dev/tun0"
#endif
#else
// good luck -- untested platform...
#define OPENTHREAD_POSIX_TUN_DEVICE "/dev/net/tun"
#endif

#endif // OPENTHREAD_TUN_DEVICE

// Some platforms will include linux/ipv6.h in netinet/in.h
#if defined(__linux__) && !defined(_IPV6_H) && !defined(_UAPI_IPV6_H)
// from linux/ipv6.h
struct in6_ifreq
{
    struct in6_addr ifr6_addr;
    __u32           ifr6_prefixlen;
    int             ifr6_ifindex;
};
#endif

#if defined(RTM_NEWMADDR) || defined(__NetBSD__)
// on some BSDs (mac OS, FreeBSD), we get RTM_NEWMADDR/RTM_DELMADDR messages, so we don't need to monitor using MLD
// on NetBSD, MLD monitoring simply doesn't work
#define OPENTHREAD_POSIX_USE_MLD_MONITOR 0
#else
// on some platforms (Linux, but others might be made to work), we do not get information about multicast
// group joining via AF_NETLINK or AF_ROUTE sockets.  on those platform, we must listen for IPv6 ICMP
// MLDv2 messages to know when mulicast memberships change
// 		https://stackoverflow.com/questions/37346289/using-netlink-is-it-possible-to-listen-whenever-multicast-group-membership-is-ch
#define OPENTHREAD_POSIX_USE_MLD_MONITOR 1
#endif

// some platforms (like NetBSD) do not have RTM_NEWMADDR/RTM_DELMADDR messages, and they ALSO lack
// working MLDv2 support.  for those platforms, we must tell the OpenThread interface to
// pass ALL multicast packets up; the kernel's IPv6 will filter and drop those that have no listeners
#define OPENTHREAD_POSIX_MULTICAST_PROMISCUOUS_REQUIRED 0
#if !OPENTHREAD_POSIX_USE_MLD_MONITOR
#if defined(__NetBSD__)
#undef OPENTHREAD_POSIX_MULTICAST_PROMISCUOUS_REQUIRED
#define OPENTHREAD_POSIX_MULTICAST_PROMISCUOUS_REQUIRED 1
#endif
#endif

#if defined(__NetBSD__) || defined(__FreeBSD__)
static otError destroyTunnel(void);
#endif

static otInstance *sInstance  = NULL;
static int         sTunFd     = -1; ///< Used to exchange IPv6 packets.
static int         sIpFd      = -1; ///< Used to manage IPv6 stack on Thread interface.
static int         sNetlinkFd = -1; ///< Used to receive netlink events.
#if OPENTHREAD_POSIX_USE_MLD_MONITOR
static int sMLDMonitorFd = -1; ///< Used to receive MLD events.
#endif
static unsigned int sTunIndex = 0;
static char         sTunName[IFNAMSIZ];
#if OPENTHREAD_POSIX_USE_MLD_MONITOR
// ff02::16
static const otIp6Address kMLDv2MulticastAddress = {
    {{0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16}}};

OT_TOOL_PACKED_BEGIN
struct MLDv2Header
{
    uint8_t  mType;
    uint8_t  _rsv0;
    uint16_t mChecksum;
    uint16_t _rsv1;
    uint16_t mNumRecords;
} OT_TOOL_PACKED_END;

OT_TOOL_PACKED_BEGIN
struct MLDv2Record
{
    uint8_t         mRecordType;
    uint8_t         mAuxDataLen;
    uint16_t        mNumSources;
    struct in6_addr mMulticastAddress;
} OT_TOOL_PACKED_END;

enum
{
    kICMPv6MLDv2Type                      = 143,
    kICMPv6MLDv2RecordChangeToExcludeType = 3,
    kICMPv6MLDv2RecordChangeToIncludeType = 4,
};
#endif

static const size_t kMaxIp6Size = 1536;

static bool gLogPackets = false;

static void dump_hex(const void *buffer, size_t len)
{
    char           ascii_buffer[sizeof("xxxx xxxx") + 1];
    const uint8_t *buf = (const uint8_t *)buffer;
    size_t         i;
    ascii_buffer[0] = 0;
    for (i = 0; i < len; i++)
    {
        uint16_t val = (*buf) & 0x00FF;
        buf++;

        ascii_buffer[i % 8]     = isprint(val) ? (char)val : '.';
        ascii_buffer[i % 8 + 1] = 0;

        if ((i % 8) == 0)
        {
            if (i != 0)
            {
                fprintf(stderr, "\r\n");
            }
            fprintf(stderr, "\t");
        }
        if ((i % 4) == 0)
            fprintf(stderr, "\t");

        fprintf(stderr, "%02hX ", val);

        if (((i + 1) % 8) == 0)
        {
            fprintf(stderr, "\t%s", ascii_buffer);
            ascii_buffer[0] = 0;
        }
    }
    while ((i % 8) != 0)
    {
        if ((i % 4) == 0)
        {
            fprintf(stderr, "\t");
        }

        fprintf(stderr, "   ");
        i++;
    }
    fprintf(stderr, "\t%s\r\n", ascii_buffer);
}

#if !defined(__linux__)
static bool unicastAddressIsSubscribed(otInstance *aInstance, const otNetifAddress *netAddr)
{
    const otNetifAddress *address = otIp6GetUnicastAddresses(aInstance);

    while (address != NULL)
    {
        if (memcmp(address->mAddress.mFields.m8, netAddr->mAddress.mFields.m8, sizeof(address->mAddress.mFields.m8)) ==
            0)
        {
            return true;
        }

        address = address->mNext;
    }

    return false;
}
#endif

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
static inline void apply_mask(struct in6_addr *address, uint8_t mask)
{
    if (mask > 128)
    {
        mask = 128;
    }

    memset((void *)(address->s6_addr + ((mask + 7) / 8)), 0, 16 - ((mask + 7) / 8));

    if (mask % 8)
    {
        address->s6_addr[mask / 8] &= ~(0xFF >> (mask % 8));
    }
}

static unsigned int calculate_prefix_len(const struct sockaddr_in6 *sa6)
{
    unsigned int result = 0;

    unsigned int i;

    for (i = 0; i < sizeof(sa6->sin6_addr.s6_addr); i++)
    {
        if (sa6->sin6_addr.s6_addr[i] == 0xFF)
        {
            result += 8;
            continue;
        }
        else
        {
            int     j;
            uint8_t byte = sa6->sin6_addr.s6_addr[i];

            for (j = 7; j >= 0; j--)
            {
                if (byte & (1 << j))
                    result++;
                else
                    break;
            }

            break;
        }
    }

    return result;
}
#endif

static void UpdateUnicast(otInstance *aInstance, const otIp6Address &aAddress, uint8_t aPrefixLength, bool aIsAdded)
{
    otError error = OT_ERROR_NONE;

    assert(sInstance == aInstance);

    VerifyOrExit(sIpFd >= 0, error = OT_ERROR_INVALID_STATE);

#if defined(__linux__)
    {
        struct in6_ifreq ifr6;
        memcpy(&ifr6.ifr6_addr, &aAddress, sizeof(ifr6.ifr6_addr));

        ifr6.ifr6_ifindex   = static_cast<int>(sTunIndex);
        ifr6.ifr6_prefixlen = aPrefixLength;

        if (aIsAdded)
        {
            VerifyOrDie(ioctl(sIpFd, SIOCSIFADDR, &ifr6) == 0, OT_EXIT_ERROR_ERRNO);
        }
        else
        {
            VerifyOrExit(ioctl(sIpFd, SIOCDIFADDR, &ifr6) == 0, perror("ioctl"); error = OT_ERROR_FAILED);
        }
    }
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    {
        int                 err;
        struct in6_aliasreq ifr6;

        memset(&ifr6, 0, sizeof(ifr6));
        strlcpy(ifr6.ifra_name, sTunName, sizeof(ifr6.ifra_name));
        ifr6.ifra_addr.sin6_family = AF_INET6;
        ifr6.ifra_addr.sin6_len    = sizeof(ifr6.ifra_addr);
        memcpy(&ifr6.ifra_addr.sin6_addr, &aAddress, sizeof(struct in6_addr));
        ifr6.ifra_prefixmask.sin6_family = AF_INET6;
        ifr6.ifra_prefixmask.sin6_len    = sizeof(ifr6.ifra_prefixmask);
        memset(&ifr6.ifra_prefixmask.sin6_addr, 0xFF, 16);
        apply_mask(&ifr6.ifra_prefixmask.sin6_addr, aPrefixLength);
        ifr6.ifra_lifetime.ia6t_vltime    = ND6_INFINITE_LIFETIME;
        ifr6.ifra_lifetime.ia6t_pltime    = ND6_INFINITE_LIFETIME;

#if defined(__APPLE__)
        ifr6.ifra_lifetime.ia6t_expire    = ND6_INFINITE_LIFETIME;
        ifr6.ifra_lifetime.ia6t_preferred = ND6_INFINITE_LIFETIME;

        ifr6.ifra_flags |= IN6_IFF_NODAD;
#endif

        err = ioctl(sIpFd, aIsAdded ? SIOCAIFADDR_IN6 : SIOCDIFADDR_IN6, &ifr6);
        if ((err == -1) && (errno == EALREADY))
        {
            err = 0;
        }
        VerifyOrExit(err == 0, perror("ioctl"); error = OT_ERROR_FAILED);
    }
#endif

exit:
    if (error != OT_ERROR_NONE)
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

static void UpdateMulticast(otInstance *aInstance, const otIp6Address &aAddress, bool aIsAdded)
{
    struct ipv6_mreq mreq;
    otError          error = OT_ERROR_NONE;

    assert(sInstance == aInstance);

    VerifyOrExit(sIpFd >= 0, OT_NOOP);
    memcpy(&mreq.ipv6mr_multiaddr, &aAddress, sizeof(mreq.ipv6mr_multiaddr));
    mreq.ipv6mr_interface = sTunIndex;

    int err;
    err = setsockopt(sIpFd, IPPROTO_IPV6, (aIsAdded ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP), &mreq, sizeof(mreq));
    if ((err != 0) && (errno == EINVAL) && (IN6_IS_ADDR_MC_LINKLOCAL(&mreq.ipv6mr_multiaddr)))
    {
        // FIX ME
        // on mac OS (and FreeBSD), the first time we run (but not subsequently), we get a failure on this particular
        // join. do we need to bring up the interface at least once prior to joining?

        char addressString[INET6_ADDRSTRLEN + 1];
        inet_ntop(AF_INET6, mreq.ipv6mr_multiaddr.s6_addr, addressString, sizeof(addressString));
        otLogWarnPlat("thread: ignoring %s failure (EINVAL) for MC LINKLOCAL address (%s)\r\n",
                      aIsAdded ? "IPV6_JOIN_GROUP" : "IPV6_LEAVE_GROUP", addressString);
        err = 0;
    }

    if (err != 0)
    {
        otLogWarnPlat(stderr, "%s failure (%d)\r\n", aIsAdded ? "IPV6_JOIN_GROUP" : "IPV6_LEAVE_GROUP", errno);
    }

    VerifyOrExit(err == 0, perror("setsockopt"); error = OT_ERROR_FAILED);

exit:
    SuccessOrDie(error);
    otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
}

static void UpdateLink(otInstance *aInstance)
{
    otError      error = OT_ERROR_NONE;
    struct ifreq ifr;
    bool         if_state = false;
    bool         ot_state = false;

    assert(sInstance == aInstance);

    VerifyOrExit(sIpFd >= 0, OT_NOOP);
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, sTunName, sizeof(ifr.ifr_name));
    VerifyOrExit(ioctl(sIpFd, SIOCGIFFLAGS, &ifr) == 0, perror("ioctl"); error = OT_ERROR_FAILED);

    if_state = ((ifr.ifr_flags & IFF_UP) == IFF_UP) ? true : false;
    ot_state = otIp6IsEnabled(aInstance);

    otLogNotePlat("thread: changing interface state to %s%s.\n", ot_state ? "UP" : "DOWN",
                  (if_state == ot_state) ? " (already set, ignoring)" : "");
    if (if_state != ot_state)
    {
        ifr.ifr_flags = ot_state ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);
        VerifyOrExit(ioctl(sIpFd, SIOCSIFFLAGS, &ifr) == 0, perror("ioctl"); error = OT_ERROR_FAILED);
    }

exit:
    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

static void processAddressChange(const otIp6Address *aAddress, uint8_t aPrefixLength, bool aIsAdded, void *aContext)
{
    if (aAddress->mFields.m8[0] == 0xff)
    {
        UpdateMulticast(static_cast<otInstance *>(aContext), *aAddress, aIsAdded);
    }
    else
    {
        UpdateUnicast(static_cast<otInstance *>(aContext), *aAddress, aPrefixLength, aIsAdded);
    }
}

static void processStateChange(otChangedFlags aFlags, void *aContext)
{
    if (OT_CHANGED_THREAD_NETIF_STATE | aFlags)
    {
        UpdateLink(static_cast<otInstance *>(aContext));
    }
}

static void processReceive(otMessage *aMessage, void *aContext)
{
    char     packet[kMaxIp6Size + 4];
    otError  error      = OT_ERROR_NONE;
    uint16_t length     = otMessageGetLength(aMessage);
    size_t   offset     = 0;
    uint16_t max_length = sizeof(packet) - 4;
#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    offset += 4;
#endif

    assert(sInstance == aContext);

    VerifyOrExit(sTunFd > 0, OT_NOOP);

    VerifyOrExit(otMessageRead(aMessage, 0, &packet[offset], max_length) == length, error = OT_ERROR_NO_BUFS);

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    packet[0] = 0;
    packet[1] = 0;
    packet[2] = (PF_INET6 << 8) & 0xFF;
    packet[3] = (PF_INET6 << 0) & 0xFF;
    length += 4;
#endif

    if (gLogPackets)
    {
        fprintf(stderr, "Packet from NCP (%hu bytes)\r\n", (uint16_t)(length - 4));
        dump_hex(&packet[4], length - 4);
        fflush(stdout);
    }

    VerifyOrExit(write(sTunFd, packet, length) == length, perror("write"); error = OT_ERROR_FAILED);

exit:
    otMessageFree(aMessage);

    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

static void processTransmit(otInstance *aInstance)
{
    otMessage *message = NULL;
    ssize_t    rval;
    char       packet[kMaxIp6Size];
    otError    error  = OT_ERROR_NONE;
    size_t     offset = 0;

    assert(sInstance == aInstance);

    rval = read(sTunFd, packet, sizeof(packet));
    VerifyOrExit(rval > 0, error = OT_ERROR_FAILED);

    message = otIp6NewMessage(aInstance, NULL);
    VerifyOrExit(message != NULL, error = OT_ERROR_NO_BUFS);

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    if ((rval >= 4) && (packet[0] == 0) && (packet[1] == 0))
    {
        rval -= 4;
        offset = 4;
    }
#endif

    if (gLogPackets)
    {
        fprintf(stderr, "Packet to NCP (%ld bytes)\r\n", (long)rval);
        dump_hex(&packet[offset], (size_t)rval);
        fflush(stdout);
    }

    SuccessOrExit(error = otMessageAppend(message, &packet[offset], static_cast<uint16_t>(rval)));

    error   = otIp6Send(aInstance, message);
    message = NULL;

exit:
    if (message != NULL)
    {
        otMessageFree(message);
    }

    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

#if defined(__linux__)

static void processNetifAddrEvent(otInstance *aInstance, struct nlmsghdr *aNetlinkMessage)
{
    struct ifaddrmsg *  ifaddr = reinterpret_cast<struct ifaddrmsg *>(NLMSG_DATA(aNetlinkMessage));
    size_t              rtaLength;
    otError             error = OT_ERROR_NONE;
    struct sockaddr_in6 addr6;
    char                addressString[INET6_ADDRSTRLEN + 1];

    (void)addressString; // to silence warnings if otLog...Plat is defined as a no-op

    VerifyOrExit(ifaddr->ifa_index == static_cast<unsigned int>(sTunIndex) && ifaddr->ifa_family == AF_INET6, OT_NOOP);

    rtaLength = IFA_PAYLOAD(aNetlinkMessage);

    for (struct rtattr *rta = reinterpret_cast<struct rtattr *>(IFA_RTA(ifaddr)); RTA_OK(rta, rtaLength);
         rta                = RTA_NEXT(rta, rtaLength))
    {
        switch (rta->rta_type)
        {
        case IFA_ADDRESS:
        case IFA_LOCAL:
        case IFA_BROADCAST:
        case IFA_ANYCAST:
        case IFA_MULTICAST:
        {
            ot::Ip6::Address addr;
            memcpy(&addr, RTA_DATA(rta), sizeof(addr));

            memset(&addr6, 0, sizeof(addr6));
            addr6.sin6_family = AF_INET6;
            memcpy(&addr6.sin6_addr, RTA_DATA(rta), sizeof(addr6.sin6_addr));

            if (aNetlinkMessage->nlmsg_type == RTM_NEWADDR)
            {
                if (!addr.IsMulticast())
                {
                    otNetifAddress netAddr;

                    netAddr.mAddress      = addr;
                    netAddr.mPrefixLength = ifaddr->ifa_prefixlen;

                    error = otIp6AddUnicastAddress(aInstance, &netAddr);

                    if ((error == OT_ERROR_NONE) || (error == OT_ERROR_ALREADY))
                    {
                        otLogNotePlat(
                            "thread: ADD [U] %s%s\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                            error == OT_ERROR_ALREADY ? " (already subscribed, ignored)" : "");
                    }
                    else
                    {
                        otLogWarnPlat(
                            "thread: ADD [U] %s failed (%s)\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                            otThreadErrorToString(error));
                    }

                    SuccessOrExit(error);
                }
                else
                {
                    otNetifMulticastAddress netAddr;

                    netAddr.mAddress = addr;

                    error = otIp6SubscribeMulticastAddress(aInstance, &addr);
                    if ((error == OT_ERROR_NONE) || (error == OT_ERROR_ALREADY))
                    {
                        otLogNotePlat(
                            stderr, "thread: ADD [M] %s%s\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                            error == OT_ERROR_ALREADY ? " (already subscribed, ignored)" : "");
                        error = OT_ERROR_NONE;
                    }
                    else
                    {
                        otLogWarnPlat(
                            "thread: ADD [M] %s failed (%s)\r\n",
                            inet_ntop(AF_INET6, mreq.ipv6mr_multiaddr.s6_addr, addressString, sizeof(addressString)),
                            otThreadErrorToString(error));
                    }
                    SuccessOrExit(error);
                }
            }
            else if (aNetlinkMessage->nlmsg_type == RTM_DELADDR)
            {
                if (!addr.IsMulticast())
                {
                    error = otIp6RemoveUnicastAddress(aInstance, &addr);
                    if ((error == OT_ERROR_NONE) || (error == OT_ERROR_NOT_FOUND))
                    {
                        otLogNotePlat(
                            "thread: DEL [U] %s%s\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                            error == OT_ERROR_NOT_FOUND ? " (not found, ignored)" : "");

                        error = OT_ERROR_NONE;
                    }
                    else
                    {
                        otLogWarnPlat(
                            "thread: DEL [U] %s failure (%d)\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)), error);
                    }
                }
                else
                {
                    error = otIp6UnsubscribeMulticastAddress(aInstance, &addr);
                    if ((error == OT_ERROR_NONE) || (error == OT_ERROR_NOT_FOUND))
                    {
                        otLogNotePlat(
                            "thread: DEL [M] %s%s\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                            error == OT_ERROR_NOT_FOUND ? " (not found, ignored)" : "");
                        error = OT_ERROR_NONE;
                    }
                    else
                    {
                        otLogWarnPlat(
                            "thread: DEL [M] %s failure (%d)\r\n",
                            inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)), error);
                    }
                }
            }
            else
            {
                continue;
            }
            break;
        }

        default:
            otLogWarnPlat("thread: unexpected address type (%d)\n", (int)rta->rta_type);
            break;
        }
    }

exit:
    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

static void processNetifLinkEvent(otInstance *aInstance, struct nlmsghdr *aNetlinkMessage)
{
    struct ifinfomsg *ifinfo = reinterpret_cast<struct ifinfomsg *>(NLMSG_DATA(aNetlinkMessage));
    otError           error  = OT_ERROR_NONE;

    VerifyOrExit(ifinfo->ifi_index == static_cast<int>(sTunIndex), OT_NOOP);
    SuccessOrExit(error = otIp6SetEnabled(aInstance, ifinfo->ifi_flags & IFF_UP));

exit:
    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}
#endif

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)

#if defined(__FreeBSD__)
// is this needed?
#define ROUNDUP(a) ((a) > 0 ? (1 + (((a)-1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))
#endif

#if defined(__APPLE__)
#define ROUNDUP(a) ((a) > 0 ? (1 + (((a)-1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))
#define DARWIN_SA_SIZE(sa) ROUNDUP(sa->sa_len)
#define SA_SIZE(sa) DARWIN_SA_SIZE(sa)
#endif

#if defined(__NetBSD__)
#define RT_ROUNDUP2(a, n) ((a) > 0 ? (1 + (((a)-1U) | ((n)-1))) : (n))
#define RT_ROUNDUP(a) RT_ROUNDUP2((a), sizeof(uint64_t))
#define SA_SIZE(sa) RT_ROUNDUP(sa->sa_len)
#endif

static void processNetifAddrEvent(otInstance *aInstance, struct rt_msghdr *rtm) //, size_t buffer_len)
{
    otError            error;
    struct ifa_msghdr *ifam;
#ifdef RTM_NEWMADDR
    struct ifma_msghdr *ifmam;
#endif
    struct sockaddr_in6 addr6;
    struct sockaddr_in6 netmask;
    uint8_t *           addrbuf;
    unsigned int        addrlen;
    unsigned int        addrmask = 0;
    unsigned int        i;
    struct sockaddr *   sa;
    bool                is_link_local;
    char                addressString[INET6_ADDRSTRLEN + 1];
    size_t              buffer_len = rtm->rtm_msglen;

    addr6.sin6_family   = 0;
    netmask.sin6_family = 0;

    if ((rtm->rtm_type == RTM_NEWADDR) || (rtm->rtm_type == RTM_DELADDR))
    {
        ifam = reinterpret_cast<struct ifa_msghdr *>(rtm);

        VerifyOrExit(ifam->ifam_index == static_cast<unsigned int>(sTunIndex), OT_NOOP);

        addrbuf  = (uint8_t *)&ifam[1];
        addrmask = (unsigned int)ifam->ifam_addrs;
    }
#ifdef RTM_NEWMADDR
    else if ((rtm->rtm_type == RTM_NEWMADDR) || (rtm->rtm_type == RTM_DELMADDR))
    {
        ifmam = reinterpret_cast<struct ifma_msghdr *>(rtm);

        VerifyOrExit(ifmam->ifmam_index == static_cast<unsigned int>(sTunIndex), OT_NOOP);

        addrbuf  = (uint8_t *)&ifmam[1];
        addrmask = (unsigned int)ifmam->ifmam_addrs;
    }
#endif
    addrlen = (unsigned int)buffer_len;

    if (addrmask != 0)
    {
        for (i = 0; i < RTAX_MAX; i++)
        {
            unsigned int mask = (addrmask & (1 << i));
            if (mask)
            {
                sa = (struct sockaddr *)addrbuf;

                if (sa->sa_family == AF_INET6)
                {
                    if (i == RTAX_IFA)
                        memcpy(&addr6, sa, sizeof(sockaddr_in6));
                    if (i == RTAX_NETMASK)
                        memcpy(&netmask, sa, sizeof(sockaddr_in6));
                }
                addrlen -= SA_SIZE(sa);
                addrbuf += SA_SIZE(sa);
            }
        }
    }

    if (addr6.sin6_family == AF_INET6)
    {
        is_link_local = false;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6.sin6_addr))
        {
            is_link_local = true;
            // clear the scope -- Mac OS X sends this to us (bozos!)
            addr6.sin6_addr.s6_addr[3] = 0;
        }
        else if (IN6_IS_ADDR_MC_LINKLOCAL(&addr6.sin6_addr))
        {
            addr6.sin6_addr.s6_addr[3] = 0;
        }

        ot::Ip6::Address addr;
        memcpy(&addr, &addr6.sin6_addr, sizeof(addr));

        if (rtm->rtm_type == RTM_NEWADDR
#ifdef RTM_NEWMADDR
            || rtm->rtm_type == RTM_NEWMADDR
#endif
        )
        {
            if (!addr.IsMulticast())
            {
                otNetifAddress netAddr;
                bool           subscribed;

                netAddr.mAddress      = addr;
                netAddr.mPrefixLength = (uint8_t)calculate_prefix_len(&netmask);

                subscribed = unicastAddressIsSubscribed(aInstance, &netAddr);

                if (subscribed)
                {
                    inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString));
                    otLogNotePlat("thread: ADD [U] %s (already subscribed, ignored)\r\n", addressString);
                }
                else
                {
                    if (is_link_local)
                    {
                        // remove the stack-added link-local address

                        int                 err;
                        struct in6_aliasreq ifr6;

                        memset(&ifr6, 0, sizeof(ifr6));
                        strlcpy(ifr6.ifra_name, sTunName, sizeof(ifr6.ifra_name));
                        ifr6.ifra_addr.sin6_family = AF_INET6;
                        ifr6.ifra_addr.sin6_len    = sizeof(ifr6.ifra_addr);
                        memcpy(&ifr6.ifra_addr.sin6_addr, &addr6.sin6_addr, sizeof(struct in6_addr));
                        ifr6.ifra_prefixmask.sin6_family = AF_INET6;
                        ifr6.ifra_prefixmask.sin6_len    = sizeof(ifr6.ifra_prefixmask);
                        memset(&ifr6.ifra_prefixmask.sin6_addr, 0xFF, 16);
                        apply_mask(&ifr6.ifra_prefixmask.sin6_addr, netAddr.mPrefixLength);
                        ifr6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
                        ifr6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

#if defined(__APPLE__)
                        ifr6.ifra_lifetime.ia6t_expire    = ND6_INFINITE_LIFETIME;
                        ifr6.ifra_lifetime.ia6t_preferred = ND6_INFINITE_LIFETIME;

                        ifr6.ifra_flags |= IN6_IFF_NODAD;
#endif

                        err = ioctl(sIpFd, SIOCDIFADDR_IN6, &ifr6);
                        if (err != 0)
                        {
                            otLogWarnPlat(
                                "thread: error (%d) removing stack-addded link-local address %s\r\n", errno,
                                inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)));
                        }
                        else
                        {
                            otLogNotePlat(
                                "thread:         %s (removed stack-added link-local)\r\n",
                                inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)));
                        }
                    }
                    else
                    {
                        // we don't know about this address -- we need to add it
                        error = otIp6AddUnicastAddress(aInstance, &netAddr);

                        if ((error == OT_ERROR_NONE) || (error == OT_ERROR_ALREADY))
                        {
                            otLogNotePlat(
                                "thread: ADD [U] %s%s\r\n",
                                inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                error == OT_ERROR_ALREADY ? " (already subscribed, ignored)" : "");
                        }
                        else
                        {
                            otLogWarnPlat(
                                "thread: ADD [U] %s failed (%s)\r\n",
                                inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                otThreadErrorToString(error));
                        }
                    }
                }
            }
            else
            {
                otNetifMulticastAddress netAddr;
                netAddr.mAddress = addr;

                error = otIp6SubscribeMulticastAddress(aInstance, &addr);
                if ((error == OT_ERROR_NONE) || (error == OT_ERROR_ALREADY))
                {
                    otLogNotePlat(stderr, "thread: ADD [M] %s%s\r\n",
                                  inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                  error == OT_ERROR_ALREADY ? " (already subscribed, ignored)" : "");
                    error = OT_ERROR_NONE;
                }
                else
                {
                    otLogWarnPlat(
                        "thread: ADD [M] %s failed (%s)\r\n",
                        inet_ntop(AF_INET6, mreq.ipv6mr_multiaddr.s6_addr, addressString, sizeof(addressString)),
                        otThreadErrorToString(error));
                }
                SuccessOrExit(error);
            }
        }
        else if (rtm->rtm_type == RTM_DELADDR
#ifdef RTM_DELMADDR
                 || rtm->rtm_type == RTM_DELMADDR
#endif
        )
        {
            if (!addr.IsMulticast())
            {
                error = otIp6RemoveUnicastAddress(aInstance, &addr);
                if ((error == OT_ERROR_NONE) || (error == OT_ERROR_NOT_FOUND))
                {
                    otLogNotePlat("thread: DEL [U] %s%s\r\n",
                                  inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                  error == OT_ERROR_NOT_FOUND ? " (not found, ignored)" : "");

                    error = OT_ERROR_NONE;
                }
                else
                {
                    otLogWarnPlat("thread: DEL [U] %s failure (%d)\r\n",
                                  inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                  error);
                }
            }
            else
            {
                error = otIp6UnsubscribeMulticastAddress(aInstance, &addr);
                if ((error == OT_ERROR_NONE) || (error == OT_ERROR_NOT_FOUND))
                {
                    otLogNotePlat("thread: DEL [M] %s%s\r\n",
                                  inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                  error == OT_ERROR_NOT_FOUND ? " (not found, ignored)" : "");
                    error = OT_ERROR_NONE;
                }
                else
                {
                    otLogWarnPlat("thread: DEL [M] %s failure (%d)\r\n",
                                  inet_ntop(AF_INET6, addr6.sin6_addr.s6_addr, addressString, sizeof(addressString)),
                                  error);
                }
            }
        }
    }

exit:;
}

static void processNetifInfoEvent(otInstance *aInstance, struct rt_msghdr *rtm)
{
    struct if_msghdr *ifm   = reinterpret_cast<struct if_msghdr *>(rtm);
    otError           error = OT_ERROR_NONE;

    VerifyOrExit(ifm->ifm_index == static_cast<int>(sTunIndex), OT_NOOP);

    UpdateLink(aInstance);

exit:
    if (error == OT_ERROR_NONE)
    {
        otLogInfoPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
    else
    {
        otLogWarnPlat("%s: %s", __func__, otThreadErrorToString(error));
    }
}

#endif

static void processNetifEvent(otInstance *aInstance)
{
    const size_t kMaxNetifEvent = 8192;
    ssize_t      length;
    char         buffer[kMaxNetifEvent];

    length = recv(sNetlinkFd, buffer, sizeof(buffer), 0);

    VerifyOrExit(length > 0, OT_NOOP);

#if defined(__linux__)
    for (struct nlmsghdr *msg = reinterpret_cast<struct nlmsghdr *>(buffer); NLMSG_OK(msg, length);
         msg                  = NLMSG_NEXT(msg, length))
    {
#else
    {
        // BSD sends one message per read to routing socket (see route.c, monitor command)
        struct rt_msghdr *msg;

        msg = (struct rt_msghdr *)buffer;

#define nlmsg_type rtm_type

#endif
        switch (msg->nlmsg_type)
        {
        case RTM_NEWADDR:
        case RTM_DELADDR:
            processNetifAddrEvent(aInstance, msg);
            break;

#if defined(RTM_NEWLINK) && defined(RTM_DELLINK)
        case RTM_NEWLINK:
        case RTM_DELLINK:
            processNetifLinkEvent(aInstance, msg);
            break;
#endif

#if defined(RTM_NEWMADDR) && defined(RTM_DELMADDR)
        case RTM_NEWMADDR:
        case RTM_DELMADDR:
            processNetifAddrEvent(aInstance, msg); //, (size_t)length );
            break;
#endif

#if !defined(__linux__)
        case RTM_IFINFO:
            processNetifInfoEvent(aInstance, msg);
            break;
#endif

        default:
            otLogWarnPlat("thread: unhandled/unexpected netlink/route message (%d)\n", (int)msg->nlmsg_type);
            break;
        }
    }

exit:
    return;
}

void platformNetifDeinit(void)
{
    if (sTunFd != -1)
    {
        close(sTunFd);
        sTunFd = -1;

#if defined(__NetBSD__) || defined(__FreeBSD__)
        destroyTunnel();
#endif
    }

    if (sIpFd != -1)
    {
        close(sIpFd);
        sIpFd = -1;
    }

    if (sNetlinkFd != -1)
    {
        close(sNetlinkFd);
        sNetlinkFd = -1;
    }

#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    if (sMLDMonitorFd != -1)
    {
        close(sMLDMonitorFd);
        sMLDMonitorFd = -1;
    }
#endif

    sTunIndex = 0;
}

#if OPENTHREAD_POSIX_USE_MLD_MONITOR
static void mldListenerInit(void)
{
    struct ipv6_mreq mreq6;

    sMLDMonitorFd          = SocketWithCloseExec(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6, kSocketNonBlock);
    mreq6.ipv6mr_interface = sTunIndex;
    memcpy(&mreq6.ipv6mr_multiaddr, kMLDv2MulticastAddress.mFields.m8, sizeof(kMLDv2MulticastAddress.mFields.m8));

    VerifyOrDie(setsockopt(sMLDMonitorFd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6)) == 0, OT_EXIT_FAILURE);
#if defined(__linux__)
    VerifyOrDie(setsockopt(sMLDMonitorFd, SOL_SOCKET, SO_BINDTODEVICE, sTunName,
                           static_cast<socklen_t>(strnlen(sTunName, IFNAMSIZ))) == 0,
                OT_EXIT_FAILURE);
#endif
}

static void processMLDEvent(otInstance *aInstance)
{
    const size_t        kMaxMLDEvent = 8192;
    uint8_t             buffer[kMaxMLDEvent];
    ssize_t             bufferLen = -1;
    struct sockaddr_in6 srcAddr;
    socklen_t           addrLen  = sizeof(srcAddr);
    bool                fromSelf = false;
    MLDv2Header *       hdr      = reinterpret_cast<MLDv2Header *>(buffer);
    size_t              offset;
    uint8_t             type;
    struct ifaddrs *    ifAddrs = NULL;
    char                addressString[INET6_ADDRSTRLEN + 1];

    bufferLen = recvfrom(sMLDMonitorFd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&srcAddr), &addrLen);
    VerifyOrExit(bufferLen > 0, OT_NOOP);

    type = buffer[0];
    VerifyOrExit(type == kICMPv6MLDv2Type && bufferLen >= static_cast<ssize_t>(sizeof(MLDv2Header)), OT_NOOP);

    // Check whether it is sent by self
    VerifyOrExit(getifaddrs(&ifAddrs) == 0, OT_NOOP);
    for (struct ifaddrs *ifAddr = ifAddrs; ifAddr != NULL; ifAddr = ifAddr->ifa_next)
    {
        if (ifAddr->ifa_addr != NULL && ifAddr->ifa_addr->sa_family == AF_INET6 &&
            strncmp(sTunName, ifAddr->ifa_name, IFNAMSIZ) == 0)
        {
            struct sockaddr_in6 *addr6 = reinterpret_cast<struct sockaddr_in6 *>(ifAddr->ifa_addr);

            if (memcmp(&addr6->sin6_addr, &srcAddr.sin6_addr, sizeof(in6_addr)) == 0)
            {
                fromSelf = true;
                break;
            }
        }
    }
    VerifyOrExit(fromSelf, OT_NOOP);

    hdr    = reinterpret_cast<MLDv2Header *>(buffer);
    offset = sizeof(MLDv2Header);

    for (size_t i = 0; i < ntohs(hdr->mNumRecords) && offset < static_cast<size_t>(bufferLen); i++)
    {
        if (static_cast<size_t>(bufferLen) >= (sizeof(MLDv2Record) + offset))
        {
            MLDv2Record *record = reinterpret_cast<MLDv2Record *>(&buffer[offset]);

            otError      err;
            otIp6Address address;

            memcpy(&address.mFields.m8, &record->mMulticastAddress, sizeof(address.mFields.m8));
            inet_ntop(AF_INET6, &record->mMulticastAddress, addressString, sizeof(addressString));
            if (record->mRecordType == kICMPv6MLDv2RecordChangeToIncludeType)
            {
                err = otIp6SubscribeMulticastAddress(aInstance, &address);
                if (err == OT_ERROR_ALREADY)
                {
                    otLogNotePlat(
                        "Will not subscribe duplicate multicast address %s",
                        inet_ntop(AF_INET6, &record->mMulticastAddress, addressString, sizeof(addressString)));
                }
                else if (err != OT_ERROR_NONE)
                {
                    otLogWarnPlat("Failed to subscribe multicast address %s: %s", addressString,
                                  otThreadErrorToString(err));
                }
                else
                {
                    otLogDebgPlat("Subscribed multicast address %s", addressString);
                }
            }
            else if (record->mRecordType == kICMPv6MLDv2RecordChangeToExcludeType)
            {
                err = otIp6UnsubscribeMulticastAddress(aInstance, &address);
                if (err != OT_ERROR_NONE)
                {
                    otLogWarnPlat("Failed to unsubscribe multicast address %s: %s", addressString,
                                  otThreadErrorToString(err));
                }
                else
                {
                    otLogDebgPlat("Unsubscribed multicast address %s", addressString);
                }
            }

            offset += sizeof(MLDv2Record) + sizeof(in6_addr) * ntohs(record->mNumSources);
        }
    }

exit:
    if (ifAddrs)
    {
        freeifaddrs(ifAddrs);
    }

    return;
}
#endif

#if defined(__linux__)
// set up the tun device
static void platformConfigureTunDevice(otInstance *aInstance,
                                       const char *aInterfaceName,
                                       char *      deviceName,
                                       size_t      deviceNameLen)
{
    struct ifreq ifr;

    (void)aInstance;

    sTunFd = open(OPENTHREAD_POSIX_TUN_DEVICE, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    VerifyOrDie(sTunFd >= 0, OT_EXIT_ERROR_ERRNO);

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (aInterfaceName)
    {
        VerifyOrDie(strlen(aInterfaceName) < IFNAMSIZ, OT_EXIT_INVALID_ARGUMENTS);

        strncpy(ifr.ifr_name, aInterfaceName, IFNAMSIZ);
    }
    else
    {
        strncpy(ifr.ifr_name, "wpan%d", IFNAMSIZ);
    }

    VerifyOrDie(ioctl(sTunFd, TUNSETIFF, static_cast<void *>(&ifr)) == 0, OT_EXIT_ERROR_ERRNO);
    VerifyOrDie(ioctl(sTunFd, TUNSETLINK, ARPHRD_VOID) == 0, OT_EXIT_ERROR_ERRNO);

    strncpy(deviceName, ifr.ifr_name, deviceNameLen);
}
#endif

#if defined(__APPLE__) && USE_APPLE_UTUN
static void platformConfigureTunDevice(otInstance *aInstance,
                                       const char *aInterfaceName,
                                       char *      deviceName,
                                       size_t      deviceNameLen)
{
    (void)aInterfaceName;
    int                 err = 0;
    struct sockaddr_ctl addr;
    struct ctl_info     info;

    (void)aInstance;

    sTunFd = NonBlockSocketWithCloseExec(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    VerifyOrDie(sTunFd >= 0, OT_EXIT_ERROR_ERRNO);

    memset(&info, 0, sizeof(info));
    strncpy(info.ctl_name, UTUN_CONTROL_NAME, strlen(UTUN_CONTROL_NAME));
    err = ioctl(fd, CTLIOCGINFO, &info);
    VerifyOrDie(err == 0, OT_EXIT_ERROR_ERRNO);

    addr.sc_id      = info.ctl_id;
    addr.sc_len     = sizeof(addr);
    addr.sc_family  = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;

    addr.sc_unit = 0;
    err          = connect(sTunFd, (struct sockaddr *)&addr, sizeof(addr));
    VerifyOrDie(err == 0, OT_EXIT_ERROR_ERRNO);

    socklen_t devNameLen;
    devNameLen = (socklen_t)deviceNameLen;
    err        = getsockopt(sTunFd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, deviceName, &devNameLen);
    VerifyOrDie(err == 0, OT_EXIT_ERROR_ERRNO);

    otLogInfoPlat("Tunnel device name = '%s'\r\n", deviceName);
}
#endif

#if defined(__NetBSD__) || defined(__FreeBSD__)
static otError destroyTunnel(void)
{
    otError      error;
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, sTunName, sizeof(ifr.ifr_name));
    VerifyOrExit(ioctl(sIpFd, SIOCIFDESTROY, &ifr) == 0, perror("ioctl"); error = OT_ERROR_FAILED);
    error = OT_ERROR_NONE;

exit:

    return error;
}
#endif

#if defined(__NetBSD__) || (defined(__APPLE__) && USE_TUN) || defined(__FreeBSD__)
static void platformConfigureTunDevice(otInstance *aInstance,
                                       const char *aInterfaceName,
                                       char *      deviceName,
                                       size_t      deviceNameLen)
{
    int         flags = IFF_BROADCAST | IFF_MULTICAST;
    int         err;
    const char *last_slash;
    const char *path;

    (void)aInterfaceName;
    (void)aInstance;

    path = OPENTHREAD_POSIX_TUN_DEVICE;

    sTunFd = open(path, O_RDWR | O_NONBLOCK);
    VerifyOrDie(sTunFd >= 0, OT_EXIT_ERROR_ERRNO);

#if defined(__NetBSD__) || defined(__FreeBSD__)
    err = ioctl(sTunFd, TUNSIFMODE, &flags);
    VerifyOrDie(err == 0, OT_EXIT_ERROR_ERRNO);
#endif

    flags = 1;
    err   = ioctl(sTunFd, TUNSIFHEAD, &flags);
    VerifyOrDie(err == 0, OT_EXIT_ERROR_ERRNO);

    last_slash = strrchr(OPENTHREAD_POSIX_TUN_DEVICE, '/');
    VerifyOrDie(last_slash != NULL, OT_EXIT_ERROR_ERRNO);
    last_slash++;

    strncpy(deviceName, last_slash, deviceNameLen);
}
#endif

static void platformConfigureNetLink(void)
{
#if defined(__linux__)
    sNetlinkFd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    sNetlinkFd = socket(PF_ROUTE, SOCK_RAW, 0);
#else
#error "!! Unknown platform !!"
#endif
    VerifyOrDie(sNetlinkFd >= 0, OT_EXIT_ERROR_ERRNO);

#if defined(__linux__)
    {
        struct sockaddr_nl sa;

        memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV6_IFADDR;
        VerifyOrDie(bind(sNetlinkFd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0, OT_EXIT_ERROR_ERRNO);
    }
#endif

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
    {
        int status;
#ifdef ROUTE_FILTER
        unsigned int msgfilter = ROUTE_FILTER(RTM_IFINFO) | ROUTE_FILTER(RTM_NEWADDR) | ROUTE_FILTER(RTM_DELADDR) |
                                 ROUTE_FILTER(RTM_NEWMADDR) | ROUTE_FILTER(RTM_DELMADDR);
#define FILTER_CMD ROUTE_MSGFILTER
#define FILTER_ARG msgfilter
#define FILTER_ARG_SZ sizeof(msgfilter)
#endif
#ifdef RO_MSGFILTER
        uint8_t msgfilter[] = {RTM_IFINFO, RTM_NEWADDR, RTM_DELADDR};
#define FILTER_CMD RO_MSGFILTER
#define FILTER_ARG msgfilter
#define FILTER_ARG_SZ sizeof(msgfilter)
#endif
#if defined(ROUTE_FILTER) || defined(RO_MSGFILTER)
        status = setsockopt(sNetlinkFd, AF_ROUTE, FILTER_CMD, FILTER_ARG, FILTER_ARG_SZ);
        VerifyOrDie(status == 0, OT_EXIT_ERROR_ERRNO);
#endif
        status = fcntl(sNetlinkFd, F_SETFL, O_NONBLOCK);
        VerifyOrDie(status == 0, OT_EXIT_ERROR_ERRNO);
    }
#endif // defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__)
}

void platformNetifInit(otInstance *aInstance, const char *aInterfaceName)
{
    sIpFd = SocketWithCloseExec(AF_INET6, SOCK_DGRAM, IPPROTO_IP, kSocketNonBlock);
    VerifyOrDie(sIpFd >= 0, OT_EXIT_ERROR_ERRNO);

    otIcmp6SetEchoMode(aInstance, OT_ICMP6_ECHO_HANDLER_DISABLED);

    platformConfigureNetLink();

    platformConfigureTunDevice(aInstance, aInterfaceName, sTunName, sizeof(sTunName));

    sTunIndex = if_nametoindex(sTunName);
    VerifyOrDie(sTunIndex > 0, OT_EXIT_FAILURE);

#if OPENTHREAD_CONFIG_PLATFORM_UDP_ENABLE
    platformUdpInit(sTunName);
#endif
#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    mldListenerInit();
#endif

    otIp6SetReceiveCallback(aInstance, processReceive, aInstance);
    otIp6SetAddressCallback(aInstance, processAddressChange, aInstance);
    otSetStateChangedCallback(aInstance, processStateChange, aInstance);

#if OPENTHREAD_POSIX_MULTICAST_PROMISCUOUS_REQUIRED
    otIp6SetMulticastPromiscuousEnabled(aInstance, true);
#endif

    sInstance = aInstance;
}

void platformNetifUpdateFdSet(fd_set *aReadFdSet, fd_set *aWriteFdSet, fd_set *aErrorFdSet, int *aMaxFd)
{
    OT_UNUSED_VARIABLE(aWriteFdSet);

    VerifyOrExit(sTunIndex > 0, OT_NOOP);

    assert(sTunFd >= 0);
    assert(sNetlinkFd >= 0);
    assert(sIpFd >= 0);

    FD_SET(sTunFd, aReadFdSet);
    FD_SET(sTunFd, aErrorFdSet);
    FD_SET(sNetlinkFd, aReadFdSet);
    FD_SET(sNetlinkFd, aErrorFdSet);
#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    FD_SET(sMLDMonitorFd, aReadFdSet);
    FD_SET(sMLDMonitorFd, aErrorFdSet);
#endif

    if (sTunFd > *aMaxFd)
    {
        *aMaxFd = sTunFd;
    }

    if (sNetlinkFd > *aMaxFd)
    {
        *aMaxFd = sNetlinkFd;
    }

#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    if (sMLDMonitorFd > *aMaxFd)
    {
        *aMaxFd = sMLDMonitorFd;
    }
#endif
exit:
    return;
}

void platformNetifProcess(const fd_set *aReadFdSet, const fd_set *aWriteFdSet, const fd_set *aErrorFdSet)
{
    OT_UNUSED_VARIABLE(aWriteFdSet);
    VerifyOrExit(sTunIndex > 0, OT_NOOP);

    if (FD_ISSET(sTunFd, aErrorFdSet))
    {
        close(sTunFd);
        DieNow(OT_EXIT_FAILURE);
    }

    if (FD_ISSET(sNetlinkFd, aErrorFdSet))
    {
        close(sNetlinkFd);
        DieNow(OT_EXIT_FAILURE);
    }

#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    if (FD_ISSET(sMLDMonitorFd, aErrorFdSet))
    {
        close(sMLDMonitorFd);
        DieNow(OT_EXIT_FAILURE);
    }
#endif

    if (FD_ISSET(sTunFd, aReadFdSet))
    {
        processTransmit(sInstance);
    }

    if (FD_ISSET(sNetlinkFd, aReadFdSet))
    {
        processNetifEvent(sInstance);
    }

#if OPENTHREAD_POSIX_USE_MLD_MONITOR
    if (FD_ISSET(sMLDMonitorFd, aReadFdSet))
    {
        processMLDEvent(sInstance);
    }
#endif

exit:
    return;
}

#endif // OPENTHREAD_CONFIG_PLATFORM_NETIF_ENABLE
