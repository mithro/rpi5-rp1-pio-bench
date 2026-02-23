There are two Raspberry Pi devices that you are able to access:

You need to ssh into the devices as the user tim and you have root access.

Each device has a set of interfaces (eth0 and wlan0) with both IPv4 and IPv6.
To use a specific interface and IP you can use:

 * ipv4.eth0.<hostname> -- IPv4 address on eth0 interface.
 * ipv6.wlan0.<hostname> -- IPv6 address on wlan0 interface.
 * eth0.<hostname> -- Both IPv4 and IPv6 addresses.
 * <hostname> -- All IPv4 and IPv6 addresses.

# rpi5-pmod.iot.welland.mithis.com

Raspberry Pi 5 with a Digilent Pmod Hat.

# rpi4-pmod.iot.welland.mithis.com

Raspberry Pi 4 with a Digilent Pmod Hat.
