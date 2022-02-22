#include <cstring>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "misc.h"

int verbose_level = 0;
bool please_stop = false;

int desired_configuration = 1;

void usage() {
	printf("Usage:\n");
	printf("\t-h/--help: print this help message\n");
	printf("\t-v/--verbose: increase verbosity\n");
	printf("\t--device: use specific device\n");
	printf("\t--driver: use specific driver\n");
	printf("\t--vendor_id: use specific vendor_id of USB device\n");
	printf("\t--product_id: use specific product_id of USB device\n\n");
	printf("If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device\n");
	printf("If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.\n");
	printf("If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect\n");
	printf("the first USB device it can find.\n\n");
	exit(1);
}

void handle_signal(int signum) {
	switch (signum) {
	case SIGTERM:
	case SIGINT:
		static bool signal_received = false;
		if (signal_received) {
			printf("Signal received again, force exiting\n");
			exit(1);
		}
		if (signum == SIGTERM)
			printf("Received SIGTERM, stopping...\n");
		else
			printf("Received SIGINT, stopping...\n");

		signal_received = true;
		please_stop = true;
		break;
	}
}

int setup_host_usb_desc() {
	host_device_desc = {
		.bLength =		device_device_desc.bLength,
		.bDescriptorType =	device_device_desc.bDescriptorType,
		.bcdUSB =		device_device_desc.bcdUSB,
		.bDeviceClass =		device_device_desc.bDeviceClass,
		.bDeviceSubClass =	device_device_desc.bDeviceSubClass,
		.bDeviceProtocol =	device_device_desc.bDeviceProtocol,
		.bMaxPacketSize0 =	device_device_desc.bMaxPacketSize0,
		.idVendor =		device_device_desc.idVendor,
		.idProduct =		device_device_desc.idProduct,
		.bcdDevice =		device_device_desc.bcdDevice,
		.iManufacturer =	device_device_desc.iManufacturer,
		.iProduct =		device_device_desc.iProduct,
		.iSerialNumber =	device_device_desc.iSerialNumber,
		.bNumConfigurations =	device_device_desc.bNumConfigurations,
	};

	host_config_desc = new struct raw_gadget_config_descriptor[host_device_desc.bNumConfigurations];
	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		struct usb_config_descriptor temp_config = {
			.bLength =		device_config_desc[i]->bLength,
			.bDescriptorType =	device_config_desc[i]->bDescriptorType,
			.wTotalLength =		device_config_desc[i]->wTotalLength,
			.bNumInterfaces =	device_config_desc[i]->bNumInterfaces,
			.bConfigurationValue =	device_config_desc[i]->bConfigurationValue,
			.iConfiguration = 	device_config_desc[i]->iConfiguration,
			.bmAttributes =		device_config_desc[i]->bmAttributes,
			.bMaxPower =		device_config_desc[i]->MaxPower,
		};
		host_config_desc[i].config = temp_config;

		struct raw_gadget_interface_descriptor *temp_interfaces =
			new struct raw_gadget_interface_descriptor[device_config_desc[i]->interface->num_altsetting];
		for (int j = 0; j < device_config_desc[i]->interface->num_altsetting; j++) {
			const struct libusb_interface_descriptor temp_altsetting = device_config_desc[i]->interface->altsetting[j];
			struct usb_interface_descriptor temp_interface = {
				.bLength =		temp_altsetting.bLength,
				.bDescriptorType =	temp_altsetting.bDescriptorType,
				.bInterfaceNumber =	temp_altsetting.bInterfaceNumber,
				.bAlternateSetting =	temp_altsetting.bAlternateSetting,
				.bNumEndpoints =	temp_altsetting.bNumEndpoints,
				.bInterfaceClass =	temp_altsetting.bInterfaceClass,
				.bInterfaceSubClass =	temp_altsetting.bInterfaceSubClass,
				.bInterfaceProtocol =	temp_altsetting.bInterfaceProtocol,
				.iInterface =		temp_altsetting.iInterface,
			};
			temp_interfaces[j].interface = temp_interface;

			struct usb_endpoint_descriptor *temp_endpoints =
				new struct usb_endpoint_descriptor[temp_altsetting.bNumEndpoints];
			for (int k = 0; k < temp_altsetting.bNumEndpoints; k++) {
				struct usb_endpoint_descriptor temp_endpoint = {
					.bLength =		temp_altsetting.endpoint[k].bLength,
					.bDescriptorType =	temp_altsetting.endpoint[k].bDescriptorType,
					.bEndpointAddress =	temp_altsetting.endpoint[k].bEndpointAddress,
					.bmAttributes =		temp_altsetting.endpoint[k].bmAttributes,
					.wMaxPacketSize =	temp_altsetting.endpoint[k].wMaxPacketSize,
					.bInterval =		temp_altsetting.endpoint[k].bInterval,
					.bRefresh =		temp_altsetting.endpoint[k].bRefresh,
					.bSynchAddress = 	temp_altsetting.endpoint[k].bSynchAddress,
				};
				temp_endpoints[k] = temp_endpoint;
			}
			temp_interfaces[j].endpoints = temp_endpoints;
		}
		host_config_desc[i].interfaces = temp_interfaces;
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "dummy_udc.0";
	const char *driver = "dummy_udc";
	int vendor_id = -1;
	int product_id = -1;

	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = handle_signal;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	int opt, lopt, loidx;
	const char *optstring = "hv";
	const struct option long_options[] = {
		{"help", no_argument, &lopt, 1},
		{"verbose", no_argument, &lopt, 2},
		{"device", required_argument, &lopt, 3},
		{"driver", required_argument, &lopt, 4},
		{"vendor_id", required_argument, &lopt, 5},
		{"product_id", required_argument, &lopt, 6},
		{0, 0, 0, 0}
	};
	while ((opt = getopt_long(argc, argv, optstring, long_options, &loidx)) != -1) {
		if(opt == 0)
			opt = lopt;
		switch (opt) {
		case 'h':
			usage();
			break;
		case 'v':
			verbose_level++;
			break;
		case 1:
			usage();
			break;
		case 2:
			verbose_level++;
			break;
		case 3:
			device = optarg;
			break;
		case 4:
			driver = optarg;
			break;
		case 5:
			vendor_id = std::stoul(optarg, nullptr, 16);
			break;
		case 6:
			product_id = std::stoul(optarg, nullptr, 16);
			break;

		default:
			usage();
			return 1;
		}
	}
	printf("Device is: %s\n", device);
	printf("Driver is: %s\n", driver);
	printf("vendor_id is: %d\n", vendor_id);
	printf("product_id is: %d\n", product_id);

	while (connect_device(vendor_id, product_id)) {
		sleep(1);
	}
	printf("Device opened successfully\n");

	setup_host_usb_desc();
	printf("Setup USB config successfully\n");

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	int thread_num = host_config_desc[0].interfaces[0].interface.bNumEndpoints;
	for (int i = 0; i < thread_num; i++) {
		if (ep_thread_list[i].ep_thread_read &&
			pthread_join(ep_thread_list[i].ep_thread_read, NULL)) {
			fprintf(stderr, "Error join ep_thread_read\n");
		}
		if (ep_thread_list[i].ep_thread_write &&
			pthread_join(ep_thread_list[i].ep_thread_write, NULL)) {
			fprintf(stderr, "Error join ep_thread_write\n");
		}

		delete[] ep_thread_list[i].ep_thread_info.data_queue;
		delete[] ep_thread_list[i].ep_thread_info.data_mutex;
	}
	delete[] ep_thread_list;

	close(fd);

	for (int i = 0; i < host_device_desc.bNumConfigurations; i++) {
		for (int j = 0; j < device_config_desc[i]->interface->num_altsetting; j++) {
			for (int k = 0; k < device_config_desc[i]->interface->altsetting[j].bNumEndpoints; k++)
				delete[] host_config_desc[i].interfaces[j].endpoints;
			delete[] host_config_desc[i].interfaces;
		}
	}
	delete[] host_config_desc;
	delete[] device_config_desc;

	release_interface(0);

	if (context && callback_handle != -1) {
		libusb_hotplug_deregister_callback(context, callback_handle);
	}
	if (hotplug_monitor_thread &&
		pthread_join(hotplug_monitor_thread, NULL)) {
		fprintf(stderr, "Error join hotplug_monitor_thread\n");
	}

	return 0;
}
