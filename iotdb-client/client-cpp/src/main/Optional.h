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
#ifndef IOTDB_OPTIONAL_H
#define IOTDB_OPTIONAL_H

#include <cstddef>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace iotdb {

struct nullopt_t {};
static const nullopt_t nullopt = nullopt_t();

template <typename T>
class Optional {
private:
    typedef typename std::aligned_storage<sizeof(T), std::alignment_of<T>::value>::type Storage;
    Storage storage_;
    bool engaged_;

    T* ptr() {
        return reinterpret_cast<T*>(&storage_);
    }

    const T* ptr() const {
        return reinterpret_cast<const T*>(&storage_);
    }

public:
    Optional() : engaged_(false) {
    }

    Optional(nullopt_t) : engaged_(false) {
    }

    Optional(const T& v) : engaged_(true) {
        new (&storage_) T(v);
    }

    Optional(const Optional& other) : engaged_(other.engaged_) {
        if (engaged_) {
            new (&storage_) T(*other.ptr());
        }
    }

    ~Optional() {
        if (engaged_) {
            ptr()->~T();
        }
    }

    Optional& operator=(nullopt_t) {
        reset();
        return *this;
    }

    Optional& operator=(const T& v) {
        if (engaged_) {
            *ptr() = v;
        } else {
            new (&storage_) T(v);
            engaged_ = true;
        }
        return *this;
    }

    Optional& operator=(const Optional& other) {
        if (this == &other) {
            return *this;
        }
        if (other.engaged_) {
            if (engaged_) {
                *ptr() = *other.ptr();
            } else {
                new (&storage_) T(*other.ptr());
                engaged_ = true;
            }
        } else {
            reset();
        }
        return *this;
    }

    void reset() {
        if (engaged_) {
            ptr()->~T();
            engaged_ = false;
        }
    }

    bool has_value() const {
        return engaged_;
    }

    bool is_initialized() const {
        return engaged_;
    }

    explicit operator bool() const {
        return engaged_;
    }

    T& value() {
        if (!engaged_) {
            throw std::logic_error("iotdb::Optional: no value");
        }
        return *ptr();
    }

    const T& value() const {
        if (!engaged_) {
            throw std::logic_error("iotdb::Optional: no value");
        }
        return *ptr();
    }

    T& operator*() {
        return value();
    }

    const T& operator*() const {
        return value();
    }

    T* operator->() {
        return &value();
    }

    const T* operator->() const {
        return &value();
    }
};

}  // namespace iotdb

#endif
