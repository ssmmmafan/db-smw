#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include "errors.h"

inline bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int days_in_month(int year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month];
}

inline int64_t pack_datetime(int year, int month, int day, int hour, int minute, int second) {
    return (static_cast<int64_t>(year) << 48) |
           (static_cast<int64_t>(month) << 40) |
           (static_cast<int64_t>(day) << 32) |
           (static_cast<int64_t>(hour) << 24) |
           (static_cast<int64_t>(minute) << 16) |
           (static_cast<int64_t>(second) << 8);
}

inline void unpack_datetime(int64_t val, int &year, int &month, int &day, int &hour, int &minute, int &second) {
    year = static_cast<int>((val >> 48) & 0xFFFF);
    month = static_cast<int>((val >> 40) & 0xFF);
    day = static_cast<int>((val >> 32) & 0xFF);
    hour = static_cast<int>((val >> 24) & 0xFF);
    minute = static_cast<int>((val >> 16) & 0xFF);
    second = static_cast<int>((val >> 8) & 0xFF);
}

inline std::string format_datetime(int64_t val) {
    int year, month, day, hour, minute, second;
    unpack_datetime(val, year, month, day, hour, minute, second);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return std::string(buf);
}

inline int64_t parse_datetime(const std::string &s) {
    if (s.size() != 19) {
        throw InvalidDateTimeError();
    }
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') {
        throw InvalidDateTimeError();
    }
    for (int i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            throw InvalidDateTimeError();
        }
    }

    int year = std::stoi(s.substr(0, 4));
    int month = std::stoi(s.substr(5, 2));
    int day = std::stoi(s.substr(8, 2));
    int hour = std::stoi(s.substr(11, 2));
    int minute = std::stoi(s.substr(14, 2));
    int second = std::stoi(s.substr(17, 2));

    if (year < 1000 || year > 9999) {
        throw InvalidDateTimeError();
    }
    if (month < 1 || month > 12) {
        throw InvalidDateTimeError();
    }
    if (day < 1 || day > days_in_month(year, month)) {
        throw InvalidDateTimeError();
    }
    if (hour > 23) {
        throw InvalidDateTimeError();
    }
    if (minute > 59) {
        throw InvalidDateTimeError();
    }
    if (second > 59) {
        throw InvalidDateTimeError();
    }

    return pack_datetime(year, month, day, hour, minute, second);
}
