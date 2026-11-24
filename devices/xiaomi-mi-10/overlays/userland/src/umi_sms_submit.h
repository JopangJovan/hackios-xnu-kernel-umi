#ifndef UMI_SMS_SUBMIT_H
#define UMI_SMS_SUBMIT_H

#include <sys/umi_sdx55_sms.h>
#include "umi_sms_deliver.h"

/* The alphabet and extension table are shared with SMS-DELIVER. */
static inline unsigned
umi_sms_septets(uint16_t c, uint8_t out[2])
{
	for (unsigned i = 0; i < 128; i++) {
		if (i != 0x1b && umi_gsm_alphabet[i] == c) {
			out[0] = i;
			return 1;
		}
	}
	for (unsigned i = 0; i < 128; i++) {
		if (umi_gsm_extension(i) == c) {
			out[0] = 0x1b;
			out[1] = i;
			return 2;
		}
	}
	return 0;
}

struct umi_sms_submit_plan {
	bool gsm;
	uint8_t count;
	uint16_t end[255];
};

static inline bool
umi_sms_plan(const uint16_t *text, size_t length, struct umi_sms_submit_plan *plan)
{
	*plan = (struct umi_sms_submit_plan){.gsm = true};
	if (!length || length > 255 * 153) return false;
	unsigned units = 0;
	uint8_t septets[2];
	for (size_t i = 0; i < length; i++) {
		unsigned width = umi_sms_septets(text[i], septets);
		if (!width) {
			plan->gsm = false;
			units = (unsigned)length;
			break;
		}
		units += width;
	}
	unsigned limit = plan->gsm ? (units <= 160 ? 160 : 153) : (units <= 70 ? 70 : 67);
	units = 0;
	for (size_t i = 0; i < length;) {
		unsigned characters = 1;
		if (!plan->gsm && text[i] >= 0xd800 && text[i] <= 0xdfff) {
			if (text[i] >= 0xdc00 || i + 1 == length ||
			    text[i + 1] < 0xdc00 || text[i + 1] > 0xdfff) return false;
			characters = 2;
		}
		unsigned width = plan->gsm ? umi_sms_septets(text[i], septets) : characters;
		if (units + width > limit) {
			if (plan->count == 254) return false;
			plan->end[plan->count++] = (uint16_t)i;
			units = 0;
		}
		units += width;
		i += characters;
	}
	plan->end[plan->count++] = (uint16_t)length;
	return true;
}

/* number is a decimal short code or phone number, optionally prefixed by '+'. */
static inline bool
umi_sms_submit(const char *number, const uint16_t *text, const struct umi_sms_submit_plan *plan,
    unsigned part, uint8_t reference, struct umi_sms_submission *submit)
{
	if (!plan->count || part >= plan->count) return false;
	bool international = *number == '+';
	number += international;
	size_t digits = strlen(number);
	if (!digits || digits > 20) return false;
	*submit = (struct umi_sms_submission){.version = UMI_SMS_VERSION};
	uint8_t *p = submit->pdu;
	bool multipart = plan->count > 1;
	p[1] = multipart ? 0x41 : 1; /* SMS-SUBMIT, no explicit validity period. */
	p[3] = (uint8_t)digits;
	p[4] = international ? 0x91 : 0x81;
	for (size_t i = 0; i < digits; i++) {
		if (number[i] < '0' || number[i] > '9') return false;
		p[5 + i / 2] |= (number[i] - '0') << (4 * (i % 2));
	}
	if (digits & 1) p[5 + digits / 2] |= 0xf0;
	p += 5 + (digits + 1) / 2; /* PID, DCS, UDL, then user data. */
	p[1] = plan->gsm ? 0 : 8;
	uint8_t *data = p + 3;
	if (multipart) memcpy(data, (const uint8_t[]){5, 0, 3, reference, plan->count, part + 1}, 6);
	unsigned start = part ? plan->end[part - 1] : 0, end = plan->end[part];
	unsigned bytes = multipart ? 6 : 0;
	if (plan->gsm) {
		unsigned bit = multipart ? 49 : 0; /* Six UDH octets plus one fill bit. */
		for (unsigned i = start; i < end; i++) {
			uint8_t septets[2];
			unsigned width = umi_sms_septets(text[i], septets);
			for (unsigned j = 0; j < width; j++, bit += 7) {
				data[bit / 8] |= septets[j] << (bit % 8);
				if (bit % 8 > 1) data[bit / 8 + 1] |= septets[j] >> (8 - bit % 8);
			}
		}
		p[2] = bit / 7;
		bytes = (bit + 7) / 8;
		/* Seven spare bits are padded with CR so they cannot decode as '@'. */
		if (bit % 8 == 1) data[bytes - 1] |= 0x0d << 1;
	} else {
		for (unsigned i = start; i < end; i++) {
			data[bytes++] = text[i] >> 8;
			data[bytes++] = text[i] & 0xff;
		}
		p[2] = bytes;
	}
	submit->length = (uint16_t)(data - submit->pdu + bytes);
	return true;
}

#endif
