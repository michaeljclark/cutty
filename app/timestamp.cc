#include <stdio.h>
#include <time.h>

#include "timestamp.h"


static inline void to_tty_timestamp(tty_timestamp *tv, const struct timespec *tp)
{
    tv->vec[0] = (int)tp->tv_nsec;
    tv->vec[1] = (int)tp->tv_sec;
    tv->vec[2] = (int)(tp->tv_sec >> 32);
}

static inline void from_tty_timestamp(struct timespec *tp, const tty_timestamp *tv)
{
    tp->tv_nsec = tv->vec[0];
    tp->tv_sec = (llong)tv->vec[1]
               | ((llong)tv->vec[2] << 32);
}

int timestamp_gettime(tty_clock_id clock_id, tty_timestamp *tv)
{
    struct timespec ts = { 0 };
    int ret;

    ret = clock_gettime(CLOCK_REALTIME, &ts);
    to_tty_timestamp(tv, &ts);

    return ret;
}

int timestamp_isostring(tty_timestamp_fmt format_id,
    char *buf, size_t buf_len, const tty_timestamp *tv)
{
    struct tm t;
    struct timespec ts = { 0 };
    int tlen, slen;
    const char* datetime_format;
    const char* subsecond_format;
    llong subsecond_div;

    if (tv) from_tty_timestamp(&ts, tv);

    tzset();

    if (localtime_r(&(ts.tv_sec), &t) == NULL) {
        return -1;
    }

    switch (format_id) {
    case tty_timestamp_fmt_iso_date:
        datetime_format = "%F";
        tlen = 10; // YYYY/mm/dd
        break;
    case tty_timestamp_fmt_iso_datetime:
    case tty_timestamp_fmt_iso_datetime_ms:
    case tty_timestamp_fmt_iso_datetime_us:
    case tty_timestamp_fmt_iso_datetime_ns:
        datetime_format = "%F %T";
        tlen = 19; // YYYY/mm/dd hh:mm:ss
        break;
    case tty_timestamp_fmt_iso_time:
    case tty_timestamp_fmt_iso_time_ms:
    case tty_timestamp_fmt_iso_time_us:
    case tty_timestamp_fmt_iso_time_ns:
        datetime_format = "%T";
        tlen = 8; // hh:mm:ss
        break;
    }

    if (buf != NULL && (tlen = strftime(buf, buf_len, datetime_format, &t)) == 0) {
        return 0;
    }

    if (buf && tlen > 0) buf += tlen;
    if (tlen > 0) buf_len -= tlen;

    switch (format_id) {
    case tty_timestamp_fmt_iso_date:
    case tty_timestamp_fmt_iso_datetime:
    case tty_timestamp_fmt_iso_time:
        subsecond_format = NULL;
        subsecond_div = 0;
        slen = 0;
        break;
    case tty_timestamp_fmt_iso_datetime_ms:
    case tty_timestamp_fmt_iso_time_ms:
        subsecond_format = ".%03ld";
        subsecond_div = 1000000;
        slen = 4;
        break;
    case tty_timestamp_fmt_iso_datetime_us:
    case tty_timestamp_fmt_iso_time_us:
        subsecond_format = ".%06ld";
        subsecond_div = 1000;
        slen = 7;
        break;
    case tty_timestamp_fmt_iso_datetime_ns:
    case tty_timestamp_fmt_iso_time_ns:
        subsecond_format = ".%09ld";
        subsecond_div = 1;
        slen = 10;
        break;
    }

    if (subsecond_format) {
        if (buf != NULL && (slen = snprintf(buf, buf_len, subsecond_format,
            ts.tv_nsec / subsecond_div)) >= buf_len) {
            return 0;
        }
    } else {
        slen = 0;
    }

    return tlen + slen;
}
