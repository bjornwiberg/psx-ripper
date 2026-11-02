#pragma once
// Host build: the console's network settings (only the IP address is asked for)
#define NET_CTL_INFO_IP_ADDRESS 16
union net_ctl_info { char ip_address[16]; };
int netCtlInit();
int netCtlGetInfo(int code, union net_ctl_info *info);
