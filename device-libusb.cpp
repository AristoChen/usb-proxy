#include "device-libusb.h"

libusb_device 			**devs;
libusb_device_handle 		*dev_handle;
libusb_context 			*context = NULL;
libusb_hotplug_callback_handle	callback_handle = -1;

struct libusb_device_descriptor		device_device_desc;
struct libusb_config_descriptor		**device_config_desc;

pthread_t hotplug_monitor_thread;

int hotplug_callback(struct libusb_context *ctx __attribute__((unused)),
			struct libusb_device *dev __attribute__((unused)),
			libusb_hotplug_event envet __attribute__((unused)),
			void *user_data __attribute__((unused))) {
	printf("Hotplug event: device disconnected, stopping proxy...\n");
	kill(0, SIGINT);
	return 0;
}

void *hotplug_monitor(void *arg __attribute__((unused))) {
	printf("Start hotplug_monitor thread, thread id(%d)\n", gettid());
	while(true) {
		usleep(100 * 1000);
		libusb_handle_events_completed(NULL, NULL);
	}
}

int get_descriptor(libusb_device *device) {
	int result;
	result = libusb_get_device_descriptor(device, &device_device_desc);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error retrieving device descriptor: %s\n",
					libusb_strerror((libusb_error)result));
		}
		return result;
	}

	device_config_desc = new struct libusb_config_descriptor *[device_device_desc.bNumConfigurations];
	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		result = libusb_get_config_descriptor(device, i, &device_config_desc[i]);
		if (result != LIBUSB_SUCCESS) {
			if (verbose_level) {
				fprintf(stderr, "Error retrieving configuration(%d) descriptor: %s\n",
						i, libusb_strerror((libusb_error)result));
			}
			return result;
		}
	}

	return LIBUSB_SUCCESS;
}

int connect_device(int vendor_id, int product_id) {
	int result;
	result = libusb_init(&context);
	if (result < 0) {
		fprintf(stderr, "Init error: %s\n", libusb_strerror((libusb_error)result));
		return 1;
	}
	libusb_set_debug(context, 3);

	libusb_device **list = NULL;
	libusb_device *found = NULL;

	int cnt = libusb_get_device_list(context, &list);
	if (cnt < 0) {
		if (verbose_level) {
			fprintf(stderr, "Error retrieving device list: %s\n",
					libusb_strerror((libusb_error)cnt));
		}
		return cnt;
	}

	while (found == NULL) {
		cnt = libusb_get_device_list(context, &devs);
		if (cnt < 0) {
			fprintf(stderr, "Get Device Error: %s\n",
					libusb_strerror((libusb_error)cnt));
			return 1;
		}
		if (verbose_level)
			printf("%d Devices in list\n", cnt);

		for (int i = 0; i < cnt; i++) {
			libusb_device *dvc = devs[i];
			result = get_descriptor(dvc);
			if (result != LIBUSB_SUCCESS)
				continue;

			if (device_device_desc.bDeviceClass == LIBUSB_CLASS_HUB)
				continue;

			if (vendor_id == -1 && product_id == -1) {
				found = dvc;
				break;
			}
			else if ((vendor_id == device_device_desc.idVendor || vendor_id == LIBUSB_HOTPLUG_MATCH_ANY) &&
				(product_id == device_device_desc.idProduct || product_id == LIBUSB_HOTPLUG_MATCH_ANY)) {
				found = dvc;
				break;
			}
		}

		if (verbose_level && vendor_id != -1 && product_id != -1)
			printf("Target device not found\n");
		libusb_free_device_list(devs, 1);
		sleep(1);
	}

	result = libusb_open(found, &dev_handle);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error opening device handle: %s\n",
					libusb_strerror((libusb_error)result));
		}
		dev_handle = NULL;
		libusb_free_device_list(list, 1);
		return result;
	}

	result = libusb_set_auto_detach_kernel_driver(dev_handle, 0);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_set_auto_detach_kernel_driver() failed: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	int config = 0;
	result = libusb_get_configuration(dev_handle, &config);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_get_configuration() failed: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		if (device_config_desc[i]->bConfigurationValue != config)
			continue;
		for (int j = 0; j < device_config_desc[i]->bNumInterfaces; j++)
			libusb_detach_kernel_driver(dev_handle, j);
	}

	if (reset_device_before_proxy) {
		result = libusb_reset_device(dev_handle);
		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "libusb_reset_device() failed: %s\n",
					libusb_strerror((libusb_error)result));
			return result;
		}
	}

	//check that device is responsive
	unsigned char unused[4];
	result = libusb_get_string_descriptor(dev_handle, 0, 0, unused, sizeof(unused));
	if (result < 0) {
		fprintf(stderr, "Device unresponsive: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	if (callback_handle == -1) {
		result = libusb_hotplug_register_callback(context,
			(libusb_hotplug_event) (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
			(libusb_hotplug_flag) 0, vendor_id, product_id,
			LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &callback_handle);

		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "Error registering callback\n");
			libusb_exit(context);
			return result;
		}
		pthread_create(&hotplug_monitor_thread, 0,
			hotplug_monitor, nullptr);
	}

	return 0;
}

void reset_device() {
	int result = libusb_reset_device(dev_handle);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error resetting device: %s\n",
				libusb_strerror((libusb_error)result));
	}
}

void set_configuration(int configuration) {
	int result = libusb_set_configuration(dev_handle, configuration);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting configuration(%d): %s\n",
				configuration, libusb_strerror((libusb_error)result));
	}
}

void claim_interface(int interface) {
	int result = libusb_claim_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error claiming interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
	}
}

void release_interface(int interface) {
	int result = libusb_release_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
		fprintf(stderr, "Error releasing interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
	}
}

void set_interface_alt_setting(int interface, int altsetting) {
	int result = libusb_set_interface_alt_setting(dev_handle, interface, altsetting);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting interface altsetting(%d, %d): %s\n",
				interface, altsetting, libusb_strerror((libusb_error)result));
	}
}

int control_request(const usb_ctrlrequest *setup_packet, int *nbytes,
			unsigned char **dataptr, int timeout) {
	int result = libusb_control_transfer(dev_handle,
					setup_packet->bRequestType, setup_packet->bRequest,
					setup_packet->wValue, setup_packet->wIndex, *dataptr,
					setup_packet->wLength, timeout);

	if (result < 0) {
		if (verbose_level) {
			fprintf(stderr, "Error sending setup packet: %s\n",
					libusb_strerror((libusb_error)result));
		}
		if (result == LIBUSB_ERROR_PIPE)
			return -1;
		return result;
	}
	else {
		if (verbose_level)
			printf("Control transfer succeed\n");
	}

	*nbytes = result;
	return 0;
}

int send_data(uint8_t endpoint, uint8_t attributes, uint8_t *dataptr,
			int length, int timeout) {
	int transferred;
	int attempt = 0;
	int result = LIBUSB_SUCCESS;

	bool incomplete_transfer = false;

	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't send on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_ISOC:
		if (verbose_level)
			fprintf(stderr, "Isochronous(write) endpoint EP%02x unhandled.\n", endpoint);
		break;
	case USB_ENDPOINT_XFER_BULK:
		do {
			result = libusb_bulk_transfer(dev_handle, endpoint, dataptr, length, &transferred, timeout);
			//TODO retry transfer if incomplete
			if (transferred != length) {
				fprintf(stderr, "Incomplete Bulk transfer on EP%02x for attempt %d. length(%d), transferred(%d)\n",
					endpoint, attempt, length, transferred);
				incomplete_transfer = true;
			}
			if (result == LIBUSB_SUCCESS) {
				if (incomplete_transfer)
					printf("Resent Bulk transfer on EP%02x for attempt %d. length(%d), transferred(%d)\n",
						endpoint, attempt, length, transferred);
				if (verbose_level > 2)
					printf("Sent %d bytes (Bulk) to EP%02x\n", transferred, endpoint);
			}
			if ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);

			attempt++;
		} while ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT || transferred != length)
					&& attempt < MAX_ATTEMPTS);
		break;
	case USB_ENDPOINT_XFER_INT:
		result = libusb_interrupt_transfer(dev_handle, endpoint, dataptr, length, &transferred, timeout);

		if (transferred != length)
			fprintf(stderr, "Incomplete Interrupt transfer on EP%02x\n", endpoint);
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Sent %d bytes (Int) to libusb EP%02x\n", transferred, endpoint);
		break;
	}
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Transfer error sending on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}
	return result;
}

void iso_transfer_callback(struct libusb_transfer *transfer) {
	int *iso_completed = (int *)transfer->user_data;
	*iso_completed = 1;
}

int receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize,
			uint8_t **dataptr, int *length, int timeout) {
	int result = LIBUSB_SUCCESS;
	struct libusb_transfer *transfer;
	int iso_completed, iso_packets;

	int attempt = 0;
	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't read on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_ISOC:
		*dataptr = new uint8_t[maxPacketSize];
		// We could retrieve multiple packets at a time, but then we
		// would need to split the received data submit each packet
		// separately via Raw Gadget. So retrieve only one packet for
		// simplicity.
		iso_packets = 1;
		transfer = libusb_alloc_transfer(iso_packets);
		if (!transfer) {
			fprintf(stderr, "Failed to allocate libusb_transfer.\n");
			result = LIBUSB_ERROR_OTHER;
		}
		iso_completed = 0;
		libusb_fill_iso_transfer(transfer, dev_handle, endpoint, *dataptr, maxPacketSize,
					iso_packets, iso_transfer_callback, &iso_completed, timeout);
		libusb_set_iso_packet_lengths(transfer, maxPacketSize / iso_packets);
		result = libusb_submit_transfer(transfer);
		if (result != LIBUSB_SUCCESS) {
			libusb_free_transfer(transfer);
			break;
		}
		while (!iso_completed)
			libusb_handle_events_completed(NULL, &iso_completed);
		*length = 0;
		for (int i = 0; i < iso_packets; i++)
			*length += transfer->iso_packet_desc[i].actual_length;
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Received iso data(%d) bytes\n", *length);
		libusb_free_transfer(transfer);
		break;
	case USB_ENDPOINT_XFER_BULK:
		*dataptr = new uint8_t[maxPacketSize * 8];
		do {
			result = libusb_bulk_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
			if (result == LIBUSB_SUCCESS && verbose_level > 2)
				printf("Received bulk data(%d) bytes\n", *length);
			if ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);

			attempt++;
		} while ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT) && attempt < MAX_ATTEMPTS);
		break;
	case USB_ENDPOINT_XFER_INT:
		*dataptr = new uint8_t[maxPacketSize];
		result = libusb_interrupt_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Received int data(%d) bytes\n", *length);
		break;
	}

	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Transfer error receiving on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}

	return result;
}
