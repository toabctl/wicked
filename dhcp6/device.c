/*
 *	DHCP6 supplicant -- client device
 *
 *	Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 *	Copyright (C) 2012 Marius Tomaschewski <mt@suse.de>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, see <http://www.gnu.org/licenses/> or write
 *	to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *	Boston, MA 02110-1301 USA.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <net/if_arp.h>
#include <netlink/netlink.h>	/* address flags, TODO: get rid of them */
#include <arpa/inet.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/util.h>
#include <wicked/vlan.h>

#include "dhcp6/dhcp6.h"
#include "dhcp6/device.h"
#include "dhcp6/protocol.h"
#include "dhcp6/duid.h"
#include "dhcp6/fsm.h"

#include "appconfig.h"
#include "util_priv.h"


/*
 * DHCP6 package name and version based on config.h
 */
#ifndef NI_DHCP6_PACKAGE_NAME
#ifndef PACKAGE_NAME
#define	NI_DHCP6_PACKAGE_NAME			"wicked-dhcp6"
#else
#define	NI_DHCP6_PACKAGE_NAME			PACKAGE_NAME "-dhcp6"
#endif
#endif
#ifndef NI_DHCP6_PACKAGE_VERSION
#ifndef PACKAGE_VERSION
#define	NI_DHCP6_PACKAGE_VERSION		"0.0.0"
#else
#define	NI_DHCP6_PACKAGE_VERSION		PACKAGE_VERSION
#endif
#endif

/*
 * Default Vendor enterprise number + data in <name>/<version> format.
 *
 * http://www.iana.org/assignments/enterprise-numbers
 */
#ifndef NI_DHCP6_VENDOR_ENTERPRISE_NUMBER
#define	NI_DHCP6_VENDOR_ENTERPRISE_NUMBER	7075	/* SUSE */
#endif
#ifndef NI_DHCP6_VENDOR_VERSION_STRING
#define NI_DHCP6_VENDOR_VERSION_STRING		NI_DHCP6_PACKAGE_NAME"/"NI_DHCP6_PACKAGE_VERSION
#endif

/*
 * How long to wait until (link-layer) address is ready to use
 */
#define NI_DHCP6_WAIT_READY_MSEC		2000


extern int			ni_dhcp6_load_duid(ni_opaque_t *duid, const char *filename);
extern int			ni_dhcp6_save_duid(const ni_opaque_t *duid, const char *filename);

ni_dhcp6_device_t *		ni_dhcp6_active;

static void			ni_dhcp6_device_close(ni_dhcp6_device_t *);
static void			ni_dhcp6_device_free(ni_dhcp6_device_t *);

static void			ni_dhcp6_device_set_config(ni_dhcp6_device_t *, ni_dhcp6_config_t *);

static void			ni_dhcp6_device_alloc_buffer(ni_dhcp6_device_t *);
static void			ni_dhcp6_device_clear_buffer(ni_dhcp6_device_t *);
static void			ni_dhcp6_device_drop_buffer(ni_dhcp6_device_t *);

static int			ni_dhcp6_device_transmit_arm_delay(ni_dhcp6_device_t *);
static void			ni_dhcp6_device_retransmit_arm(ni_dhcp6_device_t *dev);


/*
 * Create and destroy dhcp6 device handles
 */
ni_dhcp6_device_t *
ni_dhcp6_device_new(const char *ifname, const ni_linkinfo_t *link)
{
	ni_dhcp6_device_t *dev, **pos;

	for (pos = &ni_dhcp6_active; (dev = *pos) != NULL; pos = &dev->next)
		;

	dev = xcalloc(1, sizeof(*dev));
	dev->users = 1;

	ni_string_dup(&dev->ifname, ifname);
	dev->link.ifindex = link->ifindex;

	/* FIXME: for now, we always generate one */
	ni_dhcp6_device_iaid(dev, &dev->iaid);

	ni_timer_get_time(&dev->start_time);
	dev->fsm.state = NI_DHCP6_STATE_INIT;

	/* append to end of list */
	*pos = dev;

	return dev;
}

ni_dhcp6_device_t *
ni_dhcp6_device_by_index(unsigned int ifindex)
{
	ni_dhcp6_device_t *dev;

	for (dev = ni_dhcp6_active; dev; dev = dev->next) {
		if (dev->link.ifindex == ifindex)
			return dev;
	}
	return NULL;
}

/*
 * Refcount handling
 */
ni_dhcp6_device_t *
ni_dhcp6_device_get(ni_dhcp6_device_t *dev)
{
	ni_assert(dev->users);
	dev->users++;
	return dev;
}

void
ni_dhcp6_device_put(ni_dhcp6_device_t *dev)
{
	ni_assert(dev->users);
	if (--(dev->users) == 0)
		ni_dhcp6_device_free(dev);
}


/*
 * Cleanup functions
 */
static void
ni_dhcp6_device_close(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_mcast_socket_close(dev);

	if (dev->fsm.timer) {
		ni_warn("%s: timer active for while close", dev->ifname);
		ni_timer_cancel(dev->fsm.timer);
		dev->fsm.timer = NULL;
	}
}

void
ni_dhcp6_device_stop(ni_dhcp6_device_t *dev)
{
#if 0
	/* Clear the lease. This will trigger an event to wickedd
	 * with a lease that has state RELEASED. */
	ni_dhcp6_fsm_commit_lease(dev, NULL);
#else
	ni_dhcp6_device_drop_lease(dev);
	ni_dhcp6_device_drop_best_offer(dev);
	dev->fsm.state = NI_DHCP6_STATE_INIT;
#endif

	ni_dhcp6_device_close(dev);

	/* Drop existing config and request */
	ni_dhcp6_device_set_config(dev, NULL);
	ni_dhcp6_device_set_request(dev, NULL);
}

static void
ni_dhcp6_device_free(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_device_t **pos;

	ni_assert(dev->users == 0);
	ni_debug_dhcp("%s: Deleting dhcp6 device with index %u",
			dev->ifname, dev->link.ifindex);

	ni_dhcp6_device_drop_buffer(dev);
	ni_dhcp6_device_drop_lease(dev);
	ni_dhcp6_device_drop_best_offer(dev);
	ni_dhcp6_device_close(dev);

	/* Drop existing config and request */
	ni_dhcp6_device_set_config(dev, NULL);
	ni_dhcp6_device_set_request(dev, NULL);

	ni_string_free(&dev->ifname);
	dev->link.ifindex = 0;

	for (pos = &ni_dhcp6_active; *pos; pos = &(*pos)->next) {
		if (*pos == dev) {
			*pos = dev->next;
			break;
		}
	}

	free(dev);
}


/*
 * Device handle request set helper
 */
void
ni_dhcp6_device_set_request(ni_dhcp6_device_t *dev, ni_dhcp6_request_t *request)
{
	if(dev->request)
		ni_dhcp6_request_free(dev->request);
	dev->request = request;
}

/*
 * Device handle config set helper
 */
static void
__ni_dhcp6_device_config_free(ni_dhcp6_config_t *config)
{
	if (config) {
		ni_dhcp6_ia_list_destroy(&config->ia_list);
		ni_string_array_destroy(&config->user_class);
		ni_string_array_destroy(&config->vendor_class.data);
		ni_var_array_destroy(&config->vendor_opts.data);
		free(config);
	}
}

static void
ni_dhcp6_device_set_config(ni_dhcp6_device_t *dev, ni_dhcp6_config_t *config)
{
	if (dev->config)
		__ni_dhcp6_device_config_free(dev->config);
	dev->config = config;
}

void
ni_dhcp6_device_set_lease(ni_dhcp6_device_t *dev,  ni_addrconf_lease_t *lease)
{
	if (dev->lease != lease) {
		ni_addrconf_dhcp6_lease_free(dev->lease);
		dev->lease = lease;
	}
}

static void
ni_dhcp6_device_alloc_buffer(ni_dhcp6_device_t *dev)
{
	/* TODO: review this. rfc2460#section-5, Packet Size Issues */
	if (dev->message.size < NI_DHCP6_WBUF_SIZE) {
		ni_buffer_ensure_tailroom(&dev->message, NI_DHCP6_WBUF_SIZE);
	}
	ni_buffer_clear(&dev->message);
}

static void
ni_dhcp6_device_clear_buffer(ni_dhcp6_device_t *dev)
{
	ni_buffer_clear(&dev->message);
}

static void
ni_dhcp6_device_drop_buffer(ni_dhcp6_device_t *dev)
{
	ni_buffer_destroy(&dev->message);
}

void
ni_dhcp6_device_drop_lease(ni_dhcp6_device_t *dev)
{
	ni_addrconf_lease_t *lease;

	if ((lease = dev->lease) != NULL) {
		ni_addrconf_lease_file_remove(dev->ifname, lease->type, lease->family);
		ni_addrconf_dhcp6_lease_free(lease);
		dev->lease = NULL;
	}
}

void
ni_dhcp6_device_drop_best_offer(ni_dhcp6_device_t *dev)
{
	dev->best_offer.weight = -1;
	if (dev->best_offer.lease)
		ni_addrconf_dhcp6_lease_free(dev->best_offer.lease);
	dev->best_offer.lease = NULL;
}

unsigned int
ni_dhcp6_device_uptime(const ni_dhcp6_device_t *dev, unsigned int clamp)
{
	struct timeval now;
	struct timeval delta;
	long           uptime = 0;

	ni_timer_get_time(&now);
	if (timerisset(&dev->retrans.start) && timercmp(&now, &dev->retrans.start, >)) {
		timersub(&now, &dev->retrans.start, &delta);

		/* uptime in hundredths of a second (10^-2 seconds) */
		uptime = (delta.tv_sec * 100 + delta.tv_usec / 10000);
	}
	return (uptime < clamp) ? uptime : clamp;
}

int
ni_dhcp6_device_iaid(const ni_dhcp6_device_t *dev, uint32_t *iaid)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	size_t len, off;
	uint32_t tmp;

	nc = ni_global_state_handle(0);
	if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex))) {
		ni_error("%s: Unable to find network interface by index %u",
			dev->ifname, dev->link.ifindex);
		return -1;
	}

	/* FIXME: simple iaid with 4 last byte of the mac */

	*iaid = 0;
	if (ifp->link.hwaddr.len > 4) {
		off = ifp->link.hwaddr.len - 4;
		memcpy(iaid, ifp->link.hwaddr.data + off, sizeof(*iaid));
		return 0;
	}
	if ((len = ni_string_len(dev->ifname))) {
		memcpy(&tmp, dev->ifname, len % sizeof(tmp));
		*iaid ^= tmp;
		if (ifp->link.vlan && ifp->link.vlan->tag > 0)
			*iaid ^= ifp->link.vlan->tag;
		*iaid ^= dev->link.ifindex;
		return 0;
	}
	return -1;
}

void
ni_dhcp6_device_show_addrs(ni_dhcp6_device_t *dev)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
#if 0
	ni_address_t *ap;
	unsigned int nr = 0;
#endif

	nc = ni_global_state_handle(0);
	if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex))) {
		ni_error("%s: Unable to find network interface by index %u",
			dev->ifname, dev->link.ifindex);
		return;
	}
#if 0
	for (ap = ifp->addrs; ap; ap = ap->next) {
		ni_debug_dhcp("%s: address[%u] %s/%u%s, scope=%s, flags%s%s%s%s%s",
				dev->ifname, nr++,
				ni_sockaddr_print(&ap->local_addr), ap->prefixlen,
				(ni_address_is_linklocal(ap) ? " [link-local]" : ""),
				(ap->scope == RT_SCOPE_HOST ? "host" :
				 (ap->scope == RT_SCOPE_LINK ? "link" :
				  (ap->scope == RT_SCOPE_SITE ? "site" : "universe"))),
				(ni_address_is_temporary(ap) ? " temporary"  : ""),
				(ni_address_is_permanent(ap) ? " permanent"  : " dynamic"),
				(ni_address_is_deprecated(ap) ? " deprecated"  : ""),
				(ni_address_is_tentative(ap) ? " tentative " : ""),
				(ni_address_is_duplicate(ap) ? " duplicate " : "")
		);
	}
#endif
}

int
ni_dhcp6_device_start(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_device_show_addrs(dev);

	if (!dev->config) {
		ni_error("%s: Cannot start DHCPv6 without config", dev->ifname);
		return -1;
	}

	dev->failed = 0;
	ni_dhcp6_device_alloc_buffer(dev);

	return ni_dhcp6_fsm_start(dev);
}

void
ni_dhcp6_restart(void)
{
	ni_dhcp6_device_t *dev;

	for (dev = ni_dhcp6_active; dev; dev = dev->next) {
		if (dev->request) {
#if 0
			ni_trace("restarting acquire %s on dev %s",
				(dev->request->info_only ? "info" : "lease"),
				dev->ifname);
#endif
			ni_dhcp6_acquire(dev, dev->request);
		}
	}
}

static int
ni_dhcp6_device_set_lladdr(ni_dhcp6_device_t *dev, const ni_address_t *addr)
{
	if (ni_address_is_duplicate(addr)) {
		ni_error("%s: Link-local IPv6 address is marked duplicate: %s",
			dev->ifname, ni_sockaddr_print(&addr->local_addr));
		return -1;
	}
	if (ni_address_is_tentative(addr)) {
		ni_debug_dhcp("%s: Link-local IPv6 address is tentative: %s",
			dev->ifname, ni_sockaddr_print(&addr->local_addr));
		return 1;
	}

	ni_debug_dhcp("%s: Found usable link-local IPv6 address: %s",
		dev->ifname, ni_sockaddr_print(&addr->local_addr));

	memcpy(&dev->link.addr, &addr->local_addr, sizeof(dev->link.addr));
	return 0;
}

static int
ni_dhcp6_device_find_lladdr(ni_dhcp6_device_t *dev, ni_dhcp6_config_t *config)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	ni_address_t *addr;
	int rv = 1, cnt = 0;

	nc = ni_global_state_handle(0);
	if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex))) {
		ni_error("%s: Unable to find network interface by index %u",
			dev->ifname, dev->link.ifindex);
		return -1;
	}

	if (!ni_netdev_link_is_up(ifp)) {
		ni_error("%s: Link is not up", dev->ifname);
		return -1;
	}

	for(addr = ifp->addrs; addr; addr = addr->next) {
		if (addr->family != AF_INET6 || !ni_address_is_linklocal(addr))
			continue;

		cnt++;
		if ((rv = ni_dhcp6_device_set_lladdr(dev, addr)) == 0)
			return 0;
	}

	if (cnt == 0) {
		ni_debug_dhcp("%s: Link-local IPv6 address not (yet) available",
				dev->ifname);
	}
	return rv;
}

ni_bool_t
ni_dhcp6_device_is_ready(const ni_dhcp6_device_t *dev, const ni_netdev_t *ifp)
{
	ni_netconfig_t *nc;

	if (!ifp) {
		nc = ni_global_state_handle(0);
		if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex))) {
			ni_error("%s: Unable to find network interface by index %u",
				dev->ifname, dev->link.ifindex);
			return FALSE;
		}
	}
	return	ni_netdev_network_is_up(ifp) &&
		ni_sockaddr_is_ipv6_linklocal(&dev->link.addr);
}

int
ni_dhcp6_device_transmit_init(ni_dhcp6_device_t *dev)
{
	if (ni_dhcp6_device_transmit_arm_delay(dev))
		return 0;

	return ni_dhcp6_device_transmit_start(dev);
}

int
ni_dhcp6_device_transmit_start(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_device_retransmit_arm(dev);

	return ni_dhcp6_device_transmit(dev);
}

static int
ni_dhcp6_device_transmit_arm_delay(ni_dhcp6_device_t *dev)
{
	ni_int_range_t jitter;
	unsigned long  delay;

	/*
	 * rfc3315#section-5.5 (17.1.2, 18.1.2, 18.1.5):
	 *
	 * Initial delay is a MUST for Solicit, Confirm and InfoRequest.
	 */
	if (dev->retrans.delay == 0)
		return FALSE;

	ni_debug_dhcp("%s: setting initial transmit delay of %u [%d .. %d] msec",
			dev->ifname, dev->retrans.delay,
			0 - dev->retrans.jitter,
			0 + dev->retrans.jitter);

	/* we can use base jitter as is, it's 0.1 msec already */
	jitter.min = 0 - dev->retrans.jitter;
	jitter.max = 0 + dev->retrans.jitter;
	delay = ni_timeout_randomize(dev->retrans.delay, &jitter);

	ni_dhcp6_fsm_set_timeout_msec(dev, delay);

	return TRUE;
}

static void
ni_dhcp6_device_retransmit_arm(ni_dhcp6_device_t *dev)
{
	/* when we're here, initial delay is over */
	dev->retrans.delay = 0;

	/*
	 * Hmm... Remember the time of the first transmission
	 */
	ni_timer_get_time(&dev->retrans.start);

	/* Leave, when retransmissions aren't enabled */
	if (dev->retrans.params.nretries == 0)
		return;

	if (dev->fsm.state == NI_DHCP6_STATE_SELECTING && dev->retrans.count == 1) {
		/*
		 * rfc3315#section-17.1.2
		 *
		 * "[...]
		 * The message exchange is not terminated by the receipt of an Advertise
		 * before the first RT has elapsed. Rather, the client collects Advertise
		 * messages until the first RT has elapsed.
		 * Also, the first RT MUST be selected to be strictly greater than IRT
		 * by choosing RAND to be strictly greater than 0.
		 * [...]"
		 */
		dev->retrans.params.jitter = ni_dhcp6_jitter_rebase(
				dev->retrans.params.timeout,
				0, /* exception, no negative jitter */
				0 + dev->retrans.jitter);

		/*
		 * rfc3315#section-14
		 *
		 * "[...]
		 *  RT for the first message transmission is based on IRT:
		 * 	RT = IRT + RAND*IRT
		 *  [...]"
		 *
		 * IRT is already initialized in retrans.params.timeout.
		 */
		dev->retrans.params.timeout = ni_timeout_arm_msec(&dev->retrans.deadline,
								  &dev->retrans.params);

		/*
		 * Trigger fsm timeout event after first RT to process the collected
		 * Advertise messages.
		 *
		 * Note, that there is no max duration time for Solicit messages, so
		 * we can reuse the fsm duration timer ...
		 */
		ni_dhcp6_fsm_set_timeout_msec(dev, dev->retrans.params.timeout);
	} else {
		/*
		 * rfc3315#section-14
		 *
		 * "[...]
		 * Each new RT include a randomization factor (RAND) [...]
		 * between -0.1 and +0.1.
		 * [...]"
		 */
		dev->retrans.params.jitter = ni_dhcp6_jitter_rebase(
				dev->retrans.params.timeout,
				0 - dev->retrans.jitter,
				0 + dev->retrans.jitter);

		/*
		 * rfc3315#section-14
		 *
		 * "[...]RT for the first message transmission is based on IRT:
		 * 		RT = IRT + RAND*IRT
		 *  [...]"
		 *
		 *  IRT is already initialized in retrans.params.timeout.
		 */
		dev->retrans.params.timeout = ni_timeout_arm_msec(&dev->retrans.deadline,
								  &dev->retrans.params);

		if (dev->retrans.duration) {
			/*
			 * rfc3315#section-14
			 *
			 * "[...]
			 * MRD specifies an upper bound on the length of time a client may
			 * retransmit a message. Unless MRD is zero, the message exchange
			 * fails once MRD seconds have elapsed since the client first
			 * transmitted the message.
			 * [...]"
			 */
			ni_dhcp6_fsm_set_timeout_msec(dev, dev->retrans.duration);
		}
	}
}

void
ni_dhcp6_device_retransmit_disarm(ni_dhcp6_device_t *dev)
{
	struct timeval now;

	ni_timer_get_time(&now);

	ni_debug_dhcp("%s: disarming retransmission at %s",
			dev->ifname, ni_dhcp6_print_timeval(&now));

	dev->dhcp6.xid = 0;
	memset(&dev->retrans, 0, sizeof(dev->retrans));
}

static ni_bool_t
ni_dhcp6_device_retransmit_advance(ni_dhcp6_device_t *dev)
{
	/*
	 * rfc3315#section-14
	 *
	 * "[...]
	 * Each new RT include a randomization factor (RAND) [...]
	 * between -0.1 and +0.1.
	 * [...]
	 * RT for each subsequent message transmission is based on
	 * the previous value of RT:
	 *
	 * 	RT = 2*RTprev + RAND*RTprev
	 * [...]"
	 *
	 */
	if( ni_timeout_recompute(&dev->retrans.params)) {
		unsigned int old_timeout = dev->retrans.params.timeout;

		/*
		 * Hmm... should we set this as backoff callback?
		 */
		dev->retrans.params.jitter = ni_dhcp6_jitter_rebase(
				dev->retrans.params.timeout,
				0 - dev->retrans.jitter,
				0 + dev->retrans.jitter);

		dev->retrans.params.timeout = ni_timeout_arm_msec(
				&dev->retrans.deadline,
				&dev->retrans.params);

		ni_debug_dhcp("%s: increased retransmission timeout from %u to %u [%d .. %d]: %s",
				dev->ifname, old_timeout,
				dev->retrans.params.timeout,
				dev->retrans.params.jitter.min,
				dev->retrans.params.jitter.max,
				ni_dhcp6_print_timeval(&dev->retrans.deadline));

		return TRUE;
	}
#if 0
	ni_trace("Retransmissions are disabled");
#endif
	return FALSE;
}

int
ni_dhcp6_device_retransmit(ni_dhcp6_device_t *dev)
{
	if (!ni_dhcp6_device_retransmit_advance(dev)) {
		ni_dhcp6_device_retransmit_disarm(dev);
		return -1;
	}

	if (ni_dhcp6_fsm_retransmit(dev) < 0)
		return -1;
#if 0
	ni_trace("Retransmitted, next deadline at %s", ni_dhcp6_format_time(&dev->retrans.deadline));
#endif
	return 0;
}

void
ni_dhcp6_generate_duid(ni_dhcp6_device_t *dev, ni_opaque_t *duid)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	ni_uuid_t uuid;

	nc = ni_global_state_handle(0);
	if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex))) {
		ni_error("%s: Unable to find network interface by index %u",
			dev->ifname, dev->link.ifindex);
		return;
	}

	/* try the current interface first */
	if (ifp->link.hwaddr.len) {
		if(ni_duid_init_llt(duid, ifp->link.arp_type,
				ifp->link.hwaddr.data, ifp->link.hwaddr.len))
			return;
	}

	/* then another one */
	for (ifp = ni_netconfig_devlist(nc); ifp; ifp = ifp->next) {
		if (ifp->link.ifindex == dev->link.ifindex)
			continue;

		switch(ifp->link.arp_type) {
		case ARPHRD_ETHER:
		case ARPHRD_IEEE802:
		case ARPHRD_INFINIBAND:
			if (ifp->link.hwaddr.len) {
				if(ni_duid_init_llt(duid, ifp->link.arp_type,
						ifp->link.hwaddr.data, ifp->link.hwaddr.len))
					return;
			}
		break;
		}
	}

	/*
	 * TODO:
	 * 1) MAC based uuid duid, see
	 *    http://tools.ietf.org/html/rfc4122#section-4.1.6
	 * 2) There should be some system unique uuid at least on x86_64
	 */
	memset(&uuid, 0, sizeof(uuid));
	ni_uuid_generate(&uuid);
	ni_duid_init_uuid(duid, &uuid);
}

static ni_bool_t
ni_dhcp6_config_init_duid(ni_dhcp6_device_t *dev, ni_dhcp6_config_t *config, const char *preferred)
{
	ni_bool_t save = TRUE;

	if (preferred) {
		ni_duid_parse_hex(&config->client_duid, preferred);
	}
	if (config->client_duid.len == 0) {
		ni_dhcp6_config_default_duid(&config->client_duid);
	}

	if (config->client_duid.len == 0) {
		if( ni_dhcp6_load_duid(&config->client_duid, NULL) == 0)
			save = FALSE;
	}
	if (config->client_duid.len == 0) {
		ni_dhcp6_generate_duid(dev, &config->client_duid);
	}

	if (config->client_duid.len > 0 && save) {
		(void)ni_dhcp6_save_duid(&config->client_duid, NULL);
	}
	return (config->client_duid.len > 0);
}

/*
 * Process a request to reconfigure the device (ie rebind a lease, or discover
 * a new lease).
 */
int
ni_dhcp6_acquire(ni_dhcp6_device_t *dev, const ni_dhcp6_request_t *info)
{
	ni_dhcp6_config_t *config;
	ni_dhcp6_ia_t *ia;
	size_t len;
	int rv;

	config = xcalloc(1, sizeof(*config));
	config->uuid = info->uuid;
	config->update = info->update;

	config->info_only = info->info_only;
	config->rapid_commit = info->rapid_commit;
	config->lease_time = NI_DHCP6_PREFERRED_LIFETIME;

        /*
         * Make sure we have a DUID for client-id
         */
	if(!ni_dhcp6_config_init_duid(dev, config, info->clientid)) {
		ni_error("Unable to find usable or generate client duid");
		__ni_dhcp6_device_config_free(config);
		return -1;
	}

	if (!config->info_only) {
		if (info->ia_list == NULL) {
			ia = ni_dhcp6_ia_na_new(dev->iaid);
			ni_dhcp6_ia_set_default_lifetimes(ia, config->lease_time);
			ni_dhcp6_ia_list_append(&config->ia_list, ia);
		} else {
			/* TODO: Merge multiple ia's of same type into one?
			 *       for tests we take it as is -- at the moment */
			ni_dhcp6_ia_list_copy(&config->ia_list, info->ia_list, FALSE);
		}
	}

	if ((len = ni_string_len(info->hostname)) > 0) {
		if (ni_check_domain_name(info->hostname, len, 0)) {
			strncpy(config->hostname, info->hostname, sizeof(config->hostname) - 1);
		} else {
			ni_debug_dhcp("Discarded request to use suspect hostname: %s",
				ni_print_suspect(info->hostname, len));
		}
	}

	/* TODO: get from req info */
	ni_dhcp6_config_vendor_class(&config->vendor_class.en, &config->vendor_class.data);
	ni_dhcp6_config_vendor_opts(&config->vendor_opts.en, &config->vendor_opts.data);

	ni_dhcp6_device_show_addrs(dev);
	rv = ni_dhcp6_device_find_lladdr(dev, config);
	if (rv < 0) {
		__ni_dhcp6_device_config_free(config);
		return rv;
	}

	ni_dhcp6_device_set_config(dev, config);

	/* OK, then let's start when lladdr is ready or set timer when not */
	if (rv > 0) {
		dev->fsm.state = NI_DHCP6_STATE_WAIT_READY;
		ni_dhcp6_fsm_set_timeout_msec(dev, NI_DHCP6_WAIT_READY_MSEC);
		dev->fsm.fail_on_timeout = 1;
		return rv;
	}
	return ni_dhcp6_device_start(dev);

#if 0
	config->max_lease_time = ni_dhcp6_config_max_lease_time();
	if (config->max_lease_time == 0)
		config->max_lease_time = ~0U;
	if (info->lease_time && info->lease_time < config->max_lease_time)
		config->max_lease_time = info->lease_time;

	if (info->hostname)
		strncpy(config->hostname, info->hostname, sizeof(config->hostname) - 1);

	if (info->clientid) {
		strncpy(config->client_id, info->clientid, sizeof(config->client_id)-1);
		ni_dhcp6_parse_client_id(&config->raw_client_id, dev->link.type, info->clientid);
	} else {
		/* Set client ID from interface hwaddr */
		strncpy(config->client_id, ni_link_address_print(&dev->system.hwaddr), sizeof(config->client_id)-1);
		ni_dhcp6_set_client_id(&config->raw_client_id, &dev->system.hwaddr);
	}

	if ((classid = info->vendor_class) == NULL)
		classid = ni_dhcp6_config_vendor_class();
	if (classid)
		strncpy(config->classid, classid, sizeof(config->classid) - 1);

	config->flags = DHCP6_DO_ARP | DHCP6_DO_CSR | DHCP6_DO_MSCSR;
	config->flags |= ni_dhcp6_do_bits(info->update);

	if (ni_debug & NI_TRACE_DHCP) {
		ni_trace("Received request:");
		ni_trace("  acquire-timeout %u", config->request_timeout);
		ni_trace("  lease-time      %u", config->max_lease_time);
		ni_trace("  hostname        %s", config->hostname[0]? config->hostname : "<none>");
		ni_trace("  vendor-class    %s", config->classid[0]? config->classid : "<none>");
		ni_trace("  client-id       %s", ni_print_hex(config->raw_client_id.data, config->raw_client_id.len));
		ni_trace("  uuid            %s", ni_print_hex(config->uuid.octets, 16));
		ni_trace("  flags           %s", __ni_dhcp6_print_flags(config->flags));
	}

	if (dev->config)
		free(dev->config);
	dev->config = config;

#if 0
	/* FIXME: This cores for now */
	/* If we're asked to reclaim an existing lease, try to load it. */
	if (info->reuse_unexpired && ni_dhcp6_fsm_recover_lease(dev, info) >= 0)
		return 0;
#endif

	if (dev->lease) {
		if (!ni_addrconf_lease_is_valid(dev->lease)
		 || (info->hostname && !ni_string_eq(info->hostname, dev->lease->hostname))
		 || (info->clientid && !ni_string_eq(info->clientid, dev->lease->dhcp6.client_id))) {
			ni_debug_dhcp6("%s: lease doesn't match request", dev->ifname);
			ni_dhcp6_device_drop_lease(dev);
			dev->notify = 1;
		}
	}

	/* Go back to INIT state to force a rediscovery */
	dev->fsm.state = NI_DHCP6_STATE_INIT;
	ni_dhcp6_device_start(dev);
	return 1;
#endif
}


#if 0
/*
 * Translate a bitmap of NI_ADDRCONF_UPDATE_* flags into a bitmap of
 * DHCP6_DO_* masks
 */
static unsigned int
ni_dhcp6_do_bits(unsigned int update_flags)
{
	static unsigned int	do_mask[32] = {
	[NI_ADDRCONF_UPDATE_HOSTNAME]		= DHCP6_DO_HOSTNAME,
	[NI_ADDRCONF_UPDATE_RESOLVER]		= DHCP6_DO_RESOLVER,
	[NI_ADDRCONF_UPDATE_NIS]		= DHCP6_DO_NIS,
	[NI_ADDRCONF_UPDATE_NTP]		= DHCP6_DO_NTP,
	[NI_ADDRCONF_UPDATE_DEFAULT_ROUTE]	= DHCP6_DO_GATEWAY,
	};
	unsigned int bit, result = 0;

	for (bit = 0; bit < 32; ++bit) {
		if (update_flags & (1 << bit))
			result |= do_mask[bit];
	}
	return result;
}

static const char *
__ni_dhcp6_print_flags(unsigned int flags)
{
	static ni_intmap_t flag_names[] = {
	{ "arp",		DHCP6_DO_ARP		},
	{ "csr",		DHCP6_DO_CSR		},
	{ "mscsr",		DHCP6_DO_MSCSR		},
	{ "hostname",		DHCP6_DO_HOSTNAME	},
	{ "resolver",		DHCP6_DO_RESOLVER	},
	{ "nis",		DHCP6_DO_NIS		},
	{ "ntp",		DHCP6_DO_NTP		},
	{ "gateway",		DHCP6_DO_GATEWAY		},
	{ NULL }
	};
	static char buffer[1024];
	char *pos = buffer;
	unsigned int mask;

	*pos = '\0';
	for (mask = 1; mask != 0; mask <<= 1) {
		const char *name;

		if ((flags & mask) == 0)
			continue;
		if (!(name = ni_format_int_mapped(mask, flag_names)))
			continue;
		snprintf(pos, buffer + sizeof(buffer) - pos, "%s%s",
				(pos == buffer)? "" : ", ",
				name);
		pos += strlen(pos);
	}
	if (buffer[0] == '\0')
		return "<none>";

	return buffer;
}
#endif

/*
 * Process a request to unconfigure the device (ie drop the lease).
 */
int
ni_dhcp6_release(ni_dhcp6_device_t *dev, const ni_uuid_t *lease_uuid)
{
#if 0
	int rv;

	if (dev->lease == NULL) {
		ni_error("%s(%s): no lease set", __func__, dev->ifname);
		return -NI_ERROR_ADDRCONF_NO_LEASE;
	}

	if (lease_uuid) {
		/* FIXME: We should check the provided uuid against the
		 * lease's uuid, and refuse the call if it doesn't match
		 */
	}

	/* We just send out a singe RELEASE without waiting for the
	 * server's reply. We just keep our fingers crossed that it's
	 * getting out. If it doesn't, it's rather likely the network
	 * is hosed anyway, so there's little point in delaying. */
	if ((rv = ni_dhcp6_fsm_release(dev)) < 0)
		return rv;
	ni_dhcp6_device_stop(dev);
#endif
	return 0;
}

/*
 * Handle link up/down events
 */
void
ni_dhcp6_device_event(ni_dhcp6_device_t *dev, ni_netdev_t *ifp, ni_event_t event)
{
	switch (event) {
	case NI_EVENT_DEVICE_UP:
		if (!ni_string_eq(dev->ifname, ifp->name)) {
			ni_debug_dhcp("%s: Updating interface name to %s",
					dev->ifname, ifp->name);
			ni_string_dup(&dev->ifname, ifp->name);
		}
	break;

	case NI_EVENT_DEVICE_DOWN:
		/* Someone has taken the interface down completely. */
		ni_debug_dhcp("%s: network interface went down", dev->ifname);
		ni_dhcp6_device_stop(dev);
	break;

	case NI_EVENT_NETWORK_DOWN:
		ni_trace("%s: received network down event", dev->ifname);
	break;

	case NI_EVENT_NETWORK_UP:
		ni_trace("%s: received network up event", dev->ifname);
	break;

	case NI_EVENT_LINK_DOWN:
		ni_debug_dhcp("received link down event");
		//ni_dhcp6_fsm_link_down(dev);
	break;

	case NI_EVENT_LINK_UP:
		ni_debug_dhcp("received link up event");
		//ni_dhcp6_fsm_link_up(dev);
	break;

	default:
		ni_trace("%s: received other event", dev->ifname);
	break;
	}
	ni_dhcp6_device_show_addrs(dev);
}

void
ni_dhcp6_address_event(ni_dhcp6_device_t *dev, ni_netdev_t *ifp, ni_event_t event, const ni_address_t *addr)
{
	switch (event) {
	case NI_EVENT_ADDRESS_UPDATE:
		if (dev->link.addr.ss_family == AF_UNSPEC) {
			if(addr->family == AF_INET6 && ni_address_is_linklocal(addr)) {
				ni_dhcp6_device_set_lladdr(dev, addr);
			}
		}

		ni_dhcp6_fsm_address_event(dev, ifp, event, addr);
	break;

	case NI_EVENT_ADDRESS_DELETE:
		if (addr->local_addr.ss_family == AF_INET6 &&
		    ni_sockaddr_equal(&addr->local_addr, &dev->link.addr)) {
			memset(&dev->link.addr, 0, sizeof(dev->link.addr));
		}

		ni_dhcp6_fsm_address_event(dev, ifp, event, addr);
	break;

	default:
	break;
	}
}


int
ni_dhcp6_device_transmit(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_packet_header_t *header;
	ssize_t rv = -1;
	size_t cnt;

	/* sanity check: verify we have at least the message type byte */
	if (!ni_buffer_count(&dev->message)) {
		ni_error("%s: Cannot send empty DHCPv6 message packet",
			dev->ifname);
		return rv;
	}

	/* peek message code only */
	header = ni_buffer_head(&dev->message);

#if 0
	ni_debug_dhcp("%s: sending %s with xid 0x%x to %s using socket #%d",
		dev->ifname, ni_dhcp6_message_name(header->type),
		dev->dhcp6.xid, ni_sockaddr_print(&dev->config->server_addr),
		dev->sock->__fd);
#endif

	cnt = ni_buffer_count(&dev->message);
	rv = ni_dhcp6_socket_send(dev->mcast.sock, &dev->message, &dev->mcast.dest);

	if(rv < 0 || (size_t)rv != (ssize_t)ni_buffer_count(&dev->message)) {
		ni_error("%s: Unable to send %s message #%u: %m",
			dev->ifname,
			ni_dhcp6_message_name(header->type), dev->retrans.count + 1);

		ni_dhcp6_mcast_socket_close(dev);
		ni_dhcp6_device_clear_buffer(dev);
		return -1;
	} else {
		struct timeval now;

		dev->retrans.count++;

		ni_timer_get_time(&now);
		ni_debug_dhcp("%s: %s message #%u with %zu of %zd bytes sent at %s",
			dev->ifname, ni_dhcp6_message_name(header->type),
			dev->retrans.count, rv, cnt, ni_dhcp6_print_timeval(&now));

		ni_dhcp6_device_clear_buffer(dev);
		return 0;
	}
}

/*
 * Functions for accessing various global DHCP configuration options
 */
const char *
ni_dhcp6_config_default_duid(ni_opaque_t *duid)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	if (ni_string_empty(dhconf->default_duid))
		return NULL;

	if (!ni_duid_parse_hex(duid, dhconf->default_duid))
		return NULL;

	return dhconf->default_duid;
}

int
ni_dhcp6_config_user_class(ni_string_array_t *user_class_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	ni_string_array_copy(user_class_data, &dhconf->user_class_data);
	return 0;
}

int
ni_dhcp6_config_vendor_class(unsigned int *vclass_en, ni_string_array_t *vclass_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	if ((*vclass_en = dhconf->vendor_class_en) != 0) {
		ni_string_array_copy(vclass_data, &dhconf->vendor_class_data);
	} else {
		*vclass_en = NI_DHCP6_VENDOR_ENTERPRISE_NUMBER;
		ni_string_array_destroy(vclass_data);
		ni_string_array_append(vclass_data, NI_DHCP6_VENDOR_VERSION_STRING);
	}
	return 0;
}

int
ni_dhcp6_config_vendor_opts(unsigned int *vopts_en, ni_var_array_t *vopts_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	ni_var_array_destroy(vopts_data);
	if ((*vopts_en = dhconf->vendor_opts_en) != 0) {
		const ni_var_array_t *nva;
		unsigned int i;

		nva = &dhconf->vendor_opts_data;
		for (i = 0; i < nva->count; ++i) {
			if (ni_string_empty(nva->data[i].name))
				continue;
			ni_var_array_set(vopts_data, nva->data[i].name, nva->data[i].value);
		}
	}
	return 0;
}

int
ni_dhcp6_config_ignore_server(struct in6_addr addr)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	char        abuf[INET6_ADDRSTRLEN];
	const char *astr = inet_ntop(AF_INET, &addr, abuf, sizeof(abuf));

	// Hmm ... better another way around using IN6_ARE_ADDR_EQUAL(a,b)
	return (ni_string_array_index(&dhconf->ignore_servers, astr) >= 0);
}

ni_bool_t
ni_dhcp6_config_have_server_preference(void)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	return dhconf->num_preferred_servers != 0;
}

ni_bool_t
ni_dhcp6_config_server_preference(const struct in6_addr *addr, const ni_opaque_t *duid, int *weight)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	const ni_server_preference_t *pref = dhconf->preferred_server;
	unsigned int i;

	for (i = 0; i < dhconf->num_preferred_servers; ++i, ++pref) {
		ni_bool_t match = FALSE;
		if (pref->serverid.len > 0) {
			match = (duid && ni_opaque_eq(duid, &pref->serverid));
		}
		if (pref->address.ss_family == AF_INET6) {
			match = (addr && IN6_ARE_ADDR_EQUAL(addr, &pref->address.six.sin6_addr));
		}
		if (match) {
			*weight = pref->weight;
			return TRUE;
		}
	}
	return FALSE;
}

unsigned int
ni_dhcp6_config_max_lease_time(void)
{
	return ni_global.config->addrconf.dhcp6.lease_time;
}
