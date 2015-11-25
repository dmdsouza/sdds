/*
 * LocationTrackingService.h
 *
 *  Created on: Aug 3, 2016
 *      Author: o_dedi
 */

#ifndef SDDS_INCLUDE_OS_SSAL_LOCATIONTRACKINGSERVICE_H_
#define SDDS_INCLUDE_OS_SSAL_LOCATIONTRACKINGSERVICE_H_

#include "List.h"
#include "os-ssal/LocationService.h"

rc_t
LocationTrackingService_init();

rc_t
LocationTrackingService_trackDevice(SSW_NodeID_t device);

rc_t
LocationTrackingService_untrackDevice(SSW_NodeID_t device);

rc_t
LocationTrackingService_getDeviceLocation(SSW_NodeID_t device, DeviceLocation_t* devLoc);

List_t*
LocationTrackingService_getLocations();

#endif /* SDDS_INCLUDE_OS_SSAL_LOCATIONTRACKINGSERVICE_H_ */