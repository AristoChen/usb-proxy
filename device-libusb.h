#include <libusb-1.0/libusb.h>

#include "misc.h"

#define USB_REQUEST_TIMEOUT 1000

#define MAX_ATTEMPTS 5

#define ISO_BATCH_SIZE_DEFAULT 8
#define ISO_BATCH_SIZE_MAX 32

struct iso_packet_result {
	uint8_t *data;
	int actual_length;
	int status; // libusb_transfer_status
};

struct iso_batch_result {
	uint8_t *buffer;
	int num_packets;
	struct iso_packet_result packets[ISO_BATCH_SIZE_MAX];
	int total_length;
	bool success;
};

extern libusb_device			**devs;
extern libusb_device_handle		*dev_handle;
extern libusb_context			*context;
extern libusb_hotplug_callback_handle	callback_handle;

extern struct libusb_device_descriptor		device_device_desc;
extern struct libusb_config_descriptor		**device_config_desc;

extern pthread_t hotplug_monitor_thread;

int connect_device(int vendorId, int productId);
void reset_device();
void set_configuration(int configuration);
void claim_interface(int interface);
void release_interface(int interface);
void set_interface_alt_setting(int interface, int altsetting);
int control_request(const usb_ctrlrequest *setup_packet, int *nbytes,
			unsigned char **dataptr, int timeout);
int send_data(uint8_t endpoint, uint8_t attributes, uint8_t *dataptr,
			int length, int timeout);
int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout);
int receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize,
			uint8_t **dataptr, int *length, int timeout);
int receive_iso_data_batched(uint8_t endpoint, uint16_t maxPacketSize,
			struct iso_batch_result *result, int batch_size, int timeout);
