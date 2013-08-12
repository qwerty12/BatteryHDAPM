//
//  main.c
//  BatteryHDAPM
//
//  Based on hdapm, Copyright (c) 2007-2011, Bryce McKinlay
//

//Battery and sleep code: http://www.virtualbox.org/svn/vbox/trunk/src/VBox/Main/src-server/darwin/HostPowerDarwin.cpp

#include <CoreFoundation/CoreFoundation.h>
#include <syslog.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/ata/IOATAStorageDefines.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/IOMessage.h>

static bool g_prevBatteryState = true;
static bool g_force = false;

static io_object_t g_hdd;

static IONotificationPortRef g_notifyPort;
static io_connect_t g_rootPort;
static io_object_t g_notifierObject;

static void cleanup();

#ifdef DEBUG
void log_info(const char* format, ... ) {
    va_list args;
    va_start(args, format);
    vsyslog(LOG_NOTICE, format, args);
    va_end(args);
}
#endif

void setAPMLevel(int level)
{
    CFNumberRef cfLevel = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &level);
#ifdef DEBUG
    kern_return_t r = IORegistryEntrySetCFProperty(g_hdd, CFSTR("APM Level"), cfLevel);
#else
    (void) IORegistryEntrySetCFProperty(g_hdd, CFSTR("APM Level"), cfLevel);
#endif
    CFRelease(cfLevel);

#ifdef DEBUG
    const char *result;
    if (r)
    {
        if (r == kIOReturnUnsupported)
            result = "FAILED: APM not supported";
        else if (r == kIOReturnNotPrivileged)
            result = "FAILED: Permission denied";
        else
            result = "FAILED";
    }
    else
        result = "Success";
    
    log_info("  Set APM Level to 0x%02x: %s\n", level, result);
#endif
}

io_object_t find_ata_device(io_object_t device)
{
    io_object_t child = 0;
    IOReturn r;
    while (! (IOObjectConformsTo(device, "IOAHCIBlockStorageDevice") || IOObjectConformsTo(device, "IOATABlockStorageDevice")))
    {
        child = device;
        r = IORegistryEntryGetParentEntry(child, kIOServicePlane, &device);
        IOObjectRelease(child);
        if (r != 0 || device == 0)
            return 0;
    }
    return device;
}

io_object_t getATADeviceForBSDPath(const char* path)
{
    const char *device_name;
    
    /* IOBSDNameMatching doesn't want the leading path. */
    if (strncmp (path, "/dev/", 5) == 0)
        device_name = path + 5;
    else
        device_name = path;
    
    CFMutableDictionaryRef matcher = IOBSDNameMatching(kIOMasterPortDefault, 0, device_name);
    io_object_t device = IOServiceGetMatchingService(kIOMasterPortDefault, matcher);
    if (! device)
        return 0;
    
    device = find_ata_device(device);
    if (! device)
        return 0;
    
    return device;

}

static void powerStateWatcher(__unused void *param_not_used)
{
    bool usingBattery = g_prevBatteryState;
    
    CFTypeRef source = IOPSCopyPowerSourcesInfo();
    CFArrayRef powerSources = IOPSCopyPowerSourcesList(source);
    
    CFIndex count;
    if ((count = CFArrayGetCount(powerSources)) > 0) {
        for (int i = 0; i < count; ++i) {
            const void *psValue;
            CFDictionaryRef powerSource = IOPSGetPowerSourceDescription(source, CFArrayGetValueAtIndex(powerSources, i));
            if (!powerSource)
                continue;
            if (CFDictionaryGetValue(powerSource, CFSTR(kIOPSIsPresentKey)) == kCFBooleanFalse)
                continue;
            if (CFDictionaryGetValueIfPresent(powerSource, CFSTR(kIOPSTransportTypeKey), &psValue) &&
                CFStringCompare((CFStringRef)psValue, CFSTR(kIOPSInternalType), 0) == kCFCompareEqualTo) {
                if (CFDictionaryGetValueIfPresent(powerSource, CFSTR(kIOPSPowerSourceStateKey), &psValue))
                    usingBattery = CFStringCompare((CFStringRef)psValue, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo;
            }
        }
    }
    
    if (g_force || usingBattery != g_prevBatteryState) {
        g_force = false;
        setAPMLevel(usingBattery ? kIOATADefaultPerformance : kIOATAMaxPerformance);
        g_prevBatteryState = usingBattery;
    }
    
    CFRelease(powerSources);
    CFRelease(source);
}

static void powerChangeNotificationHandler(__unused void *param_not_used, __unused io_service_t param_not_used_, natural_t messageType, void *messageArgument)
{
    if (messageType == kIOMessageSystemHasPoweredOn) {
        g_force = true;
        powerStateWatcher(0);
    }
    
    IOAllowPowerChange(g_rootPort, (long) messageArgument);
}

static void initPowerStateMonitoring()
{
    g_rootPort = IORegisterForSystemPower(0, &g_notifyPort,
                                          powerChangeNotificationHandler,
                                          &g_notifierObject);
    if (g_rootPort == MACH_PORT_NULL) {
        cleanup();
        exit (EXIT_FAILURE);
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(g_notifyPort), kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IOPSNotificationCreateRunLoopSource(powerStateWatcher, 0), kCFRunLoopDefaultMode);
    powerStateWatcher(0);
}

static void sighandler(__unused int param_not_used)
{
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void cleanup()
{
    if (g_notifyPort != MACH_PORT_NULL)
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(g_notifyPort), kCFRunLoopDefaultMode);
    
    if (g_notifierObject != MACH_PORT_NULL)
        IODeregisterForSystemPower(&g_notifierObject);
    
    IOServiceClose(g_rootPort);
    
    if (g_notifyPort != MACH_PORT_NULL)
        IONotificationPortDestroy(g_notifyPort);

    if (g_hdd) {
        IOObjectRelease(g_hdd);
        g_hdd = 0;
    }
}

int main(int argc, const char * argv[])
{
    const char *disk = "disk0";
    if (argc == 2)
        disk = argv[1];

    g_hdd = getATADeviceForBSDPath(disk);
    if (!g_hdd)
        return EXIT_FAILURE;

    initPowerStateMonitoring();

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    CFRunLoopRun();
    
    cleanup();

    return EXIT_SUCCESS;
}

