#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "proxy.h"
#include "misc.h"

int verbose_level = 0;
bool please_stop_ep0 = false;
volatile bool please_stop_eps = false; // Use volatile to mark as atomic.

bool injection_enabled = false;
std::string injection_file = "injection.json";
Json::Value injection_config;

bool customized_config_enabled = false;
std::string customized_config_file = "config.json";
bool reset_device_before_proxy = true;
bool bmaxpacketsize0_must_greater_than_64 = true;

void usage() {
	printf("Usage:\n");
	printf("\t-h/--help: print this help message\n");
	printf("\t-v/--verbose: increase verbosity\n");
	printf("\t--device: use specific device\n");
	printf("\t--driver: use specific driver\n");
	printf("\t--vendor_id: use specific vendor_id of USB device\n");
	printf("\t--product_id: use specific product_id of USB device\n");
	printf("\t--enable_injection: enable the injection feature\n");
	printf("\t--injection_file: specify the file that contains injection rules\n");
	printf("\t--enable_customized_config: enable the customized config feature\n\n");
	printf("* If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device.\n");
	printf("* If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.\n");
	printf("* If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect\n");
	printf("  the first USB device it can find.\n");
	printf("* If `injection_file` not specified, `usb-proxy` will use `injection.json` by default.\n\n");
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
		please_stop_ep0 = true;
		please_stop_eps = true;
		break;
	}
}

int setup_host_usb_desc() {
	host_device_desc.device = {
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

	int bNumConfigurations = device_device_desc.bNumConfigurations;
	host_device_desc.configs = new struct raw_gadget_config[bNumConfigurations];
	for (int i = 0; i < bNumConfigurations; i++) {
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
		host_device_desc.configs[i].config = temp_config;

		int bNumInterfaces = device_config_desc[i]->bNumInterfaces;
		struct raw_gadget_interface *temp_interfaces =
			new struct raw_gadget_interface[bNumInterfaces];
		for (int j = 0; j < bNumInterfaces; j++) {
			int num_altsetting = device_config_desc[i]->interface[j].num_altsetting;
			struct raw_gadget_altsetting *temp_altsettings =
				new struct raw_gadget_altsetting[num_altsetting];
			for (int k = 0; k < num_altsetting; k++) {
				const struct libusb_interface_descriptor temp_device_altsetting =
					device_config_desc[i]->interface[j].altsetting[k];
				struct usb_interface_descriptor temp_host_altsetting = {
					.bLength =		temp_device_altsetting.bLength,
					.bDescriptorType =	temp_device_altsetting.bDescriptorType,
					.bInterfaceNumber =	temp_device_altsetting.bInterfaceNumber,
					.bAlternateSetting =	temp_device_altsetting.bAlternateSetting,
					.bNumEndpoints =	temp_device_altsetting.bNumEndpoints,
					.bInterfaceClass =	temp_device_altsetting.bInterfaceClass,
					.bInterfaceSubClass =	temp_device_altsetting.bInterfaceSubClass,
					.bInterfaceProtocol =	temp_device_altsetting.bInterfaceProtocol,
					.iInterface =		temp_device_altsetting.iInterface,
				};
				temp_altsettings[k].interface = temp_host_altsetting;

				if (!temp_device_altsetting.bNumEndpoints) {
					printf("InterfaceNumber %x AlternateSetting %x has no endpoint, skip\n",
						temp_device_altsetting.bInterfaceNumber,
						temp_device_altsetting.bAlternateSetting);
					temp_altsettings[k].endpoints = NULL;
					continue;
				}

				int bNumEndpoints = temp_device_altsetting.bNumEndpoints;
				struct raw_gadget_endpoint *temp_endpoints =
					new struct raw_gadget_endpoint[bNumEndpoints];
				for (int l = 0; l < bNumEndpoints; l++) {
					struct usb_endpoint_descriptor temp_endpoint = {
						.bLength =		temp_device_altsetting.endpoint[l].bLength,
						.bDescriptorType =	temp_device_altsetting.endpoint[l].bDescriptorType,
						.bEndpointAddress =	temp_device_altsetting.endpoint[l].bEndpointAddress,
						.bmAttributes =		temp_device_altsetting.endpoint[l].bmAttributes,
						.wMaxPacketSize =	temp_device_altsetting.endpoint[l].wMaxPacketSize,
						.bInterval =		temp_device_altsetting.endpoint[l].bInterval,
						.bRefresh =		temp_device_altsetting.endpoint[l].bRefresh,
						.bSynchAddress = 	temp_device_altsetting.endpoint[l].bSynchAddress,
					};
					temp_endpoints[l].endpoint = temp_endpoint;
					temp_endpoints[l].thread_read = 0;
					temp_endpoints[l].thread_write = 0;
					memset((void *)&temp_endpoints[l].thread_info, 0,
						sizeof(temp_endpoints[l].thread_info));
					temp_endpoints[l].thread_info.ep_num = -1;
				}
				temp_altsettings[k].endpoints = temp_endpoints;
			}
			temp_interfaces[j].altsettings = temp_altsettings;
			temp_interfaces[j].num_altsettings = device_config_desc[i]->interface[j].num_altsetting;
			temp_interfaces[j].current_altsetting = 0;

		}
		host_device_desc.configs[i].interfaces = temp_interfaces;
	}

	host_device_desc.current_config = 0;

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

	int opt, lopt, loidx;
	const char *optstring = "hv";
	const struct option long_options[] = {
		{"help", no_argument, &lopt, 1},
		{"verbose", no_argument, &lopt, 2},
		{"device", required_argument, &lopt, 3},
		{"driver", required_argument, &lopt, 4},
		{"vendor_id", required_argument, &lopt, 5},
		{"product_id", required_argument, &lopt, 6},
		{"enable_injection", no_argument, &lopt, 7},
		{"injection_file", required_argument, &lopt, 8},
		{"enable_customized_config", no_argument, &lopt, 9},
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
		case 7:
			injection_enabled = true;
			break;
		case 8:
			injection_file = optarg;
			break;
		case 9:
			customized_config_enabled = true;
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

	if (injection_enabled) {
		printf("Injection enabled\n");
		if (injection_file.empty()) {
			printf("Injection file not specified\n");
			return 1;
		}
		struct stat buffer;
		if (stat(injection_file.c_str(), &buffer) != 0) {
			printf("Injection file %s not found\n", injection_file.c_str());
			return 1;
		}

		Json::Reader jsonReader;
		std::ifstream ifs(injection_file.c_str());
		if (jsonReader.parse(ifs, injection_config))
			printf("Parsed injection file: %s\n", injection_file.c_str());
		else {
			printf("Error parsing injection file: %s\n", injection_file.c_str());
			return 1;
		}
		ifs.close();
	}

	if (customized_config_enabled) {
		struct stat buffer;
		if (stat(customized_config_file.c_str(), &buffer) != 0) {
			printf("Customized config file %s not found\n", customized_config_file.c_str());
			return 1;
		}

		Json::Reader jsonReader;
		std::ifstream ifs(customized_config_file.c_str());
		Json::Value customized_config;
		if (jsonReader.parse(ifs, customized_config))
			printf("Parsed customized config file: %s\n", customized_config_file.c_str());
		else {
			printf("Error parsing customized config file: %s\n", customized_config_file.c_str());
			return 1;
		}
		ifs.close();

		if (customized_config["reset_device_before_proxy"] == false) {
			printf("reset_device_before_proxy set to false\n");
			reset_device_before_proxy = false;
		}
		if (customized_config["bmaxpacketsize0_must_greater_than_64"] == false) {
			printf("bmaxpacketsize0_must_greater_than_64 set to false\n");
			bmaxpacketsize0_must_greater_than_64 = false;
		}
	}

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

	close(fd);

	int bNumConfigurations = device_device_desc.bNumConfigurations;
	for (int i = 0; i < bNumConfigurations; i++) {
		int bNumInterfaces = device_config_desc[i]->bNumInterfaces;
		for (int j = 0; j < bNumInterfaces; j++) {
			int num_altsetting = device_config_desc[i]->interface[j].num_altsetting;
			for (int k = 0; k < num_altsetting; k++) {
				if (host_device_desc.configs[i].interfaces[j].altsettings[k].endpoints) {
					delete[] host_device_desc.configs[i].interfaces[j].altsettings[k].endpoints;
				}
			}
			delete[] host_device_desc.configs[i].interfaces[j].altsettings;
		}
		delete[] host_device_desc.configs[i].interfaces;
	}
	delete[] host_device_desc.configs;
	delete[] device_config_desc;

	if (context && callback_handle != -1) {
		libusb_hotplug_deregister_callback(context, callback_handle);
	}
	if (hotplug_monitor_thread &&
		pthread_join(hotplug_monitor_thread, NULL)) {
		fprintf(stderr, "Error join hotplug_monitor_thread\n");
	}

	return 0;
}
