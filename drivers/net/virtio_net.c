/* A simple network driver using virtio.
 *
 * Copyright 2007 Rusty Russell <rusty@rustcorp.com.au> IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
//#define DEBUG
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/scatterlist.h>

/* FIXME: MTU in config. */
#define MAX_PACKET_LEN (ETH_HLEN+ETH_DATA_LEN)

struct virtnet_info
{
	struct virtio_device *vdev;
	struct virtqueue *rvq, *svq;
	struct net_device *dev;
	struct napi_struct napi;

	/* Number of input buffers, and max we've ever had. */
	unsigned int num, max;

	/* Receive & send queues. */
	struct sk_buff_head recv;
	struct sk_buff_head send;
};

static inline struct virtio_net_hdr *skb_vnet_hdr(struct sk_buff *skb)
{
	return (struct virtio_net_hdr *)skb->cb;
}

static inline void vnet_hdr_to_sg(struct scatterlist *sg, struct sk_buff *skb)
{
	sg_init_one(sg, skb_vnet_hdr(skb), sizeof(struct virtio_net_hdr));
}

static bool skb_xmit_done(struct virtqueue *rvq)
{
	struct virtnet_info *vi = rvq->vdev->priv;

	/* In case we were waiting for output buffers. */
	netif_wake_queue(vi->dev);
	return true;
}

static void receive_skb(struct net_device *dev, struct sk_buff *skb,
			unsigned len)
{
	struct virtio_net_hdr *hdr = skb_vnet_hdr(skb);

	if (unlikely(len < sizeof(struct virtio_net_hdr) + ETH_HLEN)) {
		pr_debug("%s: short packet %i\n", dev->name, len);
		dev->stats.rx_length_errors++;
		goto drop;
	}
	len -= sizeof(struct virtio_net_hdr);
	BUG_ON(len > MAX_PACKET_LEN);

	skb_trim(skb, len);
	skb->protocol = eth_type_trans(skb, dev);
	pr_debug("Receiving skb proto 0x%04x len %i type %i\n",
		 ntohs(skb->protocol), skb->len, skb->pkt_type);
	dev->stats.rx_bytes += skb->len;
	dev->stats.rx_packets++;

	if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
		pr_debug("Needs csum!\n");
		skb->ip_summed = CHECKSUM_PARTIAL;
		skb->csum_start = hdr->csum_start;
		skb->csum_offset = hdr->csum_offset;
		if (skb->csum_start > skb->len - 2
		    || skb->csum_offset > skb->len - 2) {
			if (net_ratelimit())
				printk(KERN_WARNING "%s: csum=%u/%u len=%u\n",
				       dev->name, skb->csum_start,
				       skb->csum_offset, skb->len);
			goto frame_err;
		}
	}

	if (hdr->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		pr_debug("GSO!\n");
		switch (hdr->gso_type) {
		case VIRTIO_NET_HDR_GSO_TCPV4:
			skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;
			break;
		case VIRTIO_NET_HDR_GSO_TCPV4_ECN:
			skb_shinfo(skb)->gso_type = SKB_GSO_TCP_ECN;
			break;
		case VIRTIO_NET_HDR_GSO_UDP:
			skb_shinfo(skb)->gso_type = SKB_GSO_UDP;
			break;
		case VIRTIO_NET_HDR_GSO_TCPV6:
			skb_shinfo(skb)->gso_type = SKB_GSO_TCPV6;
			break;
		default:
			if (net_ratelimit())
				printk(KERN_WARNING "%s: bad gso type %u.\n",
				       dev->name, hdr->gso_type);
			goto frame_err;
		}

		skb_shinfo(skb)->gso_size = hdr->gso_size;
		if (skb_shinfo(skb)->gso_size == 0) {
			if (net_ratelimit())
				printk(KERN_WARNING "%s: zero gso size.\n",
				       dev->name);
			goto frame_err;
		}

		/* Header must be checked, and gso_segs computed. */
		skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
		skb_shinfo(skb)->gso_segs = 0;
	}

	netif_receive_skb(skb);
	return;

frame_err:
	dev->stats.rx_frame_errors++;
drop:
	dev_kfree_skb(skb);
}

static void try_fill_recv(struct virtnet_info *vi)
{
	struct sk_buff *skb;
	struct scatterlist sg[1+MAX_SKB_FRAGS];
	int num, err;

	for (;;) {
		skb = netdev_alloc_skb(vi->dev, MAX_PACKET_LEN);
		if (unlikely(!skb))
			break;

		skb_put(skb, MAX_PACKET_LEN);
		vnet_hdr_to_sg(sg, skb);
		num = skb_to_sgvec(skb, sg+1, 0, skb->len) + 1;
		skb_queue_head(&vi->recv, skb);

		err = vi->rvq->vq_ops->add_buf(vi->rvq, sg, 0, num, skb);
		if (err) {
			skb_unlink(skb, &vi->recv);
			kfree_skb(skb);
			break;
		}
		vi->num++;
	}
	if (unlikely(vi->num > vi->max))
		vi->max = vi->num;
	vi->rvq->vq_ops->kick(vi->rvq);
}

static bool skb_recv_done(struct virtqueue *rvq)
{
	struct virtnet_info *vi = rvq->vdev->priv;
	netif_rx_schedule(vi->dev, &vi->napi);
	/* Suppress further interrupts. */
	return false;
}

static int virtnet_poll(struct napi_struct *napi, int budget)
{
	struct virtnet_info *vi = container_of(napi, struct virtnet_info, napi);
	struct sk_buff *skb = NULL;
	unsigned int len, received = 0;

again:
	while (received < budget &&
	       (skb = vi->rvq->vq_ops->get_buf(vi->rvq, &len)) != NULL) {
		__skb_unlink(skb, &vi->recv);
		receive_skb(vi->dev, skb, len);
		vi->num--;
		received++;
	}

	/* FIXME: If we oom and completely run out of inbufs, we need
	 * to start a timer trying to fill more. */
	if (vi->num < vi->max / 2)
		try_fill_recv(vi);

	/* All done? */
	if (!skb) {
		netif_rx_complete(vi->dev, napi);
		if (unlikely(!vi->rvq->vq_ops->restart(vi->rvq))
		    && netif_rx_reschedule(vi->dev, napi))
			goto again;
	}

	return received;
}

static void free_old_xmit_skbs(struct virtnet_info *vi)
{
	struct sk_buff *skb;
	unsigned int len;

	while ((skb = vi->svq->vq_ops->get_buf(vi->svq, &len)) != NULL) {
		pr_debug("Sent skb %p\n", skb);
		__skb_unlink(skb, &vi->send);
		vi->dev->stats.tx_bytes += len;
		vi->dev->stats.tx_packets++;
		kfree_skb(skb);
	}
}

static int start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int num, err;
	struct scatterlist sg[1+MAX_SKB_FRAGS];
	struct virtio_net_hdr *hdr;
	const unsigned char *dest = ((struct ethhdr *)skb->data)->h_dest;
	DECLARE_MAC_BUF(mac);

	pr_debug("%s: xmit %p %s\n", dev->name, skb, print_mac(mac, dest));

	free_old_xmit_skbs(vi);

	/* Encode metadata header at front. */
	hdr = skb_vnet_hdr(skb);
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
		hdr->csum_start = skb->csum_start - skb_headroom(skb);
		hdr->csum_offset = skb->csum_offset;
	} else {
		hdr->flags = 0;
		hdr->csum_offset = hdr->csum_start = 0;
	}

	if (skb_is_gso(skb)) {
		hdr->gso_size = skb_shinfo(skb)->gso_size;
		if (skb_shinfo(skb)->gso_type & SKB_GSO_TCP_ECN)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4_ECN;
		else if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		else if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
		else if (skb_shinfo(skb)->gso_type & SKB_GSO_UDP)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_UDP;
		else
			BUG();
	} else {
		hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;
		hdr->gso_size = 0;
	}

	vnet_hdr_to_sg(sg, skb);
	num = skb_to_sgvec(skb, sg+1, 0, skb->len) + 1;
	__skb_queue_head(&vi->send, skb);
	err = vi->svq->vq_ops->add_buf(vi->svq, sg, num, 0, skb);
	if (err) {
		pr_debug("%s: virtio not prepared to send\n", dev->name);
		skb_unlink(skb, &vi->send);
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}
	vi->svq->vq_ops->kick(vi->svq);

	return 0;
}

static int virtnet_open(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);

	try_fill_recv(vi);

	/* If we didn't even get one input buffer, we're useless. */
	if (vi->num == 0)
		return -ENOMEM;

	napi_enable(&vi->napi);
	return 0;
}

static int virtnet_close(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct sk_buff *skb;

	napi_disable(&vi->napi);

	/* networking core has neutered skb_xmit_done/skb_recv_done, so don't
	 * worry about races vs. get(). */
	vi->rvq->vq_ops->shutdown(vi->rvq);
	while ((skb = __skb_dequeue(&vi->recv)) != NULL) {
		kfree_skb(skb);
		vi->num--;
	}
	vi->svq->vq_ops->shutdown(vi->svq);
	while ((skb = __skb_dequeue(&vi->send)) != NULL)
		kfree_skb(skb);

	BUG_ON(vi->num != 0);
	return 0;
}

static int virtnet_probe(struct virtio_device *vdev)
{
	int err;
	unsigned int len;
	struct net_device *dev;
	struct virtnet_info *vi;
	void *token;

	/* Allocate ourselves a network device with room for our info */
	dev = alloc_etherdev(sizeof(struct virtnet_info));
	if (!dev)
		return -ENOMEM;

	/* Set up network device as normal. */
	ether_setup(dev);
	dev->open = virtnet_open;
	dev->stop = virtnet_close;
	dev->hard_start_xmit = start_xmit;
	dev->features = NETIF_F_HIGHDMA;
	SET_NETDEV_DEV(dev, &vdev->dev);

	/* Do we support "hardware" checksums? */
	token = vdev->config->find(vdev, VIRTIO_CONFIG_NET_F, &len);
	if (virtio_use_bit(vdev, token, len, VIRTIO_NET_F_NO_CSUM)) {
		/* This opens up the world of extra features. */
		dev->features |= NETIF_F_HW_CSUM|NETIF_F_SG|NETIF_F_FRAGLIST;
		if (virtio_use_bit(vdev, token, len, VIRTIO_NET_F_TSO4))
			dev->features |= NETIF_F_TSO;
		if (virtio_use_bit(vdev, token, len, VIRTIO_NET_F_UFO))
			dev->features |= NETIF_F_UFO;
		if (virtio_use_bit(vdev, token, len, VIRTIO_NET_F_TSO4_ECN))
			dev->features |= NETIF_F_TSO_ECN;
		if (virtio_use_bit(vdev, token, len, VIRTIO_NET_F_TSO6))
			dev->features |= NETIF_F_TSO6;
	}

	/* Configuration may specify what MAC to use.  Otherwise random. */
	token = vdev->config->find(vdev, VIRTIO_CONFIG_NET_MAC_F, &len);
	if (token) {
		dev->addr_len = len;
		vdev->config->get(vdev, token, dev->dev_addr, len);
	} else
		random_ether_addr(dev->dev_addr);

	/* Set up our device-specific information */
	vi = netdev_priv(dev);
	netif_napi_add(dev, &vi->napi, virtnet_poll, 16);
	vi->dev = dev;
	vi->vdev = vdev;

	/* We expect two virtqueues, receive then send. */
	vi->rvq = vdev->config->find_vq(vdev, skb_recv_done);
	if (IS_ERR(vi->rvq)) {
		err = PTR_ERR(vi->rvq);
		goto free;
	}

	vi->svq = vdev->config->find_vq(vdev, skb_xmit_done);
	if (IS_ERR(vi->svq)) {
		err = PTR_ERR(vi->svq);
		goto free_recv;
	}

	/* Initialize our empty receive and send queues. */
	skb_queue_head_init(&vi->recv);
	skb_queue_head_init(&vi->send);

	err = register_netdev(dev);
	if (err) {
		pr_debug("virtio_net: registering device failed\n");
		goto free_send;
	}
	pr_debug("virtnet: registered device %s\n", dev->name);
	vdev->priv = vi;
	return 0;

free_send:
	vdev->config->del_vq(vi->svq);
free_recv:
	vdev->config->del_vq(vi->rvq);
free:
	free_netdev(dev);
	return err;
}

static void virtnet_remove(struct virtio_device *vdev)
{
	unregister_netdev(vdev->priv);
	free_netdev(vdev->priv);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_net = {
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtnet_probe,
	.remove =	__devexit_p(virtnet_remove),
};

static int __init init(void)
{
	return register_virtio_driver(&virtio_net);
}

static void __exit fini(void)
{
	unregister_virtio_driver(&virtio_net);
}
module_init(init);
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio network driver");
MODULE_LICENSE("GPL");