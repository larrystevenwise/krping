// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vim: noexpandtab ts=8 sts=0 sw=8:
 *
 * configfs_example_macros.c - This file is a demonstration module
 *      containing a number of configfs subsystems.  It uses the helper
 *      macros defined by configfs.h
 *
 * Based on sysfs:
 * 	sysfs is Copyright (C) 2001, 2002, 2003 Patrick Mochel
 *
 * configfs Copyright (C) 2005 Oracle.  All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/configfs.h>
#include "krping.h"

struct krping_endpoint {
	struct config_item item;
	struct krping_cb cb;
	char params[128];
};

static inline struct krping_endpoint *to_krping_endpoint(
		struct config_item *item)
{
	return item ? container_of(item, struct krping_endpoint, item) : NULL;
}

static ssize_t krping_endpoint_params_show(struct config_item *item,
		char *page)
{
	return sprintf(page, "%s\n", to_krping_endpoint(item)->params);
}

static ssize_t krping_endpoint_params_store(struct config_item *item,
		const char *page, size_t count)
{
	struct krping_endpoint *krping_endpoint = to_krping_endpoint(item);
	char *p = (char *) page;
	int ret;

	if (krping_endpoint->params[0] != 0)
		return -EINVAL;
	if (count > sizeof(krping_endpoint->params))
		return -E2BIG;

	strncpy(krping_endpoint->params, p, count);
	if (krping_endpoint->params[count-1] == '\n')
		krping_endpoint->params[count-1] = 0;

	/* parse it */
	ret = krping_doit(&krping_endpoint->cb, krping_endpoint->params);
	if (ret)
		krping_endpoint->params[0] = 0;
	return ret < 0 ? ret : count;
}

static ssize_t krping_endpoint_stats_show(struct config_item *item,
		char *page)
{
	struct krping_endpoint *krping_endpoint = to_krping_endpoint(item);
	struct krping_cb *cb = &krping_endpoint->cb;
	int ret;

	if (cb->state < IDLE)
		return 0;

	if (cb->pd) {
		ret = sprintf(page,
			"dev %s send_bytes %lld send_msgs %lld recv_bytes %lld recv_msgs %lld write_bytes %lld write_msgs %lld read_bytes %lld read_msgs %lld\n",
			cb->pd->device->name, cb->stats.send_bytes,
			cb->stats.send_msgs, cb->stats.recv_bytes,
			cb->stats.recv_msgs, cb->stats.write_bytes,
			cb->stats.write_msgs,
			cb->stats.read_bytes,
			cb->stats.read_msgs);
	} else {
		ret = sprintf(page, "listening\n");
	}
	return ret;
}

CONFIGFS_ATTR(krping_endpoint_, params);
CONFIGFS_ATTR_RO(krping_endpoint_, stats);

static struct configfs_attribute *krping_endpoint_attrs[] = {
	&krping_endpoint_attr_params,
	&krping_endpoint_attr_stats,
	NULL,
};

static void krping_endpoint_release(struct config_item *item)
{
	kfree(to_krping_endpoint(item));
}

static struct configfs_item_operations krping_endpoint_item_ops = {
	.release		= krping_endpoint_release,
};

static const struct config_item_type krping_endpoint_type = {
	.ct_item_ops	= &krping_endpoint_item_ops,
	.ct_attrs	= krping_endpoint_attrs,
	.ct_owner	= THIS_MODULE,
};


struct krping_endpoints {
	struct config_group group;
};

static inline struct krping_endpoints *to_krping_endpoints(
		struct config_item *item)
{
	return item ? container_of(to_config_group(item),
			struct krping_endpoints, group) : NULL;
}

static struct config_item *krping_endpoints_make_item(
		struct config_group *group, const char *name)
{
	struct krping_endpoint *krping_endpoint;

	krping_endpoint = kzalloc(sizeof(struct krping_endpoint), GFP_KERNEL);
	if (!krping_endpoint)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&krping_endpoint->item, name,
				   &krping_endpoint_type);

	return &krping_endpoint->item;
}

static void krping_endpoints_drop_item(struct config_group *group,
		struct config_item *item)
{
	struct krping_endpoint *krping_endpoint = to_krping_endpoint(item);
	struct krping_cb *cb = &krping_endpoint->cb;

	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
	cancel_work_sync(&cb->krping_work);

	return;
}

static ssize_t krping_endpoints_readme_show(struct config_item *item,
		char *page)
{
	return sprintf(page,
"[krping-endpoints]\n"
"\n"
"Create directories here to allocate krping endpoints, then write to the\n"
"<newdir>/params file to setup the krping endpoint and start the job.\n"
"\n"
"The syntax for krping commands is a string of options separated by commas.\n"
"Options can be single keywords, or in the form: option=operand.\n"
"\n"
"Operands can be integers or strings.\n"
"\n"
"Note you must specify the _same_ options on both sides.  For instance,\n"
"if you want to use the server_invalidate option, then you must specify\n"
"it on both the server and client command lines.\n"
"\n"
"Opcode          Operand Type    Description\n"
"------------------------------------------------------------------------\n"
"client          none            Initiate a client side krping thread.\n"
"server          none            Initiate a server side krping thread.\n"
"addr            string          The server's IP address in dotted\n"
"                                decimal format.  Note the server can\n"
"                                use 0.0.0.0 to bind to all devices.\n"
"addr6           string          The server's IPv6 address.\n"
"port            integer         The server's port number in host byte\n"
"                                order.\n"
"count           integer         The number of rping iterations to\n"
"                                perform before shutting down the test.\n"
"                                If unspecified, the count is infinite.\n"
"size            integer         The size of the rping data.  Default for\n"
"                                rping is 65 bytes.\n"
"verbose         none            Enables printk()s that dump the rping\n"
"                                data. Use with caution!\n"
"validate        none            Enables validating the rping data on\n"
"                                each iteration to detect data\n"
"                                corruption.\n"
"server_inv      none            Valid only in reg mr mode, this\n"
"                                option enables invalidating the\n"
"                                client's reg mr via\n"
"                                SEND_WITH_INVALIDATE messages from\n"
"                                the server.\n"
"local_dma_lkey  none            Use the local dma lkey for the source\n"
"                                of writes and sends, and in recvs\n"
"tos             integer	 Type of Service to the connection\n"
"read_inv        none            Server will use READ_WITH_INV. Only\n"
"                                valid in reg mem_mode.\n"
"---- experimental tests ----\n"
"rlat            none            Run read latency test\n"
"wlat            none            Run write latency test\n"
"bw              none            Run write bw test\n"
"duplex          none            bidir write bw test (requires bw)\n"
"fr              none            Run fastreg mr test\n"
);
}

CONFIGFS_ATTR_RO(krping_endpoints_, readme);

static struct configfs_attribute *krping_endpoints_attrs[] = {
	&krping_endpoints_attr_readme,
	NULL,
};

static void krping_endpoints_release(struct config_item *item)
{
	kfree(to_krping_endpoints(item));
}

static struct configfs_item_operations krping_endpoints_item_ops = {
	.release	= krping_endpoints_release,
};

static struct configfs_group_operations krping_endpoints_group_ops = {
	.make_item	= krping_endpoints_make_item,
	.drop_item	= krping_endpoints_drop_item,
};

static const struct config_item_type krping_endpoints_type = {
	.ct_item_ops	= &krping_endpoints_item_ops,
	.ct_group_ops	= &krping_endpoints_group_ops,
	.ct_attrs	= krping_endpoints_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem krping_endpoints_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "krping",
			.ci_type = &krping_endpoints_type,
		},
	},
};

int __init krping_configfs_init(void)
{
	config_group_init(&krping_endpoints_subsys.su_group);
	mutex_init(&krping_endpoints_subsys.su_mutex);
	return configfs_register_subsystem(&krping_endpoints_subsys);
}

void __exit krping_configfs_exit(void)
{
	configfs_unregister_subsystem(&krping_endpoints_subsys);
}

