/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGfx/Rect.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

class Box : public NodeWithStyleAndBoxModelMetrics {
    JS_CELL(Box, NodeWithStyleAndBoxModelMetrics);

public:
    Painting::PaintableBox const* paint_box() const;

    virtual void set_needs_display() override;

    bool is_body() const;

    virtual Optional<CSSPixels> intrinsic_width() const { return {}; }
    virtual Optional<CSSPixels> intrinsic_height() const { return {}; }
    virtual Optional<float> intrinsic_aspect_ratio() const { return {}; }

    bool has_intrinsic_width() const { return intrinsic_width().has_value(); }
    bool has_intrinsic_height() const { return intrinsic_height().has_value(); }
    bool has_intrinsic_aspect_ratio() const { return intrinsic_aspect_ratio().has_value(); }

    virtual ~Box() override;

    virtual void did_set_rect() { }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

    bool is_scrollable() const;
    CSSPixelPoint scroll_offset() const { return m_scroll_offset; }
    void set_scroll_offset(CSSPixelPoint);

protected:
    Box(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::StyleProperties>);
    Box(DOM::Document&, DOM::Node*, CSS::ComputedValues);

private:
    virtual bool is_box() const final { return true; }

    CSSPixelPoint m_scroll_offset;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
