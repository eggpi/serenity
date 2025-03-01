/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebAssemblyInstanceObject.h"
#include "WebAssemblyMemoryPrototype.h"
#include "WebAssemblyModuleConstructor.h"
#include "WebAssemblyModuleObject.h"
#include "WebAssemblyModulePrototype.h"
#include "WebAssemblyTableObject.h"
#include "WebAssemblyTablePrototype.h"
#include <AK/ScopeGuard.h>
#include <LibCore/MemoryStream.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAssembly/WebAssemblyInstanceConstructor.h>
#include <LibWeb/WebAssembly/WebAssemblyObject.h>

namespace Web::Bindings {

WebAssemblyObject::WebAssemblyObject(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, *realm.intrinsics().object_prototype())
{
    s_abstract_machine.enable_instruction_count_limit();
}

JS::ThrowCompletionOr<void> WebAssemblyObject::initialize(JS::Realm& realm)
{
    MUST_OR_THROW_OOM(Object::initialize(realm));

    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "validate", validate, 1, attr);
    define_native_function(realm, "compile", compile, 1, attr);
    define_native_function(realm, "instantiate", instantiate, 1, attr);

    auto& memory_constructor = Bindings::ensure_web_constructor<WebAssemblyMemoryPrototype>(realm, "WebAssembly.Memory"sv);
    define_direct_property("Memory", &memory_constructor, JS::Attribute::Writable | JS::Attribute::Configurable);

    auto& instance_constructor = Bindings::ensure_web_constructor<WebAssemblyInstancePrototype>(realm, "WebAssembly.Instance"sv);
    define_direct_property("Instance", &instance_constructor, JS::Attribute::Writable | JS::Attribute::Configurable);

    auto& module_constructor = Bindings::ensure_web_constructor<WebAssemblyModulePrototype>(realm, "WebAssembly.Module"sv);
    define_direct_property("Module", &module_constructor, JS::Attribute::Writable | JS::Attribute::Configurable);

    auto& table_constructor = Bindings::ensure_web_constructor<WebAssemblyTablePrototype>(realm, "WebAssembly.Table"sv);
    define_direct_property("Table", &table_constructor, JS::Attribute::Writable | JS::Attribute::Configurable);

    return {};
}

NonnullOwnPtrVector<WebAssemblyObject::CompiledWebAssemblyModule> WebAssemblyObject::s_compiled_modules;
NonnullOwnPtrVector<Wasm::ModuleInstance> WebAssemblyObject::s_instantiated_modules;
Vector<WebAssemblyObject::ModuleCache> WebAssemblyObject::s_module_caches;
WebAssemblyObject::GlobalModuleCache WebAssemblyObject::s_global_cache;
Wasm::AbstractMachine WebAssemblyObject::s_abstract_machine;

void WebAssemblyObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    for (auto& entry : s_global_cache.function_instances)
        visitor.visit(entry.value);
    for (auto& module_cache : s_module_caches) {
        for (auto& entry : module_cache.function_instances)
            visitor.visit(entry.value);
        for (auto& entry : module_cache.memory_instances)
            visitor.visit(entry.value);
        for (auto& entry : module_cache.table_instances)
            visitor.visit(entry.value);
    }
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::validate)
{
    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    // Note: There's no need to copy the bytes here as the buffer data cannot change while we're compiling the module.
    auto buffer = TRY(vm.argument(0).to_object(vm));

    // 2. Compile stableBytes as a WebAssembly module and store the results as module.
    auto maybe_module = parse_module(vm, buffer);

    // 3. If module is error, return false.
    if (maybe_module.is_error())
        return JS::Value(false);

    // Drop the module from the cache, we're never going to refer to it.
    ScopeGuard drop_from_cache {
        [&] {
            (void)s_compiled_modules.take_last();
        }
    };

    // 3 continued - our "compile" step is lazy with validation, explicitly do the validation.
    if (s_abstract_machine.validate(s_compiled_modules[maybe_module.value()].module).is_error())
        return JS::Value(false);

    // 4. Return true.
    return JS::Value(true);
}

JS::ThrowCompletionOr<size_t> parse_module(JS::VM& vm, JS::Object* buffer_object)
{
    ReadonlyBytes data;
    if (is<JS::ArrayBuffer>(buffer_object)) {
        auto& buffer = static_cast<JS::ArrayBuffer&>(*buffer_object);
        data = buffer.buffer();
    } else if (is<JS::TypedArrayBase>(buffer_object)) {
        auto& buffer = static_cast<JS::TypedArrayBase&>(*buffer_object);
        data = buffer.viewed_array_buffer()->buffer().span().slice(buffer.byte_offset(), buffer.byte_length());
    } else if (is<JS::DataView>(buffer_object)) {
        auto& buffer = static_cast<JS::DataView&>(*buffer_object);
        data = buffer.viewed_array_buffer()->buffer().span().slice(buffer.byte_offset(), buffer.byte_length());
    } else {
        return vm.throw_completion<JS::TypeError>("Not a BufferSource");
    }
    auto stream = Core::Stream::FixedMemoryStream::construct(data).release_value_but_fixme_should_propagate_errors();
    auto module_result = Wasm::Module::parse(*stream);
    if (module_result.is_error()) {
        // FIXME: Throw CompileError instead.
        return vm.throw_completion<JS::TypeError>(Wasm::parse_error_to_deprecated_string(module_result.error()));
    }

    if (auto validation_result = WebAssemblyObject::s_abstract_machine.validate(module_result.value()); validation_result.is_error()) {
        // FIXME: Throw CompileError instead.
        return vm.throw_completion<JS::TypeError>(validation_result.error().error_string);
    }

    WebAssemblyObject::s_compiled_modules.append(make<WebAssemblyObject::CompiledWebAssemblyModule>(module_result.release_value()));
    return WebAssemblyObject::s_compiled_modules.size() - 1;
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::compile)
{
    auto& realm = *vm.current_realm();

    // FIXME: This shouldn't block!
    auto buffer_or_error = vm.argument(0).to_object(vm);
    JS::Value rejection_value;
    if (buffer_or_error.is_error())
        rejection_value = *buffer_or_error.throw_completion().value();

    auto promise = JS::Promise::create(realm);
    if (!rejection_value.is_empty()) {
        promise->reject(rejection_value);
        return promise;
    }
    auto* buffer = buffer_or_error.release_value();
    auto result = parse_module(vm, buffer);
    if (result.is_error())
        promise->reject(*result.release_error().value());
    else
        promise->fulfill(MUST_OR_THROW_OOM(vm.heap().allocate<WebAssemblyModuleObject>(realm, realm, result.release_value())));
    return promise;
}

JS::ThrowCompletionOr<size_t> WebAssemblyObject::instantiate_module(JS::VM& vm, Wasm::Module const& module)
{
    Wasm::Linker linker { module };
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> resolved_imports;
    auto import_argument = vm.argument(1);
    if (!import_argument.is_undefined()) {
        auto* import_object = TRY(import_argument.to_object(vm));
        dbgln("Trying to resolve stuff because import object was specified");
        for (Wasm::Linker::Name const& import_name : linker.unresolved_imports()) {
            dbgln("Trying to resolve {}::{}", import_name.module, import_name.name);
            auto value_or_error = import_object->get(import_name.module);
            if (value_or_error.is_error())
                break;
            auto value = value_or_error.release_value();
            auto object_or_error = value.to_object(vm);
            if (object_or_error.is_error())
                break;
            auto* object = object_or_error.release_value();
            auto import_or_error = object->get(import_name.name);
            if (import_or_error.is_error())
                break;
            auto import_ = import_or_error.release_value();
            TRY(import_name.type.visit(
                [&](Wasm::TypeIndex index) -> JS::ThrowCompletionOr<void> {
                    dbgln("Trying to resolve a function {}::{}, type index {}", import_name.module, import_name.name, index.value());
                    auto& type = module.type(index);
                    // FIXME: IsCallable()
                    if (!import_.is_function())
                        return {};
                    auto& function = import_.as_function();
                    // FIXME: If this is a function created by create_native_function(),
                    //        just extract its address and resolve to that.
                    Wasm::HostFunction host_function {
                        [&](auto&, auto& arguments) -> Wasm::Result {
                            JS::MarkedVector<JS::Value> argument_values { vm.heap() };
                            for (auto& entry : arguments)
                                argument_values.append(to_js_value(vm, entry));

                            auto result_or_error = JS::call(vm, function, JS::js_undefined(), move(argument_values));
                            if (result_or_error.is_error()) {
                                return Wasm::Trap();
                            }
                            if (type.results().is_empty())
                                return Wasm::Result { Vector<Wasm::Value> {} };

                            if (type.results().size() == 1) {
                                auto value_or_error = to_webassembly_value(vm, result_or_error.release_value(), type.results().first());
                                if (value_or_error.is_error())
                                    return Wasm::Trap {};

                                return Wasm::Result { Vector<Wasm::Value> { value_or_error.release_value() } };
                            }

                            // FIXME: Multiple returns
                            TODO();
                        },
                        type
                    };
                    auto address = s_abstract_machine.store().allocate(move(host_function));
                    dbgln("Resolved to {}", address->value());
                    // FIXME: LinkError instead.
                    VERIFY(address.has_value());

                    resolved_imports.set(import_name, Wasm::ExternValue { Wasm::FunctionAddress { *address } });
                    return {};
                },
                [&](Wasm::GlobalType const& type) -> JS::ThrowCompletionOr<void> {
                    Optional<Wasm::GlobalAddress> address;
                    // https://webassembly.github.io/spec/js-api/#read-the-imports step 5.1
                    if (import_.is_number() || import_.is_bigint()) {
                        if (import_.is_number() && type.type().kind() == Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            return vm.throw_completion<JS::TypeError>("LinkError: Import resolution attempted to cast a Number to a BigInteger");
                        }
                        if (import_.is_bigint() && type.type().kind() != Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            return vm.throw_completion<JS::TypeError>("LinkError: Import resolution attempted to cast a BigInteger to a Number");
                        }
                        auto cast_value = TRY(to_webassembly_value(vm, import_, type.type()));
                        address = s_abstract_machine.store().allocate({ type.type(), false }, cast_value);
                    } else {
                        // FIXME: https://webassembly.github.io/spec/js-api/#read-the-imports step 5.2
                        //        if v implements Global
                        //            let globaladdr be v.[[Global]]

                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Invalid value for global type");
                    }

                    resolved_imports.set(import_name, Wasm::ExternValue { *address });
                    return {};
                },
                [&](Wasm::MemoryType const&) -> JS::ThrowCompletionOr<void> {
                    if (!import_.is_object() || !is<WebAssemblyMemoryObject>(import_.as_object())) {
                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Expected an instance of WebAssembly.Memory for a memory import");
                    }
                    auto address = static_cast<WebAssemblyMemoryObject const&>(import_.as_object()).address();
                    resolved_imports.set(import_name, Wasm::ExternValue { address });
                    return {};
                },
                [&](Wasm::TableType const&) -> JS::ThrowCompletionOr<void> {
                    if (!import_.is_object() || !is<WebAssemblyTableObject>(import_.as_object())) {
                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Expected an instance of WebAssembly.Table for a table import");
                    }
                    auto address = static_cast<WebAssemblyTableObject const&>(import_.as_object()).address();
                    resolved_imports.set(import_name, Wasm::ExternValue { address });
                    return {};
                },
                [&](auto const&) -> JS::ThrowCompletionOr<void> {
                    // FIXME: Implement these.
                    dbgln("Unimplemented import of non-function attempted");
                    return vm.throw_completion<JS::TypeError>("LinkError: Not Implemented");
                }));
        }
    }

    linker.link(resolved_imports);
    auto link_result = linker.finish();
    if (link_result.is_error()) {
        // FIXME: Throw a LinkError.
        StringBuilder builder;
        builder.append("LinkError: Missing "sv);
        builder.join(' ', link_result.error().missing_imports);
        return vm.throw_completion<JS::TypeError>(builder.to_deprecated_string());
    }

    auto instance_result = s_abstract_machine.instantiate(module, link_result.release_value());
    if (instance_result.is_error()) {
        // FIXME: Throw a LinkError instead.
        return vm.throw_completion<JS::TypeError>(instance_result.error().error);
    }

    s_instantiated_modules.append(instance_result.release_value());
    s_module_caches.empend();
    return s_instantiated_modules.size() - 1;
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::instantiate)
{
    auto& realm = *vm.current_realm();

    // FIXME: This shouldn't block!
    auto buffer_or_error = vm.argument(0).to_object(vm);
    auto promise = JS::Promise::create(realm);
    bool should_return_module = false;
    if (buffer_or_error.is_error()) {
        auto rejection_value = *buffer_or_error.throw_completion().value();
        promise->reject(rejection_value);
        return promise;
    }
    auto* buffer = buffer_or_error.release_value();

    Wasm::Module const* module { nullptr };
    if (is<JS::ArrayBuffer>(buffer) || is<JS::TypedArrayBase>(buffer)) {
        auto result = parse_module(vm, buffer);
        if (result.is_error()) {
            promise->reject(*result.release_error().value());
            return promise;
        }
        module = &WebAssemblyObject::s_compiled_modules.at(result.release_value()).module;
        should_return_module = true;
    } else if (is<WebAssemblyModuleObject>(buffer)) {
        module = &static_cast<WebAssemblyModuleObject*>(buffer)->module();
    } else {
        auto error = JS::TypeError::create(realm, DeprecatedString::formatted("{} is not an ArrayBuffer or a Module", buffer->class_name()));
        promise->reject(error);
        return promise;
    }
    VERIFY(module);

    auto result = instantiate_module(vm, *module);
    if (result.is_error()) {
        promise->reject(*result.release_error().value());
    } else {
        auto instance_object = MUST_OR_THROW_OOM(vm.heap().allocate<WebAssemblyInstanceObject>(realm, realm, result.release_value()));
        if (should_return_module) {
            auto object = JS::Object::create(realm, nullptr);
            object->define_direct_property("module", MUST_OR_THROW_OOM(vm.heap().allocate<WebAssemblyModuleObject>(realm, realm, s_compiled_modules.size() - 1)), JS::default_attributes);
            object->define_direct_property("instance", instance_object, JS::default_attributes);
            promise->fulfill(object);
        } else {
            promise->fulfill(instance_object);
        }
    }
    return promise;
}

JS::Value to_js_value(JS::VM& vm, Wasm::Value& wasm_value)
{
    auto& realm = *vm.current_realm();
    switch (wasm_value.type().kind()) {
    case Wasm::ValueType::I64:
        return realm.heap().allocate<JS::BigInt>(realm, ::Crypto::SignedBigInteger { wasm_value.to<i64>().value() }).release_allocated_value_but_fixme_should_propagate_errors();
    case Wasm::ValueType::I32:
        return JS::Value(wasm_value.to<i32>().value());
    case Wasm::ValueType::F64:
        return JS::Value(wasm_value.to<double>().value());
    case Wasm::ValueType::F32:
        return JS::Value(static_cast<double>(wasm_value.to<float>().value()));
    case Wasm::ValueType::FunctionReference:
        // FIXME: What's the name of a function reference that isn't exported?
        return create_native_function(vm, wasm_value.to<Wasm::Reference::Func>().value().address, "FIXME_IHaveNoIdeaWhatThisShouldBeCalled");
    case Wasm::ValueType::NullFunctionReference:
        return JS::js_null();
    case Wasm::ValueType::ExternReference:
    case Wasm::ValueType::NullExternReference:
        TODO();
    }
    VERIFY_NOT_REACHED();
}

JS::ThrowCompletionOr<Wasm::Value> to_webassembly_value(JS::VM& vm, JS::Value value, Wasm::ValueType const& type)
{
    static ::Crypto::SignedBigInteger two_64 = "1"_sbigint.shift_left(64);

    switch (type.kind()) {
    case Wasm::ValueType::I64: {
        auto bigint = TRY(value.to_bigint(vm));
        auto value = bigint->big_integer().divided_by(two_64).remainder;
        VERIFY(value.unsigned_value().trimmed_length() <= 2);
        i64 integer = static_cast<i64>(value.unsigned_value().to_u64());
        if (value.is_negative())
            integer = -integer;
        return Wasm::Value { integer };
    }
    case Wasm::ValueType::I32: {
        auto _i32 = TRY(value.to_i32(vm));
        return Wasm::Value { static_cast<i32>(_i32) };
    }
    case Wasm::ValueType::F64: {
        auto number = TRY(value.to_double(vm));
        return Wasm::Value { static_cast<double>(number) };
    }
    case Wasm::ValueType::F32: {
        auto number = TRY(value.to_double(vm));
        return Wasm::Value { static_cast<float>(number) };
    }
    case Wasm::ValueType::FunctionReference:
    case Wasm::ValueType::NullFunctionReference: {
        if (value.is_null())
            return Wasm::Value { Wasm::ValueType(Wasm::ValueType::NullExternReference), 0ull };

        if (value.is_function()) {
            auto& function = value.as_function();
            for (auto& entry : WebAssemblyObject::s_global_cache.function_instances) {
                if (entry.value == &function)
                    return Wasm::Value { Wasm::Reference { Wasm::Reference::Func { entry.key } } };
            }
        }

        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Exported function");
    }
    case Wasm::ValueType::ExternReference:
    case Wasm::ValueType::NullExternReference:
        TODO();
    }

    VERIFY_NOT_REACHED();
}

JS::NativeFunction* create_native_function(JS::VM& vm, Wasm::FunctionAddress address, DeprecatedString const& name)
{
    auto& realm = *vm.current_realm();
    Optional<Wasm::FunctionType> type;
    WebAssemblyObject::s_abstract_machine.store().get(address)->visit([&](auto const& value) { type = value.type(); });
    if (auto entry = WebAssemblyObject::s_global_cache.function_instances.get(address); entry.has_value())
        return *entry;

    auto function = JS::NativeFunction::create(
        realm,
        name,
        [address, type = type.release_value()](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto& realm = *vm.current_realm();
            Vector<Wasm::Value> values;
            values.ensure_capacity(type.parameters().size());

            // Grab as many values as needed and convert them.
            size_t index = 0;
            for (auto& type : type.parameters())
                values.append(TRY(to_webassembly_value(vm, vm.argument(index++), type)));

            auto result = WebAssemblyObject::s_abstract_machine.invoke(address, move(values));
            // FIXME: Use the convoluted mapping of errors defined in the spec.
            if (result.is_trap())
                return vm.throw_completion<JS::TypeError>(DeprecatedString::formatted("Wasm execution trapped (WIP): {}", result.trap().reason));

            if (result.values().is_empty())
                return JS::js_undefined();

            if (result.values().size() == 1)
                return to_js_value(vm, result.values().first());

            Vector<JS::Value> result_values;
            for (auto& entry : result.values())
                result_values.append(to_js_value(vm, entry));

            return JS::Value(JS::Array::create_from(realm, result_values));
        });

    WebAssemblyObject::s_global_cache.function_instances.set(address, function);
    return function;
}

WebAssemblyMemoryObject::WebAssemblyMemoryObject(JS::Realm& realm, Wasm::MemoryAddress address)
    : Object(ConstructWithPrototypeTag::Tag, Bindings::ensure_web_prototype<WebAssemblyMemoryPrototype>(realm, "WebAssembly.Memory"))
    , m_address(address)
{
}

}
