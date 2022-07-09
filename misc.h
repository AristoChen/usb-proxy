#include <iomanip>
#include <iostream>
#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <chrono>
#include <linux/usb/ch9.h>

extern int verbose_level;
extern bool please_stop_ep0;
extern bool please_stop_eps;

extern int desired_configuration;
extern int desired_interface;
extern int desired_interface_altsetting;
