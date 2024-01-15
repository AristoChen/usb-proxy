#include <assert.h>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <stdio.h>
#include <string>
#include <getopt.h>
#include <signal.h>
#include <chrono>
#include <sys/stat.h>
#include <linux/usb/ch9.h>
#include <jsoncpp/json/json.h>

extern int verbose_level;
extern bool please_stop_ep0;
extern volatile bool please_stop_eps;

extern bool injection_enabled;
extern std::string injection_file;
extern Json::Value injection_config;

extern bool customized_config_enabled;
extern bool reset_device_before_proxy;
extern bool bmaxpacketsize0_must_greater_than_64;

std::string hexToAscii(std::string input);
int hexToDecimal(int input);
