#ifndef UDP_SERVER_H
#define UDP_SERVER_H

#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

// Structure to describe mouse report format (learned from real packets)
struct MouseReportFormat {
    uint8_t report_size;      // Total size of report in bytes
    uint8_t button_offset;    // Offset of button byte
    uint8_t x_offset;         // Offset of X coordinate low byte
    uint8_t x_size;           // Size of X coordinate (1 or 2 bytes)
    uint8_t y_offset;         // Offset of Y coordinate low byte
    uint8_t y_size;           // Size of Y coordinate (1 or 2 bytes)
    uint8_t scroll_offset;    // Offset of scroll wheel byte (0 if none)
    bool has_report_id;       // Whether first byte is report ID
    uint8_t report_id;        // Report ID value (if has_report_id)
    bool is_learned;          // Whether format has been learned
};

// Global variable to track real mouse button state from physical mouse
extern std::atomic<uint8_t> g_real_mouse_button_state;

// Global mouse report format (learned from real packets)
extern MouseReportFormat g_mouse_format;

// Function to update real mouse state (called from proxy.cpp)
void update_real_mouse_state(uint8_t button_state);

// Function to learn mouse format from real packet
void learn_mouse_format(const uint8_t* data, size_t length);

class UdpServer {
public:
    UdpServer(int port);
    ~UdpServer();

    void start();
    void stop();
    void join();

private:
    int port;
    int sockfd;
    std::thread server_thread;
    std::atomic<bool> running;
    
    // Track current mouse button state (for UDP commands only)
    uint8_t current_button_state;

    void server_loop();
    void process_packet(const std::string& packet);
    void handle_command(const std::string& command);
    void handle_raw_injection(const std::string& data);
    void inject_packet(int ep_addr, const std::vector<uint8_t>& data);
    
    // Helper to find mouse endpoint
    int find_mouse_endpoint();
};

#endif // UDP_SERVER_H
