/*
 * include/linux/fotg210.h
 *
 * Definitions for Faraday OTG 210 Dual Role Hoset Device controller
 */

#ifndef _FOTG210_OF_H_
#define _FOTG210_OF_H_

enum fotg210_operating_modes {
	FOTG210_DR_HOST,
	FOTG210_DR_DEVICE,
};

struct fotg210_usb2_platform_data {
	enum fotg210_operating_modes	operating_mode;
};

#endif
