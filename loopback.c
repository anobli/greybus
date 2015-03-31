/*
 * Loopback bridge driver for the Greybus loopback module.
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/random.h>
#include "greybus.h"

struct gb_loopback_stats {
	u32 min;
	u32 max;
	u32 avg;
	u32 sum;
	u32 count;
};

struct gb_loopback {
	struct gb_connection *connection;
	u8 version_major;
	u8 version_minor;

	struct task_struct *task;

	int type;
	u32 size;
	int ms_wait;

	struct gb_loopback_stats latency;
	struct gb_loopback_stats throughput;
	struct gb_loopback_stats frequency;
	struct timeval ts;
	struct timeval te;
	u64 elapsed_nsecs;
	u32 error;
};

/* Version of the Greybus loopback protocol we support */
#define	GB_LOOPBACK_VERSION_MAJOR			0x00
#define	GB_LOOPBACK_VERSION_MINOR			0x01

/* Greybus loopback request types */
#define	GB_LOOPBACK_TYPE_INVALID			0x00
#define	GB_LOOPBACK_TYPE_PROTOCOL_VERSION		0x01
#define	GB_LOOPBACK_TYPE_PING				0x02
#define	GB_LOOPBACK_TYPE_TRANSFER			0x03

#define GB_LOOPBACK_SIZE_MAX				SZ_4K

/* Define get_version() routine */
define_get_version(gb_loopback, LOOPBACK);

/* interface sysfs attributes */
#define gb_loopback_ro_attr(field, type)				\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct gb_connection *connection = to_gb_connection(dev);	\
	struct gb_loopback *gb =					\
		(struct gb_loopback *)connection->private;		\
	return sprintf(buf, "%"#type"\n", gb->field);			\
}									\
static DEVICE_ATTR_RO(field)

#define gb_loopback_ro_stats_attr(name, field, type)			\
static ssize_t name##_##field##_show(struct device *dev,		\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct gb_connection *connection = to_gb_connection(dev);	\
	struct gb_loopback *gb =					\
		(struct gb_loopback *)connection->private;		\
	return sprintf(buf, "%"#type"\n", gb->name.field);		\
}									\
static DEVICE_ATTR_RO(name##_##field)

#define gb_loopback_stats_attrs(field)					\
	gb_loopback_ro_stats_attr(field, min, d);			\
	gb_loopback_ro_stats_attr(field, max, d);			\
	gb_loopback_ro_stats_attr(field, avg, d);

#define gb_loopback_attr(field, type)					\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct gb_connection *connection = to_gb_connection(dev);	\
	struct gb_loopback *gb =					\
		(struct gb_loopback *)connection->private;		\
	return sprintf(buf, "%"#type"\n", gb->field);			\
}									\
static ssize_t field##_store(struct device *dev,			\
			    struct device_attribute *attr,		\
			    const char *buf,				\
			    size_t len)					\
{									\
	int ret;							\
	struct gb_connection *connection = to_gb_connection(dev);	\
	struct gb_loopback *gb =					\
		(struct gb_loopback *)connection->private;		\
	ret = sscanf(buf, "%"#type, &gb->field);			\
	pr_err("%s = %"#type"\n", #field, gb->field);			\
	if (ret != 1)							\
		return -EINVAL;						\
	gb_loopback_check_attr(gb);					\
	return len;							\
}									\
static DEVICE_ATTR_RW(field)

static void gb_loopback_reset_stats(struct gb_loopback *gb);
static void gb_loopback_check_attr(struct gb_loopback *gb)
{
	if (gb->ms_wait > 1000)
		gb->ms_wait = 1000;
	if (gb->type > 3)
		gb->type = 0;
	if (gb->size > GB_LOOPBACK_SIZE_MAX)
		gb->size = GB_LOOPBACK_SIZE_MAX;
	gb->error = 0;
	gb_loopback_reset_stats(gb);
}

/* Time to send and receive one message */
gb_loopback_stats_attrs(latency);
/* Number of packet sent per second on this cport */
gb_loopback_stats_attrs(frequency);
/* Quantity of data sent and received on this cport */
gb_loopback_stats_attrs(throughput);
gb_loopback_ro_attr(error, d);

/*
 * Type of loopback message to send
 * 0 => Don't send message
 * 1 => Send ping message continuously (message without payload)
 * 2 => Send transer message continuously (message with payload)
 */
gb_loopback_attr(type, d);
/* Size of transfer message payload: 0-4096 bytes */
gb_loopback_attr(size, u);
/* Time to wait between two messages: 0-1024 ms */
gb_loopback_attr(ms_wait, d);

#define dev_stats_attrs(name)						\
	&dev_attr_##name##_min.attr,					\
	&dev_attr_##name##_max.attr,					\
	&dev_attr_##name##_avg.attr

static struct attribute *loopback_attrs[] = {
	dev_stats_attrs(latency),
	dev_stats_attrs(frequency),
	dev_stats_attrs(throughput),
	&dev_attr_type.attr,
	&dev_attr_size.attr,
	&dev_attr_ms_wait.attr,
	&dev_attr_error.attr,
	NULL,
};
ATTRIBUTE_GROUPS(loopback);

struct gb_loopback_transfer_request {
	__le32	len;
	__u8	data[0];
};

struct gb_loopback_transfer_response {
	__u8	data[0];
};


static int gb_loopback_transfer(struct gb_loopback *gb,
				struct timeval *tping, u32 len)
{
	struct timeval ts, te;
	u64 elapsed_nsecs;
	struct gb_loopback_transfer_request *request;
	struct gb_loopback_transfer_response *response;
	int retval;

	request = kmalloc(len + sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;
	response = kmalloc(len + sizeof(*response), GFP_KERNEL);
	if (!response) {
		kfree(request);
		return -ENOMEM;
	}

	request->len = cpu_to_le32(len);

	do_gettimeofday(&ts);
	retval = gb_operation_sync(gb->connection, GB_LOOPBACK_TYPE_TRANSFER,
				   request, len + sizeof(*request),
				   response, len + sizeof(*response));
	do_gettimeofday(&te);
	elapsed_nsecs = timeval_to_ns(&te) - timeval_to_ns(&ts);
	*tping = ns_to_timeval(elapsed_nsecs);

	if (retval)
		goto gb_error;

	if (memcmp(request->data, response->data, len))
		retval = -EREMOTEIO;

gb_error:
	kfree(request);
	kfree(response);

	return retval;
}

static int gb_loopback_ping(struct gb_loopback *gb, struct timeval *tping)
{
	struct timeval ts, te;
	u64 elapsed_nsecs;
	int retval;

	do_gettimeofday(&ts);
	retval = gb_operation_sync(gb->connection, GB_LOOPBACK_TYPE_PING,
				   NULL, 0, NULL, 0);
	do_gettimeofday(&te);
	elapsed_nsecs = timeval_to_ns(&te) - timeval_to_ns(&ts);
	*tping = ns_to_timeval(elapsed_nsecs);

	return retval;
}

static void gb_loopback_reset_stats(struct gb_loopback *gb)
{
	struct gb_loopback_stats reset = {
		.min = 0xffffffff,
	};
	memcpy(&gb->latency, &reset, sizeof(struct gb_loopback_stats));
	memcpy(&gb->throughput, &reset, sizeof(struct gb_loopback_stats));
	memcpy(&gb->frequency, &reset, sizeof(struct gb_loopback_stats));
	memset(&gb->ts, 0, sizeof(struct timeval));
}

static void gb_loopback_update_stats(struct gb_loopback_stats *stats,
					u64 elapsed_nsecs)
{
	u32 avg;

	if (elapsed_nsecs >= NSEC_PER_SEC) {
		if (!stats->count)
			avg = stats->sum * (elapsed_nsecs / NSEC_PER_SEC);
		else
			avg = stats->sum / stats->count;
		if (stats->min > avg)
			stats->min = avg;
		if (stats->max < avg)
			stats->max = avg;
		stats->avg = avg;
		stats->count = 0;
		stats->sum = 0;
	}
}

static void gb_loopback_freq_update(struct gb_loopback *gb)
{
	gb->frequency.sum++;
	gb_loopback_update_stats(&gb->frequency, gb->elapsed_nsecs);
}

static void gb_loopback_bw_update(struct gb_loopback *gb, int error)
{
	if (!error)
		gb->throughput.sum += gb->size * 2;
	gb_loopback_update_stats(&gb->throughput, gb->elapsed_nsecs);
}

static void gb_loopback_latency_update(struct gb_loopback *gb,
					struct timeval *tlat)
{
	u32 lat;
	u64 nsecs;

	nsecs = timeval_to_ns(tlat);
	lat = nsecs / NSEC_PER_MSEC;

	if (gb->latency.min > lat)
		gb->latency.min = lat;
	if (gb->latency.max < lat)
		gb->latency.max = lat;
	gb->latency.sum += lat;
	gb->latency.count++;
	gb_loopback_update_stats(&gb->latency, gb->elapsed_nsecs);
}

static int gb_loopback_fn(void *data)
{
	int error = 0;
	struct timeval tlat = {0, 0};
	struct gb_loopback *gb = (struct gb_loopback *)data;

	while (!kthread_should_stop()) {
		if (gb->type == 0) {
			msleep(1000);
			continue;
		}
		if (gb->type == 1)
			error = gb_loopback_ping(gb, &tlat);
		if (gb->type == 2)
			error = gb_loopback_transfer(gb, &tlat, gb->size);
		if (error)
			gb->error++;
		if (gb->ts.tv_usec == 0 && gb->ts.tv_sec == 0) {
			do_gettimeofday(&gb->ts);
			continue;
		}
		do_gettimeofday(&gb->te);
		gb->elapsed_nsecs = timeval_to_ns(&gb->te) -
					timeval_to_ns(&gb->ts);
		gb_loopback_freq_update(gb);
		if (gb->type == 2)
			gb_loopback_bw_update(gb, error);
		gb_loopback_latency_update(gb, &tlat);
		if (gb->elapsed_nsecs >= NSEC_PER_SEC)
			gb->ts = gb->te;
		if (gb->ms_wait)
			msleep(gb->ms_wait);

	}
	return 0;
}

static int gb_loopback_connection_init(struct gb_connection *connection)
{
	struct gb_loopback *gb;
	int retval;

	gb = kzalloc(sizeof(*gb), GFP_KERNEL);
	if (!gb)
		return -ENOMEM;

	gb->connection = connection;
	connection->private = gb;
	retval = sysfs_update_group(&connection->dev.kobj, &loopback_group);
	if (retval)
		goto error;

	/* Check the version */
	retval = get_version(gb);
	if (retval)
		goto error;

	gb_loopback_reset_stats(gb);
	gb->task = kthread_run(gb_loopback_fn, gb, "gb_loopback");
	if (IS_ERR(gb->task)) {
		retval = IS_ERR(gb->task);
		goto error;
	}

	return 0;

error:
	kfree(gb);
	return retval;
}

static void gb_loopback_connection_exit(struct gb_connection *connection)
{
	struct gb_loopback *gb = connection->private;

	if (!IS_ERR_OR_NULL(gb->task))
		kthread_stop(gb->task);
	sysfs_remove_group(&connection->dev.kobj, &loopback_group);
	kfree(gb);
}

static struct gb_protocol loopback_protocol = {
	.name			= "loopback",
	.id			= GREYBUS_PROTOCOL_LOOPBACK,
	.major			= GB_LOOPBACK_VERSION_MAJOR,
	.minor			= GB_LOOPBACK_VERSION_MINOR,
	.connection_init	= gb_loopback_connection_init,
	.connection_exit	= gb_loopback_connection_exit,
	.request_recv		= NULL,	/* no incoming requests */
};

gb_protocol_driver(&loopback_protocol);

MODULE_LICENSE("GPL v2");