/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/DeprecatedString.h>
#include <AK/Forward.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <AK/Try.h>
#include <AK/TypeList.h>
#include <AK/Variant.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibCore/Stream.h>
#include <LibIPC/Concepts.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>

namespace IPC {

template<typename T>
inline ErrorOr<T> decode(Decoder&)
{
    static_assert(DependentFalse<T>, "Base IPC::decoder() instantiated");
    VERIFY_NOT_REACHED();
}

class Decoder {
public:
    Decoder(Core::Stream::Stream& stream, Core::Stream::LocalSocket& socket)
        : m_stream(stream)
        , m_socket(socket)
    {
    }

    template<typename T>
    ErrorOr<T> decode();

    template<typename T>
    ErrorOr<void> decode_into(T& value)
    {
        value = TRY(m_stream.read_value<T>());
        return {};
    }

    ErrorOr<void> decode_into(Bytes bytes)
    {
        TRY(m_stream.read_entire_buffer(bytes));
        return {};
    }

    ErrorOr<size_t> decode_size();

    Core::Stream::LocalSocket& socket() { return m_socket; }

private:
    Core::Stream::Stream& m_stream;
    Core::Stream::LocalSocket& m_socket;
};

template<Arithmetic T>
ErrorOr<T> decode(Decoder& decoder)
{
    T value { 0 };
    TRY(decoder.decode_into(value));
    return value;
}

template<Enum T>
ErrorOr<T> decode(Decoder& decoder)
{
    auto value = TRY(decoder.decode<UnderlyingType<T>>());
    return static_cast<T>(value);
}

template<>
ErrorOr<DeprecatedString> decode(Decoder&);

template<>
ErrorOr<ByteBuffer> decode(Decoder&);

template<>
ErrorOr<JsonValue> decode(Decoder&);

template<>
ErrorOr<URL> decode(Decoder&);

template<>
ErrorOr<Dictionary> decode(Decoder&);

template<>
ErrorOr<File> decode(Decoder&);

template<>
ErrorOr<Empty> decode(Decoder&);

template<Concepts::Vector T>
ErrorOr<T> decode(Decoder& decoder)
{
    T vector;

    auto size = TRY(decoder.decode_size());
    TRY(vector.try_ensure_capacity(size));

    for (size_t i = 0; i < size; ++i) {
        auto value = TRY(decoder.decode<typename T::ValueType>());
        vector.template unchecked_append(move(value));
    }

    return vector;
}

template<Concepts::HashMap T>
ErrorOr<T> decode(Decoder& decoder)
{
    T hashmap;

    auto size = TRY(decoder.decode_size());
    TRY(hashmap.try_ensure_capacity(size));

    for (size_t i = 0; i < size; ++i) {
        auto key = TRY(decoder.decode<typename T::KeyType>());
        auto value = TRY(decoder.decode<typename T::ValueType>());
        TRY(hashmap.try_set(move(key), move(value)));
    }

    return hashmap;
}

template<Concepts::SharedSingleProducerCircularQueue T>
ErrorOr<T> decode(Decoder& decoder)
{
    auto anon_file = TRY(decoder.decode<IPC::File>());
    return T::create(anon_file.take_fd());
}

template<Concepts::Optional T>
ErrorOr<T> decode(Decoder& decoder)
{
    if (auto has_value = TRY(decoder.decode<bool>()); !has_value)
        return T {};
    return T { TRY(decoder.decode<typename T::ValueType>()) };
}

namespace Detail {

template<Concepts::Variant T, size_t Index = 0>
ErrorOr<T> decode_variant(Decoder& decoder, size_t index)
{
    using ElementList = TypeList<T>;

    if constexpr (Index < ElementList::size) {
        if (index == Index) {
            using ElementType = typename ElementList::template Type<Index>;
            return T { TRY(decoder.decode<ElementType>()) };
        }

        return decode_variant<T, Index + 1>(decoder, index);
    } else {
        VERIFY_NOT_REACHED();
    }
}

}

template<Concepts::Variant T>
ErrorOr<T> decode(Decoder& decoder)
{
    auto index = TRY(decoder.decode<typename T::IndexType>());
    return Detail::decode_variant<T>(decoder, index);
}

// This must be last so that it knows about the above specializations.
template<typename T>
ErrorOr<T> Decoder::decode()
{
    return IPC::decode<T>(*this);
}

}
