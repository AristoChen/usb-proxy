#include "udp_server.h"
#include "host-raw-gadget.h"
#include "proxy.h"
#include "misc.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <iomanip>

// Global variable to track real mouse button state from physical mouse
std::atomic<uint8_t> g_real_mouse_button_state(0x00);

// Global mouse report format (learned from real packets)
MouseReportFormat g_mouse_format = {
    .report_size = 0,
    .button_offset = 0,
    .x_offset = 0,
    .x_size = 0,
    .y_offset = 0,
    .y_size = 0,
    .scroll_offset = 0,
    .has_report_id = false,
    .report_id = 0,
    .is_learned = false
};

// Function to update real mouse state (called from proxy.cpp)
void update_real_mouse_state(uint8_t button_state) {
    g_real_mouse_button_state.store(button_state);
}

// Function to learn mouse format from real packet
void learn_mouse_format(const uint8_t* data, size_t length) {
    if (g_mouse_format.is_learned || length < 3) {
        return; // Already learned or packet too small
    }
    
    // Heuristic: detect common mouse report formats
    // Most mice use: [Report ID] [Buttons] [X] [Y] [Wheel] [...]
    
    g_mouse_format.report_size = length;
    
    // Check if first byte looks like a constant report ID (e.g., 0x01, 0x02, 0x03)
    if (data[0] <= 0x0F && data[0] != 0x00) {
        g_mouse_format.has_report_id = true;
        g_mouse_format.report_id = data[0];
        g_mouse_format.button_offset = 1; // Buttons after report ID
    } else {
        g_mouse_format.has_report_id = false;
        g_mouse_format.button_offset = 0; // Buttons at start
    }
    
    // Common formats:
    // Format 1 (Standard): [ID?] [Buttons] [X] [Y] [Wheel]
    // Format 2 (Logitech): [0x02] [Buttons] [00] [X_lo] [X_hi] [Y_lo] [Y_hi] [Wheel] [00]
    
    if (length == 9 && g_mouse_format.has_report_id && data[0] == 0x02 && data[2] == 0x00) {
        // Logitech format: [0x02] [Buttons] [00] [X_lo] [X_hi] [Y_lo] [Y_hi] [Wheel] [00]
        g_mouse_format.x_offset = 3;
        g_mouse_format.x_size = 2; // 16-bit
        g_mouse_format.y_offset = 5;
        g_mouse_format.y_size = 2; // 16-bit
        g_mouse_format.scroll_offset = 7;
        
        if (debug_level >= 1) {
            printf("[LEARN] Detected Logitech mouse format (9 bytes)\n");
        }
    } else if (length >= 4) {
        // Standard format: coordinates right after button byte
        int coord_start = g_mouse_format.button_offset + 1;
        
        // Assume X is 1-2 bytes, Y is 1-2 bytes
        // Most modern mice use 2 bytes (16-bit) for each
        if (length >= coord_start + 4) {
            g_mouse_format.x_offset = coord_start;
            g_mouse_format.x_size = 2; // Assume 16-bit
            g_mouse_format.y_offset = coord_start + 2;
            g_mouse_format.y_size = 2; // Assume 16-bit
            
            if (length > coord_start + 4) {
                g_mouse_format.scroll_offset = coord_start + 4;
            }
        } else {
            //8-bit coordinates
            g_mouse_format.x_offset = coord_start;
            g_mouse_format.x_size = 1;
            g_mouse_format.y_offset = coord_start + 1;
            g_mouse_format.y_size = 1;
            
            if (length > coord_start + 2) {
                g_mouse_format.scroll_offset = coord_start + 2;
            }
        }
        
        if (debug_level >= 1) {
            printf("[LEARN] Detected standard mouse format (%zu bytes)\n", length);
        }
    }
    
    g_mouse_format.is_learned = true;
    
    if (debug_level >= 1) {
        printf("[LEARN] Mouse format: size=%d, button_offset=%d, x_offset=%d (size=%d), y_offset=%d (size=%d), scroll_offset=%d\n",
               g_mouse_format.report_size, g_mouse_format.button_offset,
               g_mouse_format.x_offset, g_mouse_format.x_size,
               g_mouse_format.y_offset, g_mouse_format.y_size,
               g_mouse_format.scroll_offset);
    }
}

UdpServer::UdpServer(int port) : port(port), sockfd(-1), running(false), current_button_state(0x00) {}

UdpServer::~UdpServer() {
    stop();
}

void UdpServer::start() {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        sockfd = -1;
        return;
    }

    running = true;
    server_thread = std::thread(&UdpServer::server_loop, this);
    printf("UDP Server started on port %d\n", port);
}

void UdpServer::stop() {
    running = false;
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
}

void UdpServer::join() {
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

void UdpServer::server_loop() {
    char buffer[1024];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while (running) {
        len = sizeof(cliaddr);
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&cliaddr, &len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string packet(buffer);
            // Remove newline if present
            packet.erase(std::remove(packet.begin(), packet.end(), '\n'), packet.end());
            packet.erase(std::remove(packet.begin(), packet.end(), '\r'), packet.end());
            
            if (debug_level >= 1) {
                printf("[UDP] Received: %s\n", packet.c_str());
            }
            
            process_packet(packet);
        }
    }
}

void UdpServer::process_packet(const std::string& packet) {
    if (packet.empty()) return;

    if (packet[0] == '+') {
        handle_command(packet);
    } else {
        handle_raw_injection(packet);
    }
}

void UdpServer::handle_command(const std::string& command) {
    std::stringstream ss(command);
    std::string cmd;
    ss >> cmd;

    int mouse_ep = find_mouse_endpoint();
    if (mouse_ep == -1) {
        printf("Error: Could not find mouse endpoint for injection\n");
        return;
    }

    if (debug_level >= 1) {
        printf("[CMD] Processing command: %s (using EP 0x%02x)\n", cmd.c_str(), mouse_ep);
    }

    if (cmd == "+move") {
        int x, y;
        if (ss >> x >> y) {
            // Check if format has been learned
            if (!g_mouse_format.is_learned) {
                printf("Error: Mouse format not yet learned. Move the physical mouse first!\n");
                return;
            }
            
            // Get the current REAL button state from physical mouse
            uint8_t real_button_state = g_real_mouse_button_state.load();
            
            // Build packet using learned format
            std::vector<uint8_t> data(g_mouse_format.report_size, 0);
            
            // Set report ID if present
            if (g_mouse_format.has_report_id) {
                data[0] = g_mouse_format.report_id;
            }
            
            // Set button state
            data[g_mouse_format.button_offset] = real_button_state;
            
            // Set X coordinate (handle both 8-bit and 16-bit)
            if (g_mouse_format.x_size == 1) {
                data[g_mouse_format.x_offset] = x & 0xFF;
            } else {
                // 16-bit little-endian
                data[g_mouse_format.x_offset] = x & 0xFF;
                data[g_mouse_format.x_offset + 1] = (x >> 8) & 0xFF;
            }
            
            // Set Y coordinate (handle both 8-bit and 16-bit)
            if (g_mouse_format.y_size == 1) {
                data[g_mouse_format.y_offset] = y & 0xFF;
            } else {
                // 16-bit little-endian
                data[g_mouse_format.y_offset] = y & 0xFF;
                data[g_mouse_format.y_offset + 1] = (y >> 8) & 0xFF;
            }
            
            // Scroll wheel remains 0 (no scroll)
            
            if (debug_level >= 2) {
                printf("[CMD] Mouse move: X=%d, Y=%d (button: 0x%02x)\n", x, y, real_button_state);
            }
            
            inject_packet(mouse_ep, data);
        } else {
            printf("Error: +move requires X and Y coordinates\n");
        }
    } else if (cmd == "+click") {
        // Click: Left button down then up
        std::vector<uint8_t> down(9, 0);
        down[0] = 0x02;  // Magic number
        down[1] = 0x01;  // Left button pressed (bit 0)
        down[2] = 0x00;  // Padding
        // Bytes 3-6: X and Y = 0 (no movement)
        down[7] = 0x00;  // No scroll
        down[8] = 0x00;  // Padding
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse left click\n");
        }
        
        inject_packet(mouse_ep, down);

        // Small delay between down and up
        usleep(10000); // 10ms
        
        // Release: All buttons up
        std::vector<uint8_t> up(9, 0);
        up[0] = 0x02;  // Magic number
        up[1] = 0x00;  // No buttons (back to normal state)
        up[2] = 0x00;  // Padding
        // Bytes 3-6: X and Y = 0
        up[7] = 0x00;  // No scroll
        up[8] = 0x00;  // Padding
        inject_packet(mouse_ep, up);
    } else if (cmd == "+mousedown") {
        // Press and hold mouse button
        int button = 1; // Default to left button
        ss >> button; // Optional: read button number
        
        current_button_state |= (1 << (button - 1)); // Set button bit
        
        std::vector<uint8_t> data(9, 0);
        data[0] = 0x02;  // Magic number
        data[1] = current_button_state;
        data[2] = 0x00;  // Padding
        // Bytes 3-8: no movement or scroll
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse button %d down (state: 0x%02x)\n", button, current_button_state);
        }
        
        inject_packet(mouse_ep, data);
    } else if (cmd == "+mouseup") {
        // Release mouse button
        int button = 1; // Default to left button
        ss >> button; // Optional: read button number
        
        current_button_state &= ~(1 << (button - 1)); // Clear button bit
        
        std::vector<uint8_t> data(9, 0);
        data[0] = 0x02;  // Magic number
        data[1] = current_button_state;
        data[2] = 0x00;  // Padding
        // Bytes 3-8: no movement or scroll
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse button %d up (state: 0x%02x)\n", button, current_button_state);
        }
        
        inject_packet(mouse_ep, data);
    } else {
        printf("Error: Unknown command: %s\n", cmd.c_str());
    }
}

void UdpServer::handle_raw_injection(const std::string& data_str) {
    std::stringstream ss(data_str);
    std::string ep_str, payload_str;
    
    // Get the first word (endpoint)
    if (!(ss >> ep_str)) {
        printf("Error: No endpoint specified\n");
        return;
    }

    int ep_addr = 0;
    try {
        ep_addr = std::stoi(ep_str, nullptr, 16);
    } catch (...) {
        printf("Invalid endpoint address: %s\n", ep_str.c_str());
        return;
    }

    // Get the remaining part as payload (can be space-separated hex)
    std::string remaining;
    std::getline(ss, remaining);
    
    // Remove leading whitespace
    size_t start = remaining.find_first_not_of(" \t");
    if (start != std::string::npos) {
        remaining = remaining.substr(start);
    }
    
    if (remaining.empty()) {
        printf("Error: No payload specified\n");
        return;
    }
    
    if (debug_level >= 2) {
        printf("[RAW] EP: 0x%02x, Payload: %s\n", ep_addr, remaining.c_str());
    }
    
    // Parse hex string (handles "010203" or "01 02 03" formats)
    std::vector<uint8_t> data = parseHexString(remaining);
    
    if (data.empty()) {
        printf("Error: Could not parse payload\n");
        return;
    }
    
    inject_packet(ep_addr, data);
}

void UdpServer::inject_packet(int ep_addr, const std::vector<uint8_t>& data) {
    // Find the endpoint queue
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
    
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
            struct raw_gadget_endpoint *ep = &alt->endpoints[j];
            if (ep->endpoint.bEndpointAddress == ep_addr) {
                // Found it
                struct usb_raw_transfer_io io;
                io.inner.ep = ep->thread_info.ep_num;
                io.inner.flags = 0;
                io.inner.length = data.size();
                if (data.size() > sizeof(io.data)) {
                    printf("Packet too large for injection: %lu\n", data.size());
                    return;
                }
                memcpy(io.data, data.data(), data.size());

                ep->thread_info.data_mutex->lock();
                ep->thread_info.data_queue->push_back(io);
                ep->thread_info.data_mutex->unlock();
                
                if (debug_level >= 1) {
                    printf("[INJ] EP 0x%02x: Injected %lu bytes\n", ep_addr, data.size());
                }
                
                if (debug_level >= 3) {
                    printHexDump("[INJ] Data: ", data.data(), data.size());
                }
                
                return;
            }
        }
    }
    printf("Endpoint 0x%02x not found for injection\n", ep_addr);
}

int UdpServer::find_mouse_endpoint() {
    // Look for HID Mouse: bInterfaceClass=3 (HID), bInterfaceProtocol=2 (Mouse)
    
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
    
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        // Check if this is a HID Mouse interface (Class 3, Protocol 2)
        if (alt->interface.bInterfaceClass == 3 && alt->interface.bInterfaceProtocol == 2) {
            // Found HID Mouse interface, return its interrupt IN endpoint
            for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
                struct raw_gadget_endpoint *ep = &alt->endpoints[j];
                if ((ep->endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                    (ep->endpoint.bEndpointAddress & USB_DIR_IN)) {
                    if (debug_level >= 2) {
                        printf("[INIT] Found mouse endpoint: 0x%02x (max packet: %d bytes)\n", 
                               ep->endpoint.bEndpointAddress, ep->endpoint.wMaxPacketSize);
                    }
                    return ep->endpoint.bEndpointAddress;
                }
            }
        }
    }
    
    if (debug_level >= 1) {
        printf("[WARN] No HID Mouse interface found, falling back to first Interrupt IN\n");
    }
    
    // Fallback: Find first Interrupt IN endpoint
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
            struct raw_gadget_endpoint *ep = &alt->endpoints[j];
            if ((ep->endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                (ep->endpoint.bEndpointAddress & USB_DIR_IN)) {
                return ep->endpoint.bEndpointAddress;
            }
        }
    }
    return -1;
}
