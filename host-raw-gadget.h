#include <pthread.h>
#include <mutex>
#include <deque>

#include "misc.h"

/*----------------------------------------------------------------------*/

#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
	__u8 driver_name[UDC_NAME_LENGTH_MAX];
	__u8 device_name[UDC_NAME_LENGTH_MAX];
	__u8 speed;
};

enum usb_raw_event_type {
	USB_RAW_EVENT_INVALID,
	USB_RAW_EVENT_CONNECT,
	USB_RAW_EVENT_CONTROL,
};

struct usb_raw_event {
	__u32		type;
	__u32		length;
	__u8		data[0];
};

struct usb_raw_ep_io {
	__u16		ep;
	__u16		flags;
	__u32		length;
	__u8		data[0];
};

#define USB_RAW_EPS_NUM_MAX	30
#define USB_RAW_EP_NAME_MAX	16
#define USB_RAW_EP_ADDR_ANY	0xff

struct usb_raw_ep_caps {
	__u32	type_control	: 1;
	__u32	type_iso	: 1;
	__u32	type_bulk	: 1;
	__u32	type_int	: 1;
	__u32	dir_in		: 1;
	__u32	dir_out		: 1;
};

struct usb_raw_ep_limits {
	__u16	maxpacket_limit;
	__u16	max_streams;
	__u32	reserved;
};

struct usb_raw_ep_info {
	__u8				name[USB_RAW_EP_NAME_MAX];
	__u32				addr;
	struct usb_raw_ep_caps		caps;
	struct usb_raw_ep_limits	limits;
};

struct usb_raw_eps_info {
	struct usb_raw_ep_info	eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT		_IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN		_IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH	_IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE		_IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ		_IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE		_IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE	_IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE		_IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ		_IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE		_IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW		_IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO		_IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL		_IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT	_IOW('U', 13, __u32)
#define USB_RAW_IOCTL_EP_CLEAR_HALT	_IOW('U', 14, __u32)
#define USB_RAW_IOCTL_EP_SET_WEDGE	_IOW('U', 15, __u32)

/*----------------------------------------------------------------------*/

struct raw_gadget_interface_descriptor {
	struct usb_interface_descriptor		interface;
	struct usb_endpoint_descriptor		*endpoints;
};

struct raw_gadget_interface {
	struct raw_gadget_interface_descriptor	*altsetting;
	int					num_altsetting;
};

struct raw_gadget_config_descriptor {
	struct usb_config_descriptor		config;
	struct raw_gadget_interface		*interfaces;
};

extern struct usb_device_descriptor 		host_device_desc;
extern struct raw_gadget_config_descriptor	*host_config_desc;

/*----------------------------------------------------------------------*/

#define EP_MAX_PACKET_CONTROL	1024
#define EP_MAX_PACKET_BULK	1024
#define EP_MAX_PACKET_INT	8

struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_CONTROL];
};

struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_BULK];
};

struct usb_raw_int_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_INT];
};

struct usb_raw_transfer_io {
	struct usb_raw_ep_io		inner;
	char				data[1024];
};

/*----------------------------------------------------------------------*/

struct data_queue_info {
	struct usb_raw_transfer_io 	io;
	int 				length = 0;
};

struct thread_info {
	int				fd;
	int				ep_num = -1;
	struct usb_endpoint_descriptor 	endpoint;
	std::string			transfer_type;
	std::string			dir;
	std::deque<data_queue_info>	*data_queue;
	std::mutex			*data_mutex;
};

struct endpoint_thread {
	pthread_t			ep_thread_read;
	pthread_t			ep_thread_write;
	struct thread_info		ep_thread_info;
};

extern endpoint_thread *ep_thread_list;

/*----------------------------------------------------------------------*/

int usb_raw_open();
void usb_raw_init(int fd, enum usb_device_speed speed,
			const char *driver, const char *device);
void usb_raw_run(int fd);

void ep0_loop(int fd);
int init_raw_gadget();
