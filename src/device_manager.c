/*
 * device_manager.c - Device Discovery and Management using D2XX
 * XVC Server for Digilent HS2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "device_manager.h"
#include "logging.h"

#include "ftd2xx.h"

int device_manager_init(device_manager_t *mgr)
{
    if (!mgr) return -1;
    
    memset(mgr, 0, sizeof(device_manager_t));
    
    /* Set VID/PID for HS2 devices */
    FT_STATUS status = FT_SetVIDPID(0x0403, 0x6010);
    if (status != FT_OK) {
        LOG_ERROR("FT_SetVIDPID failed: %d", (int)status);
        return -1;
    }
    
    mgr->initialized = true;
    return 0;
}

void device_manager_shutdown(device_manager_t *mgr)
{
    if (!mgr) return;
    
    mgr->initialized = false;
    mgr->device_count = 0;
}

int device_manager_scan(device_manager_t *mgr)
{
    if (!mgr || !mgr->initialized) return -1;
    
    DWORD numDevices = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevices);
    
    if (status != FT_OK) {
        LOG_ERROR("FT_CreateDeviceInfoList failed: %d", (int)status);
        return -1;
    }
    
    if (numDevices == 0) {
        LOG_INFO("No FTDI devices found");
        return 0;
    }
    
    FT_DEVICE_LIST_INFO_NODE *devList = calloc(numDevices, sizeof(FT_DEVICE_LIST_INFO_NODE));
    if (!devList) {
        LOG_ERROR("Memory allocation failed");
        return -1;
    }
    
    status = FT_GetDeviceInfoList(devList, &numDevices);
    if (status != FT_OK) {
        LOG_ERROR("FT_GetDeviceInfoList failed: %d", (int)status);
        free(devList);
        return -1;
    }
    
    mgr->device_count = 0;
    
    for (DWORD i = 0; i < numDevices && mgr->device_count < MAX_DEVICES; i++) {
        FT_DEVICE_LIST_INFO_NODE *ftdi_dev = &devList[i];
        
        /* Extract VID and PID from ID field */
        /* ID format: 0xVVVVPPPP where VVVV is VID and PPPP is PID */
        DWORD vid = (ftdi_dev->ID >> 16) & 0xFFFF;
        DWORD pid = ftdi_dev->ID & 0xFFFF;
        
        /* Check for supported FTDI devices (FT2232H or FT232H) */
        if (vid == 0x0403 && (pid == 0x6010 || pid == 0x6014)) {
            hs2_device_t *dev = &mgr->devices[mgr->device_count];
            memset(dev, 0, sizeof(hs2_device_t));
            
            dev->vendor_id = vid;
            dev->product_id = pid;
            dev->state = DEVICE_STATE_AVAILABLE;
            
            /* Copy strings */
            strncpy(dev->serial, ftdi_dev->SerialNumber, MAX_SERIAL_LEN - 1);
            strncpy(dev->manufacturer, "FTDI", sizeof(dev->manufacturer) - 1);
            strncpy(dev->product, ftdi_dev->Description, sizeof(dev->product) - 1);
            
            /* Format bus location using index */
            snprintf(dev->bus_location, sizeof(dev->bus_location), "FTDI-%u", (unsigned int)i);
            
            mgr->device_count++;
            LOG_DBG("Found HS2 device: %s (SN: %s)", dev->product, dev->serial);
        }
    }
    
    free(devList);
    
    LOG_INFO("Scan complete: found %d HS2 device(s)", mgr->device_count);
    return mgr->device_count;
}

hs2_device_t* device_manager_find(device_manager_t *mgr, const device_id_t *id)
{
    if (!mgr || !id) return NULL;
    
    for (int i = 0; i < mgr->device_count; i++) {
        hs2_device_t *dev = &mgr->devices[i];
        
        switch (id->type) {
            case DEVICE_ID_SERIAL:
                if (strcmp(dev->serial, id->value) == 0) return dev;
                break;
                
            case DEVICE_ID_BUS:
                if (strcmp(dev->bus_location, id->value) == 0) return dev;
                break;
                
            case DEVICE_ID_AUTO:
                if (dev->state == DEVICE_STATE_AVAILABLE) return dev;
                break;
                
            default:
                break;
        }
    }
    
    return NULL;
}

hs2_device_t* device_manager_find_available(device_manager_t *mgr)
{
    if (!mgr) return NULL;
    
    for (int i = 0; i < mgr->device_count; i++) {
        if (mgr->devices[i].state == DEVICE_STATE_AVAILABLE) {
            return &mgr->devices[i];
        }
    }
    return NULL;
}

int device_manager_assign(device_manager_t *mgr, hs2_device_t *device, int instance_id)
{
    if (!mgr || !device) return -1;
    
    if (device->state != DEVICE_STATE_AVAILABLE) {
        LOG_WARN("Device %s already in use by instance %d",
                 device->serial, device->assigned_instance);
        return -1;
    }
    
    device->state = DEVICE_STATE_IN_USE;
    device->assigned_instance = instance_id;
    
    LOG_INFO("Device %s assigned to instance %d", device->serial, instance_id);
    return 0;
}

void device_manager_release(device_manager_t *mgr, hs2_device_t *device)
{
    if (!mgr || !device) return;
    
    LOG_INFO("Device %s released from instance %d", 
             device->serial, device->assigned_instance);
    
    device->state = DEVICE_STATE_AVAILABLE;
    device->assigned_instance = 0;
}

hs2_device_t* device_manager_get(device_manager_t *mgr, int index)
{
    if (!mgr || index < 0 || index >= mgr->device_count) return NULL;
    return &mgr->devices[index];
}

void device_manager_print(const device_manager_t *mgr, bool verbose)
{
    if (!mgr) return;
    
    printf("Detected %d Digilent HS2 device(s):\n\n", mgr->device_count);
    
    for (int i = 0; i < mgr->device_count; i++) {
        const hs2_device_t *dev = &mgr->devices[i];
        
        printf("Device #%d:\n", i + 1);
        printf("  Manufacturer: %s\n", dev->manufacturer[0] ? dev->manufacturer : "Unknown");
        printf("  Product: %s\n", dev->product[0] ? dev->product : "Unknown");
        printf("  Serial Number: %s\n", dev->serial[0] ? dev->serial : "N/A");
        printf("  Suggested Instance ID: %d\n", i + 1);
        printf("  Suggested Port: %d\n", DEFAULT_BASE_PORT + i);
        
        if (verbose) {
            printf("  Vendor ID: 0x%04X\n", dev->vendor_id);
            printf("  Product ID: 0x%04X\n", dev->product_id);
            printf("  State: %s\n", 
                   dev->state == DEVICE_STATE_AVAILABLE ? "Available" :
                   dev->state == DEVICE_STATE_IN_USE ? "In Use" : "Unknown");
        }
        printf("\n");
    }
}

int device_manager_generate_config(const device_manager_t *mgr, 
                                    xvc_global_config_t *config,
                                    int base_port)
{
    if (!mgr || !config) return -1;
    
    config_init(config);
    config->base_port = base_port;
    
    for (int i = 0; i < mgr->device_count && i < MAX_INSTANCES; i++) {
        const hs2_device_t *dev = &mgr->devices[i];
        xvc_instance_config_t *inst = &config->instances[i];
        
        inst->instance_id = i + 1;
        inst->port = base_port + i;
        inst->enabled = true;
        inst->frequency = DEFAULT_FREQUENCY;
        inst->latency_timer = DEFAULT_LATENCY;
        
        /* Use serial number if available, otherwise bus location */
        if (dev->serial[0]) {
            inst->device_id.type = DEVICE_ID_SERIAL;
            strncpy(inst->device_id.value, dev->serial, MAX_SERIAL_LEN - 1);
            snprintf(inst->alias, MAX_ALIAS_LEN, "HS2-%s", dev->serial);
        } else {
            inst->device_id.type = DEVICE_ID_BUS;
            strncpy(inst->device_id.value, dev->bus_location, MAX_SERIAL_LEN - 1);
            snprintf(inst->alias, MAX_ALIAS_LEN, "HS2-%s", dev->bus_location);
        }
        
        config->instance_count++;
    }
    
    return 0;
}
