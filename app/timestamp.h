#pragma once

typedef long long llong;

struct tty_timestamp
{
    int vec[3];
};

enum tty_clock_id
{
    tty_clock_realtime
};

enum tty_timestamp_fmt
{
    tty_timestamp_fmt_iso_date,
    tty_timestamp_fmt_iso_datetime,
    tty_timestamp_fmt_iso_datetime_ms,
    tty_timestamp_fmt_iso_datetime_us,
    tty_timestamp_fmt_iso_datetime_ns,
    tty_timestamp_fmt_iso_time,
    tty_timestamp_fmt_iso_time_ms,
    tty_timestamp_fmt_iso_time_us,
    tty_timestamp_fmt_iso_time_ns,
};

int timestamp_gettime(tty_clock_id clock_id, tty_timestamp *tv);
int timestamp_isostring(tty_timestamp_fmt format_id,
    char *buf, size_t buf_len, const tty_timestamp *tv);
