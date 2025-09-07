#include <vector>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "misc.h"

void injection(struct usb_raw_transfer_io &io, Json::Value patterns, std::string replacement_hex, bool &data_modified) {
	std::string data(io.data, io.inner.length);
	std::string replacement = hexToAscii(replacement_hex);
	for (unsigned int j = 0; j < patterns.size(); j++) {
		std::string pattern_hex = patterns[j].asString();
		std::string pattern = hexToAscii(pattern_hex);

		std::string::size_type pos = data.find(pattern);
		while (pos != std::string::npos) {
			if (data.length() - pattern.length() + replacement.length() > 1023)
				break;

			data = data.replace(pos, pattern.length(), replacement);
			printf("Modified from %s to %s at Index %ld\n", pattern_hex.c_str(), replacement_hex.c_str(), pos);
			data_modified = true;

			pos = data.find(pattern);
		}
	}

	if (data_modified) {
		io.inner.length = data.length();
		for (size_t j = 0; j < data.length(); j++) {
			io.data[j] = data[j];
		}
	}
}

void injection(struct usb_raw_control_event &event, struct usb_raw_transfer_io &io, int &injection_flags) {
	// This is just a simple injection function for control transfer.
	std::vector<std::string> injection_type{"modify", "ignore", "stall"};
	std::string transfer_type = "control";

	for (unsigned int i = 0; i < injection_type.size(); i++) {
		for (unsigned int j = 0; j < injection_config[transfer_type][injection_type[i]].size(); j++) {
			Json::Value rule = injection_config[transfer_type][injection_type[i]][j];
			if (rule["enable"].asBool() != true)
				continue;

			if (event.ctrl.bRequestType != hexToDecimal(rule["bRequestType"].asInt()) ||
			    event.ctrl.bRequest     != hexToDecimal(rule["bRequest"].asInt()) ||
			    event.ctrl.wValue       != hexToDecimal(rule["wValue"].asInt()) ||
			    event.ctrl.wIndex       != hexToDecimal(rule["wIndex"].asInt()) ||
			    event.ctrl.wLength      != hexToDecimal(rule["wLength"].asInt()))
				continue;

			printf("Matched injection rule: %s, index: %d\n", injection_type[i].c_str(), j);
			if (injection_type[i] == "modify") {
				Json::Value patterns = rule["content_pattern"];
				std::string replacement_hex = rule["replacement"].asString();
				bool data_modified = false;

				injection(io, patterns, replacement_hex, data_modified);
				if (!(event.ctrl.bRequestType & USB_DIR_IN))
					event.ctrl.wLength = io.inner.length;
			}
			else if (injection_type[i] == "ignore") {
				printf("Ignore this control transfer\n");
				injection_flags = USB_INJECTION_FLAG_IGNORE;
			}
			else if (injection_type[i] == "stall") {
				injection_flags = USB_INJECTION_FLAG_STALL;
			}
		}
	}
}

void injection(struct usb_raw_transfer_io &io, struct usb_endpoint_descriptor ep, std::string transfer_type) {
	// This is just a simple injection function for int and bulk transfer.
	for (unsigned int i = 0; i < injection_config[transfer_type].size(); i++) {
		Json::Value rule = injection_config[transfer_type][i];
		if (rule["enable"].asBool() != true ||
		    hexToDecimal(rule["ep_address"].asInt()) != ep.bEndpointAddress)
			continue;

		Json::Value patterns = rule["content_pattern"];
		std::string replacement_hex = rule["replacement"].asString();
		bool data_modified = false;

		injection(io, patterns, replacement_hex, data_modified);

		if (data_modified)
			break;
	}
}

void printData(struct usb_raw_transfer_io io, __u8 bEndpointAddress, std::string transfer_type, std::string dir) {
	printf("Sending data to EP%x(%s_%s):", bEndpointAddress,
		transfer_type.c_str(), dir.c_str());
	for (unsigned int i = 0; i < io.inner.length; i++) {
		printf(" %02hhx", (unsigned)io.data[i]);
	}
	printf("\n");
}

void noop_signal_handler(int) { }

void *ep_loop_write(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;

	printf("Start writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	while (!please_stop_eps) {
		assert(ep_num != -1);
		if (data_queue->size() == 0) {
			usleep(100);
			continue;
		}

		data_mutex->lock();
		struct usb_raw_transfer_io io = data_queue->front();
		data_queue->pop_front();
		data_mutex->unlock();

		if (verbose_level >= 2)
			printData(io, ep.bEndpointAddress, transfer_type, dir);

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && (errno == EXDEV || errno == ENODATA)) {
				printf("EP%x(%s_%s): missed isochronous timing, ignoring transfer\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				continue;
			}
			if (rv < 0) {
				perror("usb_raw_ep_write()");
				exit(EXIT_FAILURE);
			}
			printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
				transfer_type.c_str(), dir.c_str(), rv);
		}
		else {
			int length = io.inner.length;
			unsigned char *data = new unsigned char[length];
			memcpy(data, io.data, length);
			int rv = send_data(ep.bEndpointAddress, ep.bmAttributes, data, length, USB_REQUEST_TIMEOUT);
			if (rv == LIBUSB_ERROR_NO_DEVICE) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}

			if (data)
				delete[] data;
		}
	}

	printf("End writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void *ep_loop_read(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	while (!please_stop_eps) {
		assert(ep_num != -1);
		struct usb_raw_transfer_io io;

		if (ep.bEndpointAddress & USB_DIR_IN) {
			unsigned char *data = NULL;
			int nbytes = -1;

			if (data_queue->size() >= 32) {
				usleep(200);
				continue;
			}

			int rv = receive_data(ep.bEndpointAddress, ep.bmAttributes, usb_endpoint_maxp(&ep),
						&data, &nbytes, USB_REQUEST_TIMEOUT);
			if (rv == LIBUSB_ERROR_NO_DEVICE) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}

			if (nbytes >= 0) {
				memcpy(io.data, data, nbytes);
				io.inner.ep = ep_num;
				io.inner.flags = 0;
				io.inner.length = nbytes;

				if (injection_enabled)
					injection(io, ep, transfer_type);

				data_mutex->lock();
				data_queue->push_back(io);
				data_mutex->unlock();
				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
							transfer_type.c_str(), dir.c_str(), nbytes);
			}

			if (data)
				delete[] data;
		}
		else {
			io.inner.ep = ep_num;
			io.inner.flags = 0;
			io.inner.length = sizeof(io.data);

			int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0) {
				perror("usb_raw_ep_read()");
				exit(EXIT_FAILURE);
			}
			printf("EP%x(%s_%s): read %d bytes from host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
			io.inner.length = rv;

			if (injection_enabled)
				injection(io, ep, transfer_type);

			data_mutex->lock();
			data_queue->push_back(io);
			data_mutex->unlock();
			if (verbose_level)
				printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
		}
	}

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void process_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	printf("Activating %d endpoints on interface %d\n", (int)alt->interface.bNumEndpoints, interface);

	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		int addr = usb_endpoint_num(&ep->endpoint);
		assert(addr != 0);

		ep->thread_info.fd = fd;
		ep->thread_info.endpoint = ep->endpoint;
		ep->thread_info.data_queue = new std::deque<usb_raw_transfer_io>;
		ep->thread_info.data_mutex = new std::mutex;

		switch (usb_endpoint_type(&ep->endpoint)) {
		case USB_ENDPOINT_XFER_ISOC:
			ep->thread_info.transfer_type = "isoc";
			break;
		case USB_ENDPOINT_XFER_BULK:
			ep->thread_info.transfer_type = "bulk";
			break;
		case USB_ENDPOINT_XFER_INT:
			ep->thread_info.transfer_type = "int";
			break;
		default:
			printf("transfer_type %d is invalid\n", usb_endpoint_type(&ep->endpoint));
			assert(false);
		}

		if (usb_endpoint_dir_in(&ep->endpoint))
			ep->thread_info.dir = "in";
		else
			ep->thread_info.dir = "out";

		ep->thread_info.ep_num = usb_raw_ep_enable(fd, &ep->thread_info.endpoint);
		printf("%s_%s: addr = %u, ep = #%d\n",
			ep->thread_info.transfer_type.c_str(),
			ep->thread_info.dir.c_str(),
			addr, ep->thread_info.ep_num);

		if (verbose_level)
			printf("Creating thread for EP%02x\n",
				ep->thread_info.endpoint.bEndpointAddress);
		pthread_create(&ep->thread_read, 0,
			ep_loop_read, (void *)&ep->thread_info);
		pthread_create(&ep->thread_write, 0,
			ep_loop_write, (void *)&ep->thread_info);
	}

	printf("process_eps done\n");
}

void terminate_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	please_stop_eps = true;

	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		// Endpoint threads might be blocked either on a Raw Gadget
		// ioctl or on a libusb transfer handling. To interrupt the
		// former, we send the SIGUSR1 signal to the threads. The
		// threads have a no-op handler set for this signal, so the
		// ioctl gets interrupted with no other side-effects.
		// The libusb transfer handling does get interrupted directly
		// and instead times out.
		pthread_kill(ep->thread_read, SIGUSR1);
		pthread_kill(ep->thread_write, SIGUSR1);

		if (ep->thread_read && pthread_join(ep->thread_read, NULL)) {
			fprintf(stderr, "Error join thread_read\n");
		}
		if (ep->thread_write && pthread_join(ep->thread_write, NULL)) {
			fprintf(stderr, "Error join thread_write\n");
		}
		ep->thread_read = 0;
		ep->thread_write = 0;

		usb_raw_ep_disable(fd, ep->thread_info.ep_num);
		ep->thread_info.ep_num = -1;

		delete ep->thread_info.data_queue;
		delete ep->thread_info.data_mutex;
	}

	please_stop_eps = false;
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;

	printf("Start for EP0, thread id(%d)\n", gettid());

	if (verbose_level)
		print_eps_info(fd);

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

		// Normally, we would only need to check for USB_RAW_EVENT_RESET to handle a reset event.
		// However, dwc2 is buggy and it reports a disconnect event instead of a reset.
		if (event.inner.type == USB_RAW_EVENT_RESET || event.inner.type == USB_RAW_EVENT_DISCONNECT) {
			printf("Resetting device\n");
			// Normally, we would need to stop endpoint threads first and only then
			// reset the device. However, libusb does not allow interrupting queued
			// requests submitted via sync I/O. Thus, we reset the proxied device to
			// force libusb to interrupt the requests and allow the endpoint threads
			// to exit on please_stop_eps checks.
			if (set_configuration_done_once)
				please_stop_eps = true;
			reset_device();
			if (set_configuration_done_once) {
				struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
				printf("Stopping endpoint threads\n");
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					int interface_num = iface->altsettings[iface->current_altsetting]
						.interface.bInterfaceNumber;
					terminate_eps(fd, host_device_desc.current_config, i,
							iface->current_altsetting);
					release_interface(interface_num);
					iface->current_altsetting = 0;
				}
				printf("Endpoint threads stopped\n");
				host_device_desc.current_config = 0;
				set_configuration_done_once = false;
			}
			continue;
		}

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_transfer_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = event.ctrl.wLength;

		int injection_flags = USB_INJECTION_FLAG_NONE;
		int nbytes = 0;
		int result = 0;
		unsigned char *control_data = new unsigned char[event.ctrl.wLength];

		int rv = -1;
		if (event.ctrl.bRequestType & USB_DIR_IN) {
			result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
			if (result == 0) {
				memcpy(&io.data[0], control_data, nbytes);
				io.inner.length = nbytes;

				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				// Some UDCs require bMaxPacketSize0 to be at least 64.
				// Ideally, the information about UDC limitations needs to be
				// exposed by Raw Gadget, but this is not implemented at the moment;
				// see https://github.com/xairy/raw-gadget/issues/41.
				if (bmaxpacketsize0_must_greater_than_64 &&
				    (event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
				    event.ctrl.bRequest == USB_REQ_GET_DESCRIPTOR &&
				    (event.ctrl.wValue >> 8) == USB_DT_DEVICE) {
					struct usb_device_descriptor *dev = (struct usb_device_descriptor *)&io.data;
					if (dev->bMaxPacketSize0 < 64)
						dev->bMaxPacketSize0 = 64;
				}

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "in");

				rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: transferred %d bytes (in)\n", rv);
			}
			else {
				usb_raw_ep0_stall(fd);
				continue;
			}
		}
		else {
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
				int desired_config = -1;
				for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
					if (host_device_desc.configs[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_config = i;
						break;
					}
				}
				if (desired_config < 0) {
					printf("[Warning] Skip changing configuration, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				struct raw_gadget_config *config = &host_device_desc.configs[desired_config];

				if (set_configuration_done_once) { // Need to stop all threads for eps and cleanup
					printf("Changing configuration\n");
					for (int i = 0; i < config->config.bNumInterfaces; i++) {
						struct raw_gadget_interface *iface = &config->interfaces[i];
						int interface_num = iface->altsettings[iface->current_altsetting]
							.interface.bInterfaceNumber;
						terminate_eps(fd, host_device_desc.current_config, i,
								iface->current_altsetting);
						release_interface(interface_num);
					}
				}

				usb_raw_configure(fd);
				set_configuration(config->config.bConfigurationValue);
				host_device_desc.current_config = desired_config;

				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					iface->current_altsetting = 0;
					int interface_num = iface->altsettings[0].interface.bInterfaceNumber;
					claim_interface(interface_num);
					process_eps(fd, desired_config, i, 0);
					usleep(10000); // Give threads time to spawn.
				}

				set_configuration_done_once = true;

				// Ack request after spawning endpoint threads.
				rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: request acked\n");
			}
			else if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_INTERFACE) {
				struct raw_gadget_config *config =
					&host_device_desc.configs[host_device_desc.current_config];

				int desired_interface = -1;
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					if (config->interfaces[i].altsettings[0].interface.bInterfaceNumber ==
							event.ctrl.wIndex) {
						desired_interface = i;
						break;
					}
				}
				if (desired_interface < 0) {
					printf("[Warning] Skip changing interface, wIndex(%d) is invalid\n", event.ctrl.wIndex);
					continue;
				}

				struct raw_gadget_interface *iface = &config->interfaces[desired_interface];

				int desired_altsetting = -1;
				for (int i = 0; i < iface->num_altsettings; i++) {
					if (iface->altsettings[i].interface.bAlternateSetting == event.ctrl.wValue) {
						desired_altsetting = i;
						break;
					}
				}
				if (desired_altsetting < 0) {
					printf("[Warning] Skip changing alt_setting, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				struct raw_gadget_altsetting *alt = &iface->altsettings[desired_altsetting];

				if (desired_altsetting == iface->current_altsetting) {
					printf("Interface/altsetting already set\n");
					// But lets propagate the request to the device.
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
				}
				else {
					printf("Changing interface/altsetting\n");
					terminate_eps(fd, host_device_desc.current_config,
						desired_interface, iface->current_altsetting);
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
					process_eps(fd, host_device_desc.current_config,
						desired_interface, desired_altsetting);
					iface->current_altsetting = desired_altsetting;
					usleep(10000); // Give threads time to spawn.
				}

				// Ack request after spawning endpoint threads.
				rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: request acked\n");
			}
			else {
				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				if (event.ctrl.wLength == 0) {
					// For 0-length request, we can ack or stall the request via
					// Raw Gadget, depending on what the proxied device does.

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "out");

					result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
					if (result == 0) {
						// Ack the request.
						rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
						if (rv < 0)
							printf("ep0: ack failed: %d\n", rv);
						else
							printf("ep0: request acked\n");
					}
					else {
						// Stall the request.
						usb_raw_ep0_stall(fd);
						continue;
					}
				}
				else {
					// For non-0-length requests, we cannot retrieve the request data
					// without acking the request due to the Gadget subsystem limitations.
					// Thus, we cannot stall such request for the host even if the proxied
					// device stalls. This is not ideal but seems to work fine in practice.

					// Retrieve data for sending request to proxied device
					// (and ack the request).
					rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
					if (rv < 0) {
						printf("ep0: ack failed: %d\n", rv);
						continue;
					}

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "out");

					memcpy(control_data, io.data, event.ctrl.wLength);

					result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
					if (result == 0) {
						printf("ep0: transferred %d bytes (out)\n", rv);
					}
				}
			}
		}

		delete[] control_data;
	}

	struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		int interface_num = iface->altsettings[iface->current_altsetting]
			.interface.bInterfaceNumber;
		terminate_eps(fd, host_device_desc.current_config, i,
				iface->current_altsetting);
		release_interface(interface_num);
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}
