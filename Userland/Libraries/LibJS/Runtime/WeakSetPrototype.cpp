/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/WeakSetPrototype.h>

namespace JS {

WeakSetPrototype::WeakSetPrototype(Realm& realm)
    : PrototypeObject(*realm.intrinsics().object_prototype())
{
}

ThrowCompletionOr<void> WeakSetPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    MUST_OR_THROW_OOM(Base::initialize(realm));
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.delete_, delete_, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);

    // 24.4.3.5 WeakSet.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-weakset.prototype-@@tostringtag
    define_direct_property(*vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.WeakSet.as_string()), Attribute::Configurable);

    return {};
}

// 24.4.3.1 WeakSet.prototype.add ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.add
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::add)
{
    auto* weak_set = TRY(typed_this_object(vm));
    auto value = vm.argument(0);
    if (!can_be_held_weakly(value))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, value.to_string_without_side_effects());
    weak_set->values().set(&value.as_cell(), AK::HashSetExistingEntryBehavior::Keep);
    return weak_set;
}

// 24.4.3.3 WeakSet.prototype.delete ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.delete
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::delete_)
{
    auto* weak_set = TRY(typed_this_object(vm));
    auto value = vm.argument(0);
    if (!can_be_held_weakly(value))
        return Value(false);
    return Value(weak_set->values().remove(&value.as_cell()));
}

// 24.4.3.4 WeakSet.prototype.has ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.has
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::has)
{
    auto* weak_set = TRY(typed_this_object(vm));
    auto value = vm.argument(0);
    if (!can_be_held_weakly(value))
        return Value(false);
    auto& values = weak_set->values();
    return Value(values.find(&value.as_cell()) != values.end());
}

}
