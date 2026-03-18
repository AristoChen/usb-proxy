#pragma once
#include <string>
#include <thread>
#include <atomic>

// DynamicInjector: runtime USB packet injection via Unix domain socket.
//
// Listens on a SOCK_STREAM Unix socket. Each connected client may send one
// or more newline-terminated commands:
//
//   INJECT <ep_addr_hex> <hex_bytes>
//
// ep_addr_hex matches device_bEndpointAddress (same convention as injection.json).
// hex_bytes may be continuous ("000a0500") or space-separated ("00 0a 05 00").
//
// Each command receives a plain-text response:
//   OK
//   ERR: <reason>
//
// Shell usage:
//   echo "INJECT 81 00 0a 05 00" | nc -U /tmp/usb-proxy.sock
//
// The injector is optional; the proxy runs normally when it is not started.

class DynamicInjector {
public:
	explicit DynamicInjector(const std::string &socket_path);
	~DynamicInjector();

	// Returns false if the socket could not be created/bound.
	bool start();
	void stop();
	void join();

private:
	std::string socket_path_;
	int sockfd_ = -1;
	std::thread thread_;
	std::atomic<bool> running_{false};

	void server_loop();
	void handle_client(int client_fd);
	bool handle_command(int client_fd, const std::string &line);
};
