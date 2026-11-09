/*
 * RedwoodBT: the IOKit service by which iOS's bluetoothd finds the Bluetooth UART.
 *
 * bluetoothd does not find the serial port by scanning /dev: on our boots /dev/uart.bluetooth exists and it never
 * opens it ("No bluetooth on this device", exit). It looks through IOKit instead -- FindLocalMacAddress tries
 * IOServiceNameMatching("bluetooth") and ("marconi-bt") -- so the port has to be an IORegistry entry, as a real
 * UART-Bluetooth provider would publish. This makes a bare service named "bluetooth" under the platform, carrying the
 * tty's BSD name and a device address, the way bluetoothd's lookup expects to find one; the bytes still flow through
 * the /dev node (bsd/dev/hakios_dada_wifi.c, bt_serve). It has no user client, like RedwoodWLAN.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>

extern "C" void RedwoodBTStart(const char * bsdname, const uint8_t * address);
extern "C" void RedwoodBTSetAddress(const uint8_t * address);

class RedwoodBT : public IOService
{
	OSDeclareDefaultStructors(RedwoodBT);
};

OSDefineMetaClassAndStructors(RedwoodBT, IOService);

static RedwoodBT * gRedwoodBT;           /* the one published service, kept for its address */

/*
 * The address goes in as bluetoothd reads it back: FindLocalMacAddress takes the service named "bluetooth",
 * IORegistryEntrySearchCFProperty(.., "IODeviceTree", "local-mac-address", .., recursively), the first six bytes in
 * display order. With none, the address is all 00s, and bluetoothd quits ("Not a valid MAC address (all 00s)", then
 * "No bluetooth on this device") before it ever looks for /dev/uart.bluetooth.
 */
static void
bt_set_address(RedwoodBT * bt, const uint8_t * address)
{
	OSData * mac = OSData::withBytes(address, 6);

	if (mac == NULL) {
		return;
	}
	bt->setProperty("local-mac-address", mac);
	bt->setProperty("bluetooth-mac", mac);
	mac->release();
}

static IOService *
bt_publish(IOService * service, IOService * provider, const char * name)
{
	if (service == NULL || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	return service;
}

/* Once, when /dev/uart.bluetooth exists. bsdname is "uart.bluetooth"; address is the chip's six bytes (or NULL). */
void
RedwoodBTStart(const char * bsdname, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	RedwoodBT * bt = OSTypeAlloc(RedwoodBT);
	OSString * s;

	if (platform == NULL || bt == NULL || !bt->init()) {
		OSSafeReleaseNULL(bt);
		IOLog("RedwoodBT: cannot create the service\n");
		return;
	}
	if ((s = OSString::withCString(bsdname)) != NULL) {
		bt->setProperty("BSD Name", s);         /* bluetoothd reads this to find the tty */
		s->release();
	}
	if ((s = OSString::withCString("uart")) != NULL) {
		bt->setProperty("transport", s);
		s->release();
	}
	if (address != NULL) {
		bt_set_address(bt, address);
	}
	bt->setProperty("IOBuiltin", true);
	/* the two names bluetoothd's lookup tries (IOServiceNameMatching): publish under "bluetooth" */
	if (bt_publish(bt, platform, "bluetooth") == NULL) {
		IOLog("RedwoodBT: the service did not start\n");
	} else {
		IOLog("RedwoodBT: published IOKit service 'bluetooth' (BSD Name %s) for bluetoothd\n", bsdname);
		gRedwoodBT = bt;                /* keeps the reference */
	}
}

/* Later, once the chip has said its address (HCI Read BD_ADDR; six bytes in display order): launchd starts bluetoothd
 * again every ~10 s, and the next one finds it. Nothing happens without a published service. */
void
RedwoodBTSetAddress(const uint8_t * address)
{
	if (gRedwoodBT == NULL || address == NULL) {
		return;
	}
	bt_set_address(gRedwoodBT, address);
	IOLog("RedwoodBT: local-mac-address %02x:%02x:%02x:%02x:%02x:%02x\n", address[0], address[1], address[2],
	    address[3], address[4], address[5]);
}
