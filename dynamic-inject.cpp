#include "dynamic-inject.h"
#include "host-raw-gadget.h"
#include "misc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

DynamicInjector::DynamicInjector(const std::string &socket_path)
	: socket_path_(socket_path) {}

DynamicInjector::~DynamicInjector()
{
	stop();
}

bool DynamicInjector::start()
{
	unlink(socket_path_.c_str());

	sockfd_ = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd_ < 0) {
		fprintf(stderr, "DynamicInjector: socket() failed: %s\n", strerror(errno));
		return false;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

	auto fail = [&](const char *msg) {
		fprintf(stderr, "DynamicInjector: %s: %s\n", msg, strerror(errno));
		close(sockfd_);
		sockfd_ = -1;
	};

	if (bind(sockfd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fail("bind() failed");
		return false;
	}

	if (listen(sockfd_, 5) < 0) {
		fail("listen() failed");
		return false;
	}

	running_ = true;
	thread_ = std::thread(&DynamicInjector::server_loop, this);
	printf("DynamicInjector: listening on %s\n", socket_path_.c_str());
	return true;
}

void DynamicInjector::stop()
{
	running_ = false;
	if (sockfd_ >= 0) {
		close(sockfd_);
		sockfd_ = -1;
	}
}

void DynamicInjector::join()
{
	if (thread_.joinable())
		thread_.join();
}

void DynamicInjector::server_loop()
{
	while (running_) {
		int client_fd = accept(sockfd_, nullptr, nullptr);
		if (client_fd < 0) {
			if (!running_)
				break;
			continue;
		}

		// Wake handle_client periodically so it can check running_ on shutdown
		struct timeval tv{1, 0};
		setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		handle_client(client_fd);
		close(client_fd);
	}

	unlink(socket_path_.c_str());
}

void DynamicInjector::handle_client(int client_fd)
{
	char buf[256];
	std::string pending;

	while (running_) {
		int n = recv(client_fd, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;  // SO_RCVTIMEO fired; loop to check running_
			break;
		}
		if (n == 0)
			break;

		pending.append(buf, n);
		if (pending.size() > MAX_TRANSFER_SIZE * 3) {
			dprintf(client_fd, "ERR: line too long\n");
			break;
		}

		size_t pos;
		while ((pos = pending.find('\n')) != std::string::npos) {
			std::string line = pending.substr(0, pos);
			pending.erase(0, pos + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (!line.empty())
				handle_command(client_fd, line);
		}
	}
}

bool DynamicInjector::handle_command(int client_fd, const std::string &line)
{
	if (line.compare(0, 6, "INJECT") != 0) {
		dprintf(client_fd, "ERR: unknown command\n");
		return false;
	}

	size_t ep_start = line.find_first_not_of(' ', 6);
	if (ep_start == std::string::npos) {
		dprintf(client_fd, "ERR: missing endpoint\n");
		return false;
	}

	size_t ep_end = line.find_first_of(' ', ep_start);
	std::string ep_str = line.substr(ep_start,
		ep_end == std::string::npos ? std::string::npos : ep_end - ep_start);

	uint8_t ep_addr;
	try {
		ep_addr = (uint8_t)std::stoul(ep_str, nullptr, 16);
	} catch (...) {
		dprintf(client_fd, "ERR: bad endpoint address\n");
		return false;
	}

	std::string hex_payload;
	if (ep_end != std::string::npos) {
		size_t payload_start = line.find_first_not_of(' ', ep_end);
		if (payload_start != std::string::npos)
			hex_payload = line.substr(payload_start);
	}

	if (hex_payload.empty()) {
		dprintf(client_fd, "ERR: missing payload\n");
		return false;
	}

	std::vector<uint8_t> data = parseHexBytes(hex_payload);
	if (data.empty()) {
		dprintf(client_fd, "ERR: bad hex\n");
		return false;
	}

	if (data.size() > MAX_TRANSFER_SIZE) {
		dprintf(client_fd, "ERR: payload too large\n");
		return false;
	}

	// Find the endpoint and inject under the lifecycle mutex so we can't
	// race with terminate_eps phase 3 freeing data_queue/data_mutex/data_cond.
	std::lock_guard<std::mutex> lifecycle_guard(g_endpoint_lifecycle_mutex);

	struct raw_gadget_config *cfg =
		&host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < cfg->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &cfg->interfaces[i];
		struct raw_gadget_altsetting *alt =
			&iface->altsettings[iface->current_altsetting];

		for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
			struct raw_gadget_endpoint *ep = &alt->endpoints[j];

			if (ep->device_bEndpointAddress != ep_addr)
				continue;

			if (!ep->thread_info.data_queue) {
				dprintf(client_fd, "ERR: endpoint not ready\n");
				return false;
			}

			{
				std::lock_guard<std::mutex> qlock(*ep->thread_info.data_mutex);
				if ((int)ep->thread_info.data_queue->size() >= 32) {
					dprintf(client_fd, "ERR: queue full\n");
					return false;
				}

				struct usb_raw_transfer_io io;
				io.inner.ep    = ep->thread_info.ep_num;
				io.inner.flags = 0;
				io.inner.length = data.size();
				memcpy(io.data, data.data(), data.size());
				ep->thread_info.data_queue->push_back(io);
			}
			ep->thread_info.data_cond->notify_one();

			if (verbose_level >= 1) {
				printf("DynamicInjector: EP 0x%02x injected %zu bytes\n",
					ep_addr, data.size());
			}

			dprintf(client_fd, "OK\n");
			return true;
		}
	}

	dprintf(client_fd, "ERR: endpoint 0x%02x not found\n", ep_addr);
	return false;
}
