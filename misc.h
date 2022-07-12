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
extern bool please_stop_eps;

extern int desired_configuration;
extern int desired_interface;
extern int desired_interface_altsetting;

extern bool injection_enabled;
extern std::string injection_file;
extern Json::Value injection_config;

std::string hexToAscii(std::string input);
int hexToDecimal(int input);
