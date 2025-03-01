/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Image.h"
#include "Layer.h"
#include "Selection.h"
#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <LibGUI/Painter.h>
#include <LibGfx/BMPWriter.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PNGWriter.h>
#include <LibGfx/QOIWriter.h>
#include <LibImageDecoderClient/Client.h>
#include <stdio.h>

namespace PixelPaint {

ErrorOr<NonnullRefPtr<Image>> Image::create_with_size(Gfx::IntSize size)
{
    VERIFY(!size.is_empty());

    if (size.width() > 16384 || size.height() > 16384)
        return Error::from_string_literal("Image size too large");

    return adopt_nonnull_ref_or_enomem(new (nothrow) Image(size));
}

Image::Image(Gfx::IntSize size)
    : m_size(size)
    , m_selection(*this)
{
}

void Image::paint_into(GUI::Painter& painter, Gfx::IntRect const& dest_rect) const
{
    float scale = (float)dest_rect.width() / (float)rect().width();
    Gfx::PainterStateSaver saver(painter);
    painter.add_clip_rect(dest_rect);
    for (auto const& layer : m_layers) {
        if (!layer.is_visible())
            continue;
        auto target = dest_rect.translated(layer.location().x() * scale, layer.location().y() * scale);
        target.set_size(layer.size().width() * scale, layer.size().height() * scale);
        painter.draw_scaled_bitmap(target, layer.display_bitmap(), layer.rect(), (float)layer.opacity_percent() / 100.0f);
    }
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Image::decode_bitmap(ReadonlyBytes bitmap_data)
{
    // Spawn a new ImageDecoder service process and connect to it.
    auto client = TRY(ImageDecoderClient::Client::try_create());

    // FIXME: Find a way to avoid the memory copying here.
    auto maybe_decoded_image = client->decode_image(bitmap_data);
    if (!maybe_decoded_image.has_value())
        return Error::from_string_literal("Image decode failed");

    // FIXME: Support multi-frame images?
    auto decoded_image = maybe_decoded_image.release_value();
    if (decoded_image.frames.is_empty())
        return Error::from_string_literal("Image decode failed (no frames)");

    auto decoded_bitmap = decoded_image.frames.first().bitmap;
    if (decoded_bitmap.is_null())
        return Error::from_string_literal("Image decode failed (no bitmap for frame)");
    return decoded_bitmap.release_nonnull();
}

ErrorOr<NonnullRefPtr<Image>> Image::create_from_bitmap(NonnullRefPtr<Gfx::Bitmap> const& bitmap)
{
    auto image = TRY(create_with_size({ bitmap->width(), bitmap->height() }));
    auto layer = TRY(Layer::create_with_bitmap(*image, *bitmap, "Background"));
    image->add_layer(move(layer));
    return image;
}

ErrorOr<NonnullRefPtr<Image>> Image::create_from_pixel_paint_json(JsonObject const& json)
{
    // FIXME: Handle invalid JSON data
    auto image = TRY(create_with_size({ json.get_i32("width"sv).value_or(0), json.get_i32("height"sv).value_or(0) }));

    auto layers_value = json.get_array("layers"sv).value();
    for (auto& layer_value : layers_value.values()) {
        auto const& layer_object = layer_value.as_object();
        auto name = layer_object.get_deprecated_string("name"sv).value();

        auto bitmap_base64_encoded = layer_object.get_deprecated_string("bitmap"sv).value();
        auto bitmap_data = TRY(decode_base64(bitmap_base64_encoded));
        auto bitmap = TRY(decode_bitmap(bitmap_data));
        auto layer = TRY(Layer::create_with_bitmap(*image, move(bitmap), name));

        if (auto const& mask_object = layer_object.get_deprecated_string("mask"sv); mask_object.has_value()) {
            auto mask_base64_encoded = mask_object.value();
            auto mask_data = TRY(decode_base64(mask_base64_encoded));
            auto mask = TRY(decode_bitmap(mask_data));
            TRY(layer->set_bitmaps(layer->content_bitmap(), mask));
        }

        auto width = layer_object.get_i32("width"sv).value_or(0);
        auto height = layer_object.get_i32("height"sv).value_or(0);

        if (width != layer->size().width() || height != layer->size().height())
            return Error::from_string_literal("Decoded layer bitmap has wrong size");

        image->add_layer(*layer);

        layer->set_location({ layer_object.get_i32("locationx"sv).value_or(0), layer_object.get_i32("locationy"sv).value_or(0) });
        layer->set_opacity_percent(layer_object.get_i32("opacity_percent"sv).value());
        layer->set_visible(layer_object.get_bool("visible"sv).value());
        layer->set_selected(layer_object.get_bool("selected"sv).value());
    }

    return image;
}

ErrorOr<void> Image::serialize_as_json(JsonObjectSerializer<StringBuilder>& json) const
{
    TRY(json.add("width"sv, m_size.width()));
    TRY(json.add("height"sv, m_size.height()));
    {
        auto json_layers = TRY(json.add_array("layers"sv));
        for (auto const& layer : m_layers) {
            auto json_layer = TRY(json_layers.add_object());
            TRY(json_layer.add("width"sv, layer.size().width()));
            TRY(json_layer.add("height"sv, layer.size().height()));
            TRY(json_layer.add("name"sv, layer.name()));
            TRY(json_layer.add("locationx"sv, layer.location().x()));
            TRY(json_layer.add("locationy"sv, layer.location().y()));
            TRY(json_layer.add("opacity_percent"sv, layer.opacity_percent()));
            TRY(json_layer.add("visible"sv, layer.is_visible()));
            TRY(json_layer.add("selected"sv, layer.is_selected()));
            TRY(json_layer.add("bitmap"sv, TRY(encode_base64(TRY(Gfx::PNGWriter::encode(layer.content_bitmap()))))));
            if (layer.is_masked())
                TRY(json_layer.add("mask"sv, TRY(encode_base64(TRY(Gfx::PNGWriter::encode(*layer.mask_bitmap()))))));
            TRY(json_layer.finish());
        }

        TRY(json_layers.finish());
    }
    return {};
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Image::compose_bitmap(Gfx::BitmapFormat format) const
{
    auto bitmap = TRY(Gfx::Bitmap::create(format, m_size));
    GUI::Painter painter(bitmap);
    paint_into(painter, { 0, 0, m_size.width(), m_size.height() });
    return bitmap;
}

RefPtr<Gfx::Bitmap> Image::copy_bitmap(Selection const& selection) const
{
    if (selection.is_empty())
        return {};
    auto selection_rect = selection.bounding_rect();

    // FIXME: Add a way to only compose a certain part of the image
    auto bitmap_or_error = compose_bitmap(Gfx::BitmapFormat::BGRA8888);
    if (bitmap_or_error.is_error())
        return {};
    auto full_bitmap = bitmap_or_error.release_value();

    auto cropped_bitmap_or_error = full_bitmap->cropped(selection_rect);
    if (cropped_bitmap_or_error.is_error())
        return nullptr;
    return cropped_bitmap_or_error.release_value_but_fixme_should_propagate_errors();
}

ErrorOr<void> Image::export_bmp_to_file(NonnullOwnPtr<Core::Stream::Stream> stream, bool preserve_alpha_channel) const
{
    auto bitmap_format = preserve_alpha_channel ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto bitmap = TRY(compose_bitmap(bitmap_format));

    Gfx::BMPWriter dumper;
    auto encoded_data = dumper.dump(bitmap);
    TRY(stream->write_entire_buffer(encoded_data));
    return {};
}

ErrorOr<void> Image::export_png_to_file(NonnullOwnPtr<Core::Stream::Stream> stream, bool preserve_alpha_channel) const
{
    auto bitmap_format = preserve_alpha_channel ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto bitmap = TRY(compose_bitmap(bitmap_format));

    auto encoded_data = TRY(Gfx::PNGWriter::encode(*bitmap));
    TRY(stream->write_entire_buffer(encoded_data));
    return {};
}

ErrorOr<void> Image::export_qoi_to_file(NonnullOwnPtr<Core::Stream::Stream> stream) const
{
    auto bitmap = TRY(compose_bitmap(Gfx::BitmapFormat::BGRA8888));

    auto encoded_data = Gfx::QOIWriter::encode(bitmap);
    TRY(stream->write_entire_buffer(encoded_data));
    return {};
}

void Image::add_layer(NonnullRefPtr<Layer> layer)
{
    for (auto& existing_layer : m_layers) {
        VERIFY(&existing_layer != layer.ptr());
    }
    m_layers.append(move(layer));

    for (auto* client : m_clients)
        client->image_did_add_layer(m_layers.size() - 1);

    did_modify_layer_stack();
}

ErrorOr<NonnullRefPtr<Image>> Image::take_snapshot() const
{
    auto snapshot = TRY(create_with_size(m_size));
    for (auto const& layer : m_layers) {
        auto layer_snapshot = TRY(Layer::create_snapshot(*snapshot, layer));
        snapshot->add_layer(move(layer_snapshot));
    }
    snapshot->m_selection.set_mask(m_selection.mask());
    return snapshot;
}

ErrorOr<void> Image::restore_snapshot(Image const& snapshot)
{
    m_layers.clear();
    select_layer(nullptr);

    bool layer_selected = false;
    for (auto const& snapshot_layer : snapshot.m_layers) {
        auto layer = TRY(Layer::create_snapshot(*this, snapshot_layer));
        if (layer->is_selected()) {
            select_layer(layer.ptr());
            layer_selected = true;
        }
        add_layer(*layer);
    }

    if (!layer_selected)
        select_layer(&layer(0));

    m_size = snapshot.size();

    m_selection.set_mask(snapshot.m_selection.mask());

    did_change_rect();
    did_modify_layer_stack();
    return {};
}

size_t Image::index_of(Layer const& layer) const
{
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (&m_layers.at(i) == &layer)
            return i;
    }
    VERIFY_NOT_REACHED();
}

void Image::move_layer_to_back(Layer& layer)
{
    NonnullRefPtr<Layer> protector(layer);
    auto index = index_of(layer);
    m_layers.remove(index);
    m_layers.prepend(layer);

    did_modify_layer_stack();
}

void Image::move_layer_to_front(Layer& layer)
{
    NonnullRefPtr<Layer> protector(layer);
    auto index = index_of(layer);
    m_layers.remove(index);
    m_layers.append(layer);

    did_modify_layer_stack();
}

void Image::move_layer_down(Layer& layer)
{
    NonnullRefPtr<Layer> protector(layer);
    auto index = index_of(layer);
    if (!index)
        return;
    m_layers.remove(index);
    m_layers.insert(index - 1, layer);

    did_modify_layer_stack();
}

void Image::move_layer_up(Layer& layer)
{
    NonnullRefPtr<Layer> protector(layer);
    auto index = index_of(layer);
    if (index == m_layers.size() - 1)
        return;
    m_layers.remove(index);
    m_layers.insert(index + 1, layer);

    did_modify_layer_stack();
}

void Image::change_layer_index(size_t old_index, size_t new_index)
{
    VERIFY(old_index < m_layers.size());
    VERIFY(new_index < m_layers.size());
    auto layer = m_layers.take(old_index);
    m_layers.insert(new_index, move(layer));
    did_modify_layer_stack();
}

void Image::did_modify_layer_stack()
{
    for (auto* client : m_clients)
        client->image_did_modify_layer_stack();

    did_change();
}

void Image::remove_layer(Layer& layer)
{
    NonnullRefPtr<Layer> protector(layer);
    auto index = index_of(layer);
    m_layers.remove(index);

    for (auto* client : m_clients)
        client->image_did_remove_layer(index);

    did_modify_layer_stack();
}

void Image::flatten_all_layers()
{
    if (m_layers.size() < 2)
        return;

    auto& bottom_layer = m_layers.at(0);

    GUI::Painter painter(bottom_layer.content_bitmap());
    paint_into(painter, { 0, 0, m_size.width(), m_size.height() });

    for (size_t index = m_layers.size() - 1; index > 0; index--) {
        auto& layer = m_layers.at(index);
        remove_layer(layer);
    }
    bottom_layer.set_name("Background");
    select_layer(&bottom_layer);
}

void Image::merge_visible_layers()
{
    if (m_layers.size() < 2)
        return;

    size_t index = 0;

    while (index < m_layers.size()) {
        if (m_layers.at(index).is_visible()) {
            auto& bottom_layer = m_layers.at(index);
            GUI::Painter painter(bottom_layer.content_bitmap());
            paint_into(painter, { 0, 0, m_size.width(), m_size.height() });
            select_layer(&bottom_layer);
            index++;
            break;
        }
        index++;
    }
    while (index < m_layers.size()) {
        if (m_layers.at(index).is_visible()) {
            auto& layer = m_layers.at(index);
            remove_layer(layer);
        } else {
            index++;
        }
    }
}

void Image::merge_active_layer_up(Layer& layer)
{
    if (m_layers.size() < 2)
        return;
    size_t layer_index = this->index_of(layer);
    if ((layer_index + 1) == m_layers.size()) {
        dbgln("Cannot merge layer up: layer is already at the top");
        return; // FIXME: Notify user of error properly.
    }

    auto& layer_above = m_layers.at(layer_index + 1);
    GUI::Painter painter(layer_above.content_bitmap());
    painter.draw_scaled_bitmap(rect(), layer.display_bitmap(), layer.rect(), (float)layer.opacity_percent() / 100.0f);
    remove_layer(layer);
    select_layer(&layer_above);
}

void Image::merge_active_layer_down(Layer& layer)
{
    if (m_layers.size() < 2)
        return;
    int layer_index = this->index_of(layer);
    if (layer_index == 0) {
        dbgln("Cannot merge layer down: layer is already at the bottom");
        return; // FIXME: Notify user of error properly.
    }

    auto& layer_below = m_layers.at(layer_index - 1);
    GUI::Painter painter(layer_below.content_bitmap());
    painter.draw_scaled_bitmap(rect(), layer.display_bitmap(), layer.rect(), (float)layer.opacity_percent() / 100.0f);
    remove_layer(layer);
    select_layer(&layer_below);
}

void Image::select_layer(Layer* layer)
{
    for (auto* client : m_clients)
        client->image_select_layer(layer);
}

void Image::add_client(ImageClient& client)
{
    VERIFY(!m_clients.contains(&client));
    m_clients.set(&client);
}

void Image::remove_client(ImageClient& client)
{
    VERIFY(m_clients.contains(&client));
    m_clients.remove(&client);
}

void Image::layer_did_modify_bitmap(Badge<Layer>, Layer const& layer, Gfx::IntRect const& modified_layer_rect)
{
    auto layer_index = index_of(layer);
    for (auto* client : m_clients)
        client->image_did_modify_layer_bitmap(layer_index);

    did_change(modified_layer_rect.translated(layer.location()));
}

void Image::layer_did_modify_properties(Badge<Layer>, Layer const& layer)
{
    auto layer_index = index_of(layer);
    for (auto* client : m_clients)
        client->image_did_modify_layer_properties(layer_index);

    did_change();
}

void Image::did_change(Gfx::IntRect const& a_modified_rect)
{
    auto modified_rect = a_modified_rect.is_empty() ? this->rect() : a_modified_rect;
    for (auto* client : m_clients)
        client->image_did_change(modified_rect);
}

void Image::did_change_rect(Gfx::IntRect const& a_modified_rect)
{
    auto modified_rect = a_modified_rect.is_empty() ? this->rect() : a_modified_rect;
    for (auto* client : m_clients)
        client->image_did_change_rect(modified_rect);
}

ImageUndoCommand::ImageUndoCommand(Image& image, DeprecatedString action_text)
    : m_snapshot(image.take_snapshot().release_value_but_fixme_should_propagate_errors())
    , m_image(image)
    , m_action_text(move(action_text))
{
}

void ImageUndoCommand::undo()
{
    // FIXME: Handle errors.
    (void)m_image.restore_snapshot(*m_snapshot);
}

void ImageUndoCommand::redo()
{
    undo();
}

ErrorOr<void> Image::flip(Gfx::Orientation orientation)
{
    Vector<NonnullRefPtr<Layer>> flipped_layers;
    TRY(flipped_layers.try_ensure_capacity(m_layers.size()));

    VERIFY(m_layers.size() > 0);

    size_t selected_layer_index = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        auto& layer = m_layers[i];
        auto new_layer = TRY(Layer::create_snapshot(*this, layer));

        if (layer.is_selected())
            selected_layer_index = i;

        TRY(new_layer->flip(orientation, Layer::NotifyClients::No));

        flipped_layers.unchecked_append(new_layer);
    }

    m_layers = move(flipped_layers);
    for (auto& layer : m_layers)
        layer.did_modify_bitmap({}, Layer::NotifyClients::No);

    select_layer(&m_layers[selected_layer_index]);

    did_change();

    return {};
}

ErrorOr<void> Image::rotate(Gfx::RotationDirection direction)
{
    Vector<NonnullRefPtr<Layer>> rotated_layers;
    TRY(rotated_layers.try_ensure_capacity(m_layers.size()));

    VERIFY(m_layers.size() > 0);

    size_t selected_layer_index = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        auto& layer = m_layers[i];
        auto new_layer = TRY(Layer::create_snapshot(*this, layer));

        if (layer.is_selected())
            selected_layer_index = i;

        TRY(new_layer->rotate(direction, Layer::NotifyClients::No));

        rotated_layers.unchecked_append(new_layer);
    }

    m_layers = move(rotated_layers);
    for (auto& layer : m_layers)
        layer.did_modify_bitmap({}, Layer::NotifyClients::Yes);

    select_layer(&m_layers[selected_layer_index]);

    m_size = { m_size.height(), m_size.width() };
    did_change_rect();

    return {};
}

ErrorOr<void> Image::crop(Gfx::IntRect const& cropped_rect)
{
    Vector<NonnullRefPtr<Layer>> cropped_layers;
    TRY(cropped_layers.try_ensure_capacity(m_layers.size()));

    VERIFY(m_layers.size() > 0);

    size_t selected_layer_index = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        auto& layer = m_layers[i];
        auto new_layer = TRY(Layer::create_snapshot(*this, layer));

        if (layer.is_selected())
            selected_layer_index = i;

        auto layer_location = new_layer->location();
        auto layer_local_crop_rect = new_layer->relative_rect().intersected(cropped_rect).translated(-layer_location.x(), -layer_location.y());
        TRY(new_layer->crop(layer_local_crop_rect, Layer::NotifyClients::No));

        auto new_layer_x = max(0, layer_location.x() - cropped_rect.x());
        auto new_layer_y = max(0, layer_location.y() - cropped_rect.y());

        new_layer->set_location({ new_layer_x, new_layer_y });

        cropped_layers.unchecked_append(new_layer);
    }

    m_layers = move(cropped_layers);
    for (auto& layer : m_layers)
        layer.did_modify_bitmap({}, Layer::NotifyClients::Yes);

    select_layer(&m_layers[selected_layer_index]);

    m_size = { cropped_rect.width(), cropped_rect.height() };
    did_change_rect(cropped_rect);

    return {};
}

Optional<Gfx::IntRect> Image::nonempty_content_bounding_rect() const
{
    if (m_layers.is_empty())
        return {};

    Optional<Gfx::IntRect> bounding_rect;
    for (auto const& layer : m_layers) {
        auto layer_content_rect_in_layer_coordinates = layer.nonempty_content_bounding_rect();
        if (!layer_content_rect_in_layer_coordinates.has_value())
            continue;
        auto layer_content_rect_in_image_coordinates = layer_content_rect_in_layer_coordinates->translated(layer.location());
        if (!bounding_rect.has_value())
            bounding_rect = layer_content_rect_in_image_coordinates;
        else
            bounding_rect = bounding_rect->united(layer_content_rect_in_image_coordinates);
    }

    return bounding_rect;
}

ErrorOr<void> Image::resize(Gfx::IntSize new_size, Gfx::Painter::ScalingMode scaling_mode)
{
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    if (size().width() != 0.0f) {
        scale_x = new_size.width() / static_cast<float>(size().width());
    }

    if (size().height() != 0.0f) {
        scale_y = new_size.height() / static_cast<float>(size().height());
    }

    Vector<NonnullRefPtr<Layer>> resized_layers;
    TRY(resized_layers.try_ensure_capacity(m_layers.size()));

    VERIFY(m_layers.size() > 0);

    size_t selected_layer_index = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        auto& layer = m_layers[i];
        auto new_layer = TRY(Layer::create_snapshot(*this, layer));

        if (layer.is_selected())
            selected_layer_index = i;

        Gfx::IntPoint new_location(scale_x * new_layer->location().x(), scale_y * new_layer->location().y());
        TRY(new_layer->resize(new_size, new_location, scaling_mode, Layer::NotifyClients::No));

        resized_layers.unchecked_append(new_layer);
    }

    m_layers = move(resized_layers);
    for (auto& layer : m_layers)
        layer.did_modify_bitmap({}, Layer::NotifyClients::Yes);

    select_layer(&m_layers[selected_layer_index]);

    m_size = { new_size.width(), new_size.height() };
    did_change_rect();

    return {};
}

Color Image::color_at(Gfx::IntPoint point) const
{
    Color color;
    for (auto const& layer : m_layers) {
        if (!layer.is_visible() || !layer.rect().contains(point))
            continue;

        auto layer_color = layer.display_bitmap().get_pixel(point);
        float layer_opacity = layer.opacity_percent() / 100.0f;
        layer_color.set_alpha((u8)(layer_color.alpha() * layer_opacity));
        color = color.blend(layer_color);
    }
    return color;
}

}
