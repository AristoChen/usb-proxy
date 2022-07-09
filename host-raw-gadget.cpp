#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/types.h>

#include "host-raw-gadget.h"
#include "device-libusb.h"

struct usb_device_descriptor		host_device_desc;
struct raw_gadget_config_descriptor	*host_config_desc;

struct endpoint_thread *ep_thread_list;

/*----------------------------------------------------------------------*/

int usb_raw_open() {
	int fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open() /dev/raw-gadget");
		exit(EXIT_FAILURE);
	}
	return fd;
}

void usb_raw_init(int fd, enum usb_device_speed speed,
			const char *driver, const char *device) {
	struct usb_raw_init arg;
	strcpy((char *)&arg.driver_name[0], driver);
	strcpy((char *)&arg.device_name[0], device);
	arg.speed = speed;
	int rv = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_run(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_event_fetch(int fd, struct usb_raw_event *event) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
	if (rv < 0) {
		if (errno == EINTR) {
			event->length = 4294967295;
			return;
		}
		perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv < 0) {
		if (errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_ENABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_disable(int fd, uint32_t num) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, num);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_DISABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
	if (rv < 0) {
		if (errno == EINPROGRESS) {
			// Ignore failures caused by the test that halts endpoints.
			return rv;
		}
		else if (errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
	if (rv < 0) {
		if (errno == EINPROGRESS) {
			// Ignore failures caused by the test that halts endpoints.
			return rv;
		}
		else if (errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

void usb_raw_configure(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_CONFIGURED)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_vbus_draw(int fd, uint32_t power) {
	int rv = ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_VBUS_DRAW)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, info);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

void usb_raw_ep0_stall(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
	if (rv < 0) {
		if (errno == EBUSY)
			return;
		perror("ioctl(USB_RAW_IOCTL_EP0_STALL)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_ep_set_halt(int fd, int ep) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_SET_HALT, ep);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_SET_HALT)");
		exit(EXIT_FAILURE);
	}
}

/*----------------------------------------------------------------------*/

void log_control_request(struct usb_ctrlrequest *ctrl) {
	printf("  bRequestType: 0x%02x %5s, bRequest: 0x%02x, wValue: 0x%04x,"
		" wIndex: 0x%04x, wLength: %d\n", ctrl->bRequestType,
		(ctrl->bRequestType & USB_DIR_IN) ? "(IN)" : "(OUT)",
		ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		printf("  type = USB_TYPE_STANDARD\n");
		break;
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
		break;
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		switch (ctrl->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			printf("  req = USB_REQ_GET_DESCRIPTOR\n");
			switch (ctrl->wValue >> 8) {
			case USB_DT_DEVICE:
				printf("  desc = USB_DT_DEVICE\n");
				break;
			case USB_DT_CONFIG:
				printf("  desc = USB_DT_CONFIG\n");
				break;
			case USB_DT_STRING:
				printf("  desc = USB_DT_STRING\n");
				break;
			case USB_DT_INTERFACE:
				printf("  desc = USB_DT_INTERFACE\n");
				break;
			case USB_DT_ENDPOINT:
				printf("  desc = USB_DT_ENDPOINT\n");
				break;
			case USB_DT_DEVICE_QUALIFIER:
				printf("  desc = USB_DT_DEVICE_QUALIFIER\n");
				break;
			case USB_DT_OTHER_SPEED_CONFIG:
				printf("  desc = USB_DT_OTHER_SPEED_CONFIG\n");
				break;
			case USB_DT_INTERFACE_POWER:
				printf("  desc = USB_DT_INTERFACE_POWER\n");
				break;
			case USB_DT_OTG:
				printf("  desc = USB_DT_OTG\n");
				break;
			case USB_DT_DEBUG:
				printf("  desc = USB_DT_DEBUG\n");
				break;
			case USB_DT_INTERFACE_ASSOCIATION:
				printf("  desc = USB_DT_INTERFACE_ASSOCIATION\n");
				break;
			case USB_DT_SECURITY:
				printf("  desc = USB_DT_SECURITY\n");
				break;
			case USB_DT_KEY:
				printf("  desc = USB_DT_KEY\n");
				break;
			case USB_DT_ENCRYPTION_TYPE:
				printf("  desc = USB_DT_ENCRYPTION_TYPE\n");
				break;
			case USB_DT_BOS:
				printf("  desc = USB_DT_BOS\n");
				break;
			case USB_DT_DEVICE_CAPABILITY:
				printf("  desc = USB_DT_DEVICE_CAPABILITY\n");
				break;
			case USB_DT_WIRELESS_ENDPOINT_COMP:
				printf("  desc = USB_DT_WIRELESS_ENDPOINT_COMP\n");
				break;
			case USB_DT_PIPE_USAGE:
				printf("  desc = USB_DT_PIPE_USAGE\n");
				break;
			case USB_DT_SS_ENDPOINT_COMP:
				printf("  desc = USB_DT_SS_ENDPOINT_COMP\n");
				break;
			default:
				printf("  desc = unknown = 0x%x\n",
							ctrl->wValue >> 8);
				break;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			printf("  req = USB_REQ_SET_CONFIGURATION\n");
			break;
		case USB_REQ_GET_CONFIGURATION:
			printf("  req = USB_REQ_GET_CONFIGURATION\n");
			break;
		case USB_REQ_SET_INTERFACE:
			printf("  req = USB_REQ_SET_INTERFACE\n");
			break;
		case USB_REQ_GET_INTERFACE:
			printf("  req = USB_REQ_GET_INTERFACE\n");
			break;
		case USB_REQ_GET_STATUS:
			printf("  req = USB_REQ_GET_STATUS\n");
			break;
		case USB_REQ_CLEAR_FEATURE:
			printf("  req = USB_REQ_CLEAR_FEATURE\n");
			break;
		case USB_REQ_SET_FEATURE:
			printf("  req = USB_REQ_SET_FEATURE\n");
			break;
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	case USB_TYPE_CLASS:
		switch (ctrl->bRequest) {
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	default:
		printf("  req = unknown = 0x%x\n", ctrl->bRequest);
		break;
	}
}

void log_event(struct usb_raw_event *event) {
	switch (event->type) {
	case USB_RAW_EVENT_CONNECT:
		printf("event: connect, length: %u\n", event->length);
		break;
	case USB_RAW_EVENT_CONTROL:
		printf("event: control, length: %u\n", event->length);
		log_control_request((struct usb_ctrlrequest *)&event->data[0]);
		break;
	default:
		printf("event: unknown, length: %u\n", event->length);
	}
}

/*----------------------------------------------------------------------*/

void *ep_loop_write(void *arg) {
	struct thread_info ep_thread_info = *((struct thread_info*) arg);
	int fd = ep_thread_info.fd;
	int ep_num = ep_thread_info.ep_num;
	struct usb_endpoint_descriptor ep = ep_thread_info.endpoint;
	std::string transfer_type = ep_thread_info.transfer_type;
	std::string dir = ep_thread_info.dir;
	std::deque<data_queue_info> *data_queue = ep_thread_info.data_queue;
	std::mutex *data_mutex = ep_thread_info.data_mutex;

	printf("Start writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	while (!please_stop_eps) {
		assert(ep_num != -1);
		if (data_queue->size() == 0) {
			usleep(100);
			continue;
		}

		data_mutex->lock();
		struct data_queue_info temp_data = data_queue->front();
		data_queue->pop_front();
		data_mutex->unlock();
		struct usb_raw_transfer_io io = temp_data.io;

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv >= 0) {
				printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
			}
		}
		else {
			int length = temp_data.length;
			unsigned char *data = new unsigned char[length];
			memcpy(data, io.data, length);
			send_data(ep.bEndpointAddress, ep.bmAttributes, data, length);

			if (data)
				delete[] data;
		}
	}

	printf("End writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void *ep_loop_read(void *arg) {
	struct thread_info ep_thread_info = *((struct thread_info*) arg);
	int fd = ep_thread_info.fd;
	int ep_num = ep_thread_info.ep_num;
	struct usb_endpoint_descriptor ep = ep_thread_info.endpoint;
	std::string transfer_type = ep_thread_info.transfer_type;
	std::string dir = ep_thread_info.dir;
	std::deque<data_queue_info> *data_queue = ep_thread_info.data_queue;
	std::mutex *data_mutex = ep_thread_info.data_mutex;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	while (!please_stop_eps) {
		assert(ep_num != -1);
		struct data_queue_info temp_data;

		if (ep.bEndpointAddress & USB_DIR_IN) {
			unsigned char *data = NULL;
			int nbytes = 0;

			if (data_queue->size() >= 32) {
				usleep(200);
				continue;
			}

			receive_data(ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize, &data, &nbytes, 0);

			if (nbytes > 0) { // Not sure if we should enqueue data if nbytes == 0
				memcpy(temp_data.io.data, data, nbytes);
				temp_data.io.inner.ep = ep_num;
				temp_data.io.inner.flags = 0;
				temp_data.io.inner.length = nbytes;
				temp_data.length = nbytes;

				data_mutex->lock();
				data_queue->push_back(temp_data);
				data_mutex->unlock();
				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), nbytes);
			}

			if (data)
				delete[] data;
		}
		else {
			temp_data.io.inner.ep = ep_num;
			temp_data.io.inner.flags = 0;
			temp_data.io.inner.length = sizeof(temp_data.io.data);

			int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&temp_data.io);
			if (rv >= 0) {
				printf("EP%x(%s_%s): read %d bytes from host\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
				temp_data.length = rv;

				data_mutex->lock();
				data_queue->push_back(temp_data);
				data_mutex->unlock();
				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), rv);
			}
		}
	}

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void process_eps(int fd) {
	struct usb_raw_eps_info info;
	memset(&info, 0, sizeof(info));

	int num = usb_raw_eps_info(fd, &info);
	if (verbose_level) {
		for (int i = 0; i < num; i++) {
			printf("ep #%d:\n", i);
			printf("  name: %s\n", &info.eps[i].name[0]);
			printf("  addr: %u\n", info.eps[i].addr);
			printf("  type: %s %s %s\n",
				info.eps[i].caps.type_iso ? "iso" : "___",
				info.eps[i].caps.type_bulk ? "blk" : "___",
				info.eps[i].caps.type_int ? "int" : "___");
			printf("  dir : %s %s\n",
				info.eps[i].caps.dir_in ? "in " : "___",
				info.eps[i].caps.dir_out ? "out" : "___");
			printf("  maxpacket_limit: %u\n",
				info.eps[i].limits.maxpacket_limit);
			printf("  max_streams: %u\n", info.eps[i].limits.max_streams);
		}
	}

	struct raw_gadget_interface_descriptor temp_interface = host_config_desc[desired_configuration]
								.interfaces[desired_interface]
								.altsetting[desired_interface_altsetting];
	ep_thread_list = new struct endpoint_thread[temp_interface.interface.bNumEndpoints];
	printf("bNumEndpoints is %d\n", static_cast<int>(temp_interface.interface.bNumEndpoints));

	for (int i = 0; i < temp_interface.interface.bNumEndpoints; i++) {
		int addr = usb_endpoint_num(&temp_interface.endpoints[i]);
		assert(addr != 0);

		ep_thread_list[i].ep_thread_info.fd = fd;
		ep_thread_list[i].ep_thread_info.endpoint = temp_interface.endpoints[i];
		ep_thread_list[i].ep_thread_info.data_queue = new std::deque<data_queue_info>;
		ep_thread_list[i].ep_thread_info.data_mutex = new std::mutex;

		switch (usb_endpoint_type(&temp_interface.endpoints[i])) {
		case USB_ENDPOINT_XFER_ISOC:
			ep_thread_list[i].ep_thread_info.transfer_type = "isoc";
			break;
		case USB_ENDPOINT_XFER_BULK:
			ep_thread_list[i].ep_thread_info.transfer_type = "bulk";
			break;
		case USB_ENDPOINT_XFER_INT:
			ep_thread_list[i].ep_thread_info.transfer_type = "int";
			break;
		default:
			printf("transfer_type %d is invalid\n", usb_endpoint_type(&temp_interface.endpoints[i]));
			assert(false);
		}

		if (usb_endpoint_dir_in(&temp_interface.endpoints[i]))
			ep_thread_list[i].ep_thread_info.dir = "in";
		else
			ep_thread_list[i].ep_thread_info.dir = "out";

		ep_thread_list[i].ep_thread_info.ep_num = usb_raw_ep_enable(fd,
					&ep_thread_list[i].ep_thread_info.endpoint);
		printf("%s_%s: addr = %u, ep = #%d\n",
			ep_thread_list[i].ep_thread_info.transfer_type.c_str(),
			ep_thread_list[i].ep_thread_info.dir.c_str(),
			addr, ep_thread_list[i].ep_thread_info.ep_num);

		if (verbose_level)
			printf("Creating thread for EP%02x\n",
				ep_thread_list[i].ep_thread_info.endpoint.bEndpointAddress);
		pthread_create(&ep_thread_list[i].ep_thread_read, 0,
			ep_loop_read, (void *) &ep_thread_list[i].ep_thread_info);
		pthread_create(&ep_thread_list[i].ep_thread_write, 0,
			ep_loop_write, (void *) &ep_thread_list[i].ep_thread_info);
	}

	printf("process_eps done\n");
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;
	int previous_bConfigurationValue = -1;
	int previous_interface = 0;
	int previous_interface_altsetting = 0;

	printf("Start for EP0, thread id(%d)\n", gettid());
	while (!please_stop_ep0) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);

		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);

		if (event.inner.length == 4294967295) {
			printf("End for EP0, thread id(%d)\n", gettid());
			return;
		}

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_control_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = event.ctrl.wLength;

		int nbytes = 0;
		int result = 0;
		unsigned char *control_data = new unsigned char[event.ctrl.wLength];

		int rv = -1;
		if (event.ctrl.bRequestType & USB_DIR_IN) {
			result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
			if (result == 0) {
				memcpy(&io.data[0], control_data, nbytes);
				io.inner.length = nbytes;
				rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
				printf("ep0: transferred %d bytes (in)\n", rv);
			}
			else {
				printf("ep0: stalling\n");
				usb_raw_ep0_stall(fd);
			}
		}
		else {
			rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);

			if (event.ctrl.bRequestType == 0x00 && event.ctrl.bRequest == 0x09) {
				// Set configuration
				if (previous_bConfigurationValue == event.ctrl.wValue) {
					printf("Skip changing configuration, wValue is same as previous\n");
					continue;
				}

				if (set_configuration_done_once) {
					// Need to stop all threads for eps and cleanup
					printf("Changing configuration\n");

					please_stop_eps = true;
					desired_interface = 0;
					desired_interface_altsetting = 0;
					release_interface(desired_interface);

					int thread_num = host_config_desc[desired_configuration]
							 .interfaces[desired_interface]
							 .altsetting[desired_interface_altsetting]
							 .interface
							 .bNumEndpoints;
					for (int i = 0; i < thread_num; i++) {
						if (ep_thread_list[i].ep_thread_read &&
							pthread_join(ep_thread_list[i].ep_thread_read, NULL)) {
							fprintf(stderr, "Error join ep_thread_read\n");
						}
						if (ep_thread_list[i].ep_thread_write &&
							pthread_join(ep_thread_list[i].ep_thread_write, NULL)) {
							fprintf(stderr, "Error join ep_thread_write\n");
						}

						usb_raw_ep_disable(fd, ep_thread_list[i].ep_thread_info.ep_num);
					}
					delete[] ep_thread_list;

					set_configuration(event.ctrl.wValue);

					please_stop_eps = false;
				}

				for (int i = 0; i < host_device_desc.bNumConfigurations; i++) {
					if (host_config_desc[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_configuration = i;
						printf("Found desired configuration at index: %d\n", i);
					}
				}
				claim_interface(desired_interface);
				process_eps(fd);

				set_configuration_done_once = true;
				previous_bConfigurationValue = event.ctrl.wValue;
			}
			else if (event.ctrl.bRequestType == 0x01 && event.ctrl.bRequest == 0x0b) {
				// Set interface/alt_setting
				bool process_eps_required = false;
				desired_interface = event.ctrl.wIndex;
				desired_interface_altsetting = event.ctrl.wValue;

				if (previous_interface != desired_interface) {
					printf("Will change interface from %d to %d\n",
						previous_interface, desired_interface);
					release_interface(previous_interface);
					process_eps_required = true;
				}
				if (previous_interface_altsetting != desired_interface_altsetting) {
					printf("Will Change alt_setting from %d to %d\n",
						previous_interface_altsetting, desired_interface_altsetting);
					process_eps_required = true;
				}

				if (process_eps_required) {
					// Need to stop all threads for eps and cleanup
					printf("Changing interface/altsetting\n");

					please_stop_eps = true;

					int thread_num = host_config_desc[desired_configuration]
							 .interfaces[previous_interface]
							 .altsetting[previous_interface_altsetting]
							 .interface
							 .bNumEndpoints;
					for (int i = 0; i < thread_num; i++) {
						if (ep_thread_list[i].ep_thread_read &&
							pthread_join(ep_thread_list[i].ep_thread_read, NULL)) {
							fprintf(stderr, "Error join ep_thread_read\n");
						}
						if (ep_thread_list[i].ep_thread_write &&
							pthread_join(ep_thread_list[i].ep_thread_write, NULL)) {
							fprintf(stderr, "Error join ep_thread_write\n");
						}

						usb_raw_ep_disable(fd, ep_thread_list[i].ep_thread_info.ep_num);
					}
					delete[] ep_thread_list;

					please_stop_eps = false;

					if (previous_interface != desired_interface)
						claim_interface(desired_interface);
					process_eps(fd);
				}

				previous_interface = desired_interface;
				previous_interface_altsetting = desired_interface_altsetting;
			}
			else {
				memcpy(control_data, io.data, event.ctrl.wLength);
				result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
				if (result == 0) {
					printf("ep0: transferred %d bytes (out)\n", rv);
				}
				else {
					printf("ep0: stalling\n");
					usb_raw_ep0_stall(fd);
				}
			}
		}

		delete[] control_data;
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}
