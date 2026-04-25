#pragma once
// =============================================================================
// bbp_codec.h — Inline binary encode/decode helpers for BBP payloads
// Extracted from bbp.cpp so adapters and cmd handlers share one copy.
// =============================================================================
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

// Write helpers (little-endian)
static inline void bbp_put_u8(uint8_t *buf, size_t *pos, uint8_t v)   { buf[(*pos)++] = v; }
static inline void bbp_put_u16(uint8_t *buf, size_t *pos, uint16_t v) { buf[(*pos)++]=(uint8_t)(v&0xFF); buf[(*pos)++]=(uint8_t)(v>>8); }
static inline void bbp_put_u24(uint8_t *buf, size_t *pos, uint32_t v) { buf[(*pos)++]=(uint8_t)(v&0xFF); buf[(*pos)++]=(uint8_t)((v>>8)&0xFF); buf[(*pos)++]=(uint8_t)((v>>16)&0xFF); }
static inline void bbp_put_u32(uint8_t *buf, size_t *pos, uint32_t v) { buf[(*pos)++]=(uint8_t)(v&0xFF); buf[(*pos)++]=(uint8_t)((v>>8)&0xFF); buf[(*pos)++]=(uint8_t)((v>>16)&0xFF); buf[(*pos)++]=(uint8_t)((v>>24)&0xFF); }
static inline void bbp_put_f32(uint8_t *buf, size_t *pos, float v)    { uint32_t b; memcpy(&b,&v,4); bbp_put_u32(buf,pos,b); }
static inline void bbp_put_bool(uint8_t *buf, size_t *pos, bool v)    { buf[(*pos)++] = v ? 0x01 : 0x00; }

// Read helpers (little-endian)
static inline uint8_t  bbp_get_u8(const uint8_t *buf, size_t *pos)  { return buf[(*pos)++]; }
static inline uint16_t bbp_get_u16(const uint8_t *buf, size_t *pos) { uint16_t v=(uint16_t)buf[*pos]|((uint16_t)buf[*pos+1]<<8); *pos+=2; return v; }
static inline uint32_t bbp_get_u32(const uint8_t *buf, size_t *pos) { uint32_t v=(uint32_t)buf[*pos]|((uint32_t)buf[*pos+1]<<8)|((uint32_t)buf[*pos+2]<<16)|((uint32_t)buf[*pos+3]<<24); *pos+=4; return v; }
static inline float    bbp_get_f32(const uint8_t *buf, size_t *pos) { uint32_t b=bbp_get_u32(buf,pos); float v; memcpy(&v,&b,4); return v; }
static inline bool     bbp_get_bool(const uint8_t *buf, size_t *pos){ return buf[(*pos)++]!=0; }
