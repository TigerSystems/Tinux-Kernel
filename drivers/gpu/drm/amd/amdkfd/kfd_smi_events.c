// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2020-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/anon_inodes.h>
#include <uapi/linux/kfd_ioctl.h>
#include "amdgpu.h"
#include "amdgpu_vm.h"
#include "kfd_priv.h"
#include "kfd_smi_events.h"

struct kfd_smi_client {
	struct list_head list;
	struct kfifo fifo;
	wait_queue_head_t wait_queue;
	/* events enabled */
	uint64_t events;
	struct kfd_dev *dev;
	spinlock_t lock;
};

#define MAX_KFIFO_SIZE	1024

static __poll_t kfd_smi_ev_poll(struct file *, struct poll_table_struct *);
static ssize_t kfd_smi_ev_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t kfd_smi_ev_write(struct file *, const char __user *, size_t,
				loff_t *);
static int kfd_smi_ev_release(struct inode *, struct file *);

static const char kfd_smi_name[] = "kfd_smi_ev";

static const struct file_operations kfd_smi_ev_fops = {
	.owner = THIS_MODULE,
	.poll = kfd_smi_ev_poll,
	.read = kfd_smi_ev_read,
	.write = kfd_smi_ev_write,
	.release = kfd_smi_ev_release
};

static __poll_t kfd_smi_ev_poll(struct file *filep,
				struct poll_table_struct *wait)
{
	struct kfd_smi_client *client = filep->private_data;
	__poll_t mask = 0;

	poll_wait(filep, &client->wait_queue, wait);

	spin_lock(&client->lock);
	if (!kfifo_is_empty(&client->fifo))
		mask = EPOLLIN | EPOLLRDNORM;
	spin_unlock(&client->lock);

	return mask;
}

static ssize_t kfd_smi_ev_read(struct file *filep, char __user *user,
			       size_t size, loff_t *offset)
{
	int ret;
	size_t to_copy;
	struct kfd_smi_client *client = filep->private_data;
	unsigned char *buf;

	size = min_t(size_t, size, MAX_KFIFO_SIZE);
	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* kfifo_to_user can sleep so we can't use spinlock protection around
	 * it. Instead, we kfifo out as spinlocked then copy them to the user.
	 */
	spin_lock(&client->lock);
	to_copy = kfifo_len(&client->fifo);
	if (!to_copy) {
		spin_unlock(&client->lock);
		ret = -EAGAIN;
		goto ret_err;
	}
	to_copy = min(size, to_copy);
	ret = kfifo_out(&client->fifo, buf, to_copy);
	spin_unlock(&client->lock);
	if (ret <= 0) {
		ret = -EAGAIN;
		goto ret_err;
	}

	ret = copy_to_user(user, buf, to_copy);
	if (ret) {
		ret = -EFAULT;
		goto ret_err;
	}

	kfree(buf);
	return to_copy;

ret_err:
	kfree(buf);
	return ret;
}

static ssize_t kfd_smi_ev_write(struct file *filep, const char __user *user,
				size_t size, loff_t *offset)
{
	struct kfd_smi_client *client = filep->private_data;
	uint64_t events;

	if (!access_ok(user, size) || size < sizeof(events))
		return -EFAULT;
	if (copy_from_user(&events, user, sizeof(events)))
		return -EFAULT;

	WRITE_ONCE(client->events, events);

	return sizeof(events);
}

static int kfd_smi_ev_release(struct inode *inode, struct file *filep)
{
	struct kfd_smi_client *client = filep->private_data;
	struct kfd_dev *dev = client->dev;

	spin_lock(&dev->smi_lock);
	list_del_rcu(&client->list);
	spin_unlock(&dev->smi_lock);

	synchronize_rcu();
	kfifo_free(&client->fifo);
	kfree(client);

	return 0;
}

static void add_event_to_kfifo(struct kfd_dev *dev, unsigned int smi_event,
			      char *event_msg, int len)
{
	struct kfd_smi_client *client;

	rcu_read_lock();

	list_for_each_entry_rcu(client, &dev->smi_clients, list) {
		if (!(READ_ONCE(client->events) &
				KFD_SMI_EVENT_MASK_FROM_INDEX(smi_event)))
			continue;
		spin_lock(&client->lock);
		if (kfifo_avail(&client->fifo) >= len) {
			kfifo_in(&client->fifo, event_msg, len);
			wake_up_all(&client->wait_queue);
		} else {
			pr_debug("smi_event(EventID: %u): no space left\n",
					smi_event);
		}
		spin_unlock(&client->lock);
	}

	rcu_read_unlock();
}

__printf(3, 4)
static void kfd_smi_event_add(struct kfd_dev *dev, unsigned int event,
			      char *fmt, ...)
{
	char fifo_in[KFD_SMI_EVENT_MSG_SIZE];
	int len;
	va_list args;

	if (list_empty(&dev->smi_clients))
		return;

	len = snprintf(fifo_in, sizeof(fifo_in), "%x ", event);

	va_start(args, fmt);
	len += vsnprintf(fifo_in + len, sizeof(fifo_in) - len, fmt, args);
	va_end(args);

	add_event_to_kfifo(dev, event, fifo_in, len);
}

void kfd_smi_event_update_gpu_reset(struct kfd_dev *dev, bool post_reset)
{
	unsigned int event;

	if (post_reset) {
		event = KFD_SMI_EVENT_GPU_POST_RESET;
	} else {
		event = KFD_SMI_EVENT_GPU_PRE_RESET;
		++(dev->reset_seq_num);
	}
	kfd_smi_event_add(dev, event, "%x\n", dev->reset_seq_num);
}

void kfd_smi_event_update_thermal_throttling(struct kfd_dev *dev,
					     uint64_t throttle_bitmask)
{
	kfd_smi_event_add(dev, KFD_SMI_EVENT_THERMAL_THROTTLE, "%llx:%llx\n",
			  throttle_bitmask,
			  amdgpu_dpm_get_thermal_throttling_counter(dev->adev));
}

void kfd_smi_event_update_vmfault(struct kfd_dev *dev, uint16_t pasid)
{
	struct amdgpu_task_info task_info;

	memset(&task_info, 0, sizeof(struct amdgpu_task_info));
	amdgpu_vm_get_task_info(dev->adev, pasid, &task_info);
	/* Report VM faults from user applications, not retry from kernel */
	if (!task_info.pid)
		return;

	kfd_smi_event_add(dev, KFD_SMI_EVENT_VMFAULT, "%x:%s\n",
			  task_info.pid, task_info.task_name);
}

int kfd_smi_event_open(struct kfd_dev *dev, uint32_t *fd)
{
	struct kfd_smi_client *client;
	int ret;

	client = kzalloc(sizeof(struct kfd_smi_client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	INIT_LIST_HEAD(&client->list);

	ret = kfifo_alloc(&client->fifo, MAX_KFIFO_SIZE, GFP_KERNEL);
	if (ret) {
		kfree(client);
		return ret;
	}

	ret = anon_inode_getfd(kfd_smi_name, &kfd_smi_ev_fops, (void *)client,
			       O_RDWR);
	if (ret < 0) {
		kfifo_free(&client->fifo);
		kfree(client);
		return ret;
	}
	*fd = ret;

	init_waitqueue_head(&client->wait_queue);
	spin_lock_init(&client->lock);
	client->events = 0;
	client->dev = dev;

	spin_lock(&dev->smi_lock);
	list_add_rcu(&client->list, &dev->smi_clients);
	spin_unlock(&dev->smi_lock);

	return 0;
}
