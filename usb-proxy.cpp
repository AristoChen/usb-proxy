#include <atomic>
#include <unordered_map>
#include <vector>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "proxy.h"
#include "misc.h"

int verbose_level = 0;
bool please_stop_ep0 = false;
std::atomic<bool> please_stop_eps(false);

bool injection_enabled = false;
std::string injection_file = "injection.json";
Json::Value injection_config;

bool customized_config_enabled = false;
std::string customized_config_file = "config.json";
bool reset_device_before_proxy = true;
bool bmaxpacketsize0_must_greater_than_64 = true;
bool auto_remap_endpoints = false;
int iso_batch_size = ISO_BATCH_SIZE_DEFAULT;
enum usb_device_speed device_speed = USB_SPEED_HIGH;

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
	printf("\t--enable_customized_config: enable the customized config feature\n");
	printf("\t--auto_remap_endpoints: enable endpoint remapping when UDC can't use descriptors directly\n");
	printf("\t--iso_batch_size N: number of isochronous packets per transfer (1-%d, default %d)\n\n",
		ISO_BATCH_SIZE_MAX, ISO_BATCH_SIZE_DEFAULT);
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

// Wrapper for UDC endpoint info, allowing future extension with additional state.
struct EndpointCandidate {
	struct usb_raw_ep_info info;
};

static bool candidate_supports_endpoint(const EndpointCandidate &candidate,
					const struct usb_endpoint_descriptor &endpoint)
{
	bool dir_in = usb_endpoint_dir_in(&endpoint);
	int type = usb_endpoint_type(&endpoint);

	if (dir_in && !candidate.info.caps.dir_in)
		return false;
	if (!dir_in && !candidate.info.caps.dir_out)
		return false;

	switch (type) {
	case USB_ENDPOINT_XFER_ISOC:
		if (!candidate.info.caps.type_iso)
			return false;
		break;
	case USB_ENDPOINT_XFER_BULK:
		if (!candidate.info.caps.type_bulk)
			return false;
		break;
	case USB_ENDPOINT_XFER_INT:
		if (!candidate.info.caps.type_int)
			return false;
		break;
	default:
		return false;
	}

	uint16_t max_packet = usb_endpoint_maxp(&endpoint);
	if (candidate.info.limits.maxpacket_limit &&
	    max_packet > candidate.info.limits.maxpacket_limit)
		return false;

	return true;
}

static uint8_t compute_host_endpoint_address(const EndpointCandidate &candidate,
					     uint8_t device_address,
					     bool dir_in)
{
	if (candidate.info.addr == USB_RAW_EP_ADDR_ANY)
		return device_address;

	uint8_t host_address = static_cast<uint8_t>(candidate.info.addr);
	if (dir_in)
		host_address |= USB_DIR_IN;
	else
		host_address &= ~USB_DIR_IN;

	return host_address;
}

static int find_candidate_index(const std::vector<EndpointCandidate> &candidates,
				std::vector<bool> &candidate_used,
				const struct usb_endpoint_descriptor &endpoint)
{
	for (size_t i = 0; i < candidates.size(); i++) {
		if (candidate_used[i])
			continue;
		if (!candidate_supports_endpoint(candidates[i], endpoint))
			continue;
		return i;
	}
	return -1;
}

static int remap_config_endpoints(struct raw_gadget_config *config,
				  const std::vector<EndpointCandidate> &candidates)
{
	std::vector<bool> candidate_used(candidates.size(), false);
	std::unordered_map<uint8_t, size_t> device_to_candidate;

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			uint8_t iface_num = alt->interface.bInterfaceNumber;
			uint8_t alt_setting = alt->interface.bAlternateSetting;
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				uint8_t device_address = ep->device_bEndpointAddress;
				auto existing = device_to_candidate.find(device_address);

				size_t candidate_index;
				if (existing != device_to_candidate.end()) {
					candidate_index = existing->second;
				}
				else {
					int idx = find_candidate_index(candidates,
								       candidate_used,
								       ep->endpoint);
					if (idx < 0) {
						printf("Failed to remap endpoint 0x%02x "
						       "(interface %u, alt %u)\n",
						       device_address, iface_num, alt_setting);
						return -1;
					}
					candidate_index = (size_t)idx;
					device_to_candidate[device_address] = candidate_index;
					candidate_used[candidate_index] = true;
				}

				bool dir_in = usb_endpoint_dir_in(&ep->endpoint);
				uint8_t host_address = compute_host_endpoint_address(
					candidates[candidate_index], device_address, dir_in);

				if (host_address != ep->endpoint.bEndpointAddress) {
					printf("Remapping endpoint 0x%02x -> 0x%02x "
					       "(interface %u, alt %u)\n",
						device_address, host_address, iface_num, alt_setting);
				}

				ep->endpoint.bEndpointAddress = host_address;
				ep->udc_maxpacket_limit = candidates[candidate_index].info.limits.maxpacket_limit;

				// Clamp isochronous max packet size to UDC limit; UDC can't do high bandwidth.
				if (usb_endpoint_type(&ep->endpoint) == USB_ENDPOINT_XFER_ISOC &&
				    ep->udc_maxpacket_limit) {
					uint16_t maxp = usb_endpoint_maxp(&ep->endpoint);
					uint16_t base = maxp & 0x7ff;
					if (base > ep->udc_maxpacket_limit ||
					    (ep->endpoint.wMaxPacketSize & 0x1800)) {
						ep->endpoint.wMaxPacketSize = ep->udc_maxpacket_limit;
					}
				}
			}
		}
	}

	return 0;
}

static int remap_host_endpoints_if_needed(int fd)
{
	if (!auto_remap_endpoints)
		return 0;

	struct usb_raw_eps_info eps_info;
	memset(&eps_info, 0, sizeof(eps_info));

	int num = usb_raw_eps_info(fd, &eps_info);
	if (num <= 0) {
		printf("Failed to fetch endpoint info for remapping\n");
		return -1;
	}

	std::vector<EndpointCandidate> candidates;
	for (int i = 0; i < num; i++) {
		EndpointCandidate candidate;
		candidate.info = eps_info.eps[i];
		candidates.push_back(candidate);
	}

	for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
		if (remap_config_endpoints(&host_device_desc.configs[i], candidates) < 0)
			return -1;
	}

	return 0;
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
					// When a full-speed device is proxied through a high-speed
					// gadget, convert isochronous bInterval from FS (ms) to HS
					// (125µs microframes): HS interval = 2^(bInterval-1) * 125µs.
					// FS bInterval=1 (1ms) → HS bInterval=4 (1ms).
					if (device_speed == USB_SPEED_FULL &&
					    (temp_endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					     == USB_ENDPOINT_XFER_ISOC) {
						uint8_t fs_interval = temp_endpoint.bInterval;
						// Convert ms to nearest 125µs exponent:
						// fs_interval ms = fs_interval * 8 microframes
						// 2^(n-1) = fs_interval * 8 → n = log2(fs_interval*8) + 1
						// For bInterval=1: n = log2(8)+1 = 4
						uint8_t hs_interval = 4;
						if (fs_interval > 1) {
							int val = fs_interval * 8;
							hs_interval = 1;
							while (val > 1) {
								val >>= 1;
								hs_interval++;
							}
						}
						if (hs_interval > 16)
							hs_interval = 16;
						printf("Converting ISO bInterval %d (FS ms) -> %d (HS 125us)\n",
							fs_interval, hs_interval);
						temp_endpoint.bInterval = hs_interval;
					}

					temp_endpoints[l].endpoint = temp_endpoint;
					temp_endpoints[l].device_bEndpointAddress = temp_endpoint.bEndpointAddress;
					temp_endpoints[l].udc_maxpacket_limit = 0;
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
		{"auto_remap_endpoints", no_argument, &lopt, 10},
		{"iso_batch_size", required_argument, &lopt, 11},
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
		case 10:
			auto_remap_endpoints = true;
			printf("Automatic endpoint remapping enabled\n");
			break;
		case 11:
			iso_batch_size = std::stoi(optarg);
			if (iso_batch_size < 1)
				iso_batch_size = 1;
			if (iso_batch_size > ISO_BATCH_SIZE_MAX)
				iso_batch_size = ISO_BATCH_SIZE_MAX;
			printf("Isochronous batch size set to %d\n", iso_batch_size);
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

	// Detect physical device speed.
	int libusb_speed = libusb_get_device_speed(libusb_get_device(dev_handle));
	switch (libusb_speed) {
	case LIBUSB_SPEED_LOW:
		device_speed = USB_SPEED_LOW;
		printf("Device speed: Low Speed (1.5Mbps)\n");
		break;
	case LIBUSB_SPEED_FULL:
		device_speed = USB_SPEED_FULL;
		printf("Device speed: Full Speed (12Mbps)\n");
		break;
	case LIBUSB_SPEED_HIGH:
		device_speed = USB_SPEED_HIGH;
		printf("Device speed: High Speed (480Mbps)\n");
		break;
	case LIBUSB_SPEED_SUPER:
	case LIBUSB_SPEED_SUPER_PLUS:
		device_speed = USB_SPEED_SUPER;
		printf("Device speed: SuperSpeed (5Gbps+)\n");
		break;
	default:
		device_speed = USB_SPEED_HIGH;
		printf("Device speed: Unknown, defaulting to High Speed\n");
		break;
	}

	setup_host_usb_desc();
	printf("Setup USB config successfully\n");

	int fd = usb_raw_open();
	// Always use USB_SPEED_HIGH for the gadget; some UDCs (e.g., musb-hdrc)
	// reject lower speeds. We compensate by adjusting bInterval below.
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	if (remap_host_endpoints_if_needed(fd) < 0) {
		close(fd);
		return 1;
	}

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
