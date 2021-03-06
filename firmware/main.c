/* peripherials */
#include "compiler.h"
#include "sampler.h"
#include "ucd_api.h"

/* include proper usb driver headers */
#ifndef USBDRV
#error no USB driver selected
#endif

#if USBDRV == vusb
#include "usbdrv/usbdrv.h"
#else /* USBDRV */
#error USB driver not supported
#endif /* USBDRV */

/* library headers */
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <string.h>

/* definitions */
#define USBRQ_HID_REPORT_TYPE_INPUT 1
#define USBRQ_HID_REPORT_TYPE_OUTPUT 2
#define USBRQ_HID_REPORT_TYPE_FEATURE 3
#define HARD_SENSITIVITY_OFFSET 12

#define min(a,b) ( ((a)<(b))?(a):(b) )

/* globals */
NOINIT uint8_t mcusr_mirror;
/* globals: reports */
static uint16_t input_report;
static uint8_t feature_report[UCD_FEATURE_REPORT_COUNT+1];
static uint8_t active_subrq_mux;

/* tracks progress of usb write */
static struct
{
	uint8_t count;
	uint8_t max;
}usb_transfer;

/* tracks progress of eeprom write transfer */
static struct
{
	uint8_t *src;
	uint8_t *dst;
	uint8_t count;
}eeprom_transfer;

/* globals: operating parameters */
ucd_parameters_request_type g_parameters;

/* we use combined report, INPUT returns data, FEATURE allows for controlling of parameters */
PROGMEM char usbHidReportDescriptor[49] = {
	/* input part */
	0x06, 0x00, 0xff,              // USAGE_PAGE (Vendor Defined Page 1)
	0xa1, 0x01,                    // COLLECTION (Application)
	0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
	0x27, 0xff, 0xff, 0x00, 0x00,  //   LOGICAL_MAXIMUM (65535)
#if 0
	0x35, 0x00,                    //   PHYSICAL_MINIMUM (0)
	0x47, 0xff, 0xff, 0x00, 0x00,  //   PHYSICAL_MAXIMUM (65535)
	0x55, 0x0E,                    //   UNIT_EXPONENT (-2)
	0x67, 0xe1, 0x00, 0x00, 0x01,  //   UNIT (SI Lin:0x10000e1)
#endif
	0x75, 0x10,                    //   REPORT_SIZE (16)
	0x95, 0x01,                    //   REPORT_COUNT (1)
	0x09, 0x01,                    //   USAGE(vendor usage 1)
	0x82, 0x22, 0x01,              // INPUT (Data,Var,Abs,NPrf,Buf)

	0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
	0x26, 0xff, 0x00,              //   LOGICAL_MAXIMUM (255)
	0x75, 0x08,                    //   REPORT_SIZE (8)
	0x85, UCD_SUBRQ_MUX_REPORT_ID, //   REPORT_ID(0)
	0x95, 0x01,                    //   REPORT_COUNT (1)
	0x09, 0x00,                    //   USAGE(Undefined)
	0xb2, 0x02, 0x01,              //   FEATURE(Data,Var,Abs,Buf)

	0x75, 0x08,                    //   REPORT_SIZE (8)
	0x85, UCD_SUBRQ_DATA_REPORT_ID,//   REPORT_ID(1)
	0x95, UCD_FEATURE_REPORT_COUNT,//   REPORT_COUNT (8)
	0x09, 0x00,                    //   USAGE(Undefined)
	0xb2, 0x02, 0x01,              //   FEATURE(Data,Var,Abs,Buf)

	0xc0,
};

ucd_calibration_request_type ee_calibration[8] EEMEM;

/*
 * USB part
 */
void sensor_read_feature_data(uint8_t id)
{
	switch ( id )
	{
	case UCD_SUBRQ_PARAMETERS:
		memcpy(feature_report + 1, &g_parameters, sizeof(ucd_parameters_request_type));
		break;
	break;
	case UCD_SUBRQ_CALIBRATION_SET_0:
	case UCD_SUBRQ_CALIBRATION_SET_1:
	case UCD_SUBRQ_CALIBRATION_SET_2:
	case UCD_SUBRQ_CALIBRATION_SET_3:
	case UCD_SUBRQ_CALIBRATION_SET_4:
	case UCD_SUBRQ_CALIBRATION_SET_5:
	case UCD_SUBRQ_CALIBRATION_SET_6:
	case UCD_SUBRQ_CALIBRATION_SET_7:
		eeprom_read_block(&feature_report[1],
				  &ee_calibration[id - UCD_SUBRQ_CALIBRATION_SET_0],
				  sizeof(ucd_calibration_request_type));
		break;
	}
}

void sensor_write_feature_data(uint8_t id)
{
	switch ( id )
	{
	case UCD_SUBRQ_PARAMETERS:
		memcpy(&g_parameters, feature_report + 1, sizeof(ucd_parameters_request_type));
		break;
	case UCD_SUBRQ_CALIBRATION_SET_0:
	case UCD_SUBRQ_CALIBRATION_SET_1:
	case UCD_SUBRQ_CALIBRATION_SET_2:
	case UCD_SUBRQ_CALIBRATION_SET_3:
	case UCD_SUBRQ_CALIBRATION_SET_4:
	case UCD_SUBRQ_CALIBRATION_SET_5:
	case UCD_SUBRQ_CALIBRATION_SET_6:
	case UCD_SUBRQ_CALIBRATION_SET_7:
		if ( eeprom_transfer.count )
			break; /* EEPROM transfer already in progress, wait for it to complete.
				* However, we have gotten our data garbled anyway :-( */
		eeprom_transfer.dst = (uint8_t*)&ee_calibration[id - UCD_SUBRQ_CALIBRATION_SET_0];
		eeprom_transfer.src = feature_report + 1;
		eeprom_transfer.count = sizeof(ucd_calibration_request_type);
		break;
	}
}

USB_PUBLIC usbMsgLen_t usbFunctionSetup(uchar data[8])
{
	const usbRequest_t * const rq = (void *)data;
	const uint8_t report_type = rq->wValue.bytes[1]; /* wValue: ReportType (highbyte) */
	const uint8_t report_id = rq->wValue.bytes[0]; /* ReportID (lowbyte) */

	switch ( rq->bmRequestType & USBRQ_TYPE_MASK )
	{
	case USBRQ_TYPE_CLASS:
		switch ( rq->bRequest )
		{
		case USBRQ_HID_GET_REPORT:
			switch ( report_type ) 
			{
			case USBRQ_HID_REPORT_TYPE_INPUT:
				usbMsgPtr = (void *)&input_report;
				return sizeof(input_report);
			case USBRQ_HID_REPORT_TYPE_FEATURE:
				feature_report[0] = report_id;
				usbMsgPtr = (void*)&feature_report;
				if ( UCD_SUBRQ_MUX_REPORT_ID == report_id )
				{
					feature_report[1] = active_subrq_mux;
					return 2; /* 1 byte report id + 1 byte mux id */
				}
				if ( UCD_SUBRQ_DATA_REPORT_ID == report_id )
				{
					sensor_read_feature_data(active_subrq_mux);
					return sizeof(feature_report);
				}
				break;
			}
			break;
		case USBRQ_HID_SET_REPORT:
			usb_transfer.count = 0;
			usb_transfer.max = min( rq->wLength.bytes[0], sizeof(feature_report) );
			if ( USBRQ_HID_REPORT_TYPE_FEATURE == report_type ) /* wValue: ReportType (highbyte) */
				return USB_NO_MSG;
			break;
		}
		break;
	default: /* no vendor specific requests implemented */
		break;
	}
	/* default for not implemented requests: return no data back to host */
	return 0;
}

USB_PUBLIC uchar usbFunctionWrite(uchar *data, uchar len)
{
	if ( usb_transfer.count >= usb_transfer.max )
		return 1; /* transfer complete */

	uint8_t remaining = min(usb_transfer.max - usb_transfer.count, len);
	while ( remaining-- )
		feature_report[usb_transfer.count++] = *data++;

	if ( usb_transfer.count != usb_transfer.max )
		return 0; /* transfer still incomplete, more data expected */

	const uint8_t report_id = feature_report[0];
	if ( UCD_SUBRQ_MUX_REPORT_ID == report_id )
	{
		ucd_mux_request_type *rq = (ucd_mux_request_type *)(feature_report+1);
		active_subrq_mux = rq->subrq_id;
	}
	if ( UCD_SUBRQ_DATA_REPORT_ID == report_id )
	{
		sensor_write_feature_data(active_subrq_mux);
	}
	return 1;
}



/*
 * Initialization and entry point
 */

INIT_FUNC_3 void early_init(void)
{
	mcusr_mirror = MCUSR;
	wdt_disable();
}

INIT_FUNC_8 void late_init(void)
{
	usbInit();
	sampler_init();
}

#if defined(__GNUC__) && defined(__AVR__)
int main(void) __attribute__((OS_main));
#endif
int main(void)
{
	usbDeviceDisconnect();
	_delay_ms(250);
	usbDeviceConnect();

	sei();

	sampler_start();
	for(;;)
	{
		usbPoll();
		if ( usbInterruptIsReady() )
			usbSetInterrupt((void*)&input_report, sizeof(input_report));

		/* check for sample data availability */
		if ( sampler_poll() )
		{
			const uint16_t fp_sample = sampler_get_sample();
			const uint16_t sample = 
				fp_to_uint16(
					fp_inverse(
						fp_sample,
						g_parameters.sensitivity + HARD_SENSITIVITY_OFFSET)
					);
			/* filter values: report = 15/16 * sample + 1/16 * input_report */
			const uint8_t filter_strength = 1;
			input_report = sample - (sample>>filter_strength) + (input_report>>filter_strength);
			sampler_start();
		}

		/* check if background eeprom write operation pending */
		if ( eeprom_transfer.count )
		{
			eeprom_write_byte(
				eeprom_transfer.dst++,
				*eeprom_transfer.src++
				);
			--eeprom_transfer.count;
		}
	}
}
