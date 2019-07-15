/*
 * Generated by asn1c-0.9.22 (http://lionet.info/asn1c)
 * From ASN.1 module "RRLP-Components"
 * 	found in "../rrlp-components.asn"
 */

#ifndef	_BSICAndCarrier_H_
#define	_BSICAndCarrier_H_


#include <asn_application.h>

/* Including external dependencies */
#include "BCCHCarrier.h"
#include "BSIC.h"
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BSICAndCarrier */
typedef struct BSICAndCarrier {
	BCCHCarrier_t	 carrier;
	BSIC_t	 bsic;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} BSICAndCarrier_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_BSICAndCarrier;

#ifdef __cplusplus
}
#endif

#endif	/* _BSICAndCarrier_H_ */
#include <asn_internal.h>