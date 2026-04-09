/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#ifndef IOTDB_DATE_H
#define IOTDB_DATE_H

#include <string>

namespace iotdb {

/**
 * Calendar date (year, month, day). Replaces former boost::gregorian::date in the public API.
 */
struct Date {
    int year{};
    int month{};
    int day{};

    Date() = default;

    Date(int y, int m, int d) : year(y), month(m), day(d) {
    }

    bool isNotADate() const {
        return year == 0 && month == 0 && day == 0;
    }

    static Date not_a_date() {
        return Date(0, 0, 0);
    }
};

inline bool operator==(const Date& a, const Date& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

inline bool operator!=(const Date& a, const Date& b) {
    return !(a == b);
}

std::string dateToIsoExtendedString(const Date& date);

}  // namespace iotdb

#endif
