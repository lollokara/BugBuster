#pragma once
#include "hub_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bounded JSON writer. Once `over` is set nothing more is written. Always NUL-terminated. */
typedef struct { char *p; size_t cap, len; bool over; } hub_jw_t;

void hub_jw_init(hub_jw_t *w, char *buf, size_t cap);
void hub_jw_raw(hub_jw_t *w, const char *s);
void hub_jw_fmt(hub_jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** "..." with JSON escaping; bytes that are not valid UTF-8 become '?'. */
void hub_jw_str(hub_jw_t *w, const char *s, size_t n);
bool hub_jw_ok(const hub_jw_t *w);

/** {"ts":<s.mmm>,"source":"s3|mpy|p4","level":"E","tag":"..","msg":".."}. On overflow the writer is rolled back. */
bool hub_json_log_entry(hub_jw_t *w, uint64_t ts_ms, const hub_log_rec_t *r);
/** [ts,v,i,soc,null,state(,vmin,vmax,imin,imax)]. On overflow the writer is rolled back. */
bool hub_json_sample(hub_jw_t *w, const hub_sample_t *s);

#ifdef __cplusplus
}
#endif
