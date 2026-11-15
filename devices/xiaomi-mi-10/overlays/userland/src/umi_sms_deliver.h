#ifndef UMI_SMS_DELIVER_H
#define UMI_SMS_DELIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct umi_sms_deliver {
	uint16_t sender[24], text[160];
	uint8_t sender_length, text_length;
	uint8_t timestamp[7];
	uint16_t reference;
	uint8_t parts, part;
	bool long_reference, silent;
};

/* 3GPP TS 23.038 default alphabet, in septet order. */
static const uint16_t umi_gsm_alphabet[128] = {
	0x40,0xa3,0x24,0xa5,0xe8,0xe9,0xf9,0xec,0xf2,0xc7,0xa,0xd8,0xf8,0xd,0xc5,0xe5,
	0x394,0x5f,0x3a6,0x393,0x39b,0x3a9,0x3a0,0x3a8,0x3a3,0x398,0x39e,0x1b,0xc6,0xe6,0xdf,0xc9,
	' ','!','"','#',0xa4,'%','&','\'','(',')','*','+',',','-','.','/',
	'0','1','2','3','4','5','6','7','8','9',':',';','<','=','>','?',
	0xa1,'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O',
	'P','Q','R','S','T','U','V','W','X','Y','Z',0xc4,0xd6,0xd1,0xdc,0xa7,
	0xbf,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
	'p','q','r','s','t','u','v','w','x','y','z',0xe4,0xf6,0xf1,0xfc,0xe0
};

static inline uint16_t
umi_gsm_extension(unsigned value)
{
	switch (value) {
	case 0x0a: return 0x0c;
	case 0x14: return '^';
	case 0x28: return '{';
	case 0x29: return '}';
	case 0x2f: return '\\';
	case 0x3c: return '[';
	case 0x3d: return '~';
	case 0x3e: return ']';
	case 0x40: return '|';
	case 0x65: return 0x20ac;
	default: return ' ';
	}
}

static inline uint8_t
umi_gsm_unpack(const uint8_t *p, size_t bytes, unsigned bit, unsigned septets, uint16_t *out)
{
	unsigned count = 0;
	bool escaped = false;
	for (unsigned i = 0; i < septets; i++, bit += 7) {
		unsigned value = p[bit / 8] >> (bit % 8);
		if (bit % 8 > 1 && bit / 8 + 1 < bytes) value |= (unsigned)p[bit / 8 + 1] << (8 - bit % 8);
		value &= 0x7f;
		if (escaped) out[count++] = umi_gsm_extension(value);
		else if (value != 0x1b) out[count++] = umi_gsm_alphabet[value];
		escaped = !escaped && value == 0x1b;
	}
	if (escaped) out[count++] = ' ';
	return (uint8_t)count;
}

static inline bool
umi_sms_udh(const uint8_t *p, size_t end, struct umi_sms_deliver *message)
{
	for (size_t i = 1; i < end;) {
		if (end - i < 2) return false;
		unsigned tag = p[i++], n = p[i++];
		if (n > end - i) return false;
		if (tag == 0 || tag == 8) {
			if (message->parts || n != (tag == 0 ? 3u : 4u)) return false;
			message->long_reference = tag == 8;
			message->reference = tag == 0 ? p[i] : (uint16_t)(p[i] << 8 | p[i + 1]);
			message->parts = p[i + n - 2];
			message->part = p[i + n - 1];
			if (!message->part || message->part > message->parts) return false;
		} else if (tag == 4 || tag == 5 || ((tag == 0x24 || tag == 0x25) && (n != 1 || p[i] != 0))) {
			return false;
		}
		i += n;
	}
	return true;
}

static inline bool
umi_sms_deliver_decode(const uint8_t *p, size_t n, struct umi_sms_deliver *message)
{
	memset(message, 0, sizeof(*message));
	if (n < 13 || (p[0] & 3) != 0 || !p[1] || p[1] > 20 || !(p[2] & 0x80)) return false;
	unsigned address = (p[1] + 1) / 2, ton = (p[2] >> 4) & 7;
	if (n < 13 + address) return false;
	if (ton == 5) {
		message->sender_length = umi_gsm_unpack(p + 3, address, 0, p[1] * 4 / 7, message->sender);
	} else {
		static const char dial[] = "0123456789*#abc";
		if (ton == 1) message->sender[message->sender_length++] = '+';
		for (unsigned i = 0; i < p[1]; i++) {
			unsigned digit = p[3 + i / 2] >> (4 * (i % 2)) & 0xf;
			if (digit >= sizeof(dial) - 1) return false;
			message->sender[message->sender_length++] = dial[digit];
		}
		if ((p[1] & 1) && (p[2 + address] >> 4) != 0xf) return false;
	}
	if (!message->sender_length) return false;
	unsigned pid = p[3 + address], dcs = p[4 + address], udl = p[12 + address], alphabet;
	message->silent = pid == 0x40;
	if ((dcs & 0xc0) == 0) {
		if (dcs & 0x20) return false;
		alphabet = dcs >> 2 & 3;
	} else if ((dcs & 0xf0) == 0xf0) alphabet = dcs >> 2 & 1;
	else return false;
	if (alphabet != 0 && alphabet != 2) return false;
	memcpy(message->timestamp, p + 5 + address, sizeof(message->timestamp));
	unsigned bytes = alphabet == 0 ? (udl * 7 + 7) / 8 : udl;
	if (bytes > 140 || n != 13 + address + bytes) return false;
	bool header = p[0] & 0x40;
	p += 13 + address;
	unsigned skip = header && bytes ? p[0] + 1u : 0;
	if (header && (!skip || skip > bytes || !umi_sms_udh(p, skip, message))) return false;
	if (alphabet == 0) {
		unsigned header_units = (skip * 8 + 6) / 7;
		if (udl < header_units) return false;
		message->text_length = umi_gsm_unpack(p, bytes, header_units * 7, udl - header_units, message->text);
	} else {
		if ((bytes - skip) & 1) return false;
		for (unsigned i = skip; i < bytes; i += 2)
			message->text[message->text_length++] = (uint16_t)(p[i] << 8 | p[i + 1]);
	}
	return true;
}

#endif
