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

		__u32 pos = data.find(pattern);
		while (pos != std::string::npos) {
			if (data.length() - pattern.length() + replacement.length() > 1023)
				break;

			data = data.replace(pos, pattern.length(), replacement);
			printf("Modified from %s to %s at Index %d\n", pattern_hex.c_str(), replacement_hex.c_str(), pos);
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
		printf(" %02x", (unsigned)io.data[i]);
	}
	printf("\n");
}

void *ep_loop_write(void *arg) {
	struct thread_info ep_thread_info = *((struct thread_info*) arg);
	int fd = ep_thread_info.fd;
	int ep_num = ep_thread_info.ep_num;
	struct usb_endpoint_descriptor ep = ep_thread_info.endpoint;
	std::string transfer_type = ep_thread_info.transfer_type;
	std::string dir = ep_thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = ep_thread_info.data_queue;
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
		struct usb_raw_transfer_io io = data_queue->front();
		data_queue->pop_front();
		data_mutex->unlock();

		if (verbose_level >= 2)
			printData(io, ep.bEndpointAddress, transfer_type, dir);

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv >= 0) {
				printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
			}
		}
		else {
			int length = io.inner.length;
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
	std::deque<usb_raw_transfer_io> *data_queue = ep_thread_info.data_queue;
	std::mutex *data_mutex = ep_thread_info.data_mutex;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

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

			receive_data(ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize, &data, &nbytes, 0);

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
			if (rv >= 0) {
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
	}

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void process_eps(int fd) {
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
		ep_thread_list[i].ep_thread_info.data_queue = new std::deque<usb_raw_transfer_io>;
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

void terminate_eps(int fd, int interface, int altsetting) {
	int thread_num = host_config_desc[desired_configuration]
			.interfaces[interface]
			.altsetting[altsetting]
			.interface
			.bNumEndpoints;

	please_stop_eps = true;

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

		delete ep_thread_list[i].ep_thread_info.data_queue;
		delete ep_thread_list[i].ep_thread_info.data_mutex;
	}
	delete[] ep_thread_list;

	please_stop_eps = false;
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;
	int previous_bConfigurationValue = -1;
	int previous_interface = 0;
	int previous_interface_altsetting = 0;

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
			result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
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

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "in");

				rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
				printf("ep0: transferred %d bytes (in)\n", rv);
			}
			else {
				usb_raw_ep0_stall(fd);
			}
		}
		else {
			rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);

			if (event.ctrl.bRequestType == 0x00 && event.ctrl.bRequest == 0x09) { // Set configuration
				if (previous_bConfigurationValue == event.ctrl.wValue) {
					printf("Skip changing configuration, wValue is same as previous\n");
					continue;
				}

				if (event.ctrl.wValue > host_device_desc.bNumConfigurations) {
					printf("[Warning] Skip changing configuration, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				if (set_configuration_done_once) { // Need to stop all threads for eps and cleanup
					printf("Changing configuration\n");

					desired_interface = 0;
					desired_interface_altsetting = 0;
					release_interface(desired_interface);

					terminate_eps(fd, previous_interface, previous_interface_altsetting);

					set_configuration(event.ctrl.wValue);
				}

				for (int i = 0; i < host_device_desc.bNumConfigurations; i++) {
					if (host_config_desc[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_configuration = i;
					}
				}
				claim_interface(desired_interface);
				process_eps(fd);

				set_configuration_done_once = true;
				previous_bConfigurationValue = event.ctrl.wValue;
				previous_interface = desired_interface;
				previous_interface_altsetting = desired_interface_altsetting;
			}
			else if (event.ctrl.bRequestType == 0x01 && event.ctrl.bRequest == 0x0b) { // Set interface/alt_setting
				bool process_eps_required = false;

				if (event.ctrl.wIndex >= host_config_desc[desired_configuration].config.bNumInterfaces) {
					printf("[Warning] Skip changing interface, wIndex(%d) is invalid\n", event.ctrl.wIndex);
					continue;
				}
				if (event.ctrl.wValue >= host_config_desc[desired_configuration]
							.interfaces[event.ctrl.wIndex].num_altsetting) {
					printf("[Warning] Skip changing alt_setting, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

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

				if (process_eps_required) { // Need to stop all threads for eps and cleanup
					printf("Changing interface/altsetting\n");

					terminate_eps(fd, previous_interface, previous_interface_altsetting);

					if (previous_interface != desired_interface)
						claim_interface(desired_interface);
					process_eps(fd);
				}

				previous_interface = desired_interface;
				previous_interface_altsetting = desired_interface_altsetting;
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

				memcpy(control_data, io.data, event.ctrl.wLength);

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "out");

				result = control_request(&event.ctrl, &nbytes, &control_data, 1000);
				if (result == 0) {
					printf("ep0: transferred %d bytes (out)\n", rv);
				}
				else {
					usb_raw_ep0_stall(fd);
				}
			}
		}

		delete[] control_data;
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}
