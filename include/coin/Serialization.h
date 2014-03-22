/* -*-c++-*- libcoin - Copyright (C) 2012 Michael Gronager
 *
 * libcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * libcoin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libcoin.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SERIALIZATION_H
#define SERIALIZATION_H

#include <vector>
#include <set>
#include <iostream>
#include <stdint.h>

#include <boost/type_traits.hpp>

template <typename T>
class binary {
public:
    binary(T& t) : _t(t) {}
    
    template <typename TT>
    friend std::istream& operator>>(std::istream& is, const binary<TT>& bin);
private:
    T& _t;
};

template <typename T>
std::istream& operator>>(std::istream& is, const binary<T>& bin) {
    return is.read((char*)&bin._t, sizeof(T));
}

template <typename T>
class const_binary {
public:
    const_binary(const T& t) : _t(t) {}
    
    template <typename TT>
    friend std::ostream& operator<<(std::ostream& os, const const_binary<TT>& bin);
    
private:
    const T& _t;
};

template <typename T>
std::ostream& operator<<(std::ostream& os, const const_binary<T>& bin) {
    return os.write((const char*)&bin._t, sizeof(T));
}

class const_varint {
public:
    const_varint(const uint64_t& var_int) : _int(var_int) {}
    
    friend std::ostream& operator<<(std::ostream& os, const const_varint& var);
    
private:
    const uint64_t& _int;
};

class varint {
public:
    varint(uint64_t& var_int) : _int(var_int) {}
    
    friend std::istream& operator>>(std::istream& is, const varint& var);
private:
    uint64_t& _int;
};

class const_varstr {
public:
    const_varstr(const std::string& str) : _str(str) {}
    
    friend std::ostream& operator<<(std::ostream& os, const const_varstr& var);
    
private:
    const std::string& _str;
};

class varstr {
public:
    varstr(std::string& str) : _str(str) {}
    
    friend std::istream& operator>>(std::istream& is, const varstr& var);
private:
    std::string& _str;
};

template <class T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& array) {
    os << const_varint(array.size());
    for (typename std::vector<T>::const_iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            os << *t;
        else
            os << const_binary<T>(*t);
    }
    return os;
}

template <class T>
std::istream& operator>>(std::istream& is, std::vector<T>& array) {
    uint64_t size = 0;
    is >> varint(size);
    array.resize(size);
    for (typename std::vector<T>::iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            is >> *t;
        else
            is >> binary<T>(*t);
    }
    return is;
}

template <class T>
std::ostream& operator<<(std::ostream& os, const std::set<T>& array) {
    os << const_varint(array.size());
    for (typename std::set<T>::const_iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            os << *t;
        else
            os << const_binary<T>(*t);
    }
    return os;
}

template <class T>
std::istream& operator>>(std::istream& is, std::set<T>& array) {
    uint64_t size = 0;
    is >> varint(size);
    for (size_t i = 0; i < size; ++i) {
        T t;
        if (boost::is_class<T>())
            is >> t;
        else
            is >> binary<T>(t);
        array.insert(t);
    }
    return is;
}

template <typename T>
std::string serialize(const T& t) {
    std::ostringstream os;
    if (boost::is_class<T>())
        os << t;
    else
        os << const_binary<T>(t);
    return os.str();
}

inline std::string serialize(const std::string& t) {
    std::ostringstream os;
    os << const_varstr(t);
    return os.str();
}

template <typename T>
T deserialize(const std::string& s) {
    std::istringstream is(s);
    T t;
    if (boost::is_class<T>())
        is >> t;
    else
        is >> binary<T>(t);
    return t;
}

inline std::string deserialize(const std::string& s) {
    std::istringstream is(s);
    std::string t;
    is >> varstr(t);
    return t;
}

#endif // SERIALIZATION_H