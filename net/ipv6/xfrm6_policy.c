/*
 * xfrm6_policy.c: based on xfrm4_policy.c
 *
 * Authors:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 * 	YOSHIFUJI Hideaki
 * 		Split up af-specific portion
 * 
 */

#include <linux/config.h>
#include <net/xfrm.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#ifdef CONFIG_IPV6_MIP6
#include <net/mip6.h>
#endif

static struct dst_ops xfrm6_dst_ops;
static struct xfrm_policy_afinfo xfrm6_policy_afinfo;

static struct xfrm_type_map xfrm6_type_map = { .lock = RW_LOCK_UNLOCKED };

static int xfrm6_dst_lookup(struct xfrm_dst **dst, struct flowi *fl)
{
	int err = 0;
	err = ip6_dst_lookup(NULL, (struct dst_entry **)dst, fl);
	if (err)
		err = -ENETUNREACH;
	return err;
}

static struct dst_entry *
__xfrm6_find_bundle(struct flowi *fl, struct xfrm_policy *policy)
{
	struct dst_entry *dst;

	read_lock_bh(&policy->lock);
	for (dst = policy->bundles; dst; dst = dst->next) {
		struct xfrm_dst *xdst = (struct xfrm_dst*)dst;
		struct in6_addr fl_dst_prefix, fl_src_prefix;

		ipv6_addr_prefix(&fl_dst_prefix,
				 &fl->fl6_dst,
				 xdst->u.rt6.rt6i_dst.plen);
		ipv6_addr_prefix(&fl_src_prefix,
				 &fl->fl6_src,
				 xdst->u.rt6.rt6i_src.plen);
		if (ipv6_addr_equal(&xdst->u.rt6.rt6i_dst.addr, &fl_dst_prefix) &&
		    ipv6_addr_equal(&xdst->u.rt6.rt6i_src.addr, &fl_src_prefix) &&
		    xfrm_bundle_ok(xdst, fl, AF_INET6)) {
			dst_clone(dst);
			break;
		}
	}
	read_unlock_bh(&policy->lock);
	return dst;
}

/* Allocate chain of dst_entry's, attach known xfrm's, calculate
 * all the metrics... Shortly, bundle a bundle.
 */

static int
__xfrm6_bundle_create(struct xfrm_policy *policy, struct xfrm_state **xfrm, int nx,
		      struct flowi *fl, struct dst_entry **dst_p)
{
	struct dst_entry *dst, *dst_prev;
	struct rt6_info *rt0 = (struct rt6_info*)(*dst_p);
	struct rt6_info *rt  = rt0;
	struct in6_addr *remote = &fl->fl6_dst;
	struct in6_addr *local  = &fl->fl6_src;
	struct flowi fl_tunnel = {
		.nl_u = {
			.ip6_u = {
				.saddr = *local,
				.daddr = *remote
			}
		}
	};
	int i;
	int err = 0;
	int header_len = 0;
	int trailer_len = 0;
	int cookie = atomic_read(&flow_cache_genid);

	dst = dst_prev = NULL;
	dst_hold(&rt->u.dst);

	for (i = 0; i < nx; i++) {
		struct dst_entry *dst1 = dst_alloc(&xfrm6_dst_ops);
		struct xfrm_dst *xdst;

		if (unlikely(dst1 == NULL)) {
			err = -ENOBUFS;
			dst_release(&rt->u.dst);
			goto error;
		}

		if (!dst)
			dst = dst1;
		else {
			dst_prev->child = dst1;
			dst1->flags |= DST_NOHASH;
			dst_clone(dst1);
		}

		xdst = (struct xfrm_dst *)dst1;
		xdst->route = &rt->u.dst;
		if (rt->rt6i_node)
			xdst->route_cookie = cookie;

		dst1->next = dst_prev;
		dst_prev = dst1;
		if (xfrm[i]->props.mode) {
			remote = (struct in6_addr*)&xfrm[i]->id.daddr;
			local  = (struct in6_addr*)&xfrm[i]->props.saddr;
#ifdef CONFIG_IPV6_MIP6
		} else if (xfrm[i]->id.proto == IPPROTO_ROUTING) {
			remote = (struct in6_addr*)xfrm[i]->coaddr;
		} else if (xfrm[i]->id.proto == IPPROTO_DSTOPTS) {
			local = (struct in6_addr*)xfrm[i]->coaddr;
#endif
		}
		header_len += xfrm[i]->props.header_len;
		trailer_len += xfrm[i]->props.trailer_len;

#ifdef CONFIG_IPV6_MIP6
		if (!ipv6_addr_equal(remote, &fl_tunnel.fl6_dst) ||
		    !ipv6_addr_equal(local, &fl_tunnel.fl6_src)) {
#else
		if (!ipv6_addr_equal(remote, &fl_tunnel.fl6_dst)) {
#endif
			struct inet6_ifaddr *ifa;
			ipv6_addr_copy(&fl_tunnel.fl6_dst, remote);
			ipv6_addr_copy(&fl_tunnel.fl6_src, local);
			ifa = ipv6_get_ifaddr(local, NULL, 0);
			if (ifa) {
				fl_tunnel.oif = ifa->idev->dev->ifindex;
				in6_ifa_put(ifa);
			}
			err = xfrm_dst_lookup((struct xfrm_dst **) &rt,
					      &fl_tunnel, AF_INET6);
			if (err)
				goto error;
		} else
			dst_hold(&rt->u.dst);
	}

	dst_prev->child = &rt->u.dst;
	dst->path = &rt->u.dst;
	if (rt->rt6i_node)
		((struct xfrm_dst *)dst)->path_cookie = cookie;

	*dst_p = dst;
	dst = dst_prev;

	dst_prev = *dst_p;
	i = 0;
	for (; dst_prev != &rt->u.dst; dst_prev = dst_prev->child) {
		struct xfrm_dst *x = (struct xfrm_dst*)dst_prev;

		dst_prev->xfrm = xfrm[i++];
		dst_prev->dev = rt->u.dst.dev;
		if (rt->u.dst.dev)
			dev_hold(rt->u.dst.dev);
		dst_prev->obsolete	= -1;
		dst_prev->flags	       |= DST_HOST;
		dst_prev->lastuse	= jiffies;
		dst_prev->header_len	= header_len;
		dst_prev->trailer_len	= trailer_len;
		memcpy(&dst_prev->metrics, &rt->u.dst.metrics, sizeof(dst_prev->metrics));

		/* Copy neighbour for reachability confirmation */
		dst_prev->neighbour	= neigh_clone(rt->u.dst.neighbour);
		dst_prev->input		= rt->u.dst.input;
		dst_prev->output	= xfrm6_output;
		/* Sheit... I remember I did this right. Apparently,
		 * it was magically lost, so this code needs audit */
		x->u.rt6.rt6i_flags    = rt0->rt6i_flags&(RTCF_BROADCAST|RTCF_MULTICAST|RTCF_LOCAL);
		x->u.rt6.rt6i_metric   = rt0->rt6i_metric;
		x->u.rt6.rt6i_node     = rt0->rt6i_node;
		x->u.rt6.rt6i_gateway  = rt0->rt6i_gateway;
		memcpy(&x->u.rt6.rt6i_gateway, &rt0->rt6i_gateway, sizeof(x->u.rt6.rt6i_gateway)); 
		x->u.rt6.rt6i_dst      = rt0->rt6i_dst;
		x->u.rt6.rt6i_src      = rt0->rt6i_src;	
		header_len -= x->u.dst.xfrm->props.header_len;
		trailer_len -= x->u.dst.xfrm->props.trailer_len;
	}
#if XFRM_DEBUG >= 4
	printk(KERN_DEBUG "%s: new bundle (policy index=%u) ", __FUNCTION__,
	       ((policy) ? policy->index : 0));
	for (dst_prev = dst; dst_prev; dst_prev = dst_prev->child) {
		if (dst_prev->xfrm)
			printk("+[%d]", dst_prev->xfrm->id.proto);
		else
			printk("+[-]");
		printk("%s", ((dst_prev == &rt0->u.dst) ? "(orig0)" :
			      (dst_prev == &rt->u.dst) ? "(orig)" : ""));
	}
	printk("\n");
#endif

	xfrm_init_pmtu(dst);
	return 0;

error:
	if (dst)
		dst_free(dst);
	return err;
}

static inline void
_decode_session6(struct sk_buff *skb, struct flowi *fl)
{
	u16 offset = sizeof(struct ipv6hdr);
	struct ipv6hdr *hdr = skb->nh.ipv6h;
	struct ipv6_opt_hdr *exthdr = (struct ipv6_opt_hdr*)(skb->nh.raw + offset);
	u8 nexthdr = skb->nh.ipv6h->nexthdr;

	memset(fl, 0, sizeof(struct flowi));
	ipv6_addr_copy(&fl->fl6_dst, &hdr->daddr);
	ipv6_addr_copy(&fl->fl6_src, &hdr->saddr);

#ifdef CONFIG_IPV6_MIP6
	/*
	 * XXX: required to check like pskb_may_pull!!
	 */
	while (offset + 1 <= (skb->tail - skb->nh.raw)) {
#else
	while (pskb_may_pull(skb, skb->nh.raw + offset + 1 - skb->data)) {
#endif
		switch (nexthdr) {
		case NEXTHDR_ROUTING:
		case NEXTHDR_HOP:
		case NEXTHDR_DEST:
			offset += ipv6_optlen(exthdr);
			nexthdr = exthdr->nexthdr;
			exthdr = (struct ipv6_opt_hdr*)(skb->nh.raw + offset);
			break;

		case IPPROTO_UDP:
		case IPPROTO_TCP:
		case IPPROTO_SCTP:
			if (pskb_may_pull(skb, skb->nh.raw + offset + 4 - skb->data)) {
				u16 *ports = (u16 *)exthdr;

				fl->fl_ip_sport = ports[0];
				fl->fl_ip_dport = ports[1];
			}
			fl->proto = nexthdr;
			fl->oif = skb->dev->ifindex;
			return;

		case IPPROTO_ICMPV6:
			if (pskb_may_pull(skb, skb->nh.raw + offset + 2 - skb->data)) {
				struct icmp6hdr *icmp;
				icmp = (struct icmp6hdr *)exthdr;

				fl->fl_icmp_type = icmp->icmp6_type;
				fl->fl_icmp_code = icmp->icmp6_code;
			}
			fl->proto = nexthdr;
			fl->oif = skb->dev->ifindex;
			return;

#ifdef CONFIG_IPV6_MIP6
		case IPPROTO_MH:
			if (pskb_may_pull(skb, skb->nh.raw + offset + 3 - skb->data)) {
				struct ip6_mh *mh;
				mh = (struct ip6_mh *)exthdr;

				fl->fl_mh_type = mh->ip6mh_type;
				if (fl->fl_mh_type == IP6_MH_TYPE_BU &&
				    pskb_may_pull(skb, skb->nh.raw + offset + 10 - skb->data)) {
					struct ip6_mh_binding_update *bu;
					bu = (struct ip6_mh_binding_update *)exthdr;
					fl->fl_mh_bu_flags = bu->ip6mhbu_flags;
				}
			}
			fl->proto = nexthdr;
			fl->oif = skb->dev->ifindex;
			return;
#endif

		/* XXX Why are there these headers? */
		case IPPROTO_AH:
		case IPPROTO_ESP:
		case IPPROTO_COMP:
		default:
			fl->fl_ipsec_spi = 0;
			fl->proto = nexthdr;
			fl->oif = skb->dev->ifindex;
			return;
		};
	}
}

static inline int xfrm6_garbage_collect(void)
{
	read_lock(&xfrm6_policy_afinfo.lock);
	xfrm6_policy_afinfo.garbage_collect();
	read_unlock(&xfrm6_policy_afinfo.lock);
	return (atomic_read(&xfrm6_dst_ops.entries) > xfrm6_dst_ops.gc_thresh*2);
}

static void xfrm6_update_pmtu(struct dst_entry *dst, u32 mtu)
{
	struct dst_entry *path = dst->path;

	if (mtu >= IPV6_MIN_MTU && mtu < dst_pmtu(dst))
		path->ops->update_pmtu(path, mtu);
	
	return;
}

static struct dst_ops xfrm6_dst_ops = {
	.family =		AF_INET6,
	.protocol =		__constant_htons(ETH_P_IPV6),
	.gc =			xfrm6_garbage_collect,
	.update_pmtu =		xfrm6_update_pmtu,
	.gc_thresh =		1024,
	.entry_size =		sizeof(struct xfrm_dst),
};

static struct xfrm_policy_afinfo xfrm6_policy_afinfo = {
	.family =		AF_INET6,
	.lock = 		RW_LOCK_UNLOCKED,
	.type_map = 		&xfrm6_type_map,
	.dst_ops =		&xfrm6_dst_ops,
	.dst_lookup =		xfrm6_dst_lookup,
	.find_bundle =		__xfrm6_find_bundle,
	.bundle_create =	__xfrm6_bundle_create,
	.decode_session =	_decode_session6,
};

static void __init xfrm6_policy_init(void)
{
	xfrm_policy_register_afinfo(&xfrm6_policy_afinfo);
}

static void xfrm6_policy_fini(void)
{
	xfrm_policy_unregister_afinfo(&xfrm6_policy_afinfo);
}

void __init xfrm6_init(void)
{
	xfrm6_policy_init();
	xfrm6_state_init();
}

void xfrm6_fini(void)
{
	//xfrm6_input_fini();
	xfrm6_policy_fini();
	xfrm6_state_fini();
}
