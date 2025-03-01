/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedString.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/CanvasGradient.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasfillstrokestyles
template<typename IncludingClass>
class CanvasFillStrokeStyles {
public:
    ~CanvasFillStrokeStyles() = default;
    using FillOrStrokeStyleVariant = Variant<DeprecatedString, JS::Handle<CanvasGradient>>;

    static CanvasState::FillOrStrokeStyle to_canvas_state_fill_or_stoke_style(auto const& style)
    {
        return style.visit(
            [&](DeprecatedString const& string) -> CanvasState::FillOrStrokeStyle {
                return Gfx::Color::from_string(string).value_or(Color::Black);
            },
            [&](JS::Handle<CanvasGradient> gradient) -> CanvasState::FillOrStrokeStyle {
                return gradient;
            });
    }

    void set_fill_style(FillOrStrokeStyleVariant style)
    {
        // FIXME: 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's origin-clean flag to false.
        my_drawing_state().fill_style = to_canvas_state_fill_or_stoke_style(style);
    }

    FillOrStrokeStyleVariant fill_style() const
    {
        return my_drawing_state().fill_style.to_js_fill_or_stoke_style();
    }

    void set_stroke_style(FillOrStrokeStyleVariant style)
    {
        // FIXME: 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's origin-clean flag to false.
        my_drawing_state().stroke_style = to_canvas_state_fill_or_stoke_style(style);
    }

    FillOrStrokeStyleVariant stroke_style() const
    {
        return my_drawing_state().stroke_style.to_js_fill_or_stoke_style();
    }

    WebIDL::ExceptionOr<JS::NonnullGCPtr<CanvasGradient>> create_radial_gradient(double x0, double y0, double r0, double x1, double y1, double r1)
    {
        auto& realm = static_cast<IncludingClass&>(*this).realm();
        return CanvasGradient::create_radial(realm, x0, y0, r0, x1, y1, r1);
    }

    JS::NonnullGCPtr<CanvasGradient> create_linear_gradient(double x0, double y0, double x1, double y1)
    {
        auto& realm = static_cast<IncludingClass&>(*this).realm();
        return CanvasGradient::create_linear(realm, x0, y0, x1, y1);
    }

    JS::NonnullGCPtr<CanvasGradient> create_conic_gradient(double start_angle, double x, double y)
    {
        auto& realm = static_cast<IncludingClass&>(*this).realm();
        return CanvasGradient::create_conic(realm, start_angle, x, y);
    }

protected:
    CanvasFillStrokeStyles() = default;

private:
    CanvasState::DrawingState& my_drawing_state() { return reinterpret_cast<IncludingClass&>(*this).drawing_state(); }
    CanvasState::DrawingState const& my_drawing_state() const { return reinterpret_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
