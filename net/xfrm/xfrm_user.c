/* xfrm_user.c: User interface to configure xfrm engine.
 *
 * Copyright (C) 2002 David S. Miller (davem@redhat.com)
 *
 * Changes:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 * 	
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pfkeyv2.h>
#include <linux/ipsec.h>
#include <linux/init.h>
#include <linux/security.h>
#include <net/sock.h>
#include <net/xfrm.h>
#include <asm/uaccess.h>

#include <linux/in6.h>

#ifdef CONFIG_IPV6_MIP6
#include <net/mip6.h>
#endif

static struct sock *xfrm_nl;

static int verify_one_alg(struct rtattr **xfrma, enum xfrm_attr_type_t type)
{
	struct rtattr *rt = xfrma[type - 1];
	struct xfrm_algo *algp;
	int len;

	if (!rt)
		return 0;

	len = (rt->rta_len - sizeof(*rt)) - sizeof(*algp);
	if (len < 0)
		return -EINVAL;

	algp = RTA_DATA(rt);

	len -= (algp->alg_key_len + 7U) / 8;
	if (len < 0)
		return -EINVAL;

	switch (type) {
	case XFRMA_ALG_AUTH:
		if (!algp->alg_key_len &&
		    strcmp(algp->alg_name, "digest_null") != 0)
			return -EINVAL;
		break;

	case XFRMA_ALG_CRYPT:
		if (!algp->alg_key_len &&
		    strcmp(algp->alg_name, "cipher_null") != 0)
			return -EINVAL;
		break;

	case XFRMA_ALG_COMP:
		/* Zero length keys are legal.  */
		break;

	default:
		return -EINVAL;
	};

	algp->alg_name[CRYPTO_MAX_ALG_NAME - 1] = '\0';
	return 0;
}

static int verify_encap_tmpl(struct rtattr **xfrma)
{
	struct rtattr *rt = xfrma[XFRMA_ENCAP - 1];
	struct xfrm_encap_tmpl *encap;

	if (!rt)
		return 0;

	if ((rt->rta_len - sizeof(*rt)) < sizeof(*encap))
		return -EINVAL;

	return 0;
}

#ifdef CONFIG_XFRM_ENHANCEMENT
static int verify_one_addr(struct rtattr **xfrma)
{
	struct rtattr *rt = xfrma[XFRMA_ADDR - 1];
	xfrm_address_t *addr;

	if (!rt)
		return 0;

	if ((rt->rta_len - sizeof(*rt)) < sizeof(*addr))
		return -EINVAL;

	return 0;
}
#endif

static int verify_newsa_info(struct xfrm_usersa_info *p,
			     struct rtattr **xfrma)
{
	int err;

	err = -EINVAL;
	switch (p->family) {
	case AF_INET:
		break;

	case AF_INET6:
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		break;
#else
		err = -EAFNOSUPPORT;
		goto out;
#endif

	default:
		goto out;
	};

	err = -EINVAL;
	switch (p->id.proto) {
	case IPPROTO_AH:
		if (!xfrma[XFRMA_ALG_AUTH-1]	||
		    xfrma[XFRMA_ALG_CRYPT-1]	||
		    xfrma[XFRMA_ALG_COMP-1])
			goto out;
		break;

	case IPPROTO_ESP:
		if ((!xfrma[XFRMA_ALG_AUTH-1] &&
		     !xfrma[XFRMA_ALG_CRYPT-1])	||
		    xfrma[XFRMA_ALG_COMP-1])
			goto out;
		break;

	case IPPROTO_COMP:
		if (!xfrma[XFRMA_ALG_COMP-1]	||
		    xfrma[XFRMA_ALG_AUTH-1]	||
		    xfrma[XFRMA_ALG_CRYPT-1])
			goto out;
		break;

#ifdef CONFIG_IPV6_MIP6
	case IPPROTO_IPV6:
	case IPPROTO_DSTOPTS:
	case IPPROTO_ROUTING:
		if (xfrma[XFRMA_ALG_COMP-1]	||
		    xfrma[XFRMA_ALG_AUTH-1]	||
		    xfrma[XFRMA_ALG_CRYPT-1]	||
		    !xfrma[XFRMA_ADDR-1]) {
			XFRM_DBG("invalid netlink message attributes.\n");
			goto out;
		}
		/*
		 * SPI must be 0 until assigning by xfrm6_tunnel_alloc_spi().
		 */
		if (p->id.spi) {
			printk(KERN_WARNING "%s: spi(specified %u) will be assigned by kernel\n", __FUNCTION__, p->id.spi);
			p->id.spi = 0;
		}

		break;
#endif

	default:
#ifdef CONFIG_XFRM_ENHANCEMENT
		err = -EPROTONOSUPPORT;
#endif
		goto out;
	};

	if ((err = verify_one_alg(xfrma, XFRMA_ALG_AUTH)))
		goto out;
	if ((err = verify_one_alg(xfrma, XFRMA_ALG_CRYPT)))
		goto out;
	if ((err = verify_one_alg(xfrma, XFRMA_ALG_COMP)))
		goto out;
	if ((err = verify_encap_tmpl(xfrma)))
		goto out;
#ifdef CONFIG_XFRM_ENHANCEMENT
	if ((err = verify_one_addr(xfrma)))
		goto out;
#endif

	err = -EINVAL;
	switch (p->mode) {
	case 0:
	case 1:
		break;

	default:
		goto out;
	};

	err = 0;

out:
	return err;
}

static int attach_one_algo(struct xfrm_algo **algpp, u8 *props,
			   struct xfrm_algo_desc *(*get_byname)(char *, int),
			   struct rtattr *u_arg)
{
	struct rtattr *rta = u_arg;
	struct xfrm_algo *p, *ualg;
	struct xfrm_algo_desc *algo;
	int len;

	if (!rta)
		return 0;

	ualg = RTA_DATA(rta);

	algo = get_byname(ualg->alg_name, 1);
	if (!algo)
		return -ENOSYS;
	*props = algo->desc.sadb_alg_id;

	len = sizeof(*ualg) + (ualg->alg_key_len + 7U) / 8;
	p = kmalloc(len, GFP_KERNEL);	
	if (!p)
		return -ENOMEM;

	memcpy(p, ualg, len); 
	*algpp = p;
	return 0;
}

static int attach_encap_tmpl(struct xfrm_encap_tmpl **encapp, struct rtattr *u_arg)
{
	struct rtattr *rta = u_arg;
	struct xfrm_encap_tmpl *p, *uencap;

	if (!rta)
		return 0;

	uencap = RTA_DATA(rta);
	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	memcpy(p, uencap, sizeof(*p));
	*encapp = p;
	return 0;
}

#ifdef CONFIG_XFRM_ENHANCEMENT
static int attach_one_addr(xfrm_address_t **addrpp, struct rtattr *u_arg)
{
	struct rtattr *rta = u_arg;
	xfrm_address_t *p, *uaddrp;

	if (!rta)
		return 0;

	uaddrp = RTA_DATA(rta);
	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	memcpy(p, uaddrp, sizeof(*p));
	*addrpp = p;
	return 0;
}
#endif

static void copy_from_user_state(struct xfrm_state *x, struct xfrm_usersa_info *p)
{
	memcpy(&x->id, &p->id, sizeof(x->id));
	memcpy(&x->sel, &p->sel, sizeof(x->sel));
	memcpy(&x->lft, &p->lft, sizeof(x->lft));
	x->props.mode = p->mode;
	x->props.replay_window = p->replay_window;
	x->props.reqid = p->reqid;
	x->props.family = p->family;
	x->props.saddr = p->saddr;
	x->props.flags = p->flags;
}

static struct xfrm_state *xfrm_state_construct(struct xfrm_usersa_info *p,
					       struct rtattr **xfrma,
					       int *errp)
{
	struct xfrm_state *x = xfrm_state_alloc();
	int err = -ENOMEM;

	if (!x)
		goto error_no_put;

	copy_from_user_state(x, p);

	if ((err = attach_one_algo(&x->aalg, &x->props.aalgo,
				   xfrm_aalg_get_byname,
				   xfrma[XFRMA_ALG_AUTH-1])))
		goto error;
	if ((err = attach_one_algo(&x->ealg, &x->props.ealgo,
				   xfrm_ealg_get_byname,
				   xfrma[XFRMA_ALG_CRYPT-1])))
		goto error;
	if ((err = attach_one_algo(&x->calg, &x->props.calgo,
				   xfrm_calg_get_byname,
				   xfrma[XFRMA_ALG_COMP-1])))
		goto error;
	if ((err = attach_encap_tmpl(&x->encap, xfrma[XFRMA_ENCAP-1])))
		goto error;
#ifdef CONFIG_XFRM_ENHANCEMENT
	if ((err = attach_one_addr(&x->coaddr, xfrma[XFRMA_ADDR-1])))
		goto error;
#endif
	err = -ENOENT;
	x->type = xfrm_get_type(x->id.proto, x->props.family);
	if (x->type == NULL)
		goto error;

	err = x->type->init_state(x, NULL);
	if (err)
		goto error;

	x->curlft.add_time = (unsigned long) xtime.tv_sec;
	x->km.state = XFRM_STATE_VALID;
	x->km.seq = p->seq;

	return x;

error:
	x->km.state = XFRM_STATE_DEAD;
	xfrm_state_put(x);
error_no_put:
	*errp = err;
	return NULL;
}

static int xfrm_add_sa(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_usersa_info *p = NLMSG_DATA(nlh);
	struct xfrm_state *x;
	int err;

	err = verify_newsa_info(p, (struct rtattr **) xfrma);
	if (err)
		return err;

	x = xfrm_state_construct(p, (struct rtattr **) xfrma, &err);
	if (!x)
		return err;

	if (nlh->nlmsg_type == XFRM_MSG_NEWSA)
		err = xfrm_state_add(x);
	else
		err = xfrm_state_update(x);

	if (err < 0) {
		x->km.state = XFRM_STATE_DEAD;
		xfrm_state_put(x);
	}

	return err;
}

#ifdef CONFIG_XFRM_ENHANCEMENT
static struct xfrm_state *xfrm_user_state_lookup(struct xfrm_usersa_id *p)
{
	switch (p->proto) {
	case IPPROTO_AH:
	case IPPROTO_ESP:
	case IPPROTO_COMP:
		return xfrm_state_lookup(&p->daddr, p->spi, p->proto, p->family);
#ifdef CONFIG_IPV6_MIP6
	case IPPROTO_IPV6:
	case IPPROTO_ROUTING:
	case IPPROTO_DSTOPTS:
		return xfrm_state_lookup_byaddr(&p->daddr, &p->saddr, p->proto,
						p->family);
#endif
	default:
		break;
	};
	return NULL;
}
#endif

static int xfrm_del_sa(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_state *x;
	struct xfrm_usersa_id *p = NLMSG_DATA(nlh);

#ifdef CONFIG_XFRM_ENHANCEMENT
	x = xfrm_user_state_lookup(p);
#else
	x = xfrm_state_lookup(&p->daddr, p->spi, p->proto, p->family);
#endif

	if (x == NULL)
		return -ESRCH;

	if (xfrm_state_kern(x)) {
		xfrm_state_put(x);
		return -EPERM;
	}

	xfrm_state_delete(x);
	xfrm_state_put(x);

	return 0;
}

static void copy_to_user_state(struct xfrm_state *x, struct xfrm_usersa_info *p)
{
	memcpy(&p->id, &x->id, sizeof(p->id));
	memcpy(&p->sel, &x->sel, sizeof(p->sel));
	memcpy(&p->lft, &x->lft, sizeof(p->lft));
	memcpy(&p->curlft, &x->curlft, sizeof(p->curlft));
	memcpy(&p->stats, &x->stats, sizeof(p->stats));
	p->saddr = x->props.saddr;
	p->mode = x->props.mode;
	p->replay_window = x->props.replay_window;
	p->reqid = x->props.reqid;
	p->family = x->props.family;
	p->flags = x->props.flags;
	p->seq = x->km.seq;
}

struct xfrm_dump_info {
	struct sk_buff *in_skb;
	struct sk_buff *out_skb;
	u32 nlmsg_seq;
	u16 nlmsg_flags;
	int start_idx;
	int this_idx;
};

static int dump_one_state(struct xfrm_state *x, int count, void *ptr)
{
	struct xfrm_dump_info *sp = ptr;
	struct sk_buff *in_skb = sp->in_skb;
	struct sk_buff *skb = sp->out_skb;
	struct xfrm_usersa_info *p;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;

	if (sp->this_idx < sp->start_idx)
		goto out;

	nlh = NLMSG_PUT(skb, NETLINK_CB(in_skb).pid,
			sp->nlmsg_seq,
			XFRM_MSG_NEWSA, sizeof(*p));
	nlh->nlmsg_flags = sp->nlmsg_flags;

	p = NLMSG_DATA(nlh);
	copy_to_user_state(x, p);

	if (x->aalg)
		RTA_PUT(skb, XFRMA_ALG_AUTH,
			sizeof(*(x->aalg))+(x->aalg->alg_key_len+7)/8, x->aalg);
	if (x->ealg)
		RTA_PUT(skb, XFRMA_ALG_CRYPT,
			sizeof(*(x->ealg))+(x->ealg->alg_key_len+7)/8, x->ealg);
	if (x->calg)
		RTA_PUT(skb, XFRMA_ALG_COMP, sizeof(*(x->calg)), x->calg);

	if (x->encap)
		RTA_PUT(skb, XFRMA_ENCAP, sizeof(*x->encap), x->encap);

#ifdef CONFIG_XFRM_ENHANCEMENT
	if (x->coaddr)
		RTA_PUT(skb, XFRMA_ADDR, sizeof(*x->coaddr), x->coaddr);
#endif

	nlh->nlmsg_len = skb->tail - b;
out:
	sp->this_idx++;
	return 0;

nlmsg_failure:
rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int xfrm_dump_sa(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct xfrm_dump_info info;

	info.in_skb = cb->skb;
	info.out_skb = skb;
	info.nlmsg_seq = cb->nlh->nlmsg_seq;
	info.nlmsg_flags = NLM_F_MULTI;
	info.this_idx = 0;
	info.start_idx = cb->args[0];
	(void) xfrm_state_walk(IPSEC_PROTO_ANY, dump_one_state, &info);
	cb->args[0] = info.this_idx;

	return skb->len;
}

static struct sk_buff *xfrm_state_netlink(struct sk_buff *in_skb,
					  struct xfrm_state *x, u32 seq)
{
	struct xfrm_dump_info info;
	struct sk_buff *skb;

	skb = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	NETLINK_CB(skb).dst_pid = NETLINK_CB(in_skb).pid;
	info.in_skb = in_skb;
	info.out_skb = skb;
	info.nlmsg_seq = seq;
	info.nlmsg_flags = 0;
	info.this_idx = info.start_idx = 0;

	if (dump_one_state(x, 0, &info)) {
		kfree_skb(skb);
		return NULL;
	}

	return skb;
}

static int xfrm_get_sa(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_usersa_id *p = NLMSG_DATA(nlh);
	struct xfrm_state *x;
	struct sk_buff *resp_skb;
	int err;

#ifdef CONFIG_XFRM_ENHANCEMENT
	x = xfrm_user_state_lookup(p);
#else
	x = xfrm_state_lookup(&p->daddr, p->spi, p->proto, p->family);
#endif

	err = -ESRCH;
	if (x == NULL)
		goto out_noput;

	resp_skb = xfrm_state_netlink(skb, x, nlh->nlmsg_seq);
	if (IS_ERR(resp_skb)) {
		err = PTR_ERR(resp_skb);
	} else {
		err = netlink_unicast(xfrm_nl, resp_skb,
				      NETLINK_CB(skb).pid, MSG_DONTWAIT);
	}
	xfrm_state_put(x);
out_noput:
	return err;
}

static int verify_userspi_info(struct xfrm_userspi_info *p)
{
	switch (p->info.id.proto) {
	case IPPROTO_AH:
	case IPPROTO_ESP:
		break;

	case IPPROTO_COMP:
		/* IPCOMP spi is 16-bits. */
		if (p->max >= 0x10000)
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	};

	if (p->min > p->max)
		return -EINVAL;

	return 0;
}

static int xfrm_alloc_userspi(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_state *x;
	struct xfrm_userspi_info *p;
	struct sk_buff *resp_skb;
	xfrm_address_t *daddr;
	int family;
	int err;

	p = NLMSG_DATA(nlh);
	err = verify_userspi_info(p);
	if (err)
		goto out_noput;

	family = p->info.family;
	daddr = &p->info.id.daddr;

	x = NULL;
	if (p->info.seq) {
		x = xfrm_find_acq_byseq(p->info.seq);
		if (x && xfrm_addr_cmp(&x->id.daddr, daddr, family)) {
			xfrm_state_put(x);
			x = NULL;
		}
	}

	if (!x)
		x = xfrm_find_acq(p->info.mode, p->info.reqid,
				  p->info.id.proto, daddr,
				  &p->info.saddr, 1,
				  family);
	err = -ENOENT;
	if (x == NULL)
		goto out_noput;

	resp_skb = ERR_PTR(-ENOENT);

	spin_lock_bh(&x->lock);
	if (x->km.state != XFRM_STATE_DEAD) {
		xfrm_alloc_spi(x, htonl(p->min), htonl(p->max));
		if (x->id.spi)
			resp_skb = xfrm_state_netlink(skb, x, nlh->nlmsg_seq);
	}
	spin_unlock_bh(&x->lock);

	if (IS_ERR(resp_skb)) {
		err = PTR_ERR(resp_skb);
		goto out;
	}

	err = netlink_unicast(xfrm_nl, resp_skb,
			      NETLINK_CB(skb).pid, MSG_DONTWAIT);

out:
	xfrm_state_put(x);
out_noput:
	return err;
}

static int verify_policy_dir(__u8 dir)
{
	switch (dir) {
	case XFRM_POLICY_IN:
	case XFRM_POLICY_OUT:
#ifdef CONFIG_USE_POLICY_FWD
	case XFRM_POLICY_FWD:
#endif
		break;

	default:
		return -EINVAL;
	};

	return 0;
}

static int verify_newpolicy_info(struct xfrm_userpolicy_info *p)
{
	switch (p->share) {
	case XFRM_SHARE_ANY:
	case XFRM_SHARE_SESSION:
	case XFRM_SHARE_USER:
	case XFRM_SHARE_UNIQUE:
		break;

	default:
		return -EINVAL;
	};

	switch (p->action) {
	case XFRM_POLICY_ALLOW:
	case XFRM_POLICY_BLOCK:
		break;

	default:
		return -EINVAL;
	};

	switch (p->sel.family) {
	case AF_INET:
		break;

	case AF_INET6:
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		break;
#else
		return  -EAFNOSUPPORT;
#endif

	default:
		return -EINVAL;
	};

	return verify_policy_dir(p->dir);
}

static void copy_templates(struct xfrm_policy *xp, struct xfrm_user_tmpl *ut,
			   int nr)
{
	int i;

	xp->xfrm_nr = nr;
	for (i = 0; i < nr; i++, ut++) {
		struct xfrm_tmpl *t = &xp->xfrm_vec[i];

		memcpy(&t->id, &ut->id, sizeof(struct xfrm_id));
		memcpy(&t->saddr, &ut->saddr,
		       sizeof(xfrm_address_t));
		t->reqid = ut->reqid;
		t->mode = ut->mode;
		t->share = ut->share;
		t->optional = ut->optional;
		t->aalgos = ut->aalgos;
		t->ealgos = ut->ealgos;
		t->calgos = ut->calgos;
	}
}

static int copy_from_user_tmpl(struct xfrm_policy *pol, struct rtattr **xfrma)
{
	struct rtattr *rt = xfrma[XFRMA_TMPL-1];
	struct xfrm_user_tmpl *utmpl;
	int nr;

	if (!rt) {
		pol->xfrm_nr = 0;
	} else {
		nr = (rt->rta_len - sizeof(*rt)) / sizeof(*utmpl);

		if (nr > XFRM_MAX_DEPTH)
			return -EINVAL;

		copy_templates(pol, RTA_DATA(rt), nr);
	}
	return 0;
}

static void copy_from_user_policy(struct xfrm_policy *xp, struct xfrm_userpolicy_info *p)
{
	xp->priority = p->priority;
	xp->index = p->index;
	memcpy(&xp->selector, &p->sel, sizeof(xp->selector));
	memcpy(&xp->lft, &p->lft, sizeof(xp->lft));
	xp->action = p->action;
	xp->flags = p->flags;
	xp->family = p->sel.family;
	/* XXX xp->share = p->share; */
}

static void copy_to_user_policy(struct xfrm_policy *xp, struct xfrm_userpolicy_info *p, int dir)
{
	memcpy(&p->sel, &xp->selector, sizeof(p->sel));
	memcpy(&p->lft, &xp->lft, sizeof(p->lft));
	memcpy(&p->curlft, &xp->curlft, sizeof(p->curlft));
	p->priority = xp->priority;
	p->index = xp->index;
	p->sel.family = xp->family;
	p->dir = dir;
	p->action = xp->action;
	p->flags = xp->flags;
	p->share = XFRM_SHARE_ANY; /* XXX xp->share */
}

static struct xfrm_policy *xfrm_policy_construct(struct xfrm_userpolicy_info *p, struct rtattr **xfrma, int *errp)
{
	struct xfrm_policy *xp = xfrm_policy_alloc(GFP_KERNEL);
	int err;

	if (!xp) {
		*errp = -ENOMEM;
		return NULL;
	}

	copy_from_user_policy(xp, p);
	err = copy_from_user_tmpl(xp, xfrma);
	if (err) {
		*errp = err;
		kfree(xp);
		xp = NULL;
	}

	return xp;
}

static int xfrm_add_policy(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_userpolicy_info *p = NLMSG_DATA(nlh);
	struct xfrm_policy *xp;
	int err;
	int excl;

	err = verify_newpolicy_info(p);
	if (err)
		return err;

	xp = xfrm_policy_construct(p, (struct rtattr **) xfrma, &err);
	if (!xp)
		return err;

	excl = nlh->nlmsg_type == XFRM_MSG_NEWPOLICY;
	err = xfrm_policy_insert(p->dir, xp, excl);
	if (err) {
		kfree(xp);
		return err;
	}

	xfrm_pol_put(xp);

	return 0;
}

static int copy_to_user_tmpl(struct xfrm_policy *xp, struct sk_buff *skb)
{
	struct xfrm_user_tmpl vec[XFRM_MAX_DEPTH];
	int i;

	if (xp->xfrm_nr == 0)
		return 0;

	for (i = 0; i < xp->xfrm_nr; i++) {
		struct xfrm_user_tmpl *up = &vec[i];
		struct xfrm_tmpl *kp = &xp->xfrm_vec[i];

		memcpy(&up->id, &kp->id, sizeof(up->id));
		up->family = xp->family;
		memcpy(&up->saddr, &kp->saddr, sizeof(up->saddr));
		up->reqid = kp->reqid;
		up->mode = kp->mode;
		up->share = kp->share;
		up->optional = kp->optional;
		up->aalgos = kp->aalgos;
		up->ealgos = kp->ealgos;
		up->calgos = kp->calgos;
	}
	RTA_PUT(skb, XFRMA_TMPL,
		(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr),
		vec);

	return 0;

rtattr_failure:
	return -1;
}

static int dump_one_policy(struct xfrm_policy *xp, int dir, int count, void *ptr)
{
	struct xfrm_dump_info *sp = ptr;
	struct xfrm_userpolicy_info *p;
	struct sk_buff *in_skb = sp->in_skb;
	struct sk_buff *skb = sp->out_skb;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;

	if (sp->this_idx < sp->start_idx)
		goto out;

	nlh = NLMSG_PUT(skb, NETLINK_CB(in_skb).pid,
			sp->nlmsg_seq,
			XFRM_MSG_NEWPOLICY, sizeof(*p));
	p = NLMSG_DATA(nlh);
	nlh->nlmsg_flags = sp->nlmsg_flags;

	copy_to_user_policy(xp, p, dir);
	if (copy_to_user_tmpl(xp, skb) < 0)
		goto nlmsg_failure;

	nlh->nlmsg_len = skb->tail - b;
out:
	sp->this_idx++;
	return 0;

nlmsg_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int xfrm_dump_policy(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct xfrm_dump_info info;

	info.in_skb = cb->skb;
	info.out_skb = skb;
	info.nlmsg_seq = cb->nlh->nlmsg_seq;
	info.nlmsg_flags = NLM_F_MULTI;
	info.this_idx = 0;
	info.start_idx = cb->args[0];
	(void) xfrm_policy_walk(dump_one_policy, &info);
	cb->args[0] = info.this_idx;

	return skb->len;
}

static struct sk_buff *xfrm_policy_netlink(struct sk_buff *in_skb,
					  struct xfrm_policy *xp,
					  int dir, u32 seq)
{
	struct xfrm_dump_info info;
	struct sk_buff *skb;

	skb = alloc_skb(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	NETLINK_CB(skb).dst_pid = NETLINK_CB(in_skb).pid;
	info.in_skb = in_skb;
	info.out_skb = skb;
	info.nlmsg_seq = seq;
	info.nlmsg_flags = 0;
	info.this_idx = info.start_idx = 0;

	if (dump_one_policy(xp, dir, 0, &info) < 0) {
		kfree_skb(skb);
		return NULL;
	}

	return skb;
}

static int xfrm_get_policy(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_policy *xp;
	struct xfrm_userpolicy_id *p;
	int err;
	int delete;

	p = NLMSG_DATA(nlh);
	delete = nlh->nlmsg_type == XFRM_MSG_DELPOLICY;

	err = verify_policy_dir(p->dir);
	if (err)
		return err;

	if (p->index)
		xp = xfrm_policy_byid(p->dir, p->index, delete);
	else
		xp = xfrm_policy_bysel(p->dir, &p->sel, delete);
	if (xp == NULL)
		return -ENOENT;

	if (!delete) {
		struct sk_buff *resp_skb;

		resp_skb = xfrm_policy_netlink(skb, xp, p->dir, nlh->nlmsg_seq);
		if (IS_ERR(resp_skb)) {
			err = PTR_ERR(resp_skb);
		} else {
			err = netlink_unicast(xfrm_nl, resp_skb,
					      NETLINK_CB(skb).pid,
					      MSG_DONTWAIT);
		}
	}

	xfrm_pol_put(xp);

	return err;
}

static int xfrm_flush_sa(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	struct xfrm_usersa_flush *p = NLMSG_DATA(nlh);

	xfrm_state_flush(p->proto);
	return 0;
}

static int xfrm_flush_policy(struct sk_buff *skb, struct nlmsghdr *nlh, void **xfrma)
{
	xfrm_policy_flush();
	return 0;
}

#ifdef CONFIG_XFRM_ENHANCEMENT
static int __xfrm_addr_any(const xfrm_address_t *a, unsigned short family)
{
	switch (family) {
	case AF_INET:
		return (a->a4 == 0);
	case AF_INET6:
		return ipv6_addr_any((const struct in6_addr *)a->a6);
	}
	return -1; /* XXX: */
}

static int __xfrm_addr_cmp(const xfrm_address_t *a, const xfrm_address_t *b,
			   unsigned short family)
{
	switch (family) {
	case AF_INET:
		return memcmp(&(a->a4), &(b->a4), sizeof(__u32));
	case AF_INET6:
		return ipv6_addr_equal((const struct in6_addr *)a->a6,
				     (const struct in6_addr *)b->a6);
	}
	return -1; /* XXX: */
}

static struct xfrm_policy *__policy_clone(struct xfrm_policy *old)
{
	int i;
	struct xfrm_policy *xp = xfrm_policy_alloc(GFP_ATOMIC);
	if (xp) {
		memcpy(&xp->selector, &old->selector, sizeof(xp->selector));
		memcpy(&xp->lft, &old->lft, sizeof(xp->lft));
		//memcpy(&xp->curlft, &old->curlft, sizeof(xp->curlft));
		xp->priority = old->priority;
		xp->index = old->index;
		xp->family = old->family;
		xp->action = old->action;
		xp->flags = old->flags;
		xp->xfrm_nr = old->xfrm_nr;
		for (i = 0; i < xp->xfrm_nr; i++)
			memcpy(&xp->xfrm_vec[i], &old->xfrm_vec[i],
			       sizeof(xp->xfrm_vec[i]));
	}
	return xp;
}

static int __tmpl_filter(struct xfrm_tmpl *t, void *arg)
{
	struct xfrm_usersa_mutateaddress *m;
	m = (struct xfrm_usersa_mutateaddress *)arg;

	if (t->id.proto != m->proto)
		return 0;
	if (__xfrm_addr_cmp(&t->id.daddr, &m->old_daddr, m->family))
		return 0;
	if (__xfrm_addr_cmp(&t->saddr, &m->old_saddr, m->family))
		return 0;
	if (t->mode != m->mode)
		return 0;
	if (t->reqid != 0 && t->reqid != m->reqid)
		return 0;

	return 1;
}

static int __tmpl_find(struct xfrm_usersa_mutateaddress *m,
		       struct xfrm_policy *xp, unsigned char *tmaskp)
{
	int matched = 0;
	int err = 0;
	int i;

	for (i = 0; i < xp->xfrm_nr; i++) {
		struct xfrm_tmpl *t = &xp->xfrm_vec[i];
		int ret = __tmpl_filter(t, m);
		if (ret < 0) {
			err = ret;
			break;
		} else if (ret == 0)
			continue;

		*tmaskp |= (1 << i);

		matched ++;
	}

	if (err < 0)
		return err;
	return matched;
}

static int __tmpl_change(struct xfrm_usersa_mutateaddress *m, int dir,
			 struct xfrm_policy *xp, unsigned char tmask)
{
	struct xfrm_policy *new_xp = NULL;
	int err;
	int i;

	/*
	 * make new policy, store new address to its tmpl
	 */
	new_xp = __policy_clone(xp);
	if (!new_xp) {
		XFRM_DBG("%s: failed to clone policy\n", __FUNCTION__);
		err = -ENOMEM;
		goto error;
	}

	for (i = 0; i < xp->xfrm_nr; i++) {
		struct xfrm_tmpl *t = &new_xp->xfrm_vec[i];

		if ((tmask & (1 << i)) == 0)
			continue;

		memcpy(&t->id.daddr, &m->new_daddr, sizeof(t->id.daddr));
		memcpy(&t->saddr, &m->new_saddr, sizeof(t->saddr));
	}

	/*
	 * replace the policy
	 */
	err = xfrm_policy_insert(dir, new_xp, 0);
	if (err) {
		XFRM_DBG("%s: failed to insert policy: %d\n", __FUNCTION__, err);
		kfree(new_xp);
		goto error;
	}
	xfrm_pol_put(new_xp);

 error:
	return err;
}

static int __policy_change(struct xfrm_usersa_mutateaddress *m,
			   struct xfrm_userpolicy_id *p, unsigned char *tmaskp)
{
	struct xfrm_policy *xp;
	int err = 0;

	/*
	 * find policy
	 */
	if (p->index)
		xp = xfrm_policy_byid(p->dir, p->index, 0);
	else
		xp = xfrm_policy_bysel(p->dir, &p->sel, 0);
	if (xp == NULL) {
		XFRM_DBG("%s: no such policy\n", __FUNCTION__);
		err = -ENOENT;
		goto error;
	}

	/*
	 * find templates and store index as bit mask.
	 */
	err = __tmpl_find(m, xp, tmaskp);
	if (err < 0) {
		xfrm_pol_put(xp);
		goto error;
	} else if (err == 0) {
		XFRM_DBG("%s: no matching template in such a policy as index = %u\n", __FUNCTION__, xp->index);
		xfrm_pol_put(xp);
		err = -ENOENT;
		goto error;
	}

	err = __tmpl_change(m, p->dir, xp, *tmaskp);
	xfrm_pol_put(xp);

 error:
	return err;
}

static int __policy_restore(struct xfrm_usersa_mutateaddress *m,
			    struct xfrm_userpolicy_id *p, unsigned char tmask)
{
	struct xfrm_policy *xp;
	int err = 0;

	/*
	 * find policy
	 */
	if (p->index)
		xp = xfrm_policy_byid(p->dir, p->index, 0);
	else
		xp = xfrm_policy_bysel(p->dir, &p->sel, 0);
	if (xp == NULL) {
		XFRM_DBG("%s: no such policy\n", __FUNCTION__);
		err = -ENOENT;
		goto error;
	}

	err = __tmpl_change(m, p->dir, xp, tmask);
	xfrm_pol_put(xp);

 error:
	return err;
}

static int __xfrm_mutate_address_policy(struct xfrm_usersa_mutateaddress *m,
					struct xfrm_userpolicy_id *pol, int nr)
{
	/* Bit mask for template index on each policy. Note XFRM_MAX_DEPTH. */
	unsigned char *tmpl_masks;
	struct xfrm_userpolicy_id *p = pol;
	int err = 0;
	int i;

	tmpl_masks = kmalloc(sizeof(*tmpl_masks) * nr, GFP_KERNEL);
	if (!tmpl_masks) {
		err = -ENOMEM;
		goto out;
	}
	memset(tmpl_masks, 0, sizeof(*tmpl_masks) * nr);

	for (i = 0; i < nr; i++, p++) {
		err = verify_policy_dir(p->dir);
		if (err) {
			XFRM_DBG("%s: invalid dir = %d at XFRMA no.%d\n", __FUNCTION__, p->dir, i);
			break;
		}

		err = __policy_change(m, p, &tmpl_masks[i]);
		if (err) {
			XFRM_DBG("%s: failed to change policy at XFRMA no.%d\n", __FUNCTION__, i);
			break;
		}
	}

	if (err && i > 0) {
		/* XXX: try to recover templates which was already modified */
		xfrm_address_t addr;
		int j;

		memcpy(&addr, &m->old_daddr, sizeof(addr));
		memcpy(&m->old_daddr, &m->new_daddr, sizeof(m->old_daddr));
		memcpy(&m->new_daddr, &addr, sizeof(m->new_daddr));

		memcpy(&addr, &m->old_saddr, sizeof(addr));
		memcpy(&m->old_saddr, &m->new_saddr, sizeof(m->old_saddr));
		memcpy(&m->new_saddr, &addr, sizeof(m->new_saddr));

		p = pol;
		for (j = 0; j < i; j++, p++)
			__policy_restore(m, p, tmpl_masks[j]);
	}

 out:
	kfree(tmpl_masks);
#if XFRM_DEBUG >= 4
	if (!err)
		XFRM_DBG("%s: %d policy/policies replaced\n", __FUNCTION__, i);
#endif
	return err;
}

static int xfrm_mutate_address_policy(struct xfrm_usersa_mutateaddress *m,
				      struct rtattr **xfrma)
{
	struct rtattr *rt = xfrma[XFRMA_POLICY-1];
	int nr;

	if (!rt)
		return 0;

	nr = (rt->rta_len - sizeof(*rt)) / sizeof(struct xfrm_userpolicy_id);
	if (nr <= 0)
		return 0;

	return __xfrm_mutate_address_policy(m, RTA_DATA(rt), nr);
}

static struct xfrm_algo *__algo_clone(struct xfrm_algo *old)
{
	struct xfrm_algo *a;
	a = kmalloc(sizeof(*old) + old->alg_key_len, GFP_KERNEL);
	if (a)
		memcpy(a, old, sizeof(*old) + old->alg_key_len);
	return a;
}

static struct xfrm_state *__state_clone(struct xfrm_state *old, int *errp)
{
	int err = -ENOMEM;
	struct xfrm_state *x = xfrm_state_alloc();
	if (!x)
		goto error;

	memcpy(&x->id, &old->id, sizeof(x->id));
	memcpy(&x->sel, &old->sel, sizeof(x->sel));
	memcpy(&x->lft, &old->lft, sizeof(x->lft));
	x->props.mode = old->props.mode;
	x->props.replay_window = old->props.replay_window;
	x->props.reqid = old->props.reqid;
	x->props.family = old->props.family;
	x->props.saddr = old->props.saddr;
	x->props.flags = old->props.flags;

	if (old->aalg) {
		x->aalg = __algo_clone(old->aalg);
		if (!x->aalg)
			goto error;
	}
	x->props.aalgo = old->props.aalgo;

	if (old->ealg) {
		x->ealg = __algo_clone(old->ealg);
		if (!x->ealg)
			goto error;
	}
	x->props.ealgo = old->props.ealgo;

	if (old->calg) {
		x->calg = __algo_clone(old->calg);
		if (!x->calg)
			goto error;
	}
	x->props.calgo = old->props.calgo;

	if (old->encap) {
		x->encap = kmalloc(sizeof(*x->encap), GFP_KERNEL);
		if (!x->encap)
			goto error;
		memcpy(x->encap, old->encap, sizeof(*x->encap));
	}

	if (old->coaddr) {
		x->coaddr = kmalloc(sizeof(*x->coaddr), GFP_KERNEL);
		if (!x->coaddr)
			goto error;
		memcpy(x->coaddr, old->coaddr, sizeof(*x->coaddr));
	}

	err = -ENOENT;
	x->type = xfrm_get_type(x->id.proto, x->props.family);
	if (x->type == NULL)
		goto error;

	err = x->type->init_state(x, NULL);
	if (err)
		goto error;

	x->curlft.add_time = old->curlft.add_time;
	x->km.state = old->km.state;
	x->km.seq = old->km.seq;

	return x;

 error:
	if (errp)
		*errp = err;
	if (x) {
		if (x->aalg)
			kfree(x->aalg);
		if (x->ealg)
			kfree(x->ealg);
		if (x->ealg)
			kfree(x->ealg);
		if (x->encap)
			kfree(x->encap);
		if (x->coaddr)
			kfree(x->coaddr);
		kfree(x);
	}
	return NULL;
}

static struct xfrm_state *__state_construct(struct xfrm_state *x, void *arg,
					    int *errp)
{
	struct xfrm_usersa_mutateaddress *m;
	struct xfrm_state *new_x;

	m = (struct xfrm_usersa_mutateaddress *)arg;

	new_x = __state_clone(x, errp);
	if (!new_x) {
		if (errp)
			*errp = -ENOMEM;
	} else {
		memcpy(&new_x->id.daddr, &m->new_daddr,
		       sizeof(new_x->id.daddr));
		memcpy(&new_x->props.saddr, &m->new_saddr,
		       sizeof(new_x->props.saddr));
	}

	return new_x;
}

static void __state_destruct(struct xfrm_state *x, void *arg)
{
	kfree(x);
}

static int __state_filter(struct xfrm_state *x, void *arg)
{
	struct xfrm_usersa_mutateaddress *m;
	m = (struct xfrm_usersa_mutateaddress *)arg;

	if (x->props.family != m->family)
		return 0;
	if (x->id.proto != m->proto)
		return 0;
	if (__xfrm_addr_cmp(&x->id.daddr, &m->old_daddr, m->family))
		return 0;
	if (__xfrm_addr_cmp(&x->props.saddr, &m->old_saddr, m->family))
		return 0;
	if (x->props.mode != m->mode)
		return 0;
	if (x->props.reqid != 0 && x->props.reqid != m->reqid)
		return 0;

	return 1;
}

/*
 * Change the endpoint addresses.
 *
 * XXX: Todo: it must require a lock for policy DB to update tempaltes
 * operation during all policy is complete; e.g. xfrm_mutate_address_poilcy()
 * feature will be placed in net/xfrm/xfrm_policy.c.
 * Alternatively it should require a lock for both state and policy DB during
 * all operation in this function.
 */
static int xfrm_mutate_address(struct sk_buff *skb, struct nlmsghdr *nlh,
			       void **xfrma)
{
	struct xfrm_usersa_mutateaddress *m = NLMSG_DATA(nlh);
	struct xfrm_state_bulk_info *old_bi = NULL;
	struct xfrm_state_bulk_info *new_bi = NULL;
	int delete_only = 0;
	int insert_force = 0;
	int err = -EINVAL;

	switch (m->family) {
	case AF_INET:
		break;
	case AF_INET6:
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		break;
#else
		XFRM_DBG("%s: not supported family = %u\n", __FUNCTION__, m->family);
		err = -EAFNOSUPPORT;
		goto error;
#endif
	default:
		XFRM_DBG("%s: invalid family = %u\n", __FUNCTION__, m->family);
		err = -EINVAL;
		goto error;
	}

	if (__xfrm_addr_cmp(&m->old_daddr, &m->new_daddr, m->family) == 0 &&
	    __xfrm_addr_cmp(&m->old_saddr, &m->new_saddr, m->family) == 0) {
		XFRM_DBG("%s: old/new addresses do not differ\n", __FUNCTION__);
		err = -EINVAL;
		goto error;
	}
	if (__xfrm_addr_any(&m->new_daddr, m->family) &&
	    __xfrm_addr_any(&m->new_saddr, m->family))
		delete_only = 1;
	if (__xfrm_addr_cmp(&m->old_daddr, &m->new_daddr, m->family) == 0)
		insert_force = 1;

	old_bi = xfrm_state_bulk_alloc();
	if (!old_bi) {
		err = -ENOMEM;
		XFRM_DBG("%s: failed to bulk alloc (for old): %d\n", __FUNCTION__, err);
		goto error;
	}

	/*
	 * find matching SAs
	 */
	err = xfrm_state_bulk_hold(old_bi, m, __state_filter);
	if (err) {
		XFRM_DBG("%s: failed to bulk hold: %d\n", __FUNCTION__, err);
		goto error;
	}

	if (!delete_only) {
		new_bi = xfrm_state_bulk_alloc();
		if (!new_bi) {
			err = -ENOMEM;
			XFRM_DBG("%s: failed to bulk alloc (for new): %d\n", __FUNCTION__, err);
			goto error_put;
		}

		/*
		 * make new states with new addersses
		 */
		err = xfrm_state_bulk_rearrange(new_bi, old_bi, m,
						__state_construct,
						__state_destruct);
		if (err) {
			XFRM_DBG("%s: failed to bulk rearrange: %d\n", __FUNCTION__, err);
			goto error_put;
		}

		/*
		 * insert new SAs
		 */
		err = xfrm_state_bulk_insert(new_bi, insert_force);
		if (err) {
			XFRM_DBG("%s: failed to bulk insert: %d\n", __FUNCTION__, err);
			xfrm_state_bulk_destruct(new_bi, m, __state_destruct);
			goto error_put;
		}

		/*
		 * update policies
		 */
		err = xfrm_mutate_address_policy(m, (struct rtattr **)xfrma);
		if (err) {
			xfrm_state_bulk_delete(new_bi);
			xfrm_state_bulk_put(new_bi);
			goto error_put;
		}
	}

	/*
	 * delete old SAs
	 */
	xfrm_state_bulk_delete(old_bi);
#if XFRM_DEBUG >= 4
	printk(KERN_DEBUG "%s: %d state(s) deleted\n", __FUNCTION__, old_bi->count);
#endif
	xfrm_state_bulk_put(old_bi);
	xfrm_state_bulk_free(old_bi);
	if (!delete_only) {
#if XFRM_DEBUG >= 4
		printk(KERN_DEBUG "%s: %d state(s) inserted\n", __FUNCTION__, new_bi->count);
#endif
		xfrm_state_bulk_put(new_bi);
		xfrm_state_bulk_free(new_bi);
	}

#if XFRM_DEBUG >= 4
	printk(KERN_DEBUG "%s: changed results:\n", __FUNCTION__);
	printk(KERN_DEBUG "  old-daddr = %s,\n", XFRMSTRADDR(m->old_daddr, m->family));
	printk(KERN_DEBUG "  old-saddr = %s\n", XFRMSTRADDR(m->old_saddr, m->family));
	printk(KERN_DEBUG "  new-daddr = %s,\n", XFRMSTRADDR(m->new_daddr, m->family));
	printk(KERN_DEBUG "  new-saddr = %s,\n", XFRMSTRADDR(m->new_saddr, m->family));
	printk(KERN_DEBUG "  proto = %u, mode = %u, reqid = %u\n", m->proto, m->mode, m->reqid);
#endif
	return 0;

 error_put:
	xfrm_state_bulk_put(old_bi);

 error:
	if (old_bi)
		xfrm_state_bulk_free(old_bi);
	if (new_bi)
		xfrm_state_bulk_free(new_bi);

	return err;
}
#endif

#define XMSGSIZE(type) NLMSG_LENGTH(sizeof(struct type))

static const int xfrm_msg_min[XFRM_NR_MSGTYPES] = {
	[XFRM_MSG_NEWSA       - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_info),
	[XFRM_MSG_DELSA       - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_id),
	[XFRM_MSG_GETSA       - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_id),
	[XFRM_MSG_NEWPOLICY   - XFRM_MSG_BASE] = XMSGSIZE(xfrm_userpolicy_info),
	[XFRM_MSG_DELPOLICY   - XFRM_MSG_BASE] = XMSGSIZE(xfrm_userpolicy_id),
	[XFRM_MSG_GETPOLICY   - XFRM_MSG_BASE] = XMSGSIZE(xfrm_userpolicy_id),
	[XFRM_MSG_ALLOCSPI    - XFRM_MSG_BASE] = XMSGSIZE(xfrm_userspi_info),
	[XFRM_MSG_ACQUIRE     - XFRM_MSG_BASE] = XMSGSIZE(xfrm_user_acquire),
	[XFRM_MSG_EXPIRE      - XFRM_MSG_BASE] = XMSGSIZE(xfrm_user_expire),
	[XFRM_MSG_UPDPOLICY   - XFRM_MSG_BASE] = XMSGSIZE(xfrm_userpolicy_info),
	[XFRM_MSG_UPDSA       - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_info),
	[XFRM_MSG_POLEXPIRE   - XFRM_MSG_BASE] = XMSGSIZE(xfrm_user_polexpire),
	[XFRM_MSG_FLUSHSA     - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_flush),
	[XFRM_MSG_FLUSHPOLICY - XFRM_MSG_BASE] = NLMSG_LENGTH(0),
#ifdef CONFIG_XFRM_ENHANCEMENT
	[XFRM_MSG_MUTATEADDR  - XFRM_MSG_BASE] = XMSGSIZE(xfrm_usersa_mutateaddress),
#endif
#ifdef CONFIG_IPV6_MIP6
	[XFRM_MSG_MIP6NOTIFY  - XFRM_MSG_BASE] = XMSGSIZE(xfrm_user_mip6notify),
#endif
};
#undef XMSGSIZE

#ifdef CONFIG_NET_KEY_MIGRATE
/*
 *   Migrate IP address stored in SPD and SADB
 *
 *   NOTE:
 *   - This routine is called when the kernel receives PF_KEY MIGRATE
 *     message from userland application (i.e. MIPv6)
 *   - Does exaclty same thing as xfrm_mutate_address().
 *   - Only single SP entry is to be updated since PF_KEY MIGRATE message
 *     is somewhat SP-oriented message.  Hence this routing should be
 *     called per SPD entry to be updated.
 */
int
xfrm_migrate_address(struct xfrm_usersa_mutateaddress *maddr,
		     struct xfrm_userpolicy_id *pol)
{
	struct xfrm_state_bulk_info *old_bi = NULL;
	struct xfrm_state_bulk_info *new_bi = NULL;
	int delete_only = 0;
	int insert_force = 0;
	int sa_found = 0;
	int err = -EINVAL;

	if (!maddr)
		goto error;

	switch (maddr->family) {
	case AF_INET:
		break;
	case AF_INET6:
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		break;
#else
		XFRM_DBG("%s: not supported family = %u\n",
			 __FUNCTION__, maddr->family);
		err = -EAFNOSUPPORT;
		goto error;
#endif
	default:
		XFRM_DBG("%s: invalid family = %u\n",
			 __FUNCTION__, maddr->family);
		err = -EINVAL;
		goto error;
	}

        if (__xfrm_addr_cmp(&maddr->old_daddr, &maddr->new_daddr,
			    maddr->family) == 0 &&
	    __xfrm_addr_cmp(&maddr->old_saddr, &maddr->new_saddr,
			     maddr->family) == 0) {
		XFRM_DBG("%s: old/new addresses do not differ\n", __FUNCTION__);
		err = -EINVAL;
		goto error;
	}

	if (__xfrm_addr_any(&maddr->new_daddr, maddr->family) &&
	    __xfrm_addr_any(&maddr->new_saddr, maddr->family))
		delete_only = 1;
        if (__xfrm_addr_cmp(&maddr->old_daddr, &maddr->new_daddr,
			    maddr->family) == 0)
		insert_force = 1;
        
	old_bi = xfrm_state_bulk_alloc();
	if (!old_bi) {
		err = -ENOMEM;
		XFRM_DBG("%s: failed to bulk alloc (for old): %d\n",
			 __FUNCTION__, err);
		goto error;
	}

	/* find matching SAs */
	err = xfrm_state_bulk_hold(old_bi, maddr, __state_filter);
	if (err) {
#if XFRM_DEBUG >= 4
		XFRM_DBG("%s: there is no matching SA found\n", __FUNCTION__);
#endif
	} else
		sa_found = 1;
	
	if (!delete_only) {
		if (sa_found) {
			new_bi = xfrm_state_bulk_alloc();
			if (!new_bi) {
				err = -ENOMEM;
				XFRM_DBG("%s: failed to bulk alloc (for new): %d\n", __FUNCTION__, err);
				goto error_put;
	                }

			/* make new states with new addresses */
			err = xfrm_state_bulk_rearrange(new_bi, old_bi, maddr,
							__state_construct,
							__state_destruct);
			if (err) {
				XFRM_DBG("%s: failed to bulk rearrange: %d\n", __FUNCTION__, err);
				goto error_put;
			}
	
			/* insert new SAs */
			err = xfrm_state_bulk_insert(new_bi, insert_force);
			if (err) {
				XFRM_DBG("%s: failed to bulk insert: %d\n", __FUNCTION__, err);
				xfrm_state_bulk_destruct(new_bi, maddr,
							 __state_destruct);
				goto error_put;
			}
		}
		
		/* update single SP entry */
		err = __xfrm_mutate_address_policy(maddr, pol, 1);
	}

	if (sa_found) {
		/* delete old SAs */
		xfrm_state_bulk_delete(old_bi);
#if XFRM_DEBUG >= 4
		printk(KERN_DEBUG "%s: %d state(s) deleted\n",
		       __FUNCTION__, old_bi->count);
#endif
		xfrm_state_bulk_put(old_bi);
		xfrm_state_bulk_free(old_bi);
		if (!delete_only) {
#if XFRM_DEBUG >= 4
			printk(KERN_DEBUG "%s: %d state(s) inserted\n",
			       __FUNCTION__, new_bi->count);
#endif
			xfrm_state_bulk_put(new_bi);
			xfrm_state_bulk_free(new_bi);
		}
	}
#if XFRM_DEBUG >= 4
	printk(KERN_DEBUG "%s: changed results:\n", __FUNCTION__);
	printk(KERN_DEBUG "  old-daddr = %s,\n",
	       XFRMSTRADDR(maddr->old_daddr, maddr->family));
        printk(KERN_DEBUG "  old-saddr = %s\n",
	       XFRMSTRADDR(maddr->old_saddr, maddr->family));
        printk(KERN_DEBUG "  new-daddr = %s,\n",
	       XFRMSTRADDR(maddr->new_daddr, maddr->family));
        printk(KERN_DEBUG "  new-saddr = %s,\n",
	       XFRMSTRADDR(maddr->new_saddr, maddr->family));
        printk(KERN_DEBUG "  proto = %u, mode = %u, reqid = %u",
	       maddr->proto, maddr->mode, maddr->reqid);
#endif
        return 0;
                                                                                
 error_put:
        xfrm_state_bulk_put(old_bi);
                                                                                
 error:
        if (old_bi)
                xfrm_state_bulk_free(old_bi);
        if (new_bi)
                xfrm_state_bulk_free(new_bi);
                                                                                
        return err;
}
#endif /* CONFIG_NET_KEY_MIGRATE */

static struct xfrm_link {
	int (*doit)(struct sk_buff *, struct nlmsghdr *, void **);
	int (*dump)(struct sk_buff *, struct netlink_callback *);
} xfrm_dispatch[XFRM_NR_MSGTYPES] = {
	[XFRM_MSG_NEWSA       - XFRM_MSG_BASE] = { .doit = xfrm_add_sa         },
	[XFRM_MSG_DELSA       - XFRM_MSG_BASE] = { .doit = xfrm_del_sa         },
	[XFRM_MSG_GETSA       - XFRM_MSG_BASE] = { .doit = xfrm_get_sa,
						   .dump = xfrm_dump_sa        },
	[XFRM_MSG_NEWPOLICY   - XFRM_MSG_BASE] = { .doit = xfrm_add_policy     },
	[XFRM_MSG_DELPOLICY   - XFRM_MSG_BASE] = { .doit = xfrm_get_policy     },
	[XFRM_MSG_GETPOLICY   - XFRM_MSG_BASE] = { .doit = xfrm_get_policy,
						   .dump = xfrm_dump_policy    },
	[XFRM_MSG_ALLOCSPI    - XFRM_MSG_BASE] = { .doit = xfrm_alloc_userspi  },
	[XFRM_MSG_UPDPOLICY   - XFRM_MSG_BASE] = { .doit = xfrm_add_policy     },
	[XFRM_MSG_UPDSA       - XFRM_MSG_BASE] = { .doit = xfrm_add_sa         },
	[XFRM_MSG_FLUSHSA     - XFRM_MSG_BASE] = { .doit = xfrm_flush_sa       },
	[XFRM_MSG_FLUSHPOLICY - XFRM_MSG_BASE] = { .doit = xfrm_flush_policy   },
#ifdef CONFIG_XFRM_ENHANCEMENT
	[XFRM_MSG_MUTATEADDR  - XFRM_MSG_BASE] = { .doit = xfrm_mutate_address }, 
#endif
#ifdef CONFIG_IPV6_MIP6
	[XFRM_MSG_MIP6NOTIFY  - XFRM_MSG_BASE] = {},
#endif
};

static int xfrm_done(struct netlink_callback *cb)
{
	return 0;
}

static int xfrm_user_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh, int *errp)
{
	struct rtattr *xfrma[XFRMA_MAX];
	struct xfrm_link *link;
	int type, min_len;

	if (!(nlh->nlmsg_flags & NLM_F_REQUEST))
		return 0;

	type = nlh->nlmsg_type;

	/* A control message: ignore them */
	if (type < XFRM_MSG_BASE)
		return 0;

	/* Unknown message: reply with EINVAL */
	if (type > XFRM_MSG_MAX)
		goto err_einval;

	type -= XFRM_MSG_BASE;
	link = &xfrm_dispatch[type];

	/* All operations require privileges, even GET */
	if (security_netlink_recv(skb)) {
		*errp = -EPERM;
		return -1;
	}

	if ((type == (XFRM_MSG_GETSA - XFRM_MSG_BASE) ||
	     type == (XFRM_MSG_GETPOLICY - XFRM_MSG_BASE)) &&
	    (nlh->nlmsg_flags & NLM_F_DUMP)) {
		u32 rlen;

		if (link->dump == NULL)
			goto err_einval;

		if ((*errp = netlink_dump_start(xfrm_nl, skb, nlh,
						link->dump,
						xfrm_done)) != 0) {
			return -1;
		}
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		skb_pull(skb, rlen);
		return -1;
	}

	memset(xfrma, 0, sizeof(xfrma));

	if (nlh->nlmsg_len < (min_len = xfrm_msg_min[type]))
		goto err_einval;

	if (nlh->nlmsg_len > min_len) {
		int attrlen = nlh->nlmsg_len - NLMSG_ALIGN(min_len);
		struct rtattr *attr = (void *) nlh + NLMSG_ALIGN(min_len);

		while (RTA_OK(attr, attrlen)) {
			unsigned short flavor = attr->rta_type;
			if (flavor) {
				if (flavor > XFRMA_MAX)
					goto err_einval;
				xfrma[flavor - 1] = attr;
			}
			attr = RTA_NEXT(attr, attrlen);
		}
	}

	if (link->doit == NULL)
		goto err_einval;
	*errp = link->doit(skb, nlh, (void **) &xfrma);

	return *errp;

err_einval:
	*errp = -EINVAL;
	return -1;
}

static int xfrm_user_rcv_skb(struct sk_buff *skb)
{
	int err;
	struct nlmsghdr *nlh;

	while (skb->len >= NLMSG_SPACE(0)) {
		u32 rlen;

		nlh = (struct nlmsghdr *) skb->data;
		if (nlh->nlmsg_len < sizeof(*nlh) ||
		    skb->len < nlh->nlmsg_len)
			return 0;
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		if (xfrm_user_rcv_msg(skb, nlh, &err) < 0) {
			if (err == 0)
				return -1;
			netlink_ack(skb, nlh, err);
		} else if (nlh->nlmsg_flags & NLM_F_ACK)
			netlink_ack(skb, nlh, 0);
		skb_pull(skb, rlen);
	}

	return 0;
}

static void xfrm_netlink_rcv(struct sock *sk, int len)
{
	unsigned int qlen = skb_queue_len(&sk->sk_receive_queue);

	do {
		struct sk_buff *skb;

		down(&xfrm_cfg_sem);

		if (qlen > skb_queue_len(&sk->sk_receive_queue))
			qlen = skb_queue_len(&sk->sk_receive_queue);

		for (; qlen; qlen--) {
			skb = skb_dequeue(&sk->sk_receive_queue);
			if (xfrm_user_rcv_skb(skb)) {
				if (skb->len)
					skb_queue_head(&sk->sk_receive_queue,
						       skb);
				else {
					kfree_skb(skb);
					qlen--;
				}
				break;
			}
			kfree_skb(skb);
		}

		up(&xfrm_cfg_sem);

	} while (qlen);
}

static int build_expire(struct sk_buff *skb, struct xfrm_state *x, int hard)
{
	struct xfrm_user_expire *ue;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;

	nlh = NLMSG_PUT(skb, 0, 0, XFRM_MSG_EXPIRE,
			sizeof(*ue));
	ue = NLMSG_DATA(nlh);
	nlh->nlmsg_flags = 0;

	copy_to_user_state(x, &ue->state);
	ue->hard = (hard != 0) ? 1 : 0;

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int xfrm_send_state_notify(struct xfrm_state *x, int hard)
{
	struct sk_buff *skb;

	skb = alloc_skb(sizeof(struct xfrm_user_expire) + 16, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_expire(skb, x, hard) < 0)
		BUG();

	NETLINK_CB(skb).dst_groups = XFRMGRP_EXPIRE;

	return netlink_broadcast(xfrm_nl, skb, 0, XFRMGRP_EXPIRE, GFP_ATOMIC);
}

static int build_acquire(struct sk_buff *skb, struct xfrm_state *x,
			 struct xfrm_tmpl *xt, struct xfrm_policy *xp,
			 int dir)
{
	struct xfrm_user_acquire *ua;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;
	__u32 seq = xfrm_get_acqseq();

	nlh = NLMSG_PUT(skb, 0, 0, XFRM_MSG_ACQUIRE,
			sizeof(*ua));
	ua = NLMSG_DATA(nlh);
	nlh->nlmsg_flags = 0;

	memcpy(&ua->id, &x->id, sizeof(ua->id));
	memcpy(&ua->saddr, &x->props.saddr, sizeof(ua->saddr));
	memcpy(&ua->sel, &x->sel, sizeof(ua->sel));
	copy_to_user_policy(xp, &ua->policy, dir);
	ua->aalgos = xt->aalgos;
	ua->ealgos = xt->ealgos;
	ua->calgos = xt->calgos;
	ua->seq = x->km.seq = seq;

	if (copy_to_user_tmpl(xp, skb) < 0)
		goto nlmsg_failure;

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int xfrm_send_acquire(struct xfrm_state *x, struct xfrm_tmpl *xt,
			     struct xfrm_policy *xp, int dir)
{
	struct sk_buff *skb;
	size_t len;

	len = RTA_SPACE(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr);
	len += NLMSG_SPACE(sizeof(struct xfrm_user_acquire));
	skb = alloc_skb(len, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_acquire(skb, x, xt, xp, dir) < 0)
		BUG();

	NETLINK_CB(skb).dst_groups = XFRMGRP_ACQUIRE;

	return netlink_broadcast(xfrm_nl, skb, 0, XFRMGRP_ACQUIRE, GFP_ATOMIC);
}

/* User gives us xfrm_user_policy_info followed by an array of 0
 * or more templates.
 */
struct xfrm_policy *xfrm_compile_policy(u16 family, int opt,
                                        u8 *data, int len, int *dir)
{
	struct xfrm_userpolicy_info *p = (struct xfrm_userpolicy_info *)data;
	struct xfrm_user_tmpl *ut = (struct xfrm_user_tmpl *) (p + 1);
	struct xfrm_policy *xp;
	int nr;

	switch (family) {
	case AF_INET:
		if (opt != IP_XFRM_POLICY && opt != IP_IPSEC_POLICY) {
			*dir = -EOPNOTSUPP;
			return NULL;
		}
		break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		if (opt != IPV6_XFRM_POLICY && opt != IPV6_IPSEC_POLICY) {
			*dir = -EOPNOTSUPP;
			return NULL;
		}
		break;
#endif
	default:
		*dir = -EINVAL;
		return NULL;
	}

	*dir = -EINVAL;

	if (len < sizeof(*p) ||
	    verify_newpolicy_info(p))
		return NULL;

	nr = ((len - sizeof(*p)) / sizeof(*ut));
	if (nr > XFRM_MAX_DEPTH)
		return NULL;

	if (p->dir > XFRM_POLICY_OUT)
		return NULL;

	xp = xfrm_policy_alloc(GFP_KERNEL);
	if (xp == NULL) {
		*dir = -ENOBUFS;
		return NULL;
	}

	copy_from_user_policy(xp, p);
	copy_templates(xp, ut, nr);

	*dir = p->dir;

	return xp;
}

static int build_polexpire(struct sk_buff *skb, struct xfrm_policy *xp,
			   int dir, int hard)
{
	struct xfrm_user_polexpire *upe;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;

	nlh = NLMSG_PUT(skb, 0, 0, XFRM_MSG_POLEXPIRE, sizeof(*upe));
	upe = NLMSG_DATA(nlh);
	nlh->nlmsg_flags = 0;

	copy_to_user_policy(xp, &upe->pol, dir);
	if (copy_to_user_tmpl(xp, skb) < 0)
		goto nlmsg_failure;
	upe->hard = !!hard;

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int xfrm_send_policy_notify(struct xfrm_policy *xp, int dir, int hard)
{
	struct sk_buff *skb;
	size_t len;

	len = RTA_SPACE(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr);
	len += NLMSG_SPACE(sizeof(struct xfrm_user_polexpire));
	skb = alloc_skb(len, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_polexpire(skb, xp, dir, hard) < 0)
		BUG();

	NETLINK_CB(skb).dst_groups = XFRMGRP_EXPIRE;

	return netlink_broadcast(xfrm_nl, skb, 0, XFRMGRP_EXPIRE, GFP_ATOMIC);
}

#ifdef CONFIG_IPV6_MIP6
static int build_mip6notify(struct sk_buff *skb, int status, int rcv_ifindex,
			    xfrm_address_t *coaddr, xfrm_address_t *hoaddr,
			    xfrm_address_t *local_addr)
{
	struct xfrm_user_mip6notify *umn;
	struct nlmsghdr *nlh;
	unsigned char *b = skb->tail;

	nlh = NLMSG_PUT(skb, 0, 0, XFRM_MSG_MIP6NOTIFY, sizeof(*umn));
	umn = NLMSG_DATA(nlh);
	nlh->nlmsg_flags = 0;

	umn->type = MIP6_BINDING_ERROR; /* XXX: currently supported only BE. */
	umn->status = status;
	umn->rcv_ifindex = rcv_ifindex;

 	memcpy(&umn->coaddr, coaddr, sizeof(umn->coaddr));
	memcpy(&umn->hoaddr, hoaddr, sizeof(umn->hoaddr));	
 	memcpy(&umn->cnaddr, local_addr, sizeof(umn->cnaddr));

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

int xfrm_send_mip6notify(int status, int iif, xfrm_address_t *coaddr,
			 xfrm_address_t *hoaddr, xfrm_address_t *local_addr)
{
	struct sk_buff *skb;
	size_t len;

	len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct xfrm_user_mip6notify)));
	skb = alloc_skb(len, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_mip6notify(skb, status, iif, coaddr, hoaddr, local_addr) < 0)
		BUG();

	XFRM_DBG("%s: ifindex = %d,\n", __FUNCTION__, iif);
	XFRM_DBG("      coa = %s,\n", XFRMSTRADDR(*coaddr, AF_INET6));
	XFRM_DBG("      hoa = %s,\n", XFRMSTRADDR(*hoaddr, AF_INET6));
	XFRM_DBG("    local = %s\n", XFRMSTRADDR(*local_addr, AF_INET6));
	NETLINK_CB(skb).dst_groups = XFRMGRP_NOTIFY;

	return netlink_broadcast(xfrm_nl, skb, 0, XFRMGRP_NOTIFY, GFP_ATOMIC);
}

EXPORT_SYMBOL(xfrm_send_mip6notify);
#endif

static struct xfrm_mgr netlink_mgr = {
	.id		= "netlink",
	.notify		= xfrm_send_state_notify,
	.acquire	= xfrm_send_acquire,
	.compile_policy	= xfrm_compile_policy,
	.notify_policy	= xfrm_send_policy_notify,
};

static int __init xfrm_user_init(void)
{
	printk(KERN_INFO "Initializing XFRM netlink socket\n");

	xfrm_nl = netlink_kernel_create(NETLINK_XFRM, xfrm_netlink_rcv);
	if (xfrm_nl == NULL)
		return -ENOMEM;

	xfrm_register_km(&netlink_mgr);

	return 0;
}

static void __exit xfrm_user_exit(void)
{
	xfrm_unregister_km(&netlink_mgr);
	sock_release(xfrm_nl->sk_socket);
}

module_init(xfrm_user_init);
module_exit(xfrm_user_exit);
MODULE_LICENSE("GPL");
