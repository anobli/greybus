/*
 * Greybus operations
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "greybus.h"

/*
 * The top bit of the type in an operation message header indicates
 * whether the message is a request (bit clear) or response (bit set)
 */
#define GB_OPERATION_TYPE_RESPONSE	0x80

/*
 * XXX This needs to be coordinated with host driver parameters
 */
#define GB_OPERATION_MESSAGE_SIZE_MAX	4096

/* Workqueue to handle Greybus operation completions. */
static struct workqueue_struct *gb_operation_recv_workqueue;

/*
 * All operation messages (both requests and responses) begin with
 * a common header that encodes the size of the data (header
 * included).  This header also contains a unique identifier, which
 * is used to keep track of in-flight operations.  Finally, the
 * header contains a operation type field, whose interpretation is
 * dependent on what type of device lies on the other end of the
 * connection.  Response messages are distinguished from request
 * messages by setting the high bit (0x80) in the operation type
 * value.
 *
 * The wire format for all numeric fields in the header is little
 * endian.  Any operation-specific data begins immediately after the
 * header, and is 64-bit aligned.
 */
struct gb_operation_msg_hdr {
	__le16	size;	/* Size in bytes of header + payload */
	__le16	id;	/* Operation unique id */
	__u8	type;	/* E.g GB_I2C_TYPE_* or GB_GPIO_TYPE_* */
	/* 3 bytes pad, must be zero (ignore when read) */
} __aligned(sizeof(u64));

/* XXX Could be per-host device, per-module, or even per-connection */
static DEFINE_SPINLOCK(gb_operations_lock);

static void gb_operation_insert(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;
	struct rb_root *root = &connection->pending;
	struct rb_node *node = &operation->node;
	struct rb_node **link = &root->rb_node;
	struct rb_node *above = NULL;
	struct gb_operation_msg_hdr *header;
	__le16 wire_id;

	/*
	 * Assign the operation's id, and store it in the header of
	 * both request and response message headers.
	 */
	operation->id = gb_connection_operation_id(connection);
	wire_id = cpu_to_le16(operation->id);
	header = operation->request->transfer_buffer;
	header->id = wire_id;

	/* OK, insert the operation into its connection's tree */
	spin_lock_irq(&gb_operations_lock);

	while (*link) {
		struct gb_operation *other;

		above = *link;
		other = rb_entry(above, struct gb_operation, node);
		header = other->request->transfer_buffer;
		if (other->id > operation->id)
			link = &above->rb_left;
		else if (other->id < operation->id)
			link = &above->rb_right;
	}
	rb_link_node(node, above, link);
	rb_insert_color(node, root);

	spin_unlock_irq(&gb_operations_lock);
}

static void gb_operation_remove(struct gb_operation *operation)
{
	spin_lock_irq(&gb_operations_lock);
	rb_erase(&operation->node, &operation->connection->pending);
	spin_unlock_irq(&gb_operations_lock);
}

static struct gb_operation *
gb_operation_find(struct gb_connection *connection, u16 id)
{
	struct gb_operation *operation = NULL;
	struct rb_node *node;
	bool found = false;

	spin_lock_irq(&gb_operations_lock);
	node = connection->pending.rb_node;
	while (node && !found) {
		operation = rb_entry(node, struct gb_operation, node);
		if (operation->id > id)
			node = node->rb_left;
		else if (operation->id < id)
			node = node->rb_right;
		else
			found = true;
	}
	spin_unlock_irq(&gb_operations_lock);

	return found ? operation : NULL;
}

/*
 * An operations's response message has arrived.  If no callback was
 * supplied it was submitted for asynchronous completion, so we notify
 * any waiters.  Otherwise we assume calling the completion is enough
 * and nobody else will be waiting.
 */
void gb_operation_complete(struct gb_operation *operation)
{
	if (operation->callback)
		operation->callback(operation);
	else
		complete_all(&operation->completion);
}

/* Wait for a submitted operation to complete */
int gb_operation_wait(struct gb_operation *operation)
{
	int ret;

	ret = wait_for_completion_interruptible(&operation->completion);
	/* If interrupted, cancel the in-flight buffer */
	if (ret < 0)
		ret = greybus_kill_gbuf(operation->request);
	return ret;

}

/*
 * This handler is used if no operation response messages are ever
 * expected for a given protocol.
 */
static void gb_operation_recv_none(struct gb_operation *operation)
{
	/* Nothing to do! */
}

typedef void (*gb_operation_recv_handler)(struct gb_operation *operation);
static gb_operation_recv_handler gb_operation_recv_handlers[] = {
	[GREYBUS_PROTOCOL_CONTROL]	= NULL,
	[GREYBUS_PROTOCOL_AP]		= NULL,
	[GREYBUS_PROTOCOL_GPIO]		= NULL,
	[GREYBUS_PROTOCOL_I2C]		= gb_operation_recv_none,
	[GREYBUS_PROTOCOL_UART]		= NULL,
	[GREYBUS_PROTOCOL_HID]		= NULL,
	[GREYBUS_PROTOCOL_BATTERY]	= gb_operation_recv_none,
	[GREYBUS_PROTOCOL_LED]		= NULL,
	[GREYBUS_PROTOCOL_VENDOR]	= NULL,
};

static void gb_operation_request_handle(struct gb_operation *operation)
{
	u8 protocol = operation->connection->protocol;

	/* Subtract one from array size to stay within u8 range */
	if (protocol <= (u8)(ARRAY_SIZE(gb_operation_recv_handlers) - 1)) {
		gb_operation_recv_handler handler;

		handler = gb_operation_recv_handlers[protocol];
		if (handler) {
			handler(operation);	/* Handle the request */
			return;
		}
	}

	gb_connection_err(operation->connection, "unrecognized protocol %u\n",
		(unsigned int)protocol);
	operation->result = GB_OP_PROTOCOL_BAD;
	gb_operation_complete(operation);
}

/*
 * Either this operation contains an incoming request, or its
 * response has arrived.  An incoming request will have a null
 * response buffer pointer (it is the responsibility of the request
 * handler to allocate and fill in the response buffer).
 */
static void gb_operation_recv_work(struct work_struct *recv_work)
{
	struct gb_operation *operation;
	bool incoming_request;

	operation = container_of(recv_work, struct gb_operation, recv_work);
	incoming_request = operation->response == NULL;
	if (incoming_request)
		gb_operation_request_handle(operation);
	gb_operation_complete(operation);

	/* We're finished with the buffer we read into */
	if (incoming_request)
		greybus_gbuf_finished(operation->request);
	else
		greybus_gbuf_finished(operation->response);
}

/*
 * Buffer completion function.  We get notified whenever any buffer
 * completes.  For outbound messages, this tells us that the message
 * has been sent.  For inbound messages, it means the data has
 * landed in the buffer and is ready to be processed.
 *
 * Either way, we don't do anything.  We don't really care when an
 * outbound message has been sent, and for incoming messages we
 * we'll be done with everything we need to do before we mark it
 * finished.
 *
 * XXX We may want to record that a request is (or is no longer) in flight.
 */
static void gb_operation_gbuf_complete(struct gbuf *gbuf)
{
	if (gbuf->status) {
		struct gb_operation *operation = gbuf->context;
		struct gb_operation_msg_hdr *header;
		int id;
		int type;

		if (gbuf == operation->request)
			header = operation->request_payload;
		else if (gbuf == operation->response)
			header = operation->response_payload;
		else
			header = NULL;
		id = header ? (int)header->id : -1;
		type = header ? (int)header->type : -1;

		gb_connection_err(operation->connection,
			"operation %d type %d gbuf error %d",
			id, type, gbuf->status);
	}
	return;
}

/*
 * Allocate a buffer to be used for an operation request or response
 * message.  For outgoing messages, both types of message contain a
 * common header, which is filled in here.  Incoming requests or
 * responses also contain the same header, but there's no need to
 * initialize it here (it'll be overwritten by the incoming
 * message).
 */
struct gbuf *gb_operation_gbuf_create(struct gb_operation *operation,
					u8 type, size_t size, bool data_out)
{
	struct gb_connection *connection = operation->connection;
	struct gb_operation_msg_hdr *header;
	struct gbuf *gbuf;
	gfp_t gfp_flags = data_out ? GFP_KERNEL : GFP_ATOMIC;

	size += sizeof(*header);
	gbuf = greybus_alloc_gbuf(connection, gb_operation_gbuf_complete,
					size, data_out, gfp_flags, operation);
	if (!gbuf)
		return NULL;

	/* Fill in the header structure */
	header = (struct gb_operation_msg_hdr *)gbuf->transfer_buffer;
	header->size = cpu_to_le16(size);
	header->id = 0;		/* Filled in when submitted */
	header->type = type;

	return gbuf;
}

/*
 * Create a Greybus operation to be sent over the given connection.
 * The request buffer will big enough for a payload of the given
 * size.  Outgoing requests must specify the size of the response
 * buffer size, which must be sufficient to hold all expected
 * response data.
 *
 * Incoming requests will supply a response size of 0, and in that
 * case no response buffer is allocated.  (A response always
 * includes a status byte, so 0 is not a valid size.)  Whatever
 * handles the operation request is responsible for allocating the
 * response buffer.
 *
 * Returns a pointer to the new operation or a null pointer if an
 * error occurs.
 */
struct gb_operation *gb_operation_create(struct gb_connection *connection,
					u8 type, size_t request_size,
					size_t response_size)
{
	struct gb_operation *operation;
	gfp_t gfp_flags = response_size ? GFP_KERNEL : GFP_ATOMIC;
	bool outgoing = response_size != 0;

	/* XXX Use a slab cache */
	operation = kzalloc(sizeof(*operation), gfp_flags);
	if (!operation)
		return NULL;
	operation->connection = connection;		/* XXX refcount? */

	operation->request = gb_operation_gbuf_create(operation, type,
							request_size,
							outgoing);
	if (!operation->request) {
		kfree(operation);
		return NULL;
	}
	operation->request_payload = operation->request->transfer_buffer +
					sizeof(struct gb_operation_msg_hdr);
	/* We always use the full request buffer */
	operation->request->actual_length = request_size;

	if (outgoing) {
		type |= GB_OPERATION_TYPE_RESPONSE;
		operation->response = gb_operation_gbuf_create(operation,
						type, response_size,
						false);
		if (!operation->response) {
			greybus_free_gbuf(operation->request);
			kfree(operation);
			return NULL;
		}
		operation->response_payload =
				operation->response->transfer_buffer +
				sizeof(struct gb_operation_msg_hdr);
	}

	INIT_WORK(&operation->recv_work, gb_operation_recv_work);
	operation->callback = NULL;	/* set at submit time */
	init_completion(&operation->completion);

	spin_lock_irq(&gb_operations_lock);
	list_add_tail(&operation->links, &connection->operations);
	spin_unlock_irq(&gb_operations_lock);

	return operation;
}

/*
 * Destroy a previously created operation.
 */
void gb_operation_destroy(struct gb_operation *operation)
{
	if (WARN_ON(!operation))
		return;

	/* XXX Make sure it's not in flight */
	spin_lock_irq(&gb_operations_lock);
	list_del(&operation->links);
	spin_unlock_irq(&gb_operations_lock);

	greybus_free_gbuf(operation->response);
	greybus_free_gbuf(operation->request);

	kfree(operation);
}

/*
 * Send an operation request message.  The caller has filled in
 * any payload so the request message is ready to go.  If non-null,
 * the callback function supplied will be called when the response
 * message has arrived indicating the operation is complete.  A null
 * callback function is used for a synchronous request; return from
 * this function won't occur until the operation is complete (or an
 * interrupt occurs).
 */
int gb_operation_request_send(struct gb_operation *operation,
				gb_operation_callback callback)
{
	int ret;

	/*
	 * XXX
	 * I think the order of operations is going to be
	 * significant, and if so, we may need a mutex to surround
	 * setting the operation id and submitting the gbuf.
	 */
	operation->callback = callback;
	gb_operation_insert(operation);
	ret = greybus_submit_gbuf(operation->request, GFP_KERNEL);
	if (ret)
		return ret;
	if (!callback)
		ret = gb_operation_wait(operation);

	return ret;
}

/*
 * Send a response for an incoming operation request.
 */
int gb_operation_response_send(struct gb_operation *operation)
{
	/* XXX
	 * Caller needs to have set operation->response->actual_length
	 */
	gb_operation_remove(operation);
	gb_operation_destroy(operation);

	return 0;
}

/*
 * Handle data arriving on a connection.  This is called in
 * interrupt context, so just copy the incoming data into a buffer
 * and do remaining handling via a work queue.
 */
void gb_connection_operation_recv(struct gb_connection *connection,
				void *data, size_t size)
{
	struct gb_operation_msg_hdr *header;
	struct gb_operation *operation;
	struct gbuf *gbuf;
	u16 msg_size;

	if (size > GB_OPERATION_MESSAGE_SIZE_MAX) {
		gb_connection_err(connection, "message too big");
		return;
	}

	header = data;
	msg_size = le16_to_cpu(header->size);
	if (header->type & GB_OPERATION_TYPE_RESPONSE) {
		u16 id = le16_to_cpu(header->id);

		operation = gb_operation_find(connection, id);
		if (!operation) {
			gb_connection_err(connection, "operation not found");
			return;
		}
		gb_operation_remove(operation);
		gbuf = operation->response;
		gbuf->status = GB_OP_SUCCESS;	/* If we got here we're good */
		if (size > gbuf->transfer_buffer_length) {
			gb_connection_err(connection, "recv buffer too small");
			return;
		}
	} else {
		WARN_ON(msg_size != size);
		operation = gb_operation_create(connection, header->type,
							msg_size, 0);
		if (!operation) {
			gb_connection_err(connection, "can't create operation");
			return;
		}
		gbuf = operation->request;
	}

	memcpy(gbuf->transfer_buffer, data, msg_size);
	gbuf->actual_length = msg_size;

	/* The rest will be handled in work queue context */
	queue_work(gb_operation_recv_workqueue, &operation->recv_work);
}

int gb_operation_init(void)
{
	gb_operation_recv_workqueue = alloc_workqueue("greybus_recv", 0, 1);
	if (!gb_operation_recv_workqueue)
		return -ENOMEM;

	return 0;
}

void gb_operation_exit(void)
{
	destroy_workqueue(gb_operation_recv_workqueue);
}